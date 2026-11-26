#include "niyah_index.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <string.h>

int main(void) {
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
    memset(a.text, 'z', sizeof(a.text));
    a.document_id = 22;
    assert(!niyah_index_add_document(&index, &a));
    niyah_index_free(&index);
    return 0;
}
