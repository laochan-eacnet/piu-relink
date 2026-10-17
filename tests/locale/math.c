/* QEMU 10.2.1's observed i386 glibc remquo(7,2) result has quotient bits zero
 * instead of four. Keep the mathematically required quotient assertion: the
 * reference-emulator quirk must not become an HLE implementation special case. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep these as symbol calls: compiler builtins must not hide the guest ABI. */
static long double (*volatile call_frexpl)(long double, int *) = frexpl;
static long double (*volatile call_ldexpl)(long double, int) = ldexpl;
static long double (*volatile call_modfl)(long double, long double *) = modfl;
static long double (*volatile call_fabsl)(long double) = fabsl;
static long double (*volatile call_sqrtl)(long double) = sqrtl;
extern int __fpclassify(double);
extern int __fpclassifyf(float);
extern int __fpclassifyl(long double);
extern int __finite(double);
extern int __finitef(float);
extern int __isnan(double);
extern int __isnanf(float);
extern int __isinf(double);
extern int __isinff(float);
extern int64_t __divdi3(int64_t, int64_t);
extern int64_t __moddi3(int64_t, int64_t);
extern uint64_t __udivdi3(uint64_t, uint64_t);
extern uint64_t __umoddi3(uint64_t, uint64_t);

static int failures;
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "math:%d: %s\n", __LINE__, #test); ++failures; } } while (0)
/* Checks that need genuine 80-bit x87 arithmetic. Windows-on-ARM's x86 emulator (Prism) rounds x87
 * results to double precision even with PC=extended, so these are skipped (and reported on stderr)
 * where the host cannot keep a 64-bit mantissa; lxrt's long-double ABI handling is still covered by
 * the raw-bit checks around them. */
static int x87_skipped;
static int x87_extended(void)
{
    static int cached = -1;
    if (cached < 0) {
        unsigned short cw = 0x037F, saved;
        unsigned char one[10] = {0,0,0,0,0,0,0,0x80,0xFF,0x3F}, tiny[10] = {0,0,0,0,0,0,0,0x80,0xC3,0x3F}, out[10];
        unsigned long long sig;
        __asm__ volatile("fnstcw %0" : "=m"(saved));
        __asm__ volatile("fldcw %3\n\tfldt %1\n\tfldt %2\n\tfaddp\n\tfstpt %0\n\tfldcw %4"
                         : "=m"(out) : "m"(one), "m"(tiny), "m"(cw), "m"(saved));
        memcpy(&sig, out, 8);   /* 1 + 2^-60 keeps its low significand bit only at 64-bit precision */
        cached = sig == 0x8000000000000010ull;
    }
    return cached;
}
#define X87CHECK(test) do { if (x87_extended()) CHECK(test); else ++x87_skipped; } while (0)

static int close_double(double value, double expected)
{
    uint64_t a, b;
    memcpy(&a, &value, sizeof a);
    memcpy(&b, &expected, sizeof b);
    a = a >> 63 ? ~a : a | UINT64_C(0x8000000000000000);
    b = b >> 63 ? ~b : b | UINT64_C(0x8000000000000000);
    return (a < b ? b - a : a - b) <= 2;
}
static int close_float(float value, float expected)
{
    uint32_t a, b;
    memcpy(&a, &value, sizeof a);
    memcpy(&b, &expected, sizeof b);
    a = a >> 31 ? ~a : a | UINT32_C(0x80000000);
    b = b >> 31 ? ~b : b | UINT32_C(0x80000000);
    return (a < b ? b - a : a - b) <= 2;
}

static void test_random(void)
{
    static const int expected[10] = {
        1804289383, 846930886, 1681692777, 1714636915, 1957747793,
        424238335, 719885386, 1649760492, 596516649, 1189641421
    };
    srand(1);
    for (unsigned i = 0; i < 10; ++i) {
        int r = rand();
        CHECK(r == expected[i]);
        printf("rand1[%u]=%d\n", i, r);
    }
    srandom(1);
    for (unsigned i = 0; i < 10; ++i)
        CHECK(random() == expected[i]);
    srand(1);
    CHECK(random() == expected[0]);
    CHECK(rand() == expected[1]);
    srand(0);
    CHECK(rand() == expected[0]);

    static const unsigned seeds[] = { 0, 1, 0x7fffffff, 0x80000000, 0xffffffff };
    for (unsigned i = 0; i < sizeof seeds / sizeof seeds[0]; ++i) {
        srandom(seeds[i]);
        printf("random(%u):", seeds[i]);
        for (unsigned j = 0; j < 8; ++j)
            printf(" %ld", random());
        putchar('\n');
    }
    static const int rand_r_expected[] = { 476707713, 1186278907, 505671508, 2137716191, 936145377 };
    unsigned state = 1;
    for (unsigned i = 0; i < sizeof rand_r_expected / sizeof rand_r_expected[0]; ++i) {
        int r = rand_r(&state);
        CHECK(r == rand_r_expected[i]);
        printf("rand_r[%u]=%d state=%u\n", i, r, state);
    }
}

