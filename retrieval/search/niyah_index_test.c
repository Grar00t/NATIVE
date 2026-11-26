#include "niyah_index.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int reference_order(const void *left, const void *right) {
    const NiyahSearchHit *a = left, *b = right;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    return (a->document_id > b->document_id) - (a->document_id < b->document_id);
}

static void test_windowed_scores(void) {
    enum { DOCS = 800 };
    NiyahInvertedIndex index;
    NiyahSearchHit expected[DOCS], actual[DOCS];
    NiyahDocument doc = {0};
    size_t used = 0;
    niyah_index_init(&index, 1.2, 0.75);
    for (size_t i = 0; i < DOCS; ++i) {
        doc.document_id = UINT64_MAX - UINT64_C(0x9e3779b97f4a7c15) * i;
        snprintf(doc.text, sizeof(doc.text), "base word%zu %s %s", i,
                 i % 257 == 0 ? "rare" : "filler", i % 7 == 0 ? "عربي" : "extra");
        assert(niyah_index_add_document(&index, &doc));
    }
    const double rare = log(1.0 + (DOCS - 4 + 0.5) / (4 + 0.5));
    const double arabic = log(1.0 + (DOCS - 115 + 0.5) / (115 + 0.5));
    for (size_t i = 0; i < DOCS; ++i) {
        uint64_t id = UINT64_MAX - UINT64_C(0x9e3779b97f4a7c15) * i;
        const NiyahDocument *found = niyah_index_document(&index, id);
        assert(found && found->document_id == id && found->term_count == 4);
        double score = 0.0;
        if (i % 257 == 0) score += rare;
        if (i % 7 == 0) score += arabic;
        if (i % 257 == 0) score += rare;
        if (score) expected[used++] = (NiyahSearchHit){id, score};
    }
    qsort(expected, used, sizeof(*expected), reference_order);
    const size_t caps[] = {1, 2, 7, 32, 115, 800};
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); ++c) {
        size_t n = niyah_index_search(&index, "rare absent عربي rare", actual, caps[c]);
        assert(n == (caps[c] < used ? caps[c] : used));
        for (size_t i = 0; i < n; ++i) {
            assert(actual[i].document_id == expected[i].document_id);
            assert(actual[i].score == expected[i].score);
        }
    }
    assert(niyah_index_search(&index, "absent", actual, DOCS) == 0);
    assert(niyah_index_search(&index, "word799", actual, 1) == 1);
    assert(actual[0].document_id == doc.document_id);
    niyah_index_free(&index);
}

int main(void) {
    test_windowed_scores();
    NiyahInvertedIndex index;
    niyah_index_init(&index, 1.2, 0.75);

    NiyahDocument a = { .document_id = 1 };
    strcpy(a.url, "https://example.test/a");
    strcpy(a.title, "Systems Programming");
    strcpy(a.text, "C programming systems memory kernel");

    NiyahDocument b = { .document_id = 2 };
    strcpy(b.url, "https://example.test/b");
    strcpy(b.title, "Search Engine");
    strcpy(b.text, "search indexing ranking crawler");

    assert(niyah_index_add_document(&index, &a));
    assert(niyah_index_add_document(&index, &b));
    assert(!niyah_index_add_document(&index, &a));

    NiyahSearchHit hits[4];
    size_t count = niyah_index_search(&index, "kernel memory", hits, 4);
    assert(count == 1);
    assert(hits[0].document_id == 1);
    assert(hits[0].score > 0.0);

    niyah_index_free(&index);
    niyah_index_init(&index, 1.2, 0.75);
    const uint64_t ids[] = {9, 8, 1, 7, 2};
    strcpy(a.text, "alpha beta");
    for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); ++i) {
        a.document_id = ids[i];
        assert(niyah_index_add_document(&index, &a));
    }
    a.document_id = 20;
    a.text[0] = '\0';
    assert(niyah_index_add_document(&index, &a));
    assert(fabs(index.average_document_length - 10.0 / 6.0) < 1e-12);
    assert(niyah_index_search(&index, "alpha", hits, 2) == 2);
    assert(hits[0].document_id == 1 && hits[1].document_id == 2);
    a.document_id = 21;
    for (size_t i = 0; i < 1100; ++i) memcpy(a.text + 2 * i, "x ", 2);
    strcpy(a.text + 2200, "lastword");
    assert(niyah_index_add_document(&index, &a));
    assert(niyah_index_search(&index, "lastword", hits, 1) == 1 && hits[0].document_id == 21);
    a.document_id = 23;
    strcpy(a.text, "مصدر،خبر؛بحث؟نص ثابت");
    assert(niyah_index_add_document(&index, &a));
    const char *words[] = {"مصدر", "خبر", "بحث", "نص", "ثابت"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        assert(niyah_index_search(&index, words[i], hits, 1) == 1 && hits[0].document_id == 23);
    }
    a.document_id = 24;
    memset(a.text, 'z', 62);
    strcpy(a.text + 62, "شx tail");
    assert(niyah_index_add_document(&index, &a));
    a.text[62] = '\0';
    assert(niyah_index_search(&index, a.text, hits, 1) == 1 && hits[0].document_id == 24);
    a.document_id = 25;
    strcpy(a.text, "left\xED\xA0\x80" "right");
    assert(niyah_index_add_document(&index, &a));
    assert(niyah_index_search(&index, "right", hits, 1) == 1 && hits[0].document_id == 25);
    memset(a.text, 'z', sizeof(a.text));
    a.document_id = 22;
    assert(!niyah_index_add_document(&index, &a));
    niyah_index_free(&index);
    return 0;
}
