/* i386 libm and libc PRNG entry points.
 * UCRT supplies float/double arithmetic; all long-double entry points below consume
 * the guest's 12-byte x87 stack arguments and return through ST(0), never C long double.
 * Decomposition/basic long-double operations never narrow through a C floating type;
 * x87 arithmetic follows the caller's control word. Additional UCRT-backed
 * long-double aliases use double precision (including its exponent range).
 * Float/double transcendental rounding inherits UCRT and can differ in the final
 * significand bit from glibc; the PRNG algorithms and sequences are glibc-compatible.
 * Windows ARM64 Prism rounds some x87 register-to-register copies to double;
 * memory-based thunk inputs/outputs cannot fix such rounding in guest instructions.
 * On the current Prism host, extended-precision/range arithmetic stress assertions
 * also fail with exact memory arguments, while raw 80-bit loads/stores and frexpl
 * pass. Full arithmetic conformance therefore needs a conforming x87 execution host.
 */
#include "lxrt.h"

#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static void math_errno(void)
{
    if (errno == EDOM)
        lx_errno = LX_EDOM;
    else if (errno == ERANGE)
        lx_errno = LX_ERANGE;
}

#define MATH_UNARY(name) \
    static double lxm_##name(double x) { errno = 0; double r = name(x); math_errno(); return r; } \
    static float lxm_##name##f(float x) { errno = 0; float r = name##f(x); math_errno(); return r; }
#define MATH_BINARY(name) \
    static double lxm_##name(double x, double y) { errno = 0; double r = name(x, y); math_errno(); return r; } \
    static float lxm_##name##f(float x, float y) { errno = 0; float r = name##f(x, y); math_errno(); return r; }
#define MATH_INT(name, type) \
    static type lxm_##name(double x) { errno = 0; type r = name(x); math_errno(); return r; } \
    static type lxm_##name##f(float x) { errno = 0; type r = name##f(x); math_errno(); return r; }

MATH_UNARY(sin)
MATH_UNARY(cos)
MATH_UNARY(tan)
MATH_UNARY(asin)
MATH_UNARY(acos)
MATH_UNARY(atan)
MATH_BINARY(atan2)
MATH_UNARY(sinh)
MATH_UNARY(cosh)
MATH_UNARY(tanh)
MATH_UNARY(exp)
MATH_UNARY(exp2)
MATH_UNARY(log)
MATH_UNARY(log2)
MATH_UNARY(log10)
MATH_BINARY(pow)
MATH_UNARY(sqrt)
MATH_UNARY(cbrt)
MATH_BINARY(hypot)
MATH_BINARY(fmod)
MATH_UNARY(floor)
MATH_UNARY(ceil)
MATH_UNARY(round)
MATH_UNARY(trunc)
MATH_INT(lround, long)
MATH_INT(llround, long long)
MATH_UNARY(rint)
MATH_INT(lrint, long)
MATH_INT(llrint, long long)
MATH_UNARY(nearbyint)
MATH_UNARY(fabs)
MATH_BINARY(copysign)
MATH_BINARY(fmin)
MATH_BINARY(fmax)
MATH_UNARY(expm1)
MATH_UNARY(log1p)
MATH_UNARY(erf)
MATH_UNARY(logb)
MATH_BINARY(nextafter)
MATH_BINARY(remainder)
MATH_BINARY(fdim)

#define MATH_SCALE(name, exponent_type) \
    static double lxm_##name(double x, exponent_type e) { errno = 0; double r = name(x, e); math_errno(); return r; } \
    static float lxm_##name##f(float x, exponent_type e) { errno = 0; float r = name##f(x, e); math_errno(); return r; }
MATH_SCALE(scalbn, int)
MATH_SCALE(scalbln, long)

static double lxm_remquo(double x, double y, int *q)
{
    errno = 0;
    double r = remquo(x, y, q);
    math_errno();
    return r;
}
static float lxm_remquof(float x, float y, int *q)
{
    errno = 0;
    float r = remquof(x, y, q);
    math_errno();
    return r;
}

static double lxm_fma(double x, double y, double z)
{
    errno = 0;
    double r = fma(x, y, z);
    math_errno();
    return r;
}
static float lxm_fmaf(float x, float y, float z)
{
    errno = 0;
    float r = fmaf(x, y, z);
    math_errno();
    return r;
}
static double lxm_frexp(double x, int *e) { return frexp(x, e); }
static float lxm_frexpf(float x, int *e) { return frexpf(x, e); }
static double lxm_ldexp(double x, int e)
{
    errno = 0;
    double r = ldexp(x, e);
    math_errno();
    return r;
}
static float lxm_ldexpf(float x, int e)
{
    errno = 0;
    float r = ldexpf(x, e);
    math_errno();
    return r;
}
static double lxm_modf(double x, double *i) { return modf(x, i); }
static float lxm_modff(float x, float *i) { return modff(x, i); }
static void lxm_sincos(double x, double *s, double *c)
{
    double sv = lxm_sin(x), cv = lxm_cos(x);
    *s = sv;
    *c = cv;
}
static void lxm_sincosf(float x, float *s, float *c)
{
    float sv = lxm_sinf(x), cv = lxm_cosf(x);
    *s = sv;
    *c = cv;
}

