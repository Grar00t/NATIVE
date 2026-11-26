#include "niyah/arithmetic_gate.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct TestCase {
    const char *expr;
    NiyahArithmeticStatus expected;
    const char *text;
} TestCase;

static const TestCase cases[] = {
    {"2+2", NIYAH_ARITH_OK, "4"},
    {"8+7", NIYAH_ARITH_OK, "15"},
    {"100-37", NIYAH_ARITH_OK, "63"},
    {"0+0", NIYAH_ARITH_OK, "0"},
    {"1+1", NIYAH_ARITH_OK, "2"},
    {"15+27", NIYAH_ARITH_OK, "42"},
    {"100+200", NIYAH_ARITH_OK, "300"},
    {"999+1", NIYAH_ARITH_OK, "1000"},
    {"10-3", NIYAH_ARITH_OK, "7"},
    {"3-10", NIYAH_ARITH_OK, "-7"},
    {"-5+8", NIYAH_ARITH_OK, "3"},
    {"-5-8", NIYAH_ARITH_OK, "-13"},
    {"-10+10", NIYAH_ARITH_OK, "0"},
    {"1+2+3+4+5", NIYAH_ARITH_OK, "15"},
    {"50-20-10", NIYAH_ARITH_OK, "20"},
    {"10-20+30-40", NIYAH_ARITH_OK, "-20"},
    {"123456+654321", NIYAH_ARITH_OK, "777777"},
    {"1000000-1", NIYAH_ARITH_OK, "999999"},
    {"+7 + +8", NIYAH_ARITH_OK, "15"},
    {"10 - -5", NIYAH_ARITH_OK, "15"},
    {"-10 - -5", NIYAH_ARITH_OK, "-5"},
    {"42 + 0", NIYAH_ARITH_OK, "42"},
    {"0 - 42", NIYAH_ARITH_OK, "-42"},
    {"-9223372036854775808 + 0", NIYAH_ARITH_OK, "-9223372036854775808"},
    {"9223372036854775807 - 0", NIYAH_ARITH_OK, "9223372036854775807"},
    {"0009+1", NIYAH_ARITH_OK, "10"},
    {"2++2", NIYAH_ARITH_OK, "4"},
    {"2+-2", NIYAH_ARITH_OK, "0"},
    {"2-+2", NIYAH_ARITH_OK, "0"},
    {"-0+0", NIYAH_ARITH_OK, "0"},
    {"3*4", NIYAH_ARITH_OK, "12"},
    {"0*9999", NIYAH_ARITH_OK, "0"},
    {"-6*7", NIYAH_ARITH_OK, "-42"},
    {"-6*-7", NIYAH_ARITH_OK, "42"},
    {"2+3*4", NIYAH_ARITH_OK, "14"},
    {"2*3+4", NIYAH_ARITH_OK, "10"},
    {"20-3*5", NIYAH_ARITH_OK, "5"},
    {"2*3*4*5", NIYAH_ARITH_OK, "120"},
    {"10+20*3-5", NIYAH_ARITH_OK, "65"},
    {"100 - 4 * 25", NIYAH_ARITH_OK, "0"},
    {"11*11", NIYAH_ARITH_OK, "121"},
    {"25*4", NIYAH_ARITH_OK, "100"},
    {"125*8", NIYAH_ARITH_OK, "1000"},
    {"-1*1", NIYAH_ARITH_OK, "-1"},
    {"7*8+9", NIYAH_ARITH_OK, "65"},
    {"9+7*8", NIYAH_ARITH_OK, "65"},
    {"3*3-4*4", NIYAH_ARITH_OK, "-7"},
    {"50+50*0", NIYAH_ARITH_OK, "50"},
    {"1000*1000", NIYAH_ARITH_OK, "1000000"},
    {"13*17", NIYAH_ARITH_OK, "221"},
    {"10/4", NIYAH_ARITH_OK, "5/2"},
    {"8/2", NIYAH_ARITH_OK, "4"},
    {"9/3", NIYAH_ARITH_OK, "3"},
    {"1/2", NIYAH_ARITH_OK, "1/2"},
    {"2/4", NIYAH_ARITH_OK, "1/2"},
    {"6/4", NIYAH_ARITH_OK, "3/2"},
    {"-6/4", NIYAH_ARITH_OK, "-3/2"},
    {"6/-4", NIYAH_ARITH_OK, "-3/2"},
    {"-6/-4", NIYAH_ARITH_OK, "3/2"},
    {"0/5", NIYAH_ARITH_OK, "0"},
    {"0/-5", NIYAH_ARITH_OK, "0"},
    {"1/2 + 1/3", NIYAH_ARITH_OK, "5/6"},
    {"1/2 - 1/3", NIYAH_ARITH_OK, "1/6"},
    {"2/3 * 3/4", NIYAH_ARITH_OK, "1/2"},
    {"3/4 / 3/2", NIYAH_ARITH_OK, "1/8"},
    {"1 + 1/2", NIYAH_ARITH_OK, "3/2"},
    {"2 - 1/4", NIYAH_ARITH_OK, "7/4"},
    {"100/8", NIYAH_ARITH_OK, "25/2"},
    {"21/14", NIYAH_ARITH_OK, "3/2"},
    {"17/19", NIYAH_ARITH_OK, "17/19"},
    {"1/0", NIYAH_ARITH_DIV_BY_ZERO, "DIVISION_BY_ZERO"},
    {"0/0", NIYAH_ARITH_DIV_BY_ZERO, "DIVISION_BY_ZERO"},
    {"-5/0", NIYAH_ARITH_DIV_BY_ZERO, "DIVISION_BY_ZERO"},
    {"10+5/0", NIYAH_ARITH_DIV_BY_ZERO, "DIVISION_BY_ZERO"},
    {"1/2-1/0", NIYAH_ARITH_DIV_BY_ZERO, "DIVISION_BY_ZERO"},
    {"100/0*5", NIYAH_ARITH_DIV_BY_ZERO, "DIVISION_BY_ZERO"},
    {"9223372036854775807+1", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"-9223372036854775808-1", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"9223372036854775807*2", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"-9223372036854775808*-1", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"99999999999999999999+1", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"9223372036854775807/1+1", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"3037000500*3037000500", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"-3037000500*3037000500", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"-9223372036854775808/-1", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"9223372036854775807*9223372036854775807", NIYAH_ARITH_OVERFLOW, "OVERFLOW"},
    {"ما حاصل 2+2؟", NIYAH_ARITH_OK, "4"},
    {"ما ناتج 8+7؟", NIYAH_ARITH_OK, "15"},
    {"احسب 12 * 12 =", NIYAH_ARITH_OK, "144"},
    {"What is 10/4?", NIYAH_ARITH_OK, "5/2"},
    {"Calculate 1/0", NIYAH_ARITH_DIV_BY_ZERO, "DIVISION_BY_ZERO"},
    {"  100 - 37 ", NIYAH_ARITH_OK, "63"},
    {"", NIYAH_ARITH_NOT_APPLICABLE, ""},
    {"42", NIYAH_ARITH_NOT_APPLICABLE, ""},
    {"hello world", NIYAH_ARITH_NOT_APPLICABLE, ""},
    {"2+*2", NIYAH_ARITH_NOT_APPLICABLE, ""},
    {"2+", NIYAH_ARITH_NOT_APPLICABLE, ""},
    {"1 / (0)", NIYAH_ARITH_NOT_APPLICABLE, ""},
    {"2.5+3.5", NIYAH_ARITH_NOT_APPLICABLE, ""},
    {"2+2 and 3+3", NIYAH_ARITH_NOT_APPLICABLE, ""},
};

