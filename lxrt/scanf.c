/* Guest i386 scanf: byte-stream lexing and explicitly sized guest destinations.
 * No guest va_list, FILE, wchar_t or long double is passed to the host CRT.
 * Supported: integer/floating/string/scanset/character/%n/%p/%% conversions,
 * all glibc i386 length modifiers, positional destinations, and %m allocation.
 * Deliberate limits: locale grouping and alternative digits are not recognized;
 * narrow scanf field widths count input bytes, and malformed formats stop at the
 * directive rather than attempting glibc's undefined extensions. */
#include "lxstdio.h"
#include "lxlocale.h"
#include <limits.h>
#include <string.h>

#define SC_EOF (-1)
#define SC_UNLIMITED ((size_t)-1)
enum sc_length { SC_INT, SC_HH, SC_H, SC_L, SC_LL, SC_LD, SC_J, SC_Z, SC_T };
enum sc_result { SC_MATCH = 0, SC_OK = 1, SC_INPUT = -1, SC_MEMORY = -2 };
typedef struct sc_input {
    lx_FILE *file;
    const unsigned char *string;
    size_t count;
    int look, pending;
} sc_input;

static int sc_peek(sc_input *s)
{
    if (!s->pending) {
        s->look = s->file ? lx_fgetc_unlocked(s->file) : (*s->string ? *s->string : SC_EOF);
        s->pending = 1;
    }
    return s->look;
}
static int sc_get(sc_input *s)
{
    int c = sc_peek(s);
    if (c != SC_EOF) {
        s->pending = 0;
        ++s->count;
        if (!s->file) ++s->string;
    }
    return c;
}
static int sc_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int sc_lower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int sc_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = sc_lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int sc_wpeek(sc_input *s, size_t left) { return left ? sc_peek(s) : -2; }
static int sc_take(sc_input *s, size_t *left)
{
    int c = sc_get(s);
    if (c >= 0 && *left != SC_UNLIMITED) --*left;
    return c;
}
static void sc_integer_store(void *dst, enum sc_length len, uint64_t n)
{
    switch (len) {
    case SC_HH: *(unsigned char *)dst = (unsigned char)n; break;
    case SC_H: *(uint16_t *)dst = (uint16_t)n; break;
    case SC_LL: case SC_LD: case SC_J: *(uint64_t *)dst = n; break;
    default: *(uint32_t *)dst = (uint32_t)n; break;
    }
}
static int sc_integer(sc_input *s, size_t left, int conversion, enum sc_length len, void *dst)
{
    uint64_t n = 0, limit;
    int c, digit, negative = 0, sign = 0, any = 0, overflow = 0;
    int is_signed = conversion == 'd' || conversion == 'i';
    int bits = len == SC_LL || len == SC_LD || len == SC_J ? 64 : 32;
    int base = conversion == 'o' ? 8 : conversion == 'x' || conversion == 'X' || conversion == 'p' ? 16 : conversion == 'i' ? 0 : 10;
    if (sc_peek(s) == SC_EOF) return SC_INPUT;
    c = sc_wpeek(s, left);
    if (c == '-' || c == '+') {
        sign = 1;
        negative = c == '-';
        sc_take(s, &left);
        c = sc_wpeek(s, left);
    }
    if (conversion == 'p' && !sign && c == '(' && (left == SC_UNLIMITED || left >= 5)) {
        static const char nil[] = "(nil)";
        unsigned i;
        for (i = 0; i != 5; ++i) {
            if (sc_lower(sc_wpeek(s, left)) != nil[i]) return SC_MATCH;
            sc_take(s, &left);
        }
        if (dst) *(uint32_t *)dst = 0;
        return SC_OK;
    }
    if (c == '0') {
        any = 1;
        sc_take(s, &left);
        c = sc_wpeek(s, left);
        if (sc_lower(c) == 'x' && (!base || base == 16)) {
            base = 16;
            sc_take(s, &left);
        } else if (!base) base = 8;
    }
    if (!base) base = 10;
    limit = bits == 64 ? UINT64_MAX : UINT32_MAX;
    if (is_signed) limit = (limit >> 1) + (unsigned)negative;
    while ((digit = sc_digit(sc_wpeek(s, left))) >= 0 && digit < base) {
        any = 1;
        sc_take(s, &left);
        if (n > (limit - (unsigned)digit) / (unsigned)base) overflow = 1;
        else if (!overflow) n = n * (unsigned)base + (unsigned)digit;
    }
    if (!any) return SC_MATCH;
    if (overflow) {
        lx_set_errno(LX_ERANGE);
        n = limit;
    }
    if (negative && (!overflow || is_signed)) n = 0 - n;
    if (dst) sc_integer_store(dst, conversion == 'p' ? SC_INT : len, n);
    return SC_OK;
}