/* glibc's classification enum differs from the UCRT enum. Infinity predicates
 * must retain the sign (glibc __isinf(-INFINITY) returns -1, not just true). */
static int lxm_fpclassify(double x)
{
    union { double f; uint64_t u; } v = { x };
    uint64_t fraction = v.u & UINT64_C(0x000fffffffffffff);
    unsigned e = (unsigned)(v.u >> 52) & 0x7ff;
    return e == 0x7ff ? (fraction ? 0 : 1) : !e ? (fraction ? 3 : 2) : 4;
}
static int lxm_fpclassifyf(float x)
{
    union { float f; uint32_t u; } v = { x };
    uint32_t fraction = v.u & 0x7fffff;
    unsigned e = v.u >> 23 & 0xff;
    return e == 0xff ? (fraction ? 0 : 1) : !e ? (fraction ? 3 : 2) : 4;
}
static int lxm_finite(double x) { return lxm_fpclassify(x) >= 2; }
static int lxm_finitef(float x) { return lxm_fpclassifyf(x) >= 2; }
static int lxm_isnan(double x) { return lxm_fpclassify(x) == 0; }
static int lxm_isnanf(float x) { return lxm_fpclassifyf(x) == 0; }
static int lxm_isinf(double x) { return lxm_fpclassify(x) == 1 ? (signbit(x) ? -1 : 1) : 0; }
static int lxm_isinff(float x) { return lxm_fpclassifyf(x) == 1 ? (signbit(x) ? -1 : 1) : 0; }

/* UCRT FP_ILOGBNAN is INT_MAX; glibc i386 uses INT_MIN. All exceptional
 * ilogb inputs also set Linux EDOM and raise invalid, including infinity. */
static int lxm_ilogb(double x)
{
    int c = lxm_fpclassify(x);
    if (c <= 2) {
        lx_errno = LX_EDOM;
        feraiseexcept(FE_INVALID);
        return c == 1 ? INT_MAX : INT_MIN;
    }
    errno = 0;
    int result = ilogb(x);
    math_errno();
    return result;
}
static int lxm_ilogbf(float x)
{
    int c = lxm_fpclassifyf(x);
    if (c <= 2) {
        lx_errno = LX_EDOM;
        feraiseexcept(FE_INVALID);
        return c == 1 ? INT_MAX : INT_MIN;
    }
    errno = 0;
    int result = ilogbf(x);
    math_errno();
    return result;
}

/* Little-endian glibc i386 long double: 64-bit explicit significand, sign/exponent,
 * then two padding bytes. Access guest storage bytewise: native uint64_t may be
 * 8-byte aligned, while guest long-double objects are 12 bytes and 4-byte aligned.
 * No native floating type or native structure layout is used for these fields. */
static uint64_t ld_significand(const void *p) { uint64_t s; memcpy(&s, p, 8); return s; }
static unsigned ld_se(const void *p) { uint16_t e; memcpy(&e, (const char *)p + 8, 2); return e; }
static void ld_store(void *p, uint64_t s, unsigned se)
{
    uint16_t tail[2] = { (uint16_t)se, 0 };
    memcpy(p, &s, 8);
    memcpy((char *)p + 8, tail, 4);
}

static void ld_frexp(const void *x, int *e, void *out)
{
    uint64_t s = ld_significand(x);
    unsigned se = ld_se(x), exponent = se & 0x7fff;
    *e = 0;
    if (s && exponent != 0x7fff) {
        int exp = exponent ? (int)exponent : 1;
        while (!(s & UINT64_C(0x8000000000000000))) {
            s <<= 1;
            --exp;
        }
        *e = exp - 16382;
        se = (se & 0x8000) | 16382;
    }
    ld_store(out, s, se);
}

static __declspec(naked) void lxm_frexpl(void)
{
    __asm {
        sub esp, 12
        lea eax, [esp]
        lea ecx, [esp + 16]
        mov edx, [esp + 28]
        push eax
        push edx
        push ecx
        call ld_frexp
        add esp, 12
        fld tbyte ptr [esp]
        add esp, 12
        ret
    }
}