static uint64_t next48(uint64_t *state, uint64_t a, unsigned c)
{
    *state = (*state * a + c) & UINT64_C(0xffffffffffff);
    return *state;
}
static uint64_t unpack48(const unsigned short x[3])
{
    return (uint64_t)x[0] | (uint64_t)x[1] << 16 | (uint64_t)x[2] << 32;
}
static void test_random48(void)
{
    const uint64_t a = UINT64_C(0x5deece66d);
    uint64_t reference = UINT64_C(0x1330e);
    CHECK(drand48() == 11.0 * 0x1p-48);
    srand48(1);
    for (unsigned i = 0; i < 10; ++i) {
        double expected = (double)next48(&reference, a, 11) * 0x1p-48;
        double r = drand48();
        CHECK(r == expected);
        printf("drand48[%u]=%.17g\n", i, r);
    }
    srand48(-1);
    reference = UINT64_C(0xffffffff330e);
    for (unsigned i = 0; i < 10; ++i) {
        CHECK(lrand48() == (long)(next48(&reference, a, 11) >> 17));
        CHECK(mrand48() == (int32_t)(next48(&reference, a, 11) >> 16));
    }
    unsigned short x[3] = { 0x330e, 0xabcd, 0x1234 };
    reference = unpack48(x);
    for (unsigned i = 0; i < 10; ++i) {
        CHECK(erand48(x) == (double)next48(&reference, a, 11) * 0x1p-48);
        CHECK(nrand48(x) == (long)(next48(&reference, a, 11) >> 17));
        CHECK(jrand48(x) == (int32_t)(next48(&reference, a, 11) >> 16));
        CHECK(unpack48(x) == reference);
    }
    unsigned short newseed[3] = { 1, 2, 3 };
    unsigned short saved[3];
    srand48(1);
    memcpy(saved, seed48(newseed), sizeof saved);
    CHECK(unpack48(saved) == UINT64_C(0x1330e));
    reference = unpack48(newseed);
    CHECK(drand48() == (double)next48(&reference, a, 11) * 0x1p-48);
    seed48(saved);
    reference = UINT64_C(0x1330e);
    CHECK(drand48() == (double)next48(&reference, a, 11) * 0x1p-48);

    unsigned short params[7] = { 1, 2, 3, 0x1235, 0x5678, 0x9abc, 7 };
    lcong48(params);
    reference = unpack48(params);
    uint64_t custom_a = unpack48(params + 3);
    for (unsigned i = 0; i < 10; ++i)
        CHECK(lrand48() == (long)(next48(&reference, custom_a, 7) >> 17));
    /* srand48/seed48 must restore the standard a and c after lcong48. */
    srand48(1);
    reference = UINT64_C(0x1330e);
    CHECK(drand48() == (double)next48(&reference, a, 11) * 0x1p-48);
    lcong48(params);
    seed48(newseed);
    reference = unpack48(newseed);
    CHECK(drand48() == (double)next48(&reference, a, 11) * 0x1p-48);
}

