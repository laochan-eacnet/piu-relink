/* glibc i386 ctype tables, byte predicates and 32-bit wide-character ctype.
 * C and C.UTF-8 share the byte tables. Wide C is ASCII; UTF-8 uses Windows
 * Unicode data with linguistic (not filesystem) casing, explicit Latin-1,
 * and POSIX whitespace/simple-case rules. iswdigit always returns 0 or 1.
 * Unicode versions (especially supplementary characters) can differ from glibc. */
#include "lxlocale.h"
#include <string.h>

enum {
    C_UPPER = 0x0100, C_LOWER = 0x0200, C_ALPHA = 0x0400,
    C_DIGIT = 0x0800, C_XDIGIT = 0x1000, C_SPACE = 0x2000,
    C_PRINT = 0x4000, C_GRAPH = 0x8000, C_BLANK = 1,
    C_CNTRL = 2, C_PUNCT = 4, C_ALNUM = 8
};

#define IN_RANGE(c, a, b) ((c) >= (a) && (c) <= (b))
#define ASCII_ALPHA(c) (IN_RANGE(c, 'A', 'Z') || IN_RANGE(c, 'a', 'z'))
#define ASCII_DIGIT(c) IN_RANGE(c, '0', '9')
#define ASCII_BITS(c) ( \
    (IN_RANGE(c, 'A', 'Z') ? C_UPPER : 0) | \
    (IN_RANGE(c, 'a', 'z') ? C_LOWER : 0) | \
    (ASCII_ALPHA(c) ? C_ALPHA : 0) | \
    (ASCII_DIGIT(c) ? C_DIGIT : 0) | \
    ((ASCII_DIGIT(c) || IN_RANGE(c, 'A', 'F') || IN_RANGE(c, 'a', 'f')) ? C_XDIGIT : 0) | \
    (((c) == ' ' || IN_RANGE(c, '\t', '\r')) ? C_SPACE : 0) | \
    (IN_RANGE(c, 32, 126) ? C_PRINT : 0) | \
    (IN_RANGE(c, 33, 126) ? C_GRAPH : 0) | \
    (((c) == ' ' || (c) == '\t') ? C_BLANK : 0) | \
    (((c) < 32 || (c) == 127) ? C_CNTRL : 0) | \
    ((IN_RANGE(c, 33, 126) && !ASCII_ALPHA(c) && !ASCII_DIGIT(c)) ? C_PUNCT : 0) | \
    ((ASCII_ALPHA(c) || ASCII_DIGIT(c)) ? C_ALNUM : 0))
#define ID(c) (c)
#define ASCII_LOWER(c) (IN_RANGE(c, 'A', 'Z') ? (c) + 32 : (c))
#define ASCII_UPPER(c) (IN_RANGE(c, 'a', 'z') ? (c) - 32 : (c))
#define ROW16(M, n) \
    M((n) + 0), M((n) + 1), M((n) + 2), M((n) + 3), \
    M((n) + 4), M((n) + 5), M((n) + 6), M((n) + 7), \
    M((n) + 8), M((n) + 9), M((n) + 10), M((n) + 11), \
    M((n) + 12), M((n) + 13), M((n) + 14), M((n) + 15)
#define ZERO16 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
#define ZERO128 ZERO16, ZERO16, ZERO16, ZERO16, ZERO16, ZERO16, ZERO16, ZERO16
#define ASCII_ROWS(M) \
    ROW16(M, 0), ROW16(M, 16), ROW16(M, 32), ROW16(M, 48), \
    ROW16(M, 64), ROW16(M, 80), ROW16(M, 96), ROW16(M, 112)
#define HIGH_ROWS \
    ROW16(ID, 128), ROW16(ID, 144), ROW16(ID, 160), ROW16(ID, 176), \
    ROW16(ID, 192), ROW16(ID, 208), ROW16(ID, 224), ROW16(ID, 240)
/* Signed-char compatibility maps -128..-2 to 128..254. -1 remains EOF. */
#define NEGATIVE_ROWS \
    ROW16(ID, 128), ROW16(ID, 144), ROW16(ID, 160), ROW16(ID, 176), \
    ROW16(ID, 192), ROW16(ID, 208), ROW16(ID, 224), \
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, -1

static const uint16_t ctype_bits[384] = { ZERO128, ASCII_ROWS(ASCII_BITS), ZERO128 };
static const int32_t ctype_lower[384] = { NEGATIVE_ROWS, ASCII_ROWS(ASCII_LOWER), HIGH_ROWS };
static const int32_t ctype_upper[384] = { NEGATIVE_ROWS, ASCII_ROWS(ASCII_UPPER), HIGH_ROWS };
static const uint16_t *ctype_b = ctype_bits + 128;
static const int32_t *ctype_lo = ctype_lower + 128;
static const int32_t *ctype_up = ctype_upper + 128;

