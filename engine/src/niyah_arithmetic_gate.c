#include "niyah/arithmetic_gate.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Intentionally C11 and library-independent: no float rounding or eval(). */
typedef struct Rational {
    int64_t num;
    int64_t den; /* Always positive after normalization. */
} Rational;

typedef struct Parser {
    const char *p;
    int has_operator;
    NiyahArithmeticStatus error;
} Parser;

static uint64_t magnitude_i64(int64_t value)
{
    uint64_t v = (uint64_t)value;
    return value < 0 ? UINT64_C(0) - v : v;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b)
{
    while (b != 0U) {
        uint64_t rest = a % b;
        a = b;
        b = rest;
    }
    return a;
}

static int64_t div_signed_magnitude(int64_t n, uint64_t d)
{
    /* Only INT64_MIN has magnitude 2^63; dividing that by 2^63 is -1. */
    if (d > (uint64_t)INT64_MAX) return n == 0 ? 0 : INT64_C(-1);
    return n / (int64_t)d;
}

static int checked_add(int64_t a, int64_t b, int64_t *out)
{
    if (b > 0 && a > INT64_MAX - b) return 0;
    if (b < 0 && a < INT64_MIN - b) return 0;
    *out = a + b;
    return 1;
}

static int checked_sub(int64_t a, int64_t b, int64_t *out)
{
    if (b < 0 && a > INT64_MAX + b) return 0;
    if (b > 0 && a < INT64_MIN + b) return 0;
    *out = a - b;
    return 1;
}

static int checked_mul(int64_t a, int64_t b, int64_t *out)
{
    if (a == 0 || b == 0) {
        *out = 0;
        return 1;
    }
    if (a == -1 && b == INT64_MIN) return 0;
    if (b == -1 && a == INT64_MIN) return 0;
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) return 0;
        } else if (b < INT64_MIN / a) {
            return 0;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) return 0;
        } else if (a < INT64_MAX / b) {
            return 0;
        }
    }
    *out = a * b;
    return 1;
}

static NiyahArithmeticStatus rational_make(
    int64_t numerator, int64_t denominator, Rational *out)
{
    uint64_t n;
    uint64_t d;
    uint64_t factor;
    int negative;

    if (denominator == 0) return NIYAH_ARITH_DIV_BY_ZERO;
    if (numerator == 0) {
        out->num = 0;
        out->den = 1;
        return NIYAH_ARITH_OK;
    }

    n = magnitude_i64(numerator);
    d = magnitude_i64(denominator);
    factor = gcd_u64(n, d);
    n /= factor;
    d /= factor;
    negative = (numerator < 0) != (denominator < 0);

    if (d > (uint64_t)INT64_MAX) return NIYAH_ARITH_OVERFLOW;
    if ((!negative && n > (uint64_t)INT64_MAX) ||
        (negative && n > (uint64_t)INT64_MAX + UINT64_C(1))) {
        return NIYAH_ARITH_OVERFLOW;
    }
    out->num = negative
        ? (n == (uint64_t)INT64_MAX + UINT64_C(1)
            ? INT64_MIN : -(int64_t)n)
        : (int64_t)n;
    out->den = (int64_t)d;
    return NIYAH_ARITH_OK;
}

static NiyahArithmeticStatus rational_addsub(
    Rational a, Rational b, int subtract, Rational *out)
{
    uint64_t common = gcd_u64((uint64_t)a.den, (uint64_t)b.den);
    int64_t left_factor = b.den / (int64_t)common;
    int64_t right_factor = a.den / (int64_t)common;
    int64_t left, right, n, den;
    if (!checked_mul(a.num, left_factor, &left) ||
        !checked_mul(b.num, right_factor, &right) ||
        !(subtract ? checked_sub(left, right, &n)
                    : checked_add(left, right, &n)) ||
        !checked_mul(a.den, left_factor, &den)) {
        return NIYAH_ARITH_OVERFLOW;
    }
    return rational_make(n, den, out);
}