struct unary { const char *name; double (*fn)(double); double x, expected; };
struct binary { const char *name; double (*fn)(double, double); double x, y; };
struct unaryf { const char *name; float (*fn)(float); float x, expected; };
struct binaryf { const char *name; float (*fn)(float, float); float x, y; };
static void test_double_float(void)
{
    static const struct unary u[] = {
        { "sin", sin, 0.75 }, { "cos", cos, 0.75 }, { "tan", tan, 0.75 },
        { "asin", asin, 0.75 }, { "acos", acos, 0.75 }, { "atan", atan, 0.75 },
        { "sinh", sinh, 0.75, 0.82231673193582999 },
        { "cosh", cosh, 0.75, 1.2946832846768448 },
        { "tanh", tanh, 0.75, 0.6351489523872873 },
        { "exp", exp, 0.75 }, { "exp2", exp2, 0.75, 1.681792830507429 }, { "log", log, 1.75 },
        { "log2", log2, 1.75, 0.80735492205760406 }, { "log10", log10, 1.75 }, { "sqrt", sqrt, 2 },
        { "cbrt", cbrt, -2, -1.2599210498948732 }, { "floor", floor, -2.75 }, { "ceil", ceil, -2.75 },
        { "round", round, -2.5 }, { "trunc", trunc, -2.75 },
        { "rint", rint, 2.5 }, { "nearbyint", nearbyint, 2.5 }, { "fabs", fabs, -2.75 },
        { "expm1", expm1, 0.0000001 },
        { "log1p", log1p, 0.0000001, 9.9999995000000335e-08 },
        { "erf", erf, 0.75, 0.71115563365351508 }
    };
    static const struct binary b[] = {
        { "atan2", atan2, -0.75, 0.25 }, { "pow", pow, 1.25, 2.5 },
        { "hypot", hypot, 3, 4 }, { "fmod", fmod, -7.25, 2 },
        { "copysign", copysign, 2.5, -0.0 }, { "fmin", fmin, 2.5, -3 }, { "fmax", fmax, 2.5, -3 }
    };
    static const struct unaryf uf[] = {
        { "sinf", sinf, 0.75f }, { "cosf", cosf, 0.75f }, { "tanf", tanf, 0.75f },
        { "asinf", asinf, 0.75f }, { "acosf", acosf, 0.75f }, { "atanf", atanf, 0.75f },
        { "sinhf", sinhf, 0.75f }, { "coshf", coshf, 0.75f }, { "tanhf", tanhf, 0.75f },
        { "expf", expf, 0.75f }, { "exp2f", exp2f, 0.75f }, { "logf", logf, 1.75f },
        { "log2f", log2f, 1.75f }, { "log10f", log10f, 1.75f }, { "sqrtf", sqrtf, 2 },
        { "cbrtf", cbrtf, -2 }, { "floorf", floorf, -2.75f }, { "ceilf", ceilf, -2.75f },
        { "roundf", roundf, -2.5f }, { "truncf", truncf, -2.75f },
        { "rintf", rintf, 2.5f }, { "nearbyintf", nearbyintf, 2.5f }, { "fabsf", fabsf, -2.75f },
        { "expm1f", expm1f, 0.0000001f, 1.000000082740371e-07f },
        { "log1pf", log1pf, 0.0000001f },
        { "erff", erff, 0.75f, 0.71115565299987793f }
    };
    static const struct binaryf bf[] = {
        { "atan2f", atan2f, -0.75f, 0.25f }, { "powf", powf, 1.25f, 2.5f },
        { "hypotf", hypotf, 3, 4 }, { "fmodf", fmodf, -7.25f, 2 },
        { "copysignf", copysignf, 2.5f, -0.0f }, { "fminf", fminf, 2.5f, -3 }, { "fmaxf", fmaxf, 2.5f, -3 }
    };
    for (unsigned i = 0; i < sizeof u / sizeof u[0]; ++i) {
        double result = u[i].fn(u[i].x);
        if (u[i].expected != 0) {
            int pass = close_double(result, u[i].expected);
            CHECK(pass);
            printf("%s=%s\n", u[i].name, pass ? "pass" : "FAIL");
        } else {
            printf("%s=%.17g\n", u[i].name, result);
        }
    }
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; ++i)
        printf("%s=%.17g\n", b[i].name, b[i].fn(b[i].x, b[i].y));
    /* i386 glibc can leave excess precision in ST0; round each result to its
     * declared float type before the variadic promotion to double. */
    for (unsigned i = 0; i < sizeof uf / sizeof uf[0]; ++i) {
        volatile float result = uf[i].fn(uf[i].x);
        if (uf[i].expected != 0) {
            int pass = close_float(result, uf[i].expected);
            CHECK(pass);
            printf("%s=%s\n", uf[i].name, pass ? "pass" : "FAIL");
        } else {
            printf("%s=%.17g\n", uf[i].name, (double)result);
        }
    }
    for (unsigned i = 0; i < sizeof bf / sizeof bf[0]; ++i) {
        volatile float result = bf[i].fn(bf[i].x, bf[i].y);
        printf("%s=%.17g\n", bf[i].name, (double)result);
    }

    double (*volatile call_fma)(double, double, double) = fma;
    float (*volatile call_fmaf)(float, float, float) = fmaf;
    double (*volatile call_frexp)(double, int *) = frexp;
    double (*volatile call_ldexp)(double, int) = ldexp;
    double (*volatile call_modf)(double, double *) = modf;
    float (*volatile call_frexpf)(float, int *) = frexpf;
    float (*volatile call_ldexpf)(float, int) = ldexpf;
    float (*volatile call_modff)(float, float *) = modff;
    void (*volatile call_sincos)(double, double *, double *) = sincos;
    void (*volatile call_sincosf)(float, float *, float *) = sincosf;
    long (*volatile call_lround)(double) = lround;
    long long (*volatile call_llround)(double) = llround;
    long (*volatile call_lrint)(double) = lrint;
    long long (*volatile call_llrint)(double) = llrint;
    long (*volatile call_lroundf)(float) = lroundf;
    long long (*volatile call_llroundf)(float) = llroundf;
    long (*volatile call_lrintf)(float) = lrintf;
    long long (*volatile call_llrintf)(float) = llrintf;
    int e;
    double f = call_frexp(123.75, &e);
    CHECK(f == 0.966796875 && e == 7);
    CHECK(call_ldexp(f, e) == 123.75);
    double integer;
    f = call_modf(-123.75, &integer);
    CHECK(f == -0.75 && integer == -123);
    printf("frexp/modf=%.17g %.17g\n", f, integer);
    float ff = call_frexpf(123.75f, &e);
    CHECK(ff == 0.966796875f && e == 7);
    CHECK(call_ldexpf(ff, e) == 123.75f);
    float integerf;
    CHECK(call_modff(-123.75f, &integerf) == -0.75f && integerf == -123);
    CHECK(call_fma(1.25, 2.5, -0.75) == 2.375);
    CHECK(call_fmaf(1.25f, 2.5f, -0.75f) == 2.375f);
    CHECK(call_lround(-2.5) == -3 && call_llround(4294967296.5) == INT64_C(4294967297));
    CHECK(call_lrint(2.5) == 2 && call_llrint(4294967296.0) == INT64_C(4294967296));
    CHECK(call_lroundf(-2.5f) == -3 && call_llroundf(4294967296.0f) == INT64_C(4294967296));
    CHECK(call_lrintf(2.5f) == 2 && call_llrintf(4294967296.0f) == INT64_C(4294967296));
    double s, c;
    call_sincos(0.75, &s, &c);
    printf("sincos=%.17g %.17g\n", s, c);
    float sf, cf;
    call_sincosf(0.75f, &sf, &cf);
    printf("sincosf=%.17g %.17g\n", (double)sf, (double)cf);
    errno = 0;
    double (*volatile call_sqrt)(double) = sqrt;
    CHECK(isnan(call_sqrt(-1)) && errno == EDOM);
}

