#define _POSIX_C_SOURCE 200809L
#include "dalw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef size_t (*K)(const uint32_t *, size_t, const uint32_t *, size_t, uint32_t *);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void fill(uint32_t *a, size_t n, unsigned seed, unsigned span) {
    uint32_t x = seed * 2654435761u + 1, v = 0;
    for (size_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; v += 1 + x % span; a[i] = v; }
}
int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
    struct { const char *name; K fn; } ks[] = {{"scalar", dalw_isect_scalar}, {"avx2", dalw_isect_avx2},
                                              {"avx512", dalw_isect_avx512}, {"gallop", dalw_isect_gallop}};
    struct { size_t na, nb; unsigned span, spanb; const char *what; } cases[] = {
        {1000000, 1000000, 3, 3, "1M x 1M dense (~50% overlap)"},
        {1000000, 1000000, 40, 40, "1M x 1M sparse"},
        {20000, 1000000, 100, 2, "20k x 1M (1:50 skew, same value range)"},
        {5000, 1000000, 400, 2, "5k x 1M (1:200)"},
        {1000, 1000000, 2000, 2, "1k x 1M (1:1000)"},
        {2000, 4000, 4, 2, "2k x 4k (L1-resident, same range)"}};
    double best[3] = {1e18, 1e18, 1e18}; double tot[3] = {0, 0, 0};
    printf("cpu avx2=%d avx512=%d\n", dalw_has_avx2(), dalw_has_avx512());
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        size_t na = cases[c].na, nb = cases[c].nb;
        uint32_t *a = malloc(na * 4), *b = malloc(nb * 4), *o = malloc((na < nb ? na : nb) * 4 + 64);
        fill(a, na, 11, cases[c].span); fill(b, nb, 22, cases[c].spanb);
        size_t ref = dalw_isect_scalar(a, na, b, nb, o);
        int reps = (int)(40e6 / (double)(na + nb)); if (reps < 5) reps = 5; if (reps > 20000) reps = 20000;
        printf("\n%s  (matches=%zu, reps=%d)\n", cases[c].what, ref, reps);
        for (size_t k = 0; k < 4; k++) {
            if (only && strcmp(only, ks[k].name)) continue;
            if (!strcmp(ks[k].name, "avx512") && !dalw_has_avx512()) { puts("  avx512  n/a"); continue; }
            size_t got = ks[k].fn(a, na, b, nb, o);          /* warm-up + check */
            if (got != ref) { printf("  %-7s MISMATCH %zu != %zu\n", ks[k].name, got, ref); return 1; }
            double bt = 1e18;
            for (int rep = 0; rep < 5; rep++) {              /* best of 5 batches */
                double t0 = now();
                for (int r = 0; r < reps; r++) ks[k].fn(a, na, b, nb, o);
                double dt = (now() - t0) / reps; if (dt < bt) bt = dt;
            }
            double ns = bt * 1e9 / (double)(na + nb);
            printf("  %-7s %8.3f ms   %6.3f ns/elem\n", ks[k].name, bt * 1e3, ns);
            if (k < 3 && c < 2) { tot[k] += ns; if (ns < best[k]) best[k] = ns; }
        }
        free(a); free(b); free(o);
    }
    if (!only) {
        printf("\nbalanced-case sum (ns/elem): scalar=%.3f avx2=%.3f avx512=%.3f\n", tot[0], tot[1], tot[2]);
        if (dalw_has_avx512() && tot[2] < tot[1] * 0.95) puts("=> avx512 is faster here: use DALW_ISECT=avx512");
        else puts("=> keep the default (avx2); avx512 is not clearly faster on this CPU");
    }
    return 0;
}