static NiyahArithmeticStatus rational_mul(
    Rational a, Rational b, Rational *out)
{
    uint64_t g1 = gcd_u64(magnitude_i64(a.num), (uint64_t)b.den);
    uint64_t g2 = gcd_u64(magnitude_i64(b.num), (uint64_t)a.den);
    int64_t n, den;
    if (!checked_mul(div_signed_magnitude(a.num, g1),
                     div_signed_magnitude(b.num, g2), &n) ||
        !checked_mul(a.den / (int64_t)g2,
                     b.den / (int64_t)g1, &den)) {
        return NIYAH_ARITH_OVERFLOW;
    }
    return rational_make(n, den, out);
}

static NiyahArithmeticStatus rational_div(
    Rational a, Rational b, Rational *out)
{
    uint64_t g1;
    uint64_t g2;
    int64_t n, den;
    if (b.num == 0) return NIYAH_ARITH_DIV_BY_ZERO;
    g1 = gcd_u64(magnitude_i64(a.num), magnitude_i64(b.num));
    g2 = gcd_u64((uint64_t)a.den, (uint64_t)b.den);
    if (!checked_mul(div_signed_magnitude(a.num, g1),
                     b.den / (int64_t)g2, &n) ||
        !checked_mul(a.den / (int64_t)g2,
                     div_signed_magnitude(b.num, g1), &den)) {
        return NIYAH_ARITH_OVERFLOW;
    }
    return rational_make(n, den, out);
}

static void skip_space(Parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' ||
           *ps->p == '\r' || *ps->p == '\n') ++ps->p;
}

static int parse_number(Parser *ps, Rational *out)
{
    int negative = 0;
    uint64_t acc = UINT64_C(0);
    uint64_t limit;
    int digits = 0;
    skip_space(ps);
    if (*ps->p == '+' || *ps->p == '-') {
        negative = *ps->p == '-';
        ++ps->p;
        skip_space(ps);
    }
    limit = (uint64_t)INT64_MAX + (negative ? UINT64_C(1) : UINT64_C(0));
    while (*ps->p >= '0' && *ps->p <= '9') {
        uint64_t digit = (uint64_t)(*ps->p - '0');
        if (acc > (limit - digit) / UINT64_C(10)) {
            ps->error = NIYAH_ARITH_OVERFLOW;
            return 0;
        }
        acc = acc * UINT64_C(10) + digit;
        ++digits;
        ++ps->p;
    }
    if (!digits) return 0;
    out->num = negative
        ? (acc == (uint64_t)INT64_MAX + UINT64_C(1)
            ? INT64_MIN : -(int64_t)acc)
        : (int64_t)acc;
    out->den = 1;
    return 1;
}

static int parse_term(Parser *ps, Rational *result)
{
    Rational lhs;
    if (!parse_number(ps, &lhs)) return 0;
    for (;;) {
        char op;
        Rational rhs;
        NiyahArithmeticStatus status;
        skip_space(ps);
        op = *ps->p;
        if (op != '*' && op != '/') break;
        ++ps->p;
        ps->has_operator = 1;
        if (!parse_number(ps, &rhs)) return 0;
        status = op == '*' ? rational_mul(lhs, rhs, &lhs)
                           : rational_div(lhs, rhs, &lhs);
        if (status != NIYAH_ARITH_OK) {
            ps->error = status;
            return 0;
        }
    }
    *result = lhs;
    return 1;
}

static int parse_expr(Parser *ps, Rational *result)
{
    Rational lhs;
    if (!parse_term(ps, &lhs)) return 0;
    for (;;) {
        char op;
        Rational rhs;
        NiyahArithmeticStatus status;
        skip_space(ps);
        op = *ps->p;
        if (op != '+' && op != '-') break;
        ++ps->p;
        ps->has_operator = 1;
        if (!parse_term(ps, &rhs)) return 0;
        status = rational_addsub(lhs, rhs, op == '-', &lhs);
        if (status != NIYAH_ARITH_OK) {
            ps->error = status;
            return 0;
        }
    }
    *result = lhs;
    return 1;
}

