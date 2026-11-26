#include "niyah_index.h"

#include <ctype.h>
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

static bool token_byte(unsigned char c) {
    return isalnum(c) != 0 || c >= 0x80u || c == '_' || c == '-';
}

static size_t tokenize(const char *text, char tokens[][NIYAH_TERM_MAX], size_t max_tokens) {
    if (!text || max_tokens == 0) return 0;
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && count < max_tokens) {
        while (*p && !token_byte(*p)) ++p;
        if (!*p) break;

        size_t len = 0;
        while (*p && token_byte(*p)) {
            if (len + 1u < NIYAH_TERM_MAX) {
                unsigned char c = *p;
                tokens[count][len++] = (char)(c < 0x80u ? tolower(c) : c);
            }
            ++p;
        }
        tokens[count][len] = '\0';
        if (len > 0) ++count;
    }
    return count;
}

static NiyahTermEntry *find_term(NiyahInvertedIndex *index, const char *term) {
    for (size_t i = 0; i < index->term_count; ++i)
        if (strcmp(index->terms[i].term, term) == 0) return &index->terms[i];
    return NULL;
}

static const NiyahTermEntry *find_term_const(const NiyahInvertedIndex *index, const char *term) {
    for (size_t i = 0; i < index->term_count; ++i)
        if (strcmp(index->terms[i].term, term) == 0) return &index->terms[i];
    return NULL;
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
        NiyahTermEntry *entry = find_term(index, tokens[first]);
        if (!entry) {
            if (index->term_count == index->term_capacity && !grow_terms(index)) goto rollback;
            entry = &index->terms[index->term_count++];
            strcpy(entry->term, tokens[first]);
        }
        if (entry->document_frequency == UINT32_MAX ||
            (entry->posting_count == entry->posting_capacity && !grow_postings(entry))) goto rollback;
        pending[used].term = (size_t)(entry - index->terms);
        pending[used++].frequency = (uint32_t)(end - first);
        first = end;
    }

    /* All storage is ready. A failed insertion must be retryable with the same ID. */
    for (size_t i = 0; i < used; ++i) {
        NiyahTermEntry *entry = &index->terms[pending[i].term];
        entry->postings[entry->posting_count++] =
            (NiyahPosting){document->document_id, pending[i].frequency};
        ++entry->document_frequency;
    }
    NiyahDocument *dst = &index->documents[index->document_count];
    *dst = *document;
    dst->term_count = (uint32_t)n;
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

static double bm25_score(const NiyahInvertedIndex *index,
                         const NiyahTermEntry *entry,
                         uint32_t tf,
                         uint32_t doc_len) {
    if (!index || !entry || tf == 0 || doc_len == 0 || index->document_count == 0) return 0.0;
    double n = (double)index->document_count;
    double df = (double)entry->document_frequency;
    double idf = log(1.0 + (n - df + 0.5) / (df + 0.5));
    double avg = index->average_document_length > 0.0 ? index->average_document_length : 1.0;
    double norm = index->k1 * (1.0 - index->b + index->b * ((double)doc_len / avg));
    return idf * (((double)tf * (index->k1 + 1.0)) / ((double)tf + norm));
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

size_t niyah_index_search(const NiyahInvertedIndex *index,
                          const char *query,
                          NiyahSearchHit *hits,
                          size_t hit_capacity) {
    if (!index || !query || !hits || hit_capacity == 0 || index->document_count == 0) return 0;

    double *scores = calloc(index->document_count, sizeof(*scores));
    if (!scores) return 0;

    char qtokens[128][NIYAH_TERM_MAX];
    size_t qn = tokenize(query, qtokens, 128);
    for (size_t qi = 0; qi < qn; ++qi) {
        const NiyahTermEntry *entry = find_term_const(index, qtokens[qi]);
        if (!entry) continue;
        for (size_t pi = 0; pi < entry->posting_count; ++pi) {
            const NiyahPosting *posting = &entry->postings[pi];
            const NiyahDocument *doc = niyah_index_document(index, posting->document_id);
            if (!doc) continue;
            size_t index_pos = (size_t)(doc - index->documents);
            scores[index_pos] += bm25_score(index, entry, posting->term_frequency, doc->term_count);
        }
    }

    size_t count = 0;
    for (size_t i = 0; i < index->document_count; ++i) {
        if (scores[i] <= 0.0) continue;
        NiyahSearchHit next = {index->documents[i].document_id, scores[i]};
        if (count < hit_capacity) {
            hits[count++] = next;
            if (count == hit_capacity) {
                for (size_t p = count / 2; p > 0; --p) sift_down(hits, count, p - 1);
            }
        } else if (term_compare(&next, &hits[0]) < 0) {
            hits[0] = next;
            sift_down(hits, count, 0);
        }
    }

    qsort(hits, count, sizeof(*hits), term_compare);
    free(scores);
    return count;
}

const NiyahDocument *niyah_index_document(const NiyahInvertedIndex *index, uint64_t document_id) {
    if (!index || document_id == 0) return NULL;
    for (size_t i = 0; i < index->document_count; ++i)
        if (index->documents[i].document_id == document_id) return &index->documents[i];
    return NULL;
}
