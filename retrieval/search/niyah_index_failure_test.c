#include "niyah_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_realloc(void *, size_t);
void *__real_calloc(size_t, size_t);
static size_t allocs, fail_at;

void *__wrap_realloc(void *p, size_t n) {
    if (fail_at && ++allocs == fail_at) return NULL;
    return __real_realloc(p, n);
}

void *__wrap_calloc(size_t count, size_t size) {
    if (fail_at && ++allocs == fail_at) return NULL;
    return __real_calloc(count, size);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "index retry at line %d\n", __LINE__); return 1; } } while (0)

static int check_retry(size_t initial) {
    for (size_t nth = 1; nth < 32; ++nth) {
        NiyahInvertedIndex index;
        NiyahDocument doc = {.document_id = 1};
        NiyahSearchHit hits[4];
        niyah_index_init(&index, 1.2, 0.75);
        for (size_t i = 0; i < initial; ++i) {
            doc.document_id = i + 1;
            snprintf(doc.text, sizeof(doc.text), "word%zu", i);
            CHECK(niyah_index_add_document(&index, &doc));
        }
        doc.document_id = UINT64_MAX;
        strcpy(doc.text, "word0 beta gamma");
        allocs = 0;
        fail_at = nth;
        int accepted = niyah_index_add_document(&index, &doc);
        fail_at = 0;
        if (!accepted) {
            CHECK(index.document_count == initial && index.term_count == initial);
            CHECK(!niyah_index_document(&index, UINT64_MAX));
            CHECK(niyah_index_search(&index, "beta", hits, 2) == 0);
            for (size_t i = 0; i < initial; ++i) {
                const NiyahDocument *old = niyah_index_document(&index, i + 1);
                CHECK(old && niyah_index_search(&index, old->text, hits, 2) == 1);
                CHECK(hits[0].document_id == i + 1);
            }
            CHECK(niyah_index_add_document(&index, &doc));
        }
        CHECK(niyah_index_search(&index, "gamma", hits, 2) == 1 && hits[0].document_id == UINT64_MAX);
        CHECK(niyah_index_search(&index, "word0", hits, 2) == (initial ? 2u : 1u));
        niyah_index_free(&index);
        if (accepted) return nth >= 3 ? 0 : 1;
    }
    return 1;
}

int main(void) {
    return check_retry(0) || check_retry(12) || check_retry(128);
}
