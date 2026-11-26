#include "dalw.h"
#include <stdlib.h>
#include <string.h>
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#define DALW_X86 1
#include <immintrin.h>
#endif

size_t dalw_isect_scalar(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *o) {
    size_t i = 0, j = 0, k = 0;
    while (i < na && j < nb) {            /* branchless merge; writes o[k] speculatively */
        uint32_t x = a[i], y = b[j];
        o[k] = x; k += (x == y); i += (x <= y); j += (y <= x);
    }
    return k;
}

size_t dalw_isect_gallop(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *o) {
    if (na > nb) { const uint32_t *t = a; a = b; b = t; size_t n = na; na = nb; nb = n; }
    size_t j = 0, k = 0;
    for (size_t i = 0; i < na && j < nb; i++) {
        uint32_t x = a[i]; size_t step = 1, lo = j;
        while (lo + step < nb && b[lo + step] < x) { lo += step; step <<= 1; }
        size_t l = lo, r = (lo + step < nb) ? lo + step : nb;
        while (l < r) { size_t m = l + (r - l) / 2; if (b[m] < x) l = m + 1; else r = m; }
        j = l;
        if (j < nb && b[j] == x) { o[k++] = x; j++; }
    }
    return k;
}

#ifdef DALW_X86
static uint32_t g_perm[256][8] __attribute__((aligned(32)));
__attribute__((constructor)) static void perm_init(void) {
    for (int m = 0; m < 256; m++) {
        int k = 0;
        for (int b = 0; b < 8; b++) if (m >> b & 1) g_perm[m][k++] = (uint32_t)b;
        while (k < 8) g_perm[m][k++] = 0;
    }
}
int dalw_has_avx2(void) {
    static int v = -1;
    if (v < 0) { __builtin_cpu_init(); v = __builtin_cpu_supports("avx2") != 0; }
    return v;
}
int dalw_has_avx512(void) {
    static int v = -1;
    if (v < 0) { __builtin_cpu_init(); v = __builtin_cpu_supports("avx512f") != 0; }
    return v;
}
__attribute__((target("avx2"))) static size_t avx2_impl(const uint32_t *a, size_t na,
                                                        const uint32_t *b, size_t nb, uint32_t *o) {
    size_t i = 0, j = 0, k = 0;
    const __m256i rot = _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 0);
    while (i + 8 <= na && j + 8 <= nb) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(a + i));
        __m256i vb = _mm256_loadu_si256((const __m256i *)(b + j));
        __m256i m = _mm256_cmpeq_epi32(va, vb);
        for (int r = 1; r < 8; r++) {
            vb = _mm256_permutevar8x32_epi32(vb, rot);
            m = _mm256_or_si256(m, _mm256_cmpeq_epi32(va, vb));
        }
        unsigned mask = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(m));
        __m256i p = _mm256_load_si256((const __m256i *)g_perm[mask]);
        _mm256_storeu_si256((__m256i *)(o + k), _mm256_permutevar8x32_epi32(va, p));
        k += (size_t)__builtin_popcount(mask);
        uint32_t am = a[i + 7], bm = b[j + 7];
        i += (am <= bm) * 8u; j += (bm <= am) * 8u;
    }
    return k + dalw_isect_scalar(a + i, na - i, b + j, nb - j, o + k);
}
size_t dalw_isect_avx2(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *o) {
    return dalw_has_avx2() ? avx2_impl(a, na, b, nb, o) : dalw_isect_scalar(a, na, b, nb, o);
}

__attribute__((target("avx512f"))) static size_t avx512_impl(const uint32_t *a, size_t na,
                                                             const uint32_t *b, size_t nb, uint32_t *o) {
    size_t i = 0, j = 0, k = 0;
    const __m512i rot = _mm512_setr_epi32(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0);
    while (i + 16 <= na && j + 16 <= nb) {
        __m512i va = _mm512_loadu_si512((const void *)(a + i));
        __m512i vb = _mm512_loadu_si512((const void *)(b + j));
        __mmask16 m = _mm512_cmpeq_epi32_mask(va, vb);
        for (int r = 1; r < 16; r++) {
            vb = _mm512_permutexvar_epi32(rot, vb);
            m |= _mm512_cmpeq_epi32_mask(va, vb);
        }
        _mm512_mask_compressstoreu_epi32(o + k, m, va);
        k += (size_t)__builtin_popcount((unsigned)m);
        uint32_t am = a[i + 15], bm = b[j + 15];
        i += (am <= bm) * 16u; j += (bm <= am) * 16u;
    }
    return k + dalw_isect_scalar(a + i, na - i, b + j, nb - j, o + k);
}
size_t dalw_isect_avx512(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *o) {
    return dalw_has_avx512() ? avx512_impl(a, na, b, nb, o) : dalw_isect_avx2(a, na, b, nb, o);
}
#else
int dalw_has_avx2(void) { return 0; }
int dalw_has_avx512(void) { return 0; }
size_t dalw_isect_avx2(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *o) {
    return dalw_isect_scalar(a, na, b, nb, o);
}
size_t dalw_isect_avx512(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *o) {
    return dalw_isect_scalar(a, na, b, nb, o);
}
#endif

/* 0 = auto (avx2), 1 scalar, 2 avx2, 3 avx512 */
static int g_mode = -1;
static void mode_init(void) {
    const char *e = getenv("DALW_ISECT");
    g_mode = 0;
    if (e) {
        if (!strcmp(e, "scalar")) g_mode = 1;
        else if (!strcmp(e, "avx2")) g_mode = 2;
        else if (!strcmp(e, "avx512")) g_mode = 3;
    }
}

size_t dalw_isect(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *o) {
    size_t lo = na < nb ? na : nb, hi = na < nb ? nb : na;
    if (lo == 0) return 0;
    if (g_mode < 0) mode_init();
    if (hi / lo >= 100) return dalw_isect_gallop(a, na, b, nb, o);   /* measured crossover ~1:100 */
    if (lo < 8 || g_mode == 1) return dalw_isect_scalar(a, na, b, nb, o);
    if (g_mode == 3) return dalw_isect_avx512(a, na, b, nb, o);
    return dalw_isect_avx2(a, na, b, nb, o);
}
