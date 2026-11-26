#include "niyah/niyah.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Bound 64*128*128 <= 2^20. Every intermediate is exactly representable in
   FP32, so the engine's matvec also provides an independent exact oracle. */
int main(int argc, char **argv)
{
    int8_t a[16][64], b[64][16];
    int32_t result[16][16];
    float matrix[16 * 64], column[64], out[16];
    uint32_t state = 12043;
    if (argc != 2) return 2;

    for (size_t i = 0; i < 2048; ++i) {
        state = state * 1664525u + 1013904223u;
        int8_t v = (int8_t)((int)(state >> 24) - 128);
        if (i < 1024) {
            a[i/64][i%64] = v;
            matrix[i] = v;
        } else {
            size_t j = i - 1024;
            b[j/16][j%16] = v;
        }
    }
    if (strcmp(argv[1], "input") == 0) {
        if (fwrite(a, sizeof a, 1, stdout) != 1 || fwrite(b, sizeof b, 1, stdout) != 1) return 1;
    } else if (strcmp(argv[1], "expected") == 0) {
        for (int j = 0; j < 16; ++j) {
            for (int k = 0; k < 64; ++k) column[k] = b[k][j];
            niyah_matvec(out, matrix, column, 16, 64);
            for (int i = 0; i < 16; ++i) result[i][j] = (int32_t)out[i];
        }
        if (fwrite(result, sizeof result, 1, stdout) != 1) return 1;
    } else return 2;
    return fflush(stdout) == 0 ? 0 : 1;
}
