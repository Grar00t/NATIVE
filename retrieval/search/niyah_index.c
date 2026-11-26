#include "niyah_index.h"
#include "../native/niyah_utf8.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int term_compare(const void *a, const void *b) {
    const NiyahSearchHit *ha = (const NiyahSearchHit *)a;
    const NiyahSearchHit *hb = (const NiyahSearchHit *)b;
    if (ha->score < hb->score) return 1;
    if (ha->score > hb->score) return -1;
    if (ha->document_id < hb->document_id) return -1;
    if (ha->document_id > hb->document_id) return 1;
    return 0;
}

static bool term_character(uint32_t c) {
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    return c > 0xA0 && c != 0x060C && c != 0x061B && c != 0x061C && c != 0x061F &&
           c != 0x06D4 && !(c >= 0x2000 && c <= 0x206F) && c != 0x3000 &&
           c != 0xFEFF && c != 0xFFFD;
}

static size_t tokenize(const char *text, char tokens[][NIYAH_TERM_MAX], size_t max_tokens) {
    if (!text || max_tokens == 0) return 0;
    size_t count = 0, pos = 0, used = 0;
    const unsigned char *p = (const unsigned char *)text;
    const size_t length = strlen(text);
    bool clipped = false;
    while (pos < length && count < max_tokens) {
        size_t start = pos;
        uint32_t c = niyah_utf8_next(p, length, &pos);
        if (!term_character(c)) {
            if (used) {
                tokens[count++][used] = '\0';
                used = 0;
            }
            clipped = false;
            continue;
        }
        size_t bytes = pos - start;
        if (used + bytes >= NIYAH_TERM_MAX) clipped = true;
        if (clipped) continue;
        if (bytes == 1) {
            tokens[count][used++] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        } else {
            memcpy(tokens[count] + used, p + start, bytes);
            used += bytes;
        }
    }
    if (used) tokens[count++][used] = '\0';
    return count;
}

enum { GROUP_WIDTH = 16 };
struct NiyahLookupGroup {
    uint8_t tags[GROUP_WIDTH];
    size_t rows[GROUP_WIDTH];
};