static void ld_scale_errno(const void *x, const void *r)
{
    unsigned xe = ld_se(x) & 0x7fff, re = ld_se(r) & 0x7fff;
    if (xe != 0x7fff && ld_significand(x) &&
        (re == 0x7fff || (!re && !ld_significand(r))))
        lx_errno = LX_ERANGE;
}
static __declspec(naked) void lxm_ldexpl(void)
{
    __asm {
        sub esp, 12
        fild dword ptr [esp + 28]
        fld tbyte ptr [esp + 16]
        fscale
        fstp tbyte ptr [esp]
        fstp st(0)
        lea eax, [esp]
        lea ecx, [esp + 16]
        push eax
        push ecx
        call ld_scale_errno
        add esp, 8
        fld tbyte ptr [esp]
        add esp, 12
        ret
    }
}

/* Split the significand without an arithmetic subtraction: the fractional part
 * must not lose bits when the host currently has 53-bit x87 precision selected. */
static void ld_integer(const void *x, void *integer, void *fraction)
{
    uint64_t s = ld_significand(x);
    unsigned se = ld_se(x), exponent = se & 0x7fff;
    int e = (int)exponent - 16383;
    if (exponent == 0x7fff) {
        ld_store(integer, s, se);
        if (s == UINT64_C(0x8000000000000000))
            ld_store(fraction, 0, se & 0x8000);
        else
            ld_store(fraction, s, se);
    } else if (e < 0) {
        ld_store(integer, 0, se & 0x8000);
        ld_store(fraction, s, se);
    } else if (e >= 63) {
        ld_store(integer, s, se);
        ld_store(fraction, 0, se & 0x8000);
    } else {
        uint64_t mask = UINT64_MAX >> (e + 1), frac = s & mask;
        ld_store(integer, s & ~mask, se);
        if (!frac) {
            ld_store(fraction, 0, se & 0x8000);
        } else {
            while (!(frac & UINT64_C(0x8000000000000000))) {
                frac <<= 1;
                --exponent;
            }
            ld_store(fraction, frac, (se & 0x8000) | exponent);
        }
    }
}
static __declspec(naked) void lxm_modfl(void)
{
    __asm {
        sub esp, 12
        lea eax, [esp + 16]
        mov ecx, [esp + 28]
        lea edx, [esp]
        push edx
        push ecx
        push eax
        call ld_integer
        add esp, 12
        fld tbyte ptr [esp]
        add esp, 12
        ret
    }
}
static __declspec(naked) void lxm_fabsl(void)
{
    __asm { fld tbyte ptr [esp + 4] }
    __asm { fabs }
    __asm { ret }
}
static void ld_sqrt_errno(const void *x)
{
    if ((ld_se(x) & 0x8000) && ld_significand(x) &&
        ((ld_se(x) & 0x7fff) != 0x7fff || ld_significand(x) == UINT64_C(0x8000000000000000)))
        lx_errno = LX_EDOM;
}
static __declspec(naked) void lxm_sqrtl(void)
{
    __asm {
        lea eax, [esp + 4]
        push eax
        call ld_sqrt_errno
        add esp, 4
        fld tbyte ptr [esp + 4]
        fsqrt
        ret
    }
}

/* ABI bridges for UCRT-backed long-double aliases. The scratch stack holds
 * native doubles, not guest long doubles; scalar pointers never cross this bridge. */
#define LD_UNARY(name) \
    static __declspec(naked) void lxm_##name##l(void) { \
        __asm { sub esp, 8 } \
        __asm { fld tbyte ptr [esp + 12] } \
        __asm { fstp qword ptr [esp] } \
        __asm { call lxm_##name } \
        __asm { add esp, 8 } \
        __asm { ret } \
    }
#define LD_BINARY(name) \
    static __declspec(naked) void lxm_##name##l(void) { \
        __asm { sub esp, 16 } \
        __asm { fld tbyte ptr [esp + 20] } \
        __asm { fstp qword ptr [esp] } \
        __asm { fld tbyte ptr [esp + 32] } \
        __asm { fstp qword ptr [esp + 8] } \
        __asm { call lxm_##name } \
        __asm { add esp, 16 } \
        __asm { ret } \
    }
