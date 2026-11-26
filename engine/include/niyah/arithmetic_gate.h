#ifndef NIYAH_ARITHMETIC_GATE_H
#define NIYAH_ARITHMETIC_GATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A deterministic, independent calculator. It does not claim model reasoning.
 * Valid output buffers must hold at least 64 bytes.
 * NOT_APPLICABLE leaves the output empty and lets the caller use its fallback.
 * Division by zero and representational overflow are explicit failures. */
typedef enum NiyahArithmeticStatus {
    NIYAH_ARITH_NOT_APPLICABLE = 0,
    NIYAH_ARITH_OK = 1,
    NIYAH_ARITH_DIV_BY_ZERO = 2,
    NIYAH_ARITH_OVERFLOW = 3
} NiyahArithmeticStatus;

NiyahArithmeticStatus niyah_arithmetic_gate_eval(
    const char *prompt,
    char *out_buf,
    size_t out_cap);

#ifdef __cplusplus
}
#endif
#endif /* NIYAH_ARITHMETIC_GATE_H */
