#include "dalw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static void make_set(uint32_t *a, size_t n, uint32_t step) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) { v += 1 + rnd() % step; a[i] = v; }
}

typedef size_t (*Isect)(const uint32_t *, size_t, const uint32_t *, size_t, uint32_t *);

static void cmp_kernel(const char *name, Isect f, const uint32_t *a, size_t na,
                       const uint32_t *b, size_t nb, const uint32_t *ref, size_t nref) {
    size_t lo = na < nb ? na : nb;
    uint32_t *o = malloc((lo + DALW_SLACK) * sizeof *o);
    size_t k = f(a, na, b, nb, o);
    if (k != nref || (k && memcmp(o, ref, k * sizeof *o) != 0)) {
        printf("FAIL kernel %s na=%zu nb=%zu got=%zu want=%zu\n", name, na, nb, k, nref);
        fails++;
    }
    free(o);
}

int main(void) {
    DalwSha256 s; uint8_t d[32]; char hex[65];
    dalw_sha256_init(&s); dalw_sha256_update(&s, "abc", 3); dalw_sha256_final(&s, d);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    static const size_t sizes[][2] = {{0,0},{0,5},{5,0},{1,1},{7,7},{8,8},{9,17},{100,100},
                                      {1000,40},{40,1000},{5000,5000},{37,60000}};
    for (size_t t = 0; t < sizeof sizes / sizeof sizes[0]; t++) {
        size_t na = sizes[t][0], nb = sizes[t][1];
        uint32_t *a = malloc((na + 1) * 4), *b = malloc((nb + 1) * 4);
        make_set(a, na, 6); make_set(b, nb, 6);
        size_t lo = na < nb ? na : nb;
        uint32_t *ref = malloc((lo + DALW_SLACK) * 4);
        size_t nref = dalw_isect_scalar(a, na, b, nb, ref);
        cmp_kernel("gallop", dalw_isect_gallop, a, na, b, nb, ref, nref);
        cmp_kernel("avx2", dalw_isect_avx2, a, na, b, nb, ref, nref);
        if (dalw_has_avx512()) cmp_kernel("avx512", dalw_isect_avx512, a, na, b, nb, ref, nref);
        cmp_kernel("dispatch", dalw_isect, a, na, b, nb, ref, nref);
        free(a); free(b); free(ref);
    }
    printf("avx2=%d avx512=%d\n", dalw_has_avx2(), dalw_has_avx512());

    static const uint32_t ns[] = {1, 2, 127, 128, 129, 1000};
    for (size_t t = 0; t < sizeof ns / sizeof ns[0]; t++) {
        uint32_t n = ns[t];
        uint32_t *docs = malloc(n * 4), *tf = malloc(n * 4);
        uint32_t *out = malloc((n + DALW_SLACK) * 4);
        uint8_t *buf = malloc(dalw_plist_bound(n) + 16);
        make_set(docs, n, 50);
        for (uint32_t i = 0; i < n; i++) tf[i] = 1 + rnd() % 40;
        size_t eb = dalw_plist_encode(docs, n, buf);
        CHECK(eb > 0 && dalw_plist_decode(buf, eb, n, out) == 0 && memcmp(docs, out, n * 4) == 0);
        if (n >= 128) CHECK(dalw_plist_decode(buf, eb / 2, n, out) < 0);
        size_t tb = dalw_tf_encode(tf, n, buf);
        CHECK(tb > 0 && dalw_tf_decode(buf, tb, n, out) == 0 && memcmp(tf, out, n * 4) == 0);
        free(docs); free(tf); free(out); free(buf);
    }

    uint64_t h[8];
    CHECK(dalw_parse_query("\xd8\xa7\xd9\x84\xd8\xb1\xd9\x8a\xd8\xa7\xd8\xb6 search", DALW_Q_STEM, h, 8) == 2);
    CHECK(dalw_parse_query("\xd8\xa7\xd9\x84\xd8\xb1\xd9\x8a\xd8\xa7\xd8\xb6 search", DALW_Q_EXACT, h, 8) == 2);
    CHECK(dalw_parse_query("", DALW_Q_STEM, h, 8) == 0);
    {
        char st[64]; size_t n = dalw_stem_utf8("\xd8\xa7\xd9\x84\xd9\x85\xd8\xaf\xd9\x8a\xd9\x86\xd8\xa9", st, sizeof st);
        CHECK(n > 0 && n < sizeof st);
        printf("stem(المدينة) -> %s\n", st);
    }

    const char *path = "./dalw_test_v2.dalw"; remove(path);
    DalwBuilder *b = dalw_builder_new();
    const char *t0 = "\xd8\xb9\xd8\xa7\xd8\xb5\xd9\x85\xd8\xa9 \xd8\xa7\xd9\x84\xd9\x85\xd9\x85\xd9\x84\xd9\x83\xd8\xa9 \xd8\xa7\xd9\x84\xd8\xb1\xd9\x8a\xd8\xa7\xd8\xb6";
    const char *t1 = "local search index in Riyadh", *t2 = "weather today";
    CHECK(b != NULL);
    CHECK(dalw_builder_add(b, "d0", t0, strlen(t0)) == 0);
    CHECK(dalw_builder_add(b, "d1", t1, strlen(t1)) == 0);
    CHECK(dalw_builder_add(b, "d2", t2, strlen(t2)) == 0);
    CHECK(dalw_builder_write(b, path) == 0);
    CHECK(dalw_builder_write(b, path) != 0);
    dalw_builder_free(b);
    DalwShard sh;
    CHECK(dalw_open(&sh, path, 1) == 0);
    dalw_close(&sh);
    FILE *f = fopen(path, "r+b"); fseek(f, 0, SEEK_END); long sz = ftell(f);
    fseek(f, sz / 2, SEEK_SET); int ch = fgetc(f); fseek(f, sz / 2, SEEK_SET); fputc(ch ^ 1, f); fclose(f);
    CHECK(dalw_open(&sh, path, 1) < 0);
    remove(path);

    puts(fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fails != 0;
}