LD_UNARY(sin)
LD_UNARY(cos)
LD_UNARY(tan)
LD_UNARY(asin)
LD_UNARY(acos)
LD_UNARY(atan)
LD_BINARY(atan2)
LD_UNARY(sinh)
LD_UNARY(cosh)
LD_UNARY(tanh)
LD_UNARY(exp)
LD_UNARY(exp2)
LD_UNARY(log)
LD_UNARY(log2)
LD_UNARY(log10)
LD_BINARY(pow)
LD_UNARY(cbrt)
LD_BINARY(hypot)
LD_UNARY(expm1)
LD_UNARY(log1p)
LD_UNARY(erf)
LD_UNARY(logb)
LD_BINARY(remainder)
LD_BINARY(fdim)
/* nextafterl needs the x87 lattice, not the UCRT/double lattice. */
static void ld_nextafter(const void *x, const void *y, void *out)
{
    const uint64_t integer = UINT64_C(0x8000000000000000);
    uint64_t xs = ld_significand(x), ys = ld_significand(y);
    unsigned xe = ld_se(x), ye = ld_se(y);
    unsigned xm = xe & 0x7fff, ym = ye & 0x7fff;
    int xnan = xm == 0x7fff && xs != integer, ynan = ym == 0x7fff && ys != integer;
    if (xnan || ynan) {
        if ((xnan && !(xs & (integer >> 1))) || (ynan && !(ys & (integer >> 1))))
            feraiseexcept(FE_INVALID);
        ld_store(out, (xnan ? xs : ys) | (integer >> 1), xnan ? xe : ye);
        return;
    }
    if ((xe == ye && xs == ys) || (!xm && !ym && !xs && !ys)) {
        ld_store(out, ys, ye);
        return;
    }
    if (!xm && !xs) {
        ld_store(out, 1, ye & 0x8000);
        lx_errno = LX_ERANGE;
        feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
        return;
    }
    int less;
    if ((xe ^ ye) & 0x8000)
        less = !!(xe & 0x8000);
    else {
        less = xm < ym || (xm == ym && xs < ys);
        if (xe & 0x8000)
            less = !less;
    }
    int outward = xe & 0x8000 ? !less : less;
    if (xm == 0x7fff) {
        xm = 0x7ffe;
        xs = UINT64_MAX;
    } else if (outward) {
        if (++xs == 0) {
            xs = integer;
            ++xm;
        } else if (!xm && xs == integer) {
            xm = 1;
        }
    } else if (xs == integer && xm) {
        --xm;
        xs = xm ? UINT64_MAX : integer - 1;
    } else {
        --xs;
    }
    ld_store(out, xs, (xe & 0x8000) | xm);
    if (!xm || xm == 0x7fff) {
        lx_errno = LX_ERANGE;
        feraiseexcept((xm ? FE_OVERFLOW : FE_UNDERFLOW) | FE_INEXACT);
    }
}
static __declspec(naked) void lxm_nextafterl(void)
{
    __asm {
        sub esp, 12
        lea eax, [esp + 16]
        lea ecx, [esp + 28]
        lea edx, [esp]
        push edx
        push ecx
        push eax
        call ld_nextafter
        add esp, 12
        fld tbyte ptr [esp]
        add esp, 12
        ret
    }
}

static int ld_ilogb(const void *x)
{
    uint64_t s = ld_significand(x);
    unsigned e = ld_se(x) & 0x7fff;
    if (!s || e == 0x7fff) {
        lx_errno = LX_EDOM;
        feraiseexcept(FE_INVALID);
        return e == 0x7fff && s == UINT64_C(0x8000000000000000) ? INT_MAX : INT_MIN;
    }
    int exponent = (e ? (int)e : 1) - 16383;
    while (!(s & UINT64_C(0x8000000000000000))) {
        s <<= 1;
        --exponent;
    }
    return exponent;
}
static __declspec(naked) int lxm_ilogbl(void)
{
    __asm {
        lea eax, [esp + 4]
        push eax
        call ld_ilogb
        add esp, 4
        ret
    }
}
static __declspec(naked) void lxm_remquol(void)
{
    __asm {
        sub esp, 20
        fld tbyte ptr [esp + 24]
        fstp qword ptr [esp]
        fld tbyte ptr [esp + 36]
        fstp qword ptr [esp + 8]
        mov eax, [esp + 48]
        mov [esp + 16], eax
        call lxm_remquo
        add esp, 20
        ret
    }
}

/* These rounding operations and fmodl stay in extended precision. */
#define LD_ROUND(name, mode) \
    static __declspec(naked) void lxm_##name##l(void) { \
        __asm { sub esp, 8 } \
        __asm { fnstcw word ptr [esp] } \
        __asm { mov ax, word ptr [esp] } \
        __asm { and ax, 0f3ffh } \
        __asm { or ax, mode } \
        __asm { mov word ptr [esp + 2], ax } \
        __asm { fldcw word ptr [esp + 2] } \
        __asm { fld tbyte ptr [esp + 12] } \
        __asm { frndint } \
        __asm { fldcw word ptr [esp] } \
        __asm { add esp, 8 } \
        __asm { ret } \
    }