static uint64_t ld_sig(const long double *x)
{
    uint64_t r;
    memcpy(&r, x, 8);
    return r;
}
static unsigned ld_exp(const long double *x)
{
    uint16_t r;
    memcpy(&r, (const char *)x + 8, 2);
    return r;
}
static void ld_init(long double *out, uint64_t significand, uint16_t exponent)
{
    uint16_t tail[2] = { exponent, 0 };
    memcpy(out, &significand, 8);
    memcpy((char *)out + 8, tail, 4);
}

/* Prism's FLD ST(0) register copy rounds to double even with CW=037f.
 * Exercise the real HLE ABI without allowing a compiler-generated register copy:
 * every guest argument is loaded/stored through its 12-byte memory representation.
 * Call frames retain SysV's 16-byte pre-call alignment. */
typedef void (*guest_math_fn)(void);
static __attribute__((naked)) void math80_unary(long double *out, const long double *in, guest_math_fn fn)
{
    __asm__ volatile(
        "movl 8(%esp), %ecx\n\t"
        "movl 12(%esp), %edx\n\t"
        "fldt (%ecx)\n\t"
        "subl $12, %esp\n\t"
        "fstpt (%esp)\n\t"
        "call *%edx\n\t"
        "addl $12, %esp\n\t"
        "movl 4(%esp), %eax\n\t"
        "fstpt (%eax)\n\t"
        "ret");
}
static __attribute__((naked)) void math80_arg(long double *out, const long double *in, uintptr_t extra, guest_math_fn fn)
{
    __asm__ volatile(
        "movl 8(%esp), %ecx\n\t"
        "movl 12(%esp), %eax\n\t"
        "movl 16(%esp), %edx\n\t"
        "fldt (%ecx)\n\t"
        "subl $28, %esp\n\t"
        "fstpt (%esp)\n\t"
        "movl %eax, 12(%esp)\n\t"
        "call *%edx\n\t"
        "addl $28, %esp\n\t"
        "movl 4(%esp), %eax\n\t"
        "fstpt (%eax)\n\t"
        "ret");
}
static __attribute__((naked)) void math80_binary(long double *out, const long double *x, const long double *y, guest_math_fn fn)
{
    __asm__ volatile(
        "movl 8(%esp), %eax\n\t"
        "movl 12(%esp), %ecx\n\t"
        "movl 16(%esp), %edx\n\t"
        "fldt (%ecx)\n\t"
        "fldt (%eax)\n\t"
        "subl $28, %esp\n\t"
        "fstpt (%esp)\n\t"
        "fstpt 12(%esp)\n\t"
        "call *%edx\n\t"
        "addl $28, %esp\n\t"
        "movl 4(%esp), %eax\n\t"
        "fstpt (%eax)\n\t"
        "ret");
}
static __attribute__((naked)) int64_t math80_integer(const long double *in, guest_math_fn fn)
{
    __asm__ volatile(
        "movl 4(%esp), %ecx\n\t"
        "movl 8(%esp), %edx\n\t"
        "fldt (%ecx)\n\t"
        "subl $12, %esp\n\t"
        "fstpt (%esp)\n\t"
        "call *%edx\n\t"
        "addl $12, %esp\n\t"
        "ret");
}
static void test_long_double(void)
{
    _Static_assert(sizeof(long double) == 12, "test requires glibc i386 x87 ABI");
    locale_t loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    CHECK(loc != (locale_t)0);
    if (!loc)
        return;
    /* strtold_l is intentionally double-backed in this HLE. Check conversion,
     * end pointers and errno independently of the extended-range math ABI. */
    char *end;
    errno = EDOM;
    long double parsed = strtold_l(" -123.75tail", &end, loc);
    CHECK((double)parsed == -123.75 && !strcmp(end, "tail") && errno == EDOM);
    int parsed_e = 0;
    long double parsed_fraction = call_frexpl(parsed, &parsed_e);
    CHECK((double)parsed_fraction == -0.966796875 && parsed_e == 7);
    errno = 0;
    parsed = strtold_l("0x1.8p+8tail", &end, loc);
    CHECK((double)parsed == 384 && !strcmp(end, "tail") && errno == 0);
    const char invalid[] = "tail";
    errno = EDOM;
    parsed = strtold_l(invalid, &end, loc);
    CHECK(ld_sig(&parsed) == 0 && end == invalid && errno == EDOM);
    errno = 0;
    parsed = strtold_l("1e9999tail", &end, loc);
    CHECK(__fpclassifyl(parsed) == 1 && !strcmp(end, "tail") && errno == ERANGE);
    errno = 0;
    parsed = strtold_l("1e-9999tail", &end, loc);
    CHECK(ld_sig(&parsed) == 0 && !strcmp(end, "tail") && errno == ERANGE);
    /* Literal initialization supplies exact x87 values without depending on the
     * deliberately narrower string conversion implementation. */
    static const struct { long double value; int exponent; } inputs[] = {
        { 0x1.0000000000000002p+0L, 1 },
        { 0x1.0000000000000002p+4000L, 4001 },
        { -0x1.0000000000000002p-4000L, -3999 },
        { 0x1p-16445L, -16444 }
    };
    for (unsigned i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
        const long double *value = &inputs[i].value;
        static const unsigned input_exponents[] = { 0x3fff, 0x4f9f, 0xb05f, 0 };
        CHECK(ld_sig(value) == (i == 3 ? 1 : UINT64_C(0x8000000000000001)));
        CHECK(ld_exp(value) == input_exponents[i]);
        printf("x87-input[%u]=%016" PRIx64 "/%04x\n", i, ld_sig(value), ld_exp(value));
        int e = 123;
        long double f;
        math80_arg(&f, value, (uintptr_t)&e, (guest_math_fn)call_frexpl);
        CHECK(e == inputs[i].exponent);
        CHECK(ld_sig(&f) == (i == 3 ? UINT64_C(0x8000000000000000) : UINT64_C(0x8000000000000001)));
        CHECK(ld_exp(&f) == (i == 2 ? 0xbffe : 0x3ffe));
        long double restored;
        math80_arg(&restored, &f, (uintptr_t)e, (guest_math_fn)call_ldexpl);
        X87CHECK(ld_sig(&restored) == ld_sig(value) && ld_exp(&restored) == ld_exp(value));
        printf("frexpl[%u]=%016" PRIx64 "/%04x e=%d as-double=%.17g\n", i, ld_sig(&f), ld_exp(&f), e, (double)f);
    }
    long double value, integer;
    ld_init(&value, UINT64_C(0xf780000000000000), 0xc005);
    long double f;
    math80_arg(&f, &value, (uintptr_t)&integer, (guest_math_fn)call_modfl);
    CHECK((double)f == -0.75 && (double)integer == -123);
    ld_init(&value, UINT64_C(0x8000000000000001), 0x3fff);
    math80_arg(&f, &value, (uintptr_t)&integer, (guest_math_fn)call_modfl);
    CHECK(ld_sig(&f) == UINT64_C(0x8000000000000000) && ld_exp(&f) == 16320);
    CHECK((double)integer == 1);
    ld_init(&value, UINT64_C(0x8000000000000001), 0xcf9f);
    math80_unary(&f, &value, (guest_math_fn)call_fabsl);
    X87CHECK(ld_sig(&f) == ld_sig(&value) && ld_exp(&f) == 0x4f9f);
    ld_init(&value, UINT64_C(0x8000000000000000), 0x4f9f);
    math80_unary(&f, &value, (guest_math_fn)call_sqrtl);
    X87CHECK(ld_sig(&f) == UINT64_C(0x8000000000000000) && ld_exp(&f) == 18383);
    ld_init(&value, 0, 0x8000);
    int e = 9;
    math80_arg(&f, &value, (uintptr_t)&e, (guest_math_fn)call_frexpl);
    CHECK(e == 0 && ld_sig(&f) == 0 && ld_exp(&f) == 0x8000);
    math80_arg(&f, &value, (uintptr_t)&integer, (guest_math_fn)call_modfl);
    CHECK(ld_exp(&f) == 0x8000 && ld_exp(&integer) == 0x8000);
    ld_init(&value, UINT64_C(0x8000000000000000), 0x7fff);
    math80_arg(&f, &value, (uintptr_t)&integer, (guest_math_fn)call_modfl);
    CHECK(ld_sig(&f) == 0 && ld_exp(&f) == 0 && __fpclassifyl(integer) == 1);
    long double (*volatile call_roundl)(long double) = roundl;
    long double (*volatile call_floorl)(long double) = floorl;
    long double (*volatile call_ceill)(long double) = ceill;
    long double (*volatile call_truncl)(long double) = truncl;
    long double (*volatile call_rintl)(long double) = rintl;
    long double (*volatile call_nearbyintl)(long double) = nearbyintl;
    long double (*volatile call_fmodl)(long double, long double) = fmodl;
    long double (*volatile call_copysignl)(long double, long double) = copysignl;
    long double (*volatile call_fminl)(long double, long double) = fminl;
    long double (*volatile call_fmaxl)(long double, long double) = fmaxl;
    long double (*volatile call_fmal)(long double, long double, long double) = fmal;
    void (*volatile call_sincosl)(long double, long double *, long double *) = sincosl;
    long (*volatile call_lroundl)(long double) = lroundl;
    long long (*volatile call_llroundl)(long double) = llroundl;
    long (*volatile call_lrintl)(long double) = lrintl;
    long long (*volatile call_llrintl)(long double) = llrintl;
    ld_init(&value, UINT64_C(0x8000000000000001), 0x403d);
    math80_unary(&f, &value, (guest_math_fn)call_roundl);
    X87CHECK(ld_sig(&f) == UINT64_C(0x8000000000000002) && ld_exp(&f) == 16445);
    X87CHECK(math80_integer(&value, (guest_math_fn)call_llroundl) == INT64_C(4611686018427387905));
    math80_unary(&f, &value, (guest_math_fn)call_floorl);
    X87CHECK(ld_sig(&f) == UINT64_C(0x8000000000000000) && ld_exp(&f) == 16445);
    math80_unary(&f, &value, (guest_math_fn)call_ceill);
    X87CHECK(ld_sig(&f) == UINT64_C(0x8000000000000002) && ld_exp(&f) == 16445);
    CHECK((double)call_truncl(-2.75L) == -2);
    CHECK((double)call_rintl(2.5L) == 2 && (double)call_nearbyintl(2.5L) == 2);
    CHECK(call_lroundl(-2.5L) == -3 && call_lrintl(2.5L) == 2);
    CHECK(call_llrintl(4294967296.0L) == INT64_C(4294967296));
    ld_init(&value, UINT64_C(0x8000000000000000), 0x4f9f);
    long double three, negative_zero, positive_zero;
    ld_init(&three, UINT64_C(0xc000000000000000), 0x4000);
    ld_init(&negative_zero, 0, 0x8000);
    ld_init(&positive_zero, 0, 0);
    math80_binary(&f, &value, &three, (guest_math_fn)call_fmodl);
    X87CHECK((double)f == 1);
    math80_binary(&f, &value, &negative_zero, (guest_math_fn)call_copysignl);
    X87CHECK(ld_sig(&f) == ld_sig(&value) && ld_exp(&f) == (ld_exp(&value) | 0x8000));
    math80_binary(&f, &negative_zero, &positive_zero, (guest_math_fn)call_fminl);
    CHECK(ld_sig(&f) == 0 && ld_exp(&f) == 0x8000);
    math80_binary(&f, &negative_zero, &positive_zero, (guest_math_fn)call_fmaxl);
    CHECK(ld_sig(&f) == 0 && ld_exp(&f) == 0);
    CHECK((double)call_fmal(1.25L, 2.5L, -0.75L) == 2.375);
    long double sl, cl;
    call_sincosl(0.75L, &sl, &cl);
    printf("sincosl=%.17g %.17g\n", (double)sl, (double)cl);
    freelocale(loc);
}