_Static_assert(sizeof(cases)/sizeof(cases[0]) == 100U, "exactly 100 fixtures");

int main(void)
{
    size_t i;
    char result[64];
    for (i = 0U; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        NiyahArithmeticStatus got;
        memset(result, 0xA5, sizeof(result));
        got = niyah_arithmetic_gate_eval(cases[i].expr, result, sizeof(result));
        if (got != cases[i].expected || strcmp(result, cases[i].text) != 0) {
            fprintf(stderr, "ARITHMETIC_FAIL case=%zu expr=%s status=%d expected=%d actual=%s expected_text=%s\n",
                    i + 1U, cases[i].expr, (int)got, (int)cases[i].expected,
                    result, cases[i].text);
            return 1;
        }
    }
    result[0] = 'x';
    if (niyah_arithmetic_gate_eval(NULL, result, sizeof(result)) != NIYAH_ARITH_NOT_APPLICABLE || result[0] != '\0') return 2;
    if (niyah_arithmetic_gate_eval("2+2", NULL, sizeof(result)) != NIYAH_ARITH_NOT_APPLICABLE) return 3;
    if (niyah_arithmetic_gate_eval("2+2", result, 1U) != NIYAH_ARITH_NOT_APPLICABLE || result[0] != '\0') return 4;
    if (niyah_arithmetic_gate_eval("2+2", result, 0U) != NIYAH_ARITH_NOT_APPLICABLE) return 5;
    puts("ARITHMETIC_GATE_REGRESSION_PASS 100/100");
    return 0;
}
