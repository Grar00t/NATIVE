#include "niyah_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_realloc(void *, size_t);
static size_t allocs, fail_at;

void *__wrap_realloc(void *p, size_t n) {
    if (fail_at && ++allocs == fail_at) return NULL;
    return __real_realloc(p, n);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "index retry at line %d\n", __LINE__); return 1; } } while (0)

int main(void) {
    for (size_t nth = 1; nth < 10; ++nth) {
        NiyahInvertedIndex index;
        NiyahDocument doc = {.document_id = 1};
        NiyahSearchHit hits[2];
        niyah_index_init(&index, 1.2, 0.75);
        strcpy(doc.text, "alpha");
        CHECK(niyah_index_add_document(&index, &doc));
        doc.document_id = 2;
        strcpy(doc.text, "alpha beta gamma");
        allocs = 0;
        fail_at = nth;
        int accepted = niyah_index_add_document(&index, &doc);
        fail_at = 0;
        if (!accepted) {
            CHECK(index.document_count == 1 && index.term_count == 1 && index.average_document_length == 1);
            CHECK(niyah_index_search(&index, "beta", hits, 2) == 0);
            CHECK(niyah_index_add_document(&index, &doc));
        }
        CHECK(niyah_index_search(&index, "gamma", hits, 2) == 1 && hits[0].document_id == 2);
        CHECK(niyah_index_search(&index, "alpha", hits, 2) == 2);
        niyah_index_free(&index);
        if (accepted) return nth >= 3 ? 0 : 1;
    }
    return 1;
}