static uint64_t mix_id(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static uint64_t term_hash(const char *text) {
    uint64_t h = UINT64_C(14695981039346656037);
    while (*text) h = (h ^ (unsigned char)*text++) * UINT64_C(1099511628211);
    return mix_id(h);
}

/* Slots are filled left to right. With no deletions, an empty slot ends a probe. */
static size_t lookup_row(const NiyahInvertedIndex *index, const NiyahLookup *map,
                         uint64_t hash, const char *word, uint64_t id) {
    if (!map->group_count) return SIZE_MAX;
    const uint8_t tag = (uint8_t)(1u + (hash & 127u));
    size_t g = (size_t)(hash >> 7) & (map->group_count - 1u);
    for (;;) {
        const struct NiyahLookupGroup *group = &map->groups[g];
        for (size_t i = 0; i < GROUP_WIDTH; ++i) {
            if (!group->tags[i]) return SIZE_MAX;
            if (group->tags[i] != tag) continue;
            size_t row = group->rows[i];
            if (word ? strcmp(index->terms[row].term, word) == 0
                     : index->documents[row].document_id == id) return row;
        }
        g = (g + 1u) & (map->group_count - 1u);
    }
}

static void insert_row(NiyahLookup *map, uint64_t hash, size_t row) {
    size_t g = (size_t)(hash >> 7) & (map->group_count - 1u);
    for (;;) {
        struct NiyahLookupGroup *group = &map->groups[g];
        for (size_t i = 0; i < GROUP_WIDTH; ++i) {
            if (group->tags[i]) continue;
            group->tags[i] = (uint8_t)(1u + (hash & 127u));
            group->rows[i] = row;
            return;
        }
        g = (g + 1u) & (map->group_count - 1u);
    }
}

static bool reserve_rows(const NiyahInvertedIndex *index, NiyahLookup *map,
                         size_t committed, size_t needed, bool terms) {
    const size_t per_group = GROUP_WIDTH * 3u / 4u;
    size_t groups = map->group_count;
    if (needed <= groups * per_group) return true;
    if (!groups) groups = 1;
    while (needed > groups * per_group) {
        if (groups > SIZE_MAX / sizeof(*map->groups) / 2u) return false;
        groups *= 2u;
    }
    NiyahLookup fresh = {calloc(groups, sizeof(*map->groups)), groups};
    if (!fresh.groups) return false;
    for (size_t i = 0; i < committed; ++i)
        insert_row(&fresh, terms ? term_hash(index->terms[i].term)
                                 : mix_id(index->documents[i].document_id), i);
    free(map->groups);
    *map = fresh;
    return true;
}

static bool grow_terms(NiyahInvertedIndex *index) {
    size_t next = index->term_capacity ? index->term_capacity * 2u : 128u;
    if (next < index->term_capacity || next > SIZE_MAX / sizeof(*index->terms)) return false;
    NiyahTermEntry *p = realloc(index->terms, next * sizeof(*p));
    if (!p) return false;
    memset(p + index->term_capacity, 0, (next - index->term_capacity) * sizeof(*p));
    index->terms = p;
    index->term_capacity = next;
    return true;
}

static bool grow_documents(NiyahInvertedIndex *index) {
    size_t next = index->document_capacity ? index->document_capacity * 2u : 64u;
    if (next < index->document_capacity || next > SIZE_MAX / sizeof(*index->documents)) return false;
    NiyahDocument *p = realloc(index->documents, next * sizeof(*p));
    if (!p) return false;
    index->documents = p;
    index->document_capacity = next;
    return true;
}

static bool grow_postings(NiyahTermEntry *entry) {
    size_t next = entry->posting_capacity ? entry->posting_capacity * 2u : 16u;
    if (next < entry->posting_capacity || next > SIZE_MAX / sizeof(*entry->postings)) return false;
    NiyahPosting *p = realloc(entry->postings, next * sizeof(*p));
    if (!p) return false;
    entry->postings = p;
    entry->posting_capacity = next;
    return true;
}

void niyah_index_init(NiyahInvertedIndex *index, double k1, double b) {
    if (!index) return;
    memset(index, 0, sizeof(*index));
    index->k1 = (k1 > 0.0) ? k1 : 1.2;
    index->b = (b >= 0.0 && b <= 1.0) ? b : 0.75;
}

void niyah_index_free(NiyahInvertedIndex *index) {
    if (!index) return;
    for (size_t i = 0; i < index->term_count; ++i)
        free(index->terms[i].postings);
    free(index->terms);
    free(index->documents);
    free(index->terms_by_text.groups);
    free(index->documents_by_id.groups);
    memset(index, 0, sizeof(*index));
}

static int compare_tokens(const void *a, const void *b) { return strcmp(a, b); }

bool niyah_index_add_document(NiyahInvertedIndex *index, const NiyahDocument *document) {
    enum { MAX_TOKENS = (NIYAH_INDEX_TEXT_MAX + 1) / 2 };
    struct PendingPosting { size_t term; uint32_t frequency; };
    char tokens[MAX_TOKENS][NIYAH_TERM_MAX];
    struct PendingPosting pending[MAX_TOKENS];
    size_t used = 0;
    if (!index || !document || !document->document_id ||
        !memchr(document->text, 0, sizeof(document->text)) ||
        !memchr(document->title, 0, sizeof(document->title)) ||
        !memchr(document->url, 0, sizeof(document->url)) ||
        niyah_index_document(index, document->document_id)) return false;

    if (index->document_count == index->document_capacity && !grow_documents(index)) return false;
    const size_t n = tokenize(document->text, tokens, MAX_TOKENS);
    qsort(tokens, n, sizeof(tokens[0]), compare_tokens);
    const size_t original_terms = index->term_count;
    for (size_t first = 0; first < n;) {
        size_t end = first + 1;
        while (end < n && strcmp(tokens[first], tokens[end]) == 0) ++end;
        size_t row = lookup_row(index, &index->terms_by_text, term_hash(tokens[first]), tokens[first], 0);
        if (row == SIZE_MAX) {
            if (index->term_count == index->term_capacity && !grow_terms(index)) goto rollback;
            row = index->term_count++;
            strcpy(index->terms[row].term, tokens[first]);
        }
        NiyahTermEntry *entry = &index->terms[row];
        if (entry->document_frequency == UINT32_MAX ||
            (entry->posting_count == entry->posting_capacity && !grow_postings(entry))) goto rollback;
        pending[used].term = row;
        pending[used++].frequency = (uint32_t)(end - first);
        first = end;
    }

    if (!reserve_rows(index, &index->terms_by_text, original_terms, index->term_count, true) ||
        !reserve_rows(index, &index->documents_by_id, index->document_count,
                      index->document_count + 1u, false)) goto rollback;

    /* No allocations follow publication. Rehashes contain only committed rows. */
    for (size_t i = original_terms; i < index->term_count; ++i)
        insert_row(&index->terms_by_text, term_hash(index->terms[i].term), i);
    uint32_t ordinal = index->document_count < UINT32_MAX ? (uint32_t)index->document_count : UINT32_MAX;
    for (size_t i = 0; i < used; ++i) {
        NiyahTermEntry *entry = &index->terms[pending[i].term];
        entry->postings[entry->posting_count++] =
            (NiyahPosting){document->document_id, pending[i].frequency, ordinal};
        ++entry->document_frequency;
    }
    NiyahDocument *dst = &index->documents[index->document_count];
    *dst = *document;
    dst->term_count = (uint32_t)n;
    insert_row(&index->documents_by_id, mix_id(dst->document_id), index->document_count);
    ++index->document_count;
    index->average_document_length +=
        ((double)n - index->average_document_length) / (double)index->document_count;
    return true;

rollback:
    while (index->term_count > original_terms) {
        NiyahTermEntry *entry = &index->terms[--index->term_count];
        free(entry->postings);
        memset(entry, 0, sizeof(*entry));
    }
    return false;
}

/* The least desirable hit is at the root, including ID as the tie-breaker. */
static void sift_down(NiyahSearchHit *heap, size_t count, size_t parent) {
    while (parent < count / 2) {
        size_t child = parent * 2 + 1;
        if (child + 1 < count && term_compare(&heap[child + 1], &heap[child]) > 0) ++child;
        if (term_compare(&heap[parent], &heap[child]) >= 0) break;
        NiyahSearchHit tmp = heap[parent];
        heap[parent] = heap[child];
        heap[child] = tmp;
        parent = child;
    }
}

/* Each posting list follows insertion order even when external IDs are sparse. */
static size_t posting_row(const NiyahInvertedIndex *index, const NiyahPosting *posting) {
    if (posting->ordinal != UINT32_MAX) return posting->ordinal;
    return lookup_row(index, &index->documents_by_id, mix_id(posting->document_id), NULL,
                      posting->document_id);
}

static void retain_hit(NiyahSearchHit *hits, size_t cap, size_t *count, NiyahSearchHit next) {
    if (*count < cap) {
        hits[(*count)++] = next;
        if (*count == cap)
            for (size_t p = cap / 2; p > 0; --p) sift_down(hits, cap, p - 1);
    } else if (term_compare(&next, &hits[0]) < 0) {
        hits[0] = next;
        sift_down(hits, cap, 0);
    }
}

size_t niyah_index_search(const NiyahInvertedIndex *index, const char *query,
                          NiyahSearchHit *hits, size_t hit_capacity) {
    enum { QUERY_LIMIT = 128, WINDOW = 256 };
    struct Cursor { const NiyahTermEntry *entry; size_t pos; double idf; };
    if (!index || !query || !hits || !hit_capacity || !index->document_count) return 0;

    char words[QUERY_LIMIT][NIYAH_TERM_MAX];
    struct Cursor cursors[QUERY_LIMIT];
    size_t used = 0, first = SIZE_MAX, count = 0;
    const size_t n = tokenize(query, words, QUERY_LIMIT);
    for (size_t i = 0; i < n; ++i) {
        size_t term = lookup_row(index, &index->terms_by_text, term_hash(words[i]), words[i], 0);
        if (term == SIZE_MAX) continue;
        const NiyahTermEntry *entry = &index->terms[term];
        double df = (double)entry->document_frequency;
        double idf = log(1.0 + ((double)index->document_count - df + 0.5) / (df + 0.5));
        cursors[used++] = (struct Cursor){entry, 0, idf};
        size_t row = posting_row(index, &entry->postings[0]);
        if (row < first) first = row;
    }
    if (!used) return 0;
    const double avg = index->average_document_length;
    double scores[WINDOW] = {0};
    bool seen[WINDOW] = {false};
    uint8_t touched[WINDOW];
    while (first != SIZE_MAX) {
        const size_t base = first - first % WINDOW;
        size_t next = SIZE_MAX, touched_count = 0;
        /* Keep query order so duplicate terms and floating-point sums match BM25. */
        for (size_t i = 0; i < used; ++i) {
            struct Cursor *c = &cursors[i];
            while (c->pos < c->entry->posting_count) {
                const NiyahPosting *p = &c->entry->postings[c->pos];
                size_t row = posting_row(index, p);
                if (row - base >= WINDOW) {
                    if (row < next) next = row;
                    break;
                }
                size_t slot = row - base;
                if (!seen[slot]) {
                    seen[slot] = true;
                    touched[touched_count++] = (uint8_t)slot;
                }
                double tf = (double)p->term_frequency;
                double dl = (double)index->documents[row].term_count;
                double norm = index->k1 * (1.0 - index->b + index->b * (dl / avg));
                scores[slot] += c->idf * ((tf * (index->k1 + 1.0)) / (tf + norm));
                ++c->pos;
            }
        }
        for (size_t i = 0; i < touched_count; ++i) {
            size_t slot = touched[i];
            NiyahSearchHit hit = {index->documents[base + slot].document_id, scores[slot]};
            retain_hit(hits, hit_capacity, &count, hit);
            scores[slot] = 0;
            seen[slot] = false;
        }
        first = next;
    }
    qsort(hits, count, sizeof(*hits), term_compare);
    return count;
}

const NiyahDocument *niyah_index_document(const NiyahInvertedIndex *index, uint64_t document_id) {
    if (!index || !document_id) return NULL;
    size_t row = lookup_row(index, &index->documents_by_id, mix_id(document_id), NULL, document_id);
    return row == SIZE_MAX ? NULL : &index->documents[row];
}