#define SCALE_CASE(name, type, exponent_type) do { \
    type (*volatile fn)(type, exponent_type) = name; \
    type result = fn((type)3.5, 4); \
    CHECK(result == (type)56); \
    printf(#name "=%.17g\n", (double)result); \
} while (0)
#define ILOGB_CASE(name, type) do { \
    int (*volatile fn)(type) = name; \
    int result = fn((type)12.5); \
    CHECK(result == 3); \
    printf(#name "=%d\n", result); \
} while (0)
#define LOGB_CASE(name, type) do { \
    type (*volatile fn)(type) = name; \
    type result = fn((type)12.5); \
    CHECK(result == (type)3); \
    printf(#name "=%.17g\n", (double)result); \
} while (0)
#define BINARY_CASE(name, type, x, y, expected) do { \
    type (*volatile fn)(type, type) = name; \
    type result = fn((type)(x), (type)(y)); \
    CHECK(result == (type)(expected)); \
    printf(#name "=%.17g\n", (double)result); \
} while (0)
#define REMQUO_CASE(name, type) do { \
    type (*volatile fn)(type, type, int *) = name; \
    int quotient = 0; \
    type result = fn((type)7, (type)2, &quotient); \
    CHECK(result == (type)-1 && quotient == 4); \
    printf(#name "=%.17g q=%d\n", (double)result, quotient); \
} while (0)
static void test_math_extensions(void)
{
    SCALE_CASE(scalbn, double, int);
    SCALE_CASE(scalbnf, float, int);
    SCALE_CASE(scalbnl, long double, int);
    SCALE_CASE(scalbln, double, long);
    SCALE_CASE(scalblnf, float, long);
    SCALE_CASE(scalblnl, long double, long);
    ILOGB_CASE(ilogb, double);
    ILOGB_CASE(ilogbf, float);
    ILOGB_CASE(ilogbl, long double);
    int (*volatile ilogb_call)(double) = ilogb;
    int (*volatile ilogbf_call)(float) = ilogbf;
    errno = 0;
    CHECK(ilogb_call(NAN) == INT32_MIN && errno == EDOM);
    errno = 0;
    CHECK(ilogbf_call(NAN) == INT32_MIN && errno == EDOM);
    errno = 0;
    CHECK(ilogb_call(0) == INT32_MIN && errno == EDOM);
    errno = 0;
    CHECK(ilogbf_call(INFINITY) == INT32_MAX && errno == EDOM);
    LOGB_CASE(logb, double);
    LOGB_CASE(logbf, float);
    LOGB_CASE(logbl, long double);
    BINARY_CASE(nextafter, double, 1, 2, 0x1.0000000000001p0);
    BINARY_CASE(nextafterf, float, 1, 2, 0x1.000002p0f);
    long double one, two, stepped;
    ld_init(&one, UINT64_C(0x8000000000000000), 0x3fff);
    ld_init(&two, UINT64_C(0x8000000000000000), 0x4000);
    long double (*volatile step)(long double, long double) = nextafterl;
    math80_binary(&stepped, &one, &two, (guest_math_fn)step);
    CHECK(ld_sig(&stepped) == UINT64_C(0x8000000000000001) && ld_exp(&stepped) == 0x3fff);
    printf("nextafterl=%016" PRIx64 "/%04x\n", ld_sig(&stepped), ld_exp(&stepped));
    BINARY_CASE(remainder, double, 7, 2, -1);
    BINARY_CASE(remainderf, float, 7, 2, -1);
    BINARY_CASE(remainderl, long double, 7, 2, -1);
    REMQUO_CASE(remquo, double);
    REMQUO_CASE(remquof, float);
    REMQUO_CASE(remquol, long double);
    BINARY_CASE(fdim, double, 2.5, -0.75, 3.25);
    BINARY_CASE(fdimf, float, 2.5, -0.75, 3.25);
    BINARY_CASE(fdiml, long double, 2.5, -0.75, 3.25);
    long double large, scaled;
    ld_init(&large, UINT64_C(0x8000000000000000), 0x4f9f);
    long double (*volatile scale)(long double, int) = scalbnl;
    math80_arg(&scaled, &large, 1, (guest_math_fn)scale);
    X87CHECK(ld_sig(&scaled) == UINT64_C(0x8000000000000000) && ld_exp(&scaled) == 20384);
    int (*volatile exponent)(long double) = ilogbl;
    CHECK((int)math80_integer(&large, (guest_math_fn)exponent) == 4000);
}
#undef SCALE_CASE
#undef ILOGB_CASE
#undef LOGB_CASE
#undef BINARY_CASE
#undef REMQUO_CASE