LD_ROUND(floor, 0400h)
LD_ROUND(ceil, 0800h)
LD_ROUND(trunc, 0c00h)
static __declspec(naked) void lxm_rintl(void)
{
    __asm { fld tbyte ptr [esp + 4] }
    __asm { frndint }
    __asm { ret }
}
static __declspec(naked) void lxm_nearbyintl(void)
{
    __asm {
        sub esp, 60
        fld tbyte ptr [esp + 64]
        fnstenv [esp]
        mov ax, word ptr [esp]
        mov word ptr [esp + 56], ax
        or word ptr [esp], 0020h
        fldenv [esp]
        frndint
        fnstenv [esp + 28]
        mov ax, word ptr [esp + 56]
        mov word ptr [esp + 28], ax
        mov ax, word ptr [esp + 4]
        and ax, 0020h
        and word ptr [esp + 32], 0ffdfh
        or word ptr [esp + 32], ax
        fldenv [esp + 28]
        add esp, 60
        ret
    }
}
static void ld_round(const void *x, void *out)
{
    uint64_t s = ld_significand(x);
    unsigned se = ld_se(x), exponent = se & 0x7fff;
    int e = (int)exponent - 16383;
    if (e < -1) {
        ld_store(out, 0, se & 0x8000);
    } else if (e == -1) {
        ld_store(out, UINT64_C(0x8000000000000000), (se & 0x8000) | 16383);
    } else if (e < 63) {
        uint64_t mask = UINT64_MAX >> (e + 1), half = (mask >> 1) + 1;
        uint64_t rounded = s & ~mask;
        if (s & half) {
            uint64_t unit = half << 1;
            rounded += unit;
            if (!rounded) {
                rounded = UINT64_C(0x8000000000000000);
                ++se;
            }
        }
        ld_store(out, rounded, se);
    } else {
        ld_store(out, s, se);
    }
}
static __declspec(naked) void lxm_roundl(void)
{
    __asm {
        sub esp, 12
        lea eax, [esp]
        lea ecx, [esp + 16]
        push eax
        push ecx
        call ld_round
        add esp, 8
        fld tbyte ptr [esp]
        add esp, 12
        ret
    }
}
static __declspec(naked) void lxm_lrintl(void)
{
    __asm {
        fld tbyte ptr [esp + 4]
        sub esp, 4
        fistp dword ptr [esp]
        pop eax
        ret
    }
}
static __declspec(naked) void lxm_llrintl(void)
{
    __asm {
        fld tbyte ptr [esp + 4]
        sub esp, 8
        fistp qword ptr [esp]
        pop eax
        pop edx
        ret
    }
}
static __declspec(naked) void lxm_lroundl(void)
{
    __asm {
        mov eax, esp
        push dword ptr [eax + 12]
        push dword ptr [eax + 8]
        push dword ptr [eax + 4]
        call lxm_roundl
        add esp, 12
        sub esp, 4
        fistp dword ptr [esp]
        pop eax
        ret
    }
}
static __declspec(naked) void lxm_llroundl(void)
{
    __asm {
        mov eax, esp
        push dword ptr [eax + 12]
        push dword ptr [eax + 8]
        push dword ptr [eax + 4]
        call lxm_roundl
        add esp, 12
        sub esp, 8
        fistp qword ptr [esp]
        pop eax
        pop edx
        ret
    }
}
static int ld_remainder_check(const void *x, const void *y)
{
    unsigned xe = ld_se(x) & 0x7fff, ye = ld_se(y) & 0x7fff;
    uint64_t xs = ld_significand(x), ys = ld_significand(y);
    int nan = (xe == 0x7fff && xs != UINT64_C(0x8000000000000000)) ||
              (ye == 0x7fff && ys != UINT64_C(0x8000000000000000));
    if (!nan && ((!ye && !ys) || xe == 0x7fff))
        lx_errno = LX_EDOM;
    return nan || xe == 0x7fff || ye == 0x7fff || (!ye && !ys);
}
static __declspec(naked) void lxm_fmodl(void)
{
    __asm {
        lea eax, [esp + 4]
        lea ecx, [esp + 16]
        push ecx
        push eax
        call ld_remainder_check
        add esp, 8
        fld tbyte ptr [esp + 16]
        fld tbyte ptr [esp + 4]
        test eax, eax
        jnz special
    reduce:
        fprem
        fnstsw ax
        test ax, 0400h
        jnz reduce
        jmp result
    special:
        fprem
    result:
        sub esp, 12
        fstp tbyte ptr [esp]
        fstp st(0)
        fld tbyte ptr [esp]
        add esp, 12
        ret
    }
}
static __declspec(naked) void lxm_copysignl(void)
{
    __asm {
        fld tbyte ptr [esp + 4]
        fabs
        test byte ptr [esp + 25], 80h
        jz done
        fchs
    done:
        ret
    }
}
static int ld_choose(const void *x, const void *y, int maximum)
{
    uint64_t xs = ld_significand(x), ys = ld_significand(y);
    unsigned xe = ld_se(x), ye = ld_se(y);
    int xnan = (xe & 0x7fff) == 0x7fff && xs != UINT64_C(0x8000000000000000);
    int ynan = (ye & 0x7fff) == 0x7fff && ys != UINT64_C(0x8000000000000000);
    if (xnan || ynan) {
        if ((xnan && ynan) || (xnan && !(xs & UINT64_C(0x4000000000000000))) ||
            (ynan && !(ys & UINT64_C(0x4000000000000000))))
            return 2; /* addition quiets a signaling NaN and raises invalid */
        return xnan ? 1 : 0;
    }
    /* i686 glibc chooses x for an equal minimum, y for an equal maximum. */
    if (!(xe & 0x7fff) && !(ye & 0x7fff) && !xs && !ys)
        return maximum ? 1 : 0;
    int less;
    if ((xe ^ ye) & 0x8000)
        less = !!(xe & 0x8000);
    else {
        less = (xe & 0x7fff) < (ye & 0x7fff) || ((xe & 0x7fff) == (ye & 0x7fff) && xs < ys);
        if (xe & 0x8000)
            less = !less && (xe != ye || xs != ys);
    }
    return maximum ? less : !less;
}
/* Keep branches and their labels in one MS-asm block. Separate asm statements
 * inside a macro do not share Clang's assembler-label scope reliably. EDX holds
 * the private min/max selector; the guest's argument stack remains untouched. */
