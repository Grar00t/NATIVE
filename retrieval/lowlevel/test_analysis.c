#include "dalw_analysis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DOCS = 385, TERMS = 24 };
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "analysis test line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static unsigned char tf[DOCS][TERMS], pairs[DOCS][DOCS];
static uint32_t df[TERMS];
static uint64_t hashes[TERMS], fingerprints[DOCS];
static char words[TERMS][24];

static unsigned weight(uint32_t n)
{
    unsigned w = 0;
    do { ++w; n /= 2; } while (n);
    return w;
}

static unsigned distance(uint64_t a, uint64_t b)
{
    unsigned differ = 0;
    for (unsigned bit = 0; bit < 64; ++bit) differ += (unsigned)((a >> bit ^ b >> bit) & 1);
    return differ;
}

static int record_pair(uint32_t a, uint32_t b, unsigned dist, void *ctx)
{
    unsigned radius = *(const unsigned *)ctx;
    if (a >= b || b >= DOCS || pairs[a][b] || dist > radius ||
        dist != distance(fingerprints[a], fingerprints[b])) return 1;
    pairs[a][b] = 1;
    return 0;
}

int main(void)
{
    /* Two frequent terms deliberately share a rank-zero row. */
    for (unsigned t = 0; t < TERMS; ++t) {
        unsigned suffix = t;
        do {
            snprintf(words[t], sizeof words[t], "term%u", suffix);
            hashes[t] = dalw_term_hash(words[t]);
            suffix += TERMS;
        } while (t == 1 && (hashes[0] & 1023) != (hashes[t] & 1023));
    }
    DalwBuilder *builder = dalw_builder_new();
    REQUIRE(builder);
    uint32_t random = 12151;
    for (unsigned d = 0; d < DOCS; ++d) {
        char line[1024] = "";
        for (unsigned t = 0; t < TERMS; ++t) {
            random = random * 1664525u + 1013904223u;
            if (t < 2) tf[d][t] = (unsigned char)((d & 1) == t);
            else if (t == 2) tf[d][t] = (d % 64 == 0 || d % 64 == 63);
            else if (t == 3) tf[d][t] = (d == 128 || d == 255 || d == DOCS-1);
            else if ((random >> 24) < (65 - t * 2)) tf[d][t] = (unsigned char)(1 + ((random >> 20) & 1));
            if (d == 5 || d == 90) tf[d][t] = tf[4][t];
            if (d == 6 || d == 7) tf[d][t] = 0;
            df[t] += tf[d][t] != 0;
            for (unsigned j = 0; j < tf[d][t]; ++j) { strcat(line, words[t]); strcat(line, " "); }
        }
        REQUIRE(dalw_builder_add(builder, "", line, strlen(line)) == 0);
    }
    REQUIRE(dalw_builder_write(builder, "analysis.dalw") == 0);
    dalw_builder_free(builder);
    DalwShard shard;
    REQUIRE(dalw_open(&shard, "analysis.dalw", 1) == 0);
    DalwRows *rows = dalw_rows_build(&shard);
    REQUIRE(rows && dalw_rows_bytes(rows));
    size_t comparisons = 0, rejected = 0;
    for (unsigned a = 0; a < TERMS; ++a) {
        for (unsigned b = 0; b <= TERMS; ++b) {
            uint64_t q[] = {hashes[a], b < TERMS ? hashes[b] : 0};
            size_t n = b < TERMS ? 2 : 1;
            uint32_t ids[DOCS];
            size_t matches, candidates, expected = 0;
            REQUIRE(!dalw_rows_match(rows, q, n, ids, &matches, &candidates));
            for (uint32_t d = 0; d < DOCS; ++d) {
                if (tf[d][a] && (n == 1 || tf[d][b])) {
                    REQUIRE(expected < matches && ids[expected] == d);
                    ++expected;
                }
            }
            REQUIRE(matches == expected && candidates >= matches && candidates <= DOCS);
            rejected += candidates - matches;
            ++comparisons;
        }
    }
    REQUIRE(rejected);
    dalw_rows_free(rows);
    for (unsigned d = 0; d < DOCS; ++d) {
        for (unsigned bit = 0; bit < 64; ++bit) {
            int vote = 0;
            for (unsigned t = 0; t < TERMS; ++t) {
                if (!tf[d][t]) continue;
                int w = (int)(weight(DOCS / df[t]) * weight(tf[d][t]));
                vote += (hashes[t] >> bit) & 1 ? w : -w;
            }
            if (vote > 0) fingerprints[d] |= UINT64_C(1) << bit;
        }
    }
    DalwFingerprint *prints;
    size_t count;
    REQUIRE(!dalw_fingerprints(&shard, &prints, &count) && count == DOCS - 2);
    for (size_t i = 0; i < count; ++i) {
        size_t d = i < 6 ? i : i + 2;
        REQUIRE(prints[i].id == d && prints[i].hash == fingerprints[d]);
    }
    free(prints);
    size_t at_radius[4] = {0};
    for (unsigned radius = 0; radius <= 3; ++radius) {
        memset(pairs, 0, sizeof pairs);
        REQUIRE(!dalw_simhash_pairs(&shard, radius, record_pair, &radius));
        for (unsigned a = 0; a < DOCS; ++a) {
            for (unsigned b = a + 1; b < DOCS; ++b) {
                int expected = a != 6 && a != 7 && b != 6 && b != 7 &&
                               distance(fingerprints[a], fingerprints[b]) <= radius;
                REQUIRE(pairs[a][b] == expected);
                at_radius[radius] += (unsigned)expected;
            }
        }
    }
    REQUIRE(at_radius[0] >= 3 && at_radius[3] > at_radius[0]);
    printf("analysis: %zu queries, %zu excess candidates removed, pairs per radius %zu/%zu/%zu/%zu\n",
           comparisons, rejected, at_radius[0], at_radius[1], at_radius[2], at_radius[3]);
    dalw_close(&shard);
    return 0;
}