static void test_classification(void)
{
    static const double values[] = { NAN, INFINITY, -INFINITY, 0.0, -0.0, 0x1p-1074, 1.0 };
    static const int classes[] = { 0, 1, 1, 2, 2, 3, 4 };
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
        int c = __fpclassify(values[i]);
        CHECK(c == classes[i]);
        CHECK(!!__finite(values[i]) == (classes[i] >= 2));
        CHECK(!!__isnan(values[i]) == (classes[i] == 0));
        CHECK(__isinf(values[i]) == (i == 1 ? 1 : i == 2 ? -1 : 0));
        printf("class[%u]=%d finite=%d nan=%d inf=%d\n", i, c, !!__finite(values[i]), !!__isnan(values[i]), __isinf(values[i]));
    }
    CHECK(__fpclassifyf(0x1p-149f) == 3);
    CHECK(__fpclassifyf(-0.0f) == 2);
    CHECK(__finitef(1.0f) && !__finitef(INFINITY));
    CHECK(__isnanf(NAN) && !__isnanf(1.0f));
    CHECK(__isinff(INFINITY) == 1 && __isinff(-INFINITY) == -1);
}

static void test_integer_helpers(void)
{
    int64_t a = -INT64_C(9223372036854775807), b = INT64_C(2147483647);
    CHECK(__divdi3(a, b) == -INT64_C(4294967298));
    CHECK(__moddi3(a, b) == -1);
    CHECK(__divdi3(a, -b) == INT64_C(4294967298));
    CHECK(__moddi3(a, -b) == -1);
    CHECK(__udivdi3(UINT64_MAX, UINT64_C(4294967295)) == UINT64_C(4294967297));
    CHECK(__umoddi3(UINT64_MAX, UINT64_C(4294967295)) == 0);
    CHECK(__udivdi3(UINT64_MAX, UINT64_C(4294967297)) == UINT64_C(4294967295));
    CHECK(__umoddi3(UINT64_MAX, UINT64_C(4294967297)) == 0);
    printf("divdi3=%" PRId64 " moddi3=%" PRId64 " udivdi3=%" PRIu64 "\n",
           __divdi3(a, b), __moddi3(a, b), __udivdi3(UINT64_MAX, UINT64_C(4294967295)));
}

int main(void)
{
    test_random();
    test_random48();
    test_double_float();
    test_long_double();
    test_math_extensions();
    test_classification();
    test_integer_helpers();
    if (x87_skipped)
        fprintf(stderr, "math: %d checks skipped: host x87 lacks 64-bit precision\n", x87_skipped);
    printf("math failures=%d\n", failures);
    return failures != 0;
}