static __declspec(naked) void ld_minmax_return(void)
{
    __asm {
        lea eax, [esp + 4]
        lea ecx, [esp + 16]
        push edx
        push ecx
        push eax
        call ld_choose
        add esp, 12
        cmp eax, 2
        je minmax_nan
        test eax, eax
        jnz minmax_second
        fld tbyte ptr [esp + 4]
        ret
    minmax_second:
        fld tbyte ptr [esp + 16]
        ret
    minmax_nan:
        fld tbyte ptr [esp + 4]
        fld tbyte ptr [esp + 16]
        faddp st(1), st(0)
        ret
    }
}
static __declspec(naked) void lxm_fminl(void)
{
    __asm {
        xor edx, edx
        jmp ld_minmax_return
    }
}
static __declspec(naked) void lxm_fmaxl(void)
{
    __asm {
        mov edx, 1
        jmp ld_minmax_return
    }
}
static __declspec(naked) void lxm_fmal(void)
{
    __asm {
        sub esp, 24
        fld tbyte ptr [esp + 28]
        fstp qword ptr [esp]
        fld tbyte ptr [esp + 40]
        fstp qword ptr [esp + 8]
        fld tbyte ptr [esp + 52]
        fstp qword ptr [esp + 16]
        call lxm_fma
        add esp, 24
        ret
    }
}
static __declspec(naked) void lxm_sincosl(void)
{
    __asm {
        sub esp, 24
        fld tbyte ptr [esp + 28]
        fstp qword ptr [esp]
        call lxm_sin
        fstp tbyte ptr [esp + 8]
        call lxm_cos
        mov eax, [esp + 44]
        fstp tbyte ptr [eax]
        fld tbyte ptr [esp + 8]
        mov eax, [esp + 40]
        fstp tbyte ptr [eax]
        add esp, 24
        ret
    }
}
static int ld_classify(const void *x)
{
    uint64_t s = ld_significand(x);
    unsigned e = ld_se(x) & 0x7fff;
    if (e == 0x7fff)
        return s == UINT64_C(0x8000000000000000) ? 1 : 0;
    if (!e)
        return !s ? 2 : s & UINT64_C(0x8000000000000000) ? 4 : 3;
    return s & UINT64_C(0x8000000000000000) ? 4 : 0;
}
static int ld_predicate(const void *x, int which)
{
    int c = ld_classify(x);
    if (!which)
        return c >= 2;
    if (which == 1)
        return c == 0;
    return c == 1 ? (ld_se(x) & 0x8000 ? -1 : 1) : 0;
}
#define LD_PREDICATE(name, which) \
    static __declspec(naked) int lxm_##name##l(void) { \
        __asm { lea eax, [esp + 4] } \
        __asm { push which } \
        __asm { push eax } \
        __asm { call ld_predicate } \
        __asm { add esp, 8 } \
        __asm { ret } \
    }
LD_PREDICATE(finite, 0)
LD_PREDICATE(isnan, 1)
LD_PREDICATE(isinf, 2)
static __declspec(naked) int lxm_fpclassifyl(void)
{
    __asm {
        lea eax, [esp + 4]
        push eax
        call ld_classify
        add esp, 4
        ret
    }
}