static const char *clean_expression(
    const char *prompt, char *buffer, size_t capacity)
{
    static const char *const prefixes[] = {
        "\xd9\x85\xd8\xa7 \xd8\xad\xd8\xa7\xd8\xb5\xd9\x84 ",
        "\xd9\x85\xd8\xa7 \xd9\x86\xd8\xa7\xd8\xaa\xd8\xac ",
        "\xd8\xa7\xd8\xad\xd8\xb3\xd8\xa8 ",
        "what is ", "What is ", "calculate ", "Calculate ",
        "eval ", "Eval "
    };
    const char *p = prompt;
    size_t i, n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    for (i = 0U; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        n = strlen(prefixes[i]);
        if (strncmp(p, prefixes[i], n) == 0) {
            p += n;
            break;
        }
    }
    n = strlen(p);
    if (n >= capacity) return NULL;
    memcpy(buffer, p, n + 1U);

    while (n > 0U) {
        if (buffer[n - 1U] == ' ' || buffer[n - 1U] == '\r' ||
            buffer[n - 1U] == '\t' || buffer[n - 1U] == '\n' ||
            buffer[n - 1U] == '?' || buffer[n - 1U] == '=') {
            buffer[--n] = '\0';
        } else if (n >= 2U &&
                   (unsigned char)buffer[n - 2U] == 0xD8U &&
                   (unsigned char)buffer[n - 1U] == 0x9FU) {
            n -= 2U;
            buffer[n] = '\0';
        } else {
            break;
        }
    }
    return buffer;
}

NiyahArithmeticStatus niyah_arithmetic_gate_eval(
    const char *prompt, char *output, size_t capacity)
{
    char expression[2048];
    const char *clean;
    const unsigned char *scan;
    Parser parser;
    Rational answer;
    int written;

    if (output == NULL || capacity == 0U) return NIYAH_ARITH_NOT_APPLICABLE;
    output[0] = '\0';
    if (prompt == NULL || capacity < 64U) return NIYAH_ARITH_NOT_APPLICABLE;

    clean = clean_expression(prompt, expression, sizeof(expression));
    if (clean == NULL || clean[0] == '\0') return NIYAH_ARITH_NOT_APPLICABLE;

    /* Prevent prefix-only parsing and false positives in ordinary prose. */
    for (scan = (const unsigned char *)clean; *scan != 0U; ++scan) {
        unsigned char c = *scan;
        if (!((c >= '0' && c <= '9') || c == '+' || c == '-' ||
              c == '*' || c == '/' || c == ' ' || c == '\t' ||
              c == '\r' || c == '\n')) {
            return NIYAH_ARITH_NOT_APPLICABLE;
        }
    }

    parser.p = clean;
    parser.has_operator = 0;
    parser.error = NIYAH_ARITH_NOT_APPLICABLE;
    if (!parse_expr(&parser, &answer)) {
        if (parser.error == NIYAH_ARITH_DIV_BY_ZERO) {
            (void)snprintf(output, capacity, "DIVISION_BY_ZERO");
        } else if (parser.error == NIYAH_ARITH_OVERFLOW) {
            (void)snprintf(output, capacity, "OVERFLOW");
        }
        return parser.error;
    }
    skip_space(&parser);
    if (*parser.p != '\0' || !parser.has_operator) {
        return NIYAH_ARITH_NOT_APPLICABLE;
    }

    if (answer.den == 1) {
        written = snprintf(output, capacity, "%lld", (long long)answer.num);
    } else {
        written = snprintf(output, capacity, "%lld/%lld",
                           (long long)answer.num, (long long)answer.den);
    }
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        return NIYAH_ARITH_NOT_APPLICABLE;
    }
    return NIYAH_ARITH_OK;
}