void lx_ctype_tables(const uint16_t **b, const int32_t **lo, const int32_t **up)
{
    if (b) *b = ctype_bits + 128;
    if (lo) *lo = ctype_lower + 128;
    if (up) *up = ctype_upper + 128;
}

static const uint16_t **ctype_b_loc_(void) { return &ctype_b; }
static const int32_t **ctype_tolower_loc_(void) { return &ctype_lo; }
static const int32_t **ctype_toupper_loc_(void) { return &ctype_up; }
static size_t ctype_get_mb_cur_max_(void) { return lx_locale_utf8(NULL) ? 6 : 1; }

static unsigned byte_bits(int c)
{
    return c >= -128 && c <= 255 ? ctype_bits[c + 128] : 0;
}

#define CHAR_CLASSES(X) \
    X(alnum, C_ALNUM) X(alpha, C_ALPHA) X(blank, C_BLANK) \
    X(cntrl, C_CNTRL) X(digit, C_DIGIT) X(graph, C_GRAPH) \
    X(lower, C_LOWER) X(print, C_PRINT) X(punct, C_PUNCT) \
    X(space, C_SPACE) X(upper, C_UPPER) X(xdigit, C_XDIGIT)
#define BYTE_PREDICATE(name, mask) \
    static int is##name##_(int c) { return byte_bits(c) & (mask); } \
    static int is##name##_l_(int c, const lx_locale *locale) \
    { (void)locale; return is##name##_(c); }
CHAR_CLASSES(BYTE_PREDICATE)

static int isctype_(int c, int mask) { return byte_bits(c) & mask; }
static int isctype_l_(int c, int mask, const lx_locale *locale)
{
    (void)locale;
    return isctype_(c, mask);
}
static int isascii_(int c) { return (unsigned)c <= 127; }
static int toascii_(int c) { return c & 127; }
static int isascii_l_(int c, const lx_locale *locale) { (void)locale; return isascii_(c); }
static int toascii_l_(int c, const lx_locale *locale) { (void)locale; return toascii_(c); }
static int tolower_(int c) { return c >= -128 && c <= 255 ? ctype_lower[c + 128] : c; }
static int toupper_(int c) { return c >= -128 && c <= 255 ? ctype_upper[c + 128] : c; }
static int tolower_l_(int c, const lx_locale *locale) { (void)locale; return tolower_(c); }
static int toupper_l_(int c, const lx_locale *locale) { (void)locale; return toupper_(c); }

static int utf16_scalar(uint32_t c, WCHAR out[2])
{
    if (c > 0x10ffff || IN_RANGE(c, 0xd800, 0xdfff)) return 0;
    if (c < 0x10000) {
        out[0] = (WCHAR)c;
        return 1;
    }
    c -= 0x10000;
    out[0] = (WCHAR)(0xd800 + (c >> 10));
    out[1] = (WCHAR)(0xdc00 + (c & 1023));
    return 2;
}

static unsigned latin1_bits(uint32_t c)
{
    if (c < 0xa0) return C_CNTRL;
    unsigned bits = C_PRINT | C_GRAPH;
    if (IN_RANGE(c, 0xc0, 0xd6) || IN_RANGE(c, 0xd8, 0xde))
        bits |= C_UPPER | C_ALPHA | C_ALNUM;
    else if (c == 0xaa || c == 0xb5 || c == 0xba ||
             IN_RANGE(c, 0xdf, 0xf6) || IN_RANGE(c, 0xf8, 0xff))
        bits |= C_LOWER | C_ALPHA | C_ALNUM;
    else
        bits |= C_PUNCT;
    return bits;
}

/* POSIX excludes no-break spaces and NEXT LINE from space/blank. Zl/Zp
 * are control characters; format characters are printable, not controls. */
static int unicode_space(uint32_t c)
{
    return c == 0x1680 || IN_RANGE(c, 0x2000, 0x2006) ||
        IN_RANGE(c, 0x2008, 0x200a) || c == 0x2028 || c == 0x2029 ||
        c == 0x205f || c == 0x3000;
}

static unsigned wide_bits(uint32_t c, int utf8)
{
    if (c < 128) return ctype_bits[c + 128];
    if (!utf8) return 0;
    if (c < 256) return latin1_bits(c);
    WCHAR text[2];
    WORD types[2] = { 0, 0 };
    int n = utf16_scalar(c, text);
    if (!n || !GetStringTypeW(CT_CTYPE1, text, n, types)) return 0;
    unsigned type = types[0] | types[n - 1];
    unsigned bits = 0;
    if (type & C1_UPPER) bits |= C_UPPER;
    if (type & C1_LOWER) bits |= C_LOWER;
    /* ISO C reserves digit for ASCII. glibc makes other decimal digits
     * alphabetic so that they remain alphanumeric. */
    if (type & (C1_ALPHA | C1_DIGIT | C1_UPPER | C1_LOWER))
        bits |= C_ALPHA | C_ALNUM;
    if (unicode_space(c)) {
        bits |= C_SPACE;
        if (c != 0x2028 && c != 0x2029) bits |= C_BLANK;
    }
    if (c == 0x2028 || c == 0x2029)
        bits |= C_CNTRL;
    else if (type) {
        bits |= C_PRINT;
        if (!(bits & C_SPACE)) bits |= C_GRAPH;
    }
    if ((bits & C_GRAPH) && !(bits & C_ALNUM)) bits |= C_PUNCT;
    return bits;
}

static int wide_predicate(uint32_t c, unsigned mask, const lx_locale *locale)
{
    /* Unlike the other direct isw* entrypoints, glibc iswdigit returns a
     * boolean and is ASCII-only in every locale. Other ASCII classes keep
     * the byte mask; descriptors and non-ASCII return 0 or 1. */
    if (mask == C_DIGIT) return IN_RANGE(c, '0', '9');
    if (c < 128) return ctype_bits[c + 128] & mask;
    return (wide_bits(c, lx_locale_utf8(locale)) & mask) != 0;
}

#define WIDE_PREDICATE(name, mask) \
    static int isw##name##_(uint32_t c) \
    { return wide_predicate(c, mask, NULL); } \
    static int isw##name##_l_(uint32_t c, const lx_locale *locale) \
    { return wide_predicate(c, mask, locale); }
CHAR_CLASSES(WIDE_PREDICATE)

static uint32_t wide_case(uint32_t c, int upper, int utf8)
{
    if (c < 128) return (uint32_t)(upper ? ctype_upper[c + 128] : ctype_lower[c + 128]);
    if (!utf8) return c;
    if (c < 256) {
        if (upper) {
            if (IN_RANGE(c, 0xe0, 0xf6) || IN_RANGE(c, 0xf8, 0xfe)) return c - 32;
            if (c == 0xff) return 0x178;
            if (c == 0xb5) return 0x39c;
        } else if (IN_RANGE(c, 0xc0, 0xd6) || IN_RANGE(c, 0xd8, 0xde)) {
            return c + 32;
        }
        return c;
    }
    WCHAR src[2], dst[4];
    int n = utf16_scalar(c, src);
    if (!n) return c;
    /* A fixed non-Turkic linguistic locale selects Unicode casing rather
     * than Windows' conservative filesystem mappings. It also avoids the
     * invariant-locale linguistic-casing special behavior. */
    int count = LCMapStringEx(L"en-US",
        LCMAP_LINGUISTIC_CASING | (upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE),
        src, n, dst, (int)LX_COUNT(dst), NULL, NULL, 0);
    if (count == 1 && !IN_RANGE(dst[0], 0xd800, 0xdfff)) return dst[0];
    if (count == 2 && IN_RANGE(dst[0], 0xd800, 0xdbff) && IN_RANGE(dst[1], 0xdc00, 0xdfff))
        return 0x10000 + (((uint32_t)dst[0] - 0xd800) << 10) + dst[1] - 0xdc00;
    /* wint_t transforms are simple one-scalar mappings, never expansions. */
    return c;
}

static uint32_t towlower_(uint32_t c) { return wide_case(c, 0, c >= 128 && lx_locale_utf8(NULL)); }
static uint32_t towupper_(uint32_t c) { return wide_case(c, 1, c >= 128 && lx_locale_utf8(NULL)); }
static uint32_t towlower_l_(uint32_t c, const lx_locale *locale)
{ return wide_case(c, 0, c >= 128 && lx_locale_utf8(locale)); }
static uint32_t towupper_l_(uint32_t c, const lx_locale *locale)
{ return wide_case(c, 1, c >= 128 && lx_locale_utf8(locale)); }

typedef struct class_descriptor {
    const char *name;
    unsigned mask;
    int utf8;
} class_descriptor;
#define C_DESCRIPTOR(name, mask) { #name, mask, 0 },
#define UTF8_DESCRIPTOR(name, mask) { #name, mask, 1 },
static const class_descriptor classes[2][12] = {
    { CHAR_CLASSES(C_DESCRIPTOR) },
    { CHAR_CLASSES(UTF8_DESCRIPTOR) }
};

typedef struct transform_descriptor {
    const char *name;
    int upper, utf8;
} transform_descriptor;
static const transform_descriptor transforms[2][2] = {
    { { "tolower", 0, 0 }, { "toupper", 1, 0 } },
    { { "tolower", 0, 1 }, { "toupper", 1, 1 } }
};

static uintptr_t wctype_l_(const char *name, const lx_locale *locale)
{
    unsigned utf8 = lx_locale_utf8(locale) != 0;
    for (unsigned i = 0; i < LX_COUNT(classes[0]); i++)
        if (!strcmp(name, classes[utf8][i].name)) return (uintptr_t)&classes[utf8][i];
    return 0;
}
static uintptr_t wctype_(const char *name) { return wctype_l_(name, NULL); }
static int iswctype_(uint32_t c, uintptr_t descriptor)
{
    if (!descriptor) return 0;
    const class_descriptor *d = (const class_descriptor *)descriptor;
    return (wide_bits(c, d->utf8) & d->mask) != 0;
}
static int iswctype_l_(uint32_t c, uintptr_t descriptor, const lx_locale *locale)
{
    (void)locale; /* glibc descriptors capture the locale at wctype[_l] time. */
    return iswctype_(c, descriptor);
}
static const transform_descriptor *wctrans_l_(const char *name, const lx_locale *locale)
{
    unsigned utf8 = lx_locale_utf8(locale) != 0;
    for (unsigned i = 0; i < LX_COUNT(transforms[0]); i++)
        if (!strcmp(name, transforms[utf8][i].name)) return &transforms[utf8][i];
    return NULL;
}
static const transform_descriptor *wctrans_(const char *name) { return wctrans_l_(name, NULL); }
static uint32_t towctrans_(uint32_t c, const transform_descriptor *descriptor)
{
    if (!descriptor) return c;
    return wide_case(c, descriptor->upper, descriptor->utf8);
}
static uint32_t towctrans_l_(uint32_t c, const transform_descriptor *descriptor, const lx_locale *locale)
{
    (void)locale;
    return towctrans_(c, descriptor);
}

#define BYTE_SYMBOLS(name, mask) \
    LX_SYM("is" #name, is##name##_), \
    LX_SYM("is" #name "_l", is##name##_l_), \
    LX_SYM("__is" #name "_l", is##name##_l_),
#define WIDE_SYMBOLS(name, mask) \
    LX_SYM("isw" #name, isw##name##_), \
    LX_SYM("__isw" #name, isw##name##_), \
    LX_SYM("isw" #name "_l", isw##name##_l_), \
    LX_SYM("__isw" #name "_l", isw##name##_l_),
LX_SYMTAB(ctype,
    LX_SYM("__ctype_b_loc", ctype_b_loc_),
    LX_SYM("__ctype_tolower_loc", ctype_tolower_loc_),
    LX_SYM("__ctype_toupper_loc", ctype_toupper_loc_),
    LX_SYM("__ctype_get_mb_cur_max", ctype_get_mb_cur_max_),
    CHAR_CLASSES(BYTE_SYMBOLS)
    LX_SYM("__isctype", isctype_), LX_SYM("__isctype_l", isctype_l_),
    LX_SYM("isascii", isascii_), LX_SYM("isascii_l", isascii_l_), LX_SYM("__isascii_l", isascii_l_),
    LX_SYM("toascii", toascii_), LX_SYM("toascii_l", toascii_l_), LX_SYM("__toascii_l", toascii_l_),
    LX_SYM("tolower", tolower_), LX_SYM("_tolower", tolower_),
    LX_SYM("toupper", toupper_), LX_SYM("_toupper", toupper_),
    LX_SYM("tolower_l", tolower_l_), LX_SYM("__tolower_l", tolower_l_),
    LX_SYM("toupper_l", toupper_l_), LX_SYM("__toupper_l", toupper_l_),
    CHAR_CLASSES(WIDE_SYMBOLS)
    LX_SYM("towlower", towlower_), LX_SYM("__towlower", towlower_),
    LX_SYM("towupper", towupper_), LX_SYM("__towupper", towupper_),
    LX_SYM("towlower_l", towlower_l_), LX_SYM("__towlower_l", towlower_l_),
    LX_SYM("towupper_l", towupper_l_), LX_SYM("__towupper_l", towupper_l_),
    LX_SYM("wctype", wctype_), LX_SYM("wctype_l", wctype_l_), LX_SYM("__wctype_l", wctype_l_),
    LX_SYM("iswctype", iswctype_), LX_SYM("__iswctype", iswctype_),
    LX_SYM("iswctype_l", iswctype_l_), LX_SYM("__iswctype_l", iswctype_l_),
    LX_SYM("wctrans", wctrans_), LX_SYM("wctrans_l", wctrans_l_), LX_SYM("__wctrans_l", wctrans_l_),
    LX_SYM("towctrans", towctrans_), LX_SYM("__towctrans", towctrans_),
    LX_SYM("towctrans_l", towctrans_l_), LX_SYM("__towctrans_l", towctrans_l_)
);