static int64_t lxm_divdi3(int64_t a, int64_t b) { return a / b; }
static int64_t lxm_moddi3(int64_t a, int64_t b) { return a % b; }
static uint64_t lxm_udivdi3(uint64_t a, uint64_t b) { return a / b; }
static uint64_t lxm_umoddi3(uint64_t a, uint64_t b) { return a % b; }

/* glibc random()/rand(): TYPE_3 additive generator, degree 31, separation 3,
 * Park-Miller initialization and 310 discarded values. These APIs share state. */
static SRWLOCK random_lock = SRWLOCK_INIT;
static uint32_t random_state[31];
static unsigned random_front, random_rear;
static int random_initialized;
static uint32_t random_next(void)
{
    uint32_t sum = random_state[random_front] + random_state[random_rear];
    random_state[random_front] = sum;
    if (++random_front == 31)
        random_front = 0;
    if (++random_rear == 31)
        random_rear = 0;
    return sum >> 1;
}
static void random_seed(unsigned seed)
{
    if (!seed)
        seed = 1;
    int32_t word = (int32_t)seed;
    random_state[0] = seed;
    for (unsigned i = 1; i < 31; ++i) {
        int64_t next = INT64_C(16807) * (word % 127773) - INT64_C(2836) * (word / 127773);
        if (next < 0)
            next += 2147483647;
        word = (int32_t)next;
        random_state[i] = (uint32_t)word;
    }
    random_front = 3;
    random_rear = 0;
    for (unsigned i = 0; i < 310; ++i)
        random_next();
    random_initialized = 1;
}
static void lxm_srandom(unsigned seed)
{
    AcquireSRWLockExclusive(&random_lock);
    random_seed(seed);
    ReleaseSRWLockExclusive(&random_lock);
}
static long lxm_random(void)
{
    AcquireSRWLockExclusive(&random_lock);
    if (!random_initialized)
        random_seed(1);
    long result = (long)random_next();
    ReleaseSRWLockExclusive(&random_lock);
    return result;
}
static int lxm_rand(void) { return (int)lxm_random(); }
static void lxm_srand(unsigned seed) { lxm_srandom(seed); }
static int lxm_rand_r(unsigned *seed)
{
    uint32_t next = *seed;
    next = next * 1103515245u + 12345u;
    unsigned result = (next >> 16) & 2047;
    next = next * 1103515245u + 12345u;
    result = (result << 10) ^ ((next >> 16) & 1023);
    next = next * 1103515245u + 12345u;
    result = (result << 10) ^ ((next >> 16) & 1023);
    *seed = next;
    return (int)result;
}

/* glibc's unseeded 48-bit state is zero. srand48 installs the usual 0x330e
 * low word; seed48 and srand48 also restore the standard multiplier/addend. */
#define MASK48 UINT64_C(0x0000ffffffffffff)
#define MULT48 UINT64_C(0x00000005deece66d)
static SRWLOCK random48_lock = SRWLOCK_INIT;
static uint64_t random48_state;
static uint64_t random48_a = MULT48;
static unsigned random48_c = 11;
static unsigned short random48_old[3];
static uint64_t unpack48(const unsigned short x[3])
{
    return (uint64_t)x[0] | (uint64_t)x[1] << 16 | (uint64_t)x[2] << 32;
}
static void pack48(unsigned short x[3], uint64_t v)
{
    x[0] = (unsigned short)v;
    x[1] = (unsigned short)(v >> 16);
    x[2] = (unsigned short)(v >> 32);
}
static uint64_t random48_next(unsigned short *x)
{
    AcquireSRWLockExclusive(&random48_lock);
    uint64_t v = x ? unpack48(x) : random48_state;
    v = (v * random48_a + random48_c) & MASK48;
    if (x)
        pack48(x, v);
    else
        random48_state = v;
    ReleaseSRWLockExclusive(&random48_lock);
    return v;
}
static double lxm_drand48(void) { return (double)random48_next(NULL) * 0x1p-48; }
static double lxm_erand48(unsigned short x[3]) { return (double)random48_next(x) * 0x1p-48; }
static long lxm_lrand48(void) { return (long)(random48_next(NULL) >> 17); }
static long lxm_nrand48(unsigned short x[3]) { return (long)(random48_next(x) >> 17); }
static long lxm_mrand48(void) { return (int32_t)(random48_next(NULL) >> 16); }
static long lxm_jrand48(unsigned short x[3]) { return (int32_t)(random48_next(x) >> 16); }
static void lxm_srand48(long seed)
{
    AcquireSRWLockExclusive(&random48_lock);
    random48_state = ((uint64_t)(uint32_t)seed << 16) | 0x330e;
    random48_a = MULT48;
    random48_c = 11;
    ReleaseSRWLockExclusive(&random48_lock);
}
static unsigned short *lxm_seed48(unsigned short x[3])
{
    AcquireSRWLockExclusive(&random48_lock);
    uint64_t seed = unpack48(x);
    pack48(random48_old, random48_state);
    random48_state = seed;
    random48_a = MULT48;
    random48_c = 11;
    ReleaseSRWLockExclusive(&random48_lock);
    return random48_old;
}
static void lxm_lcong48(unsigned short p[7])
{
    AcquireSRWLockExclusive(&random48_lock);
    random48_state = unpack48(p);
    random48_a = unpack48(p + 3);
    random48_c = p[6];
    ReleaseSRWLockExclusive(&random48_lock);
}

