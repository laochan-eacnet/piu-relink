/* Check the x87 memory/return ABI and that UCRT-backed calls preserve its control
 * word. Raw loads/stores avoid Prism's narrowing FLD ST(0) register copies. */
#define _GNU_SOURCE 1
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <locale.h>
#include <stdlib.h>

typedef struct { unsigned char bytes[12]; } guest_extended;
_Static_assert(sizeof(guest_extended)==12,"glibc x87 storage");
static int failures;
static unsigned control_word(void)
{
    unsigned short word;
    __asm__ volatile("fnstcw %0" : "=m"(word));
    return word;
}
static void probe(uint64_t significand, uint16_t exponent)
{
    guest_extended in={{0}},out={{0}};
    memcpy(in.bytes,&significand,8);memcpy(in.bytes+8,&exponent,2);
    __asm__ volatile("fldt %1; fstpt %0" : "=m"(out) : "m"(in) : "st");
    uint64_t observed;uint16_t observed_exp;
    memcpy(&observed,out.bytes,8);memcpy(&observed_exp,out.bytes+8,2);
    printf("x87 %08x%08x/%04x -> %08x%08x/%04x\n",(unsigned)(significand>>32),(unsigned)significand,exponent,(unsigned)(observed>>32),(unsigned)observed,observed_exp);
    if (observed != significand || observed_exp != exponent)
        ++failures;
    long double value;memcpy(&value,&in,12);int e=0;
    long double (*volatile fn)(long double,int *)=frexpl;
    long double fraction=fn(value,&e);
    memcpy(&observed,&fraction,8);memcpy(&observed_exp,(char *)&fraction+8,2);
    printf("frexpl %08x%08x/%04x e=%d\n",(unsigned)(observed>>32),(unsigned)observed,observed_exp,e);
    uint64_t expected_sig = exponent ? significand : UINT64_C(0x8000000000000000);
    unsigned expected_exp = (exponent & 0x8000) | 0x3ffe;
    int expected_e = exponent ? (int)(exponent & 0x7fff) - 16382 : -16444;
    if (observed != expected_sig || observed_exp != expected_exp || e != expected_e)
        ++failures;
}
static void stage(const char *name)
{
    printf("stage %s cw=%04x\n", name, control_word());
    probe(UINT64_C(0x8000000000000001),0x3fff);
    probe(UINT64_C(0x8000000000000001),0x4f9f);
    probe(UINT64_C(0x8000000000000001),0xb05f);
    probe(UINT64_C(1),0);
}
int main(void)
{
    stage("entry");
    locale_t loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    if (!loc)
        return 1;
    char *end;
    volatile long double parsed = strtold_l("1e9999", &end, loc);
    (void)parsed;
    stage("strtold-overflow");
    parsed = strtold_l("1e-9999", &end, loc);
    stage("strtold-underflow");
    static double (*const unary[])(double) = {
        sin, cos, tan, asin, acos, atan, sinh, cosh, tanh, exp, exp2,
        log, log2, log10, sqrt, cbrt, floor, ceil, round, trunc, rint,
        nearbyint, fabs, expm1, log1p, erf
    };
    static const char *const names[] = {
        "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
        "exp", "exp2", "log", "log2", "log10", "sqrt", "cbrt", "floor",
        "ceil", "round", "trunc", "rint", "nearbyint", "fabs", "expm1",
        "log1p", "erf"
    };
    for (unsigned i = 0; i < sizeof unary / sizeof unary[0]; ++i) {
        double (*volatile fn)(double) = unary[i];
        unsigned before = control_word();
        volatile double result = fn(0.75);
        (void)result;
        unsigned after = control_word();
        printf("control %s %04x -> %04x\n", names[i], before, after);
        if (after != before)
            ++failures;
    }
    stage("double-math");
    printf("format-double=%.17g\n", 0.75);
    stage("double-format");
    freelocale(loc);
    printf("x87 failures=%d\n", failures);
    return failures != 0;
}
