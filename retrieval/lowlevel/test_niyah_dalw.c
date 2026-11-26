#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dalw.h"
#include "niyah_dalw.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int build_shard(const char *path, const char *const *titles, const char *const *texts, size_t n)
{
    size_t i;
    int rc;
    DalwBuilder *b;
    remove(path);
    b = dalw_builder_new();
    if (!b) return -1;
    for (i = 0; i < n; i++) {
        if (dalw_builder_add(b, titles[i], texts[i], strlen(texts[i])) != 0) {
            dalw_builder_free(b);
            return -1;
        }
    }
    rc = dalw_builder_write(b, path);
    dalw_builder_free(b);
    return rc;
}

int main(void)
{
    static const char *t1[] = { "d0", "d1", "d2" };
    static const char *x1[] = {
        "local search index in Riyadh",
        "weather today in Jeddah",
        "search engine ranking with BM25 search"
    };
    static const char *t2[] = { "e0" };
    static const char *x2[] = { "local search across documents" };
    const char *p1 = "./niyah_dalw_a.dalw";
    const char *p2 = "./niyah_dalw_b.dalw";
    const char *both[2];
    NiyahDalw *h;
    NiyahDalwHit hits[16];
    NiyahSearchHit sh[16];
    char receipt[8192];
    int st = -99;
    size_t n, i;

    CHECK(build_shard(p1, t1, x1, 3) == 0);
    CHECK(build_shard(p2, t2, x2, 1) == 0);

    /* argument checks */
    CHECK(niyah_dalw_open(NULL, 1, 0, &st) == NULL && st == NIYAH_DALW_E_ARG);
    both[0] = "./niyah_dalw_missing.dalw";
    CHECK(niyah_dalw_open(both, 1, 0, &st) == NULL && st == NIYAH_DALW_E_IO);

    /* one shard */
    both[0] = p1;
    h = niyah_dalw_open(both, 1, 1, &st);
    CHECK(h != NULL && st == NIYAH_DALW_OK);
    if (!h) { puts("SOME TESTS FAILED"); return 1; }
    CHECK(niyah_dalw_shard_count(h) == 1u);
    CHECK(niyah_dalw_doc_count(h) == 3u);

    n = niyah_dalw_query(h, "weather", 1, hits, 16u, &st);
    CHECK(st == NIYAH_DALW_OK && n == 1u && hits[0].doc == 1u);

    n = niyah_dalw_query(h, "search", 1, hits, 16u, &st);
    CHECK(st == NIYAH_DALW_OK && n == 2u);
    CHECK(n == 2u && hits[0].score >= hits[1].score);
    CHECK(n == 2u && hits[0].document_id != hits[1].document_id);

    n = niyah_dalw_query(h, "zzzzqqq", 1, hits, 16u, &st);
    CHECK(st == NIYAH_DALW_OK && n == 0u);

    n = niyah_dalw_query(h, "", 1, hits, 16u, &st);
    CHECK(n == 0u && st == NIYAH_DALW_E_ARG);

    /* cap is respected */
    n = niyah_dalw_query(h, "search", 1, hits, 1u, &st);
    CHECK(n == 1u);

    /* receipt: one JSON line carrying an id */
    n = niyah_dalw_query(h, "weather", 1, hits, 16u, &st);
    CHECK(n == 1u);
    CHECK(niyah_dalw_receipt(h, &hits[0], "weather", 1, receipt, sizeof receipt) > 0);
    CHECK(receipt[0] == '{' && strstr(receipt, "\"id\":\"") != NULL);
    CHECK(niyah_dalw_receipt(h, &hits[0], "", 1, receipt, sizeof receipt) == -1);
    niyah_dalw_close(h);

    /* two shards merge into one ranked list */
    both[0] = p1;
    both[1] = p2;
    h = niyah_dalw_open(both, 2, 0, &st);
    CHECK(h != NULL);
    if (h) {
        CHECK(niyah_dalw_doc_count(h) == 4u);
        n = niyah_dalw_query(h, "search", 1, hits, 16u, &st);
        CHECK(st == NIYAH_DALW_OK && n == 3u);
        for (i = 1; i < n; i++) CHECK(hits[i - 1].score >= hits[i].score);
        CHECK(niyah_dalw_to_search_hits(hits, n, sh, 16u) == n);
        CHECK(n > 0u && sh[0].document_id == hits[0].document_id);
        niyah_dalw_close(h);
    }

    /* tampering must be caught when verify is on */
    {
        FILE *f = fopen(p1, "r+b");
        long sz;
        int ch;
        CHECK(f != NULL);
        if (f) {
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fseek(f, sz / 2, SEEK_SET);
            ch = fgetc(f);
            fseek(f, sz / 2, SEEK_SET);
            fputc(ch ^ 1, f);
            fclose(f);
            both[0] = p1;
            CHECK(niyah_dalw_open(both, 1, 1, &st) == NULL && st != NIYAH_DALW_OK);
        }
    }

    remove(p1);
    remove(p2);
    puts(fails ? "SOME TESTS FAILED" : "ADAPTER TESTS PASSED");
    return fails != 0;
}