#define MATH_EXPORT(name) LX_SYM(#name, lxm_##name), LX_SYM(#name "f", lxm_##name##f), LX_SYM(#name "l", lxm_##name##l)
LX_SYMTAB(math,
    MATH_EXPORT(sin), MATH_EXPORT(cos), MATH_EXPORT(tan),
    MATH_EXPORT(asin), MATH_EXPORT(acos), MATH_EXPORT(atan), MATH_EXPORT(atan2),
    MATH_EXPORT(sinh), MATH_EXPORT(cosh), MATH_EXPORT(tanh),
    MATH_EXPORT(exp), MATH_EXPORT(exp2), MATH_EXPORT(log), MATH_EXPORT(log2), MATH_EXPORT(log10),
    /* Unversioned provider matches both GLIBC_2.0 and GLIBC_2.29 pow imports. */
    MATH_EXPORT(pow), MATH_EXPORT(sqrt), MATH_EXPORT(cbrt), MATH_EXPORT(hypot), MATH_EXPORT(fmod),
    MATH_EXPORT(floor), MATH_EXPORT(ceil), MATH_EXPORT(round), MATH_EXPORT(trunc),
    MATH_EXPORT(lround), MATH_EXPORT(llround), MATH_EXPORT(rint), MATH_EXPORT(lrint), MATH_EXPORT(llrint),
    MATH_EXPORT(nearbyint), MATH_EXPORT(fabs), MATH_EXPORT(frexp), MATH_EXPORT(ldexp), MATH_EXPORT(modf),
    MATH_EXPORT(copysign), MATH_EXPORT(fmin), MATH_EXPORT(fmax), MATH_EXPORT(fma),
    MATH_EXPORT(expm1), MATH_EXPORT(log1p), MATH_EXPORT(erf), MATH_EXPORT(sincos),
    MATH_EXPORT(logb), MATH_EXPORT(ilogb), MATH_EXPORT(nextafter),
    MATH_EXPORT(remainder), MATH_EXPORT(remquo), MATH_EXPORT(fdim),
    LX_SYM("scalbn", lxm_scalbn), LX_SYM("scalbnf", lxm_scalbnf), LX_SYM("scalbnl", lxm_ldexpl),
    LX_SYM("scalbln", lxm_scalbln), LX_SYM("scalblnf", lxm_scalblnf), LX_SYM("scalblnl", lxm_ldexpl),
    MATH_EXPORT(finite), MATH_EXPORT(isinf), MATH_EXPORT(isnan),
    LX_SYM("__finite", lxm_finite), LX_SYM("__finitef", lxm_finitef), LX_SYM("__finitel", lxm_finitel),
    LX_SYM("__isnan", lxm_isnan), LX_SYM("__isnanf", lxm_isnanf), LX_SYM("__isnanl", lxm_isnanl),
    LX_SYM("__isinf", lxm_isinf), LX_SYM("__isinff", lxm_isinff), LX_SYM("__isinfl", lxm_isinfl),
    LX_SYM("__fpclassify", lxm_fpclassify), LX_SYM("__fpclassifyf", lxm_fpclassifyf), LX_SYM("__fpclassifyl", lxm_fpclassifyl),
    LX_SYM("__divdi3", lxm_divdi3), LX_SYM("__moddi3", lxm_moddi3),
    LX_SYM("__udivdi3", lxm_udivdi3), LX_SYM("__umoddi3", lxm_umoddi3),
    LX_SYM("rand", lxm_rand), LX_SYM("srand", lxm_srand), LX_SYM("random", lxm_random),
    LX_SYM("srandom", lxm_srandom), LX_SYM("rand_r", lxm_rand_r),
    LX_SYM("drand48", lxm_drand48), LX_SYM("erand48", lxm_erand48),
    LX_SYM("lrand48", lxm_lrand48), LX_SYM("nrand48", lxm_nrand48),
    LX_SYM("mrand48", lxm_mrand48), LX_SYM("jrand48", lxm_jrand48),
    LX_SYM("srand48", lxm_srand48), LX_SYM("seed48", lxm_seed48), LX_SYM("lcong48", lxm_lcong48)
);