/* Base-2^32 integers keep decimal conversion exact, including x87's 64-bit
 * significand. The common short-input path stays entirely on the stack. */
typedef struct sc_big {
    uint32_t *v;
    size_t n, capacity;
    uint32_t local[8];
} sc_big;
static void sc_big_init(sc_big *a)
{
    a->v = a->local;
    a->n = 0;
    a->capacity = LX_COUNT(a->local);
}
static void sc_big_free(sc_big *a) { if (a->v != a->local) lx_free(a->v); }
static int sc_big_reserve(sc_big *a, size_t n)
{
    size_t capacity;
    uint32_t *p;
    if (n <= a->capacity) return 1;
    if (n > SIZE_MAX / sizeof(*p)) { lx_set_errno(LX_ENOMEM); return 0; }
    capacity = a->capacity;
    while (capacity < n) {
        if (capacity > SIZE_MAX / (2 * sizeof(*p))) { capacity = n; break; }
        capacity *= 2;
    }
    p = (uint32_t *)lx_malloc(capacity * sizeof(*p));
    if (!p) return 0;
    memcpy(p, a->v, a->n * sizeof(*p));
    sc_big_free(a);
    a->v = p;
    a->capacity = capacity;
    return 1;
}
static int sc_big_muladd(sc_big *a, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    size_t i;
    if (!sc_big_reserve(a, a->n + 1)) return 0;
    for (i = 0; i < a->n; ++i) {
        carry += (uint64_t)a->v[i] * m;
        a->v[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (carry) a->v[a->n++] = (uint32_t)carry;
    return 1;
}
static int sc_big_copy(sc_big *dst, const sc_big *src)
{
    if (!sc_big_reserve(dst, src->n)) return 0;
    memcpy(dst->v, src->v, src->n * sizeof(*src->v));
    dst->n = src->n;
    return 1;
}
static int64_t sc_big_bits(const sc_big *a)
{
    uint32_t top;
    unsigned bits = 0;
    if (!a->n) return 0;
    top = a->v[a->n - 1];
    while (top) { ++bits; top >>= 1; }
    return (int64_t)(a->n - 1) * 32 + bits;
}
static int sc_big_shift(sc_big *a, int64_t bits)
{
    size_t words = (size_t)(bits / 32), i, old = a->n;
    unsigned small = (unsigned)bits % 32;
    uint32_t carry = 0;
    if (!old || !bits) return 1;
    if (words > SIZE_MAX - old - 1 || !sc_big_reserve(a, old + words + 1)) return 0;
    memmove(a->v + words, a->v, old * sizeof(*a->v));
    memset(a->v, 0, words * sizeof(*a->v));
    for (i = words; i < old + words; ++i) {
        uint32_t next = small ? a->v[i] >> (32 - small) : 0;
        a->v[i] = (a->v[i] << small) | carry;
        carry = next;
    }
    a->n = old + words;
    if (carry) a->v[a->n++] = carry;
    return 1;
}
static void sc_big_shr1(sc_big *a)
{
    size_t i = a->n;
    uint32_t carry = 0;
    while (i) {
        uint32_t next = a->v[--i] << 31;
        a->v[i] = (a->v[i] >> 1) | carry;
        carry = next;
    }
    if (a->n && !a->v[a->n - 1]) --a->n;
}
static int sc_big_cmp(const sc_big *a, const sc_big *b)
{
    size_t i = a->n;
    if (i != b->n) return i > b->n ? 1 : -1;
    while (i) {
        --i;
        if (a->v[i] != b->v[i]) return a->v[i] > b->v[i] ? 1 : -1;
    }
    return 0;
}
static void sc_big_sub(sc_big *a, const sc_big *b)
{
    uint64_t borrow = 0;
    size_t i;
    for (i = 0; i < a->n; ++i) {
        uint64_t sub = (i < b->n ? b->v[i] : 0) + borrow;
        uint32_t old = a->v[i];
        a->v[i] = old - (uint32_t)sub;
        borrow = (uint64_t)old < sub;
    }
    while (a->n && !a->v[a->n - 1]) --a->n;
}
static int sc_big_pow5(sc_big *a, int64_t n)
{
    while (n >= 13) { if (!sc_big_muladd(a, 1220703125u, 0)) return 0; n -= 13; }
    while (n--) if (!sc_big_muladd(a, 5, 0)) return 0;
    return 1;
}

static void sc_float_store(void *dst, enum sc_length len, int negative, int exponent, uint64_t significand)
{
    if (!dst) return;
    if (len == SC_LD || len == SC_LL) {
        unsigned char extended[10];
        uint16_t sign_exp = (uint16_t)((negative ? 0x8000 : 0) | exponent);
        memcpy(extended, &significand, 8);
        memcpy(extended + 8, &sign_exp, 2);
        /* The guest's two padding bytes are not part of the x87 value. */
        __asm__ volatile("fldt %0\n\tfstpt (%1)" : : "m"(extended), "r"(dst) : "memory", "st");
    } else if (len == SC_L) {
        uint64_t bits = ((uint64_t)negative << 63) | ((uint64_t)exponent << 52)
            | (significand & UINT64_C(0x000fffffffffffff));
        memcpy(dst, &bits, 8);
    } else {
        uint32_t bits = ((uint32_t)negative << 31) | ((uint32_t)exponent << 23)
            | ((uint32_t)significand & 0x7fffffu);
        memcpy(dst, &bits, 4);
    }
}
static void sc_float_special(void *dst, enum sc_length len, int negative, int nan)
{
    int ext = len == SC_LD || len == SC_LL;
    int precision = ext ? 64 : len == SC_L ? 53 : 24;
    uint64_t sig = UINT64_C(1) << (precision - 1);
    if (nan) sig |= sig >> 1;
    sc_float_store(dst, len, negative, ext ? 0x7fff : len == SC_L ? 0x7ff : 0xff, sig);
}
static unsigned sc_rounding(void)
{
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return (cw >> 10) & 3;
}
static void sc_float_overflow(void *dst, enum sc_length len, int negative, unsigned rounding)
{
    int ext = len == SC_LD || len == SC_LL;
    int max = ext ? 0x7fff : len == SC_L ? 0x7ff : 0xff;
    int p = ext ? 64 : len == SC_L ? 53 : 24;
    int infinity = rounding == 0 || (rounding == 1 && negative) || (rounding == 2 && !negative);
    uint64_t sig = infinity ? UINT64_C(1) << (p - 1) : p == 64 ? UINT64_MAX : (UINT64_C(1) << p) - 1;
    sc_float_store(dst, len, negative, infinity ? max : max - 1, sig);
    lx_set_errno(LX_ERANGE);
}
/* Convert an exact rational N / D times 2^binary_scale. */
static int sc_float_convert(sc_big *n, int64_t scale, int hexadecimal, int64_t digits,
                            enum sc_length len, int negative, void *dst)
{
    sc_big d, t;
    int ext = len == SC_LD || len == SC_LL;
    int p = ext ? 64 : len == SC_L ? 53 : 24;
    int bias = ext ? 16383 : len == SC_L ? 1023 : 127;
    int emin = 1 - bias, emax = bias;
    int64_t binary_scale, exponent, shift, delta;
    uint64_t q = 0;
    int compare, inexact, round_up, subnormal;
    unsigned rounding = sc_rounding();
    if (!n->n) {
        sc_float_store(dst, len, negative, 0, 0);
        return SC_OK;
    }
    /* A decimal exponent this far outside the useful range cannot be
     * rescued by a finite input field.  This bounds the 5^N workspace. */
    if (!hexadecimal && (scale + digits > 10000 || scale + digits < -10000)) {
        if (scale + digits > 10000)
            sc_float_overflow(dst, len, negative, rounding);
        else {
            sc_float_store(dst, len, negative, 0,
                (rounding == 1 && negative) || (rounding == 2 && !negative));
            lx_set_errno(LX_ERANGE);
        }
        return SC_OK;
    }
    if (hexadecimal && (scale + sc_big_bits(n) > 20000 || scale + sc_big_bits(n) < -20000)) {
        if (scale + sc_big_bits(n) > 20000)
            sc_float_overflow(dst, len, negative, rounding);
        else {
            sc_float_store(dst, len, negative, 0,
                (rounding == 1 && negative) || (rounding == 2 && !negative));
            lx_set_errno(LX_ERANGE);
        }
        return SC_OK;
    }
    sc_big_init(&d);
    sc_big_init(&t);
    if (!sc_big_muladd(&d, 1, 1))
        goto memory;
    binary_scale = hexadecimal ? scale : 0;
    if (!hexadecimal) {
        if (scale >= 0) {
            if (!sc_big_pow5(n, scale))
                goto memory;
            binary_scale = scale;
        } else {
            if (!sc_big_pow5(&d, -scale))
                goto memory;
            binary_scale = scale;
        }
    }
    delta = sc_big_bits(n) - sc_big_bits(&d);
    if (delta >= 0) {
        if (!sc_big_copy(&t, &d) || !sc_big_shift(&t, delta))
            goto memory;
        compare = sc_big_cmp(n, &t);
    } else {
        if (!sc_big_copy(&t, n) || !sc_big_shift(&t, -delta))
            goto memory;
        compare = sc_big_cmp(&t, &d);
    }
    exponent = delta + binary_scale - (compare < 0);
    if (exponent > emax) {
        sc_float_overflow(dst, len, negative, rounding);
        goto complete;
    }
    subnormal = exponent < emin;
    if (subnormal)
        exponent = emin;
    shift = binary_scale + p - 1 - exponent;
    if (shift >= 0) {
        if (!sc_big_shift(n, shift))
            goto memory;
    } else if (!sc_big_shift(&d, -shift))
        goto memory;
    /* Divide by repeated aligned subtraction.  The selected exponent keeps
     * this loop to at most the destination precision plus one bit. */
    delta = sc_big_bits(n) - sc_big_bits(&d);
    if (delta >= 0) {
        if (!sc_big_copy(&t, &d) || !sc_big_shift(&t, delta))
            goto memory;
        while (delta >= 0) {
            if (sc_big_cmp(n, &t) >= 0) {
                sc_big_sub(n, &t);
                if (delta < 64)
                    q |= UINT64_C(1) << (unsigned)delta;
            }
            --delta;
            sc_big_shr1(&t);
        }
    }
    inexact = n->n != 0;
    if (!sc_big_shift(n, 1))
        goto memory;
    compare = sc_big_cmp(n, &d);
    round_up = rounding == 0
        ? compare > 0 || (compare == 0 && (q & 1))
        : inexact && ((rounding == 1 && negative) || (rounding == 2 && !negative));
    if (round_up) {
        ++q;
        if (!q || (p != 64 && q == (UINT64_C(1) << p))) {
            q = UINT64_C(1) << (p - 1);
            ++exponent;
            subnormal = 0;
        }
    }
    if (exponent > emax)
        sc_float_overflow(dst, len, negative, rounding);
    else {
        if (q >= (UINT64_C(1) << (p - 1)))
            subnormal = 0;
        sc_float_store(dst, len, negative, subnormal ? 0 : (int)exponent + bias, q);
        if (inexact && subnormal)
            lx_set_errno(LX_ERANGE);
    }
complete:
    sc_big_free(&d);
    sc_big_free(&t);
    return SC_OK;
memory:
    sc_big_free(&d);
    sc_big_free(&t);
    lx_set_errno(LX_ENOMEM);
    return SC_MEMORY;
}
static int sc_float_word(sc_input *s, size_t *left, const char *word)
{
    while (*word) {
        if (!*left || sc_lower(sc_take(s, left)) != *word++) return 0;
    }
    return 1;
}
static int sc_float(sc_input *s, size_t left, enum sc_length len, void *dst)
{
    sc_big number;
    int negative = 0, hexadecimal = 0, point = 0, any = 0, c, digit, result;
    int64_t fractional = 0, digits = 0, exponent = 0;
    int exponent_negative = 0;
    if (sc_peek(s) == SC_EOF) return SC_INPUT;
    c = sc_wpeek(s, left);
    if (c == '+' || c == '-') {
        negative = c == '-';
        sc_take(s, &left);
    }
    c = sc_lower(sc_wpeek(s, left));
    if (c == 'n' || c == 'i') {
        if (!sc_float_word(s, &left, c == 'n' ? "nan" : "inf")) return SC_MATCH;
        if (c == 'i' && sc_lower(sc_wpeek(s, left)) == 'i'
            && !sc_float_word(s, &left, "inity")) return SC_MATCH;
        sc_float_special(dst, len, negative, c == 'n');
        return SC_OK;
    }
    sc_big_init(&number);
    if (sc_wpeek(s, left) == '0') {
        sc_take(s, &left);
        any = 1;
        /* glibc requires one character beyond 0x to recognize a
         * hexadecimal float; a width of two therefore scans just 0. */
        if (sc_lower(sc_wpeek(s, left)) == 'x'
            && (left == SC_UNLIMITED || left > 1)) {
            sc_take(s, &left);
            hexadecimal = 1;
            any = 0;
        }
    }
    for (;;) {
        c = sc_wpeek(s, left);
        digit = sc_digit(c);
        if (digit >= 0 && digit < (hexadecimal ? 16 : 10)) {
            sc_take(s, &left);
            any = 1;
            if (point) ++fractional;
            if (number.n || digit) {
                ++digits;
                if (!sc_big_muladd(&number, hexadecimal ? 16 : 10, (unsigned)digit)) {
                    sc_big_free(&number);
                    return SC_MEMORY;
                }
            }
        } else if (!point && c == '.') {
            sc_take(s, &left);
            point = 1;
        } else break;
    }
    if (any && sc_lower(sc_wpeek(s, left)) == (hexadecimal ? 'p' : 'e')) {
        sc_take(s, &left);
        c = sc_wpeek(s, left);
        if (c == '-' || c == '+') {
            exponent_negative = c == '-';
            sc_take(s, &left);
        }
        while ((c = sc_wpeek(s, left)) >= '0' && c <= '9') {
            sc_take(s, &left);
            if (exponent < INT64_C(10000000000)) exponent = exponent * 10 + c - '0';
        }
    }
    if (!any && !(hexadecimal && point)) result = SC_MATCH;
    else {
        if (exponent_negative) exponent = -exponent;
        exponent -= fractional * (hexadecimal ? 4 : 1);
        result = sc_float_convert(&number, exponent, hexadecimal, digits, len, negative, dst);
    }
    sc_big_free(&number);
    return result;
}

/* Allocation records are needed because glibc frees all %m results if an
 * allocation failure makes the whole call return EOF, even after assignments. */
typedef struct sc_allocation {
    struct sc_allocation *next;
    void **slot;
} sc_allocation;
typedef struct sc_output {
    unsigned char *data;
    size_t used, capacity, unit;
    int allocate, suppress;
} sc_output;
static int sc_output_put(sc_output *o, uint32_t c)
{
    if (o->suppress) return 1;
    if (o->allocate && o->used == o->capacity) {
        size_t capacity = o->capacity ? o->capacity * 2 : 32;
        void *p;
        if (capacity < o->capacity || capacity > SIZE_MAX / o->unit) { lx_set_errno(LX_ENOMEM); return 0; }
        p = lx_realloc(o->data, capacity * o->unit);
        if (!p) return 0;
        o->data = (unsigned char *)p;
        o->capacity = capacity;
    }
    if (o->unit == 4) ((uint32_t *)o->data)[o->used] = c;
    else o->data[o->used] = (unsigned char)c;
    ++o->used;
    return 1;
}
static int sc_string(sc_input *s, size_t left, int conversion, int wide, int allocate,
                     void *dst, const unsigned char set[256], int invert, sc_allocation **allocations)
{
    sc_output out;
    lx_mbstate state = { 0, 0 };
    size_t matched = 0;
    int result = SC_OK;
    out.data = allocate ? NULL : (unsigned char *)dst;
    out.used = out.capacity = 0;
    out.unit = wide ? 4 : 1;
    out.allocate = allocate;
    out.suppress = dst == NULL;
    if (allocate && dst) *(void **)dst = NULL;
    if (sc_peek(s) == SC_EOF) return SC_INPUT;
    while (left) {
        int c = sc_peek(s);
        uint32_t wc;
        size_t converted;
        if (c == SC_EOF) break;
        if ((conversion == 's' && sc_space(c)) || (conversion == '[' && set[c] == invert)) break;
        sc_take(s, &left);
        if (!wide) {
            if (!sc_output_put(&out, (uint32_t)c)) { result = SC_MEMORY; goto finish; }
            ++matched;
            continue;
        }
        {
            char byte = (char)c;
            converted = lx_mbrtowc(&wc, &byte, 1, &state);
        }
        /* Narrow scanf's %l[ applies the scanset and width to input bytes;
         * %ls and %lc count complete wide characters instead. */
        if (converted == (size_t)-1) { result = SC_MATCH; goto finish; }
        if (converted == (size_t)-2) {
            if (conversion != '[' && left != SC_UNLIMITED) ++left;
            if (!left || sc_peek(s) == SC_EOF) { lx_set_errno(LX_EILSEQ); result = SC_MATCH; goto finish; }
            continue;
        }
        if (!sc_output_put(&out, wc)) { result = SC_MEMORY; goto finish; }
        ++matched;
    }
    if (!matched) { result = SC_MATCH; goto finish; }
    if (state.count) { lx_set_errno(LX_EILSEQ); result = SC_MATCH; goto finish; }
    if (conversion != 'c' && !sc_output_put(&out, 0)) { result = SC_MEMORY; goto finish; }
    if (allocate && dst) {
        sc_allocation *record = (sc_allocation *)lx_malloc(sizeof(*record));
        if (!record) { result = SC_MEMORY; goto finish; }
        record->slot = (void **)dst;
        record->next = *allocations;
        *allocations = record;
        *(void **)dst = out.data;
        out.data = NULL;
    }
finish:
    if (allocate) lx_free(out.data);
    if (result == SC_MEMORY) lx_set_errno(LX_ENOMEM);
    return result;
}
static void *sc_argument(char **current, const char *base, unsigned position)
{
    void *p;
    const char *arg = position ? base + (position - 1) * 4 : *current;
    memcpy(&p, arg, 4);
    if (!position) *current += 4;
    return p;
}
static size_t sc_width(const unsigned char **format)
{
    size_t n = 0;
    while (**format >= '0' && **format <= '9') {
        unsigned digit = *(*format)++ - '0';
        n = n > (INT_MAX - digit) / 10u ? INT_MAX : n * 10 + digit;
    }
    return n;
}
static int sc_scan(sc_input *s, const char *format, va_list ap, int isoc99)
{
    const unsigned char *f = (const unsigned char *)format;
    char *arguments = (char *)ap;
    const char *base = arguments;
    sc_allocation *allocations = NULL;
    int count = 0, memory_failure = 0;
    while (*f) {
        enum sc_length length = SC_INT;
        size_t width = SC_UNLIMITED;
        unsigned position = 0;
        int suppress = 0, allocate = 0, conversion, result, invert = 0;
        unsigned char set[256];
        void *dst = NULL;
        if (sc_space(*f)) {
            do ++f; while (sc_space(*f));
            while (sc_space(sc_peek(s))) sc_get(s);
            continue;
        }
        if (*f++ != '%') {
            int c = sc_peek(s);
            if (c == SC_EOF) { if (!count) count = SC_EOF; break; }
            if (c != f[-1]) break;
            sc_get(s);
            continue;
        }
        if (*f >= '0' && *f <= '9') {
            size_t n = sc_width(&f);
            if (*f == '$') { position = (unsigned)n; ++f; }
            else width = n ? n : SC_UNLIMITED;
        }
        if (*f == '*') { suppress = 1; ++f; }
        if (*f >= '0' && *f <= '9') { width = sc_width(&f); if (!width) width = SC_UNLIMITED; }
        if (*f == 'm' || (!isoc99 && *f == 'a' && (f[1] == 's' || f[1] == 'S' || f[1] == '['))) {
            allocate = 1;
            ++f;
        }
        switch (*f) {
        case 'h': ++f; length = *f == 'h' ? (++f, SC_HH) : SC_H; break;
        case 'l': ++f; length = *f == 'l' ? (++f, SC_LL) : SC_L; break;
        case 'L': case 'q': ++f; length = SC_LD; break;
        case 'j': ++f; length = SC_J; break;
        case 'z': ++f; length = SC_Z; break;
        case 't': ++f; length = SC_T; break;
        default: break;
        }
        conversion = *f++;
        if (!conversion) break;
        if (conversion == 'C' || conversion == 'S') { conversion += 'a' - 'A'; length = SC_L; }
        if (conversion == '[') {
            int previous = -1;
            memset(set, 0, sizeof(set));
            if (*f == '^') { invert = 1; ++f; }
            if (*f == ']' || *f == '-') { previous = *f; set[*f++] = 1; }
            while (*f && *f != ']') {
                int c = *f++;
                if (c == '-' && previous >= 0 && *f && *f != ']' && previous <= *f) {
                    int last = *f;
                    while (previous < last) set[previous++] = 1;
                    set[last] = 1;
                    previous = last;
                } else { set[c] = 1; previous = c; }
            }
            if (*f != ']') break;
            ++f;
        }
        if (conversion != '[' && conversion != 'c' && conversion != 'n')
            while (sc_space(sc_peek(s))) sc_get(s);
        if (conversion == '%') {
            int c = sc_peek(s);
            if (c == SC_EOF) { if (!count) count = SC_EOF; break; }
            if (c != '%') break;
            sc_get(s);
            continue;
        }
        if (!strchr("diuoxXfFeEgGaAscnp[", conversion)) break;
        if (!suppress) dst = sc_argument(&arguments, base, position);
        if (conversion == 'n') {
            if (!suppress) sc_integer_store(dst, length, s->count);
            continue;
        }
        if (strchr("diuoxXp", conversion)) result = sc_integer(s, width, conversion, length, dst);
        else if (strchr("fFeEgGaA", conversion)) result = sc_float(s, width, length, dst);
        else {
            if (conversion == 'c' && width == SC_UNLIMITED) width = 1;
            result = sc_string(s, width, conversion, length == SC_L, allocate, dst, set, invert, &allocations);
        }
        if (result != SC_OK) {
            if (result == SC_INPUT && !count) count = SC_EOF;
            if (result == SC_MEMORY) { count = SC_EOF; memory_failure = 1; }
            break;
        }
        if (!suppress) ++count;
    }
    while (allocations) {
        sc_allocation *next = allocations->next;
        if (memory_failure) { lx_free(*allocations->slot); *allocations->slot = NULL; }
        lx_free(allocations);
        allocations = next;
    }
    if (s->file && s->pending && s->look != SC_EOF) lx_ungetc_unlocked(s->look, s->file);
    return count;
}
static int sc_vsscanf(const char *s, const char *format, va_list ap, int isoc99)
{
    sc_input input = { NULL, (const unsigned char *)s, 0, 0, 0 };
    return sc_scan(&input, format, ap, isoc99);
}
static int sc_vfscanf(lx_FILE *file, const char *format, va_list ap, int isoc99)
{
    sc_input input = { file, NULL, 0, 0, 0 };
    int result;
    lx_flockfile(file);
    result = sc_scan(&input, format, ap, isoc99);
    lx_funlockfile(file);
    return result;
}
static int vsscanf_(const char *s, const char *format, va_list ap) { return sc_vsscanf(s, format, ap, 0); }
static int vfscanf_(lx_FILE *file, const char *format, va_list ap) { return sc_vfscanf(file, format, ap, 0); }
static int vscanf_(const char *format, va_list ap) { return sc_vfscanf(lx_stdin, format, ap, 0); }
static int isoc99_vsscanf_(const char *s, const char *format, va_list ap) { return sc_vsscanf(s, format, ap, 1); }
static int isoc99_vfscanf_(lx_FILE *file, const char *format, va_list ap) { return sc_vfscanf(file, format, ap, 1); }
static int isoc99_vscanf_(const char *format, va_list ap) { return sc_vfscanf(lx_stdin, format, ap, 1); }
#define SC_STRING_WRAPPER(name, mode) \
    static int name(const char *s, const char *format, ...) { \
        va_list ap; int result; va_start(ap, format); \
        result = sc_vsscanf(s, format, ap, mode); va_end(ap); return result; }
#define SC_FILE_WRAPPER(name, mode) \
    static int name(lx_FILE *file, const char *format, ...) { \
        va_list ap; int result; va_start(ap, format); \
        result = sc_vfscanf(file, format, ap, mode); va_end(ap); return result; }
#define SC_STDIN_WRAPPER(name, mode) \
    static int name(const char *format, ...) { \
        va_list ap; int result; va_start(ap, format); \
        result = sc_vfscanf(lx_stdin, format, ap, mode); va_end(ap); return result; }
SC_STRING_WRAPPER(sscanf_, 0)
SC_STRING_WRAPPER(isoc99_sscanf_, 1)
SC_FILE_WRAPPER(fscanf_, 0)
SC_FILE_WRAPPER(isoc99_fscanf_, 1)
SC_STDIN_WRAPPER(scanf_, 0)
SC_STDIN_WRAPPER(isoc99_scanf_, 1)
LX_SYMTAB(scanf,
    LX_SYM("sscanf", sscanf_), LX_SYM("vsscanf", vsscanf_),
    LX_SYM("fscanf", fscanf_), LX_SYM("vfscanf", vfscanf_),
    LX_SYM("scanf", scanf_), LX_SYM("vscanf", vscanf_),
    LX_SYM("__isoc99_sscanf", isoc99_sscanf_), LX_SYM("__isoc99_vsscanf", isoc99_vsscanf_),
    LX_SYM("__isoc99_fscanf", isoc99_fscanf_), LX_SYM("__isoc99_vfscanf", isoc99_vfscanf_),
    LX_SYM("__isoc99_scanf", isoc99_scanf_), LX_SYM("__isoc99_vscanf", isoc99_vscanf_)
);
