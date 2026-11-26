/* The scalar kernel uses integer registers and the x86-64 SysV ABI. */
#include <stdint.h>

extern void nx_i8_scalar(const int8_t *a, const int8_t *b, int32_t *out);

int native_tensor_check(void)
{
    int8_t a[16][64];
    int8_t b[64][16];
    int32_t out[16][16];
    for (int row = 0; row < 16; ++row)
        for (int k = 0; k < 64; ++k)
            a[row][k] = (int8_t)(row - k);

    for (int k = 0; k < 64; ++k)
        for (int col = 0; col < 16; ++col)
            b[k][col] = (int8_t)(k == col ? -2 : (k == col + 16 ? 3 : 0));

    nx_i8_scalar(&a[0][0], &b[0][0], &out[0][0]);
    for (int row = 0; row < 16; ++row)
        for (int col = 0; col < 16; ++col)
            if (out[row][col] != row - col - 48)
                return 0;
    return 1;
}
