/* Deterministic glibc/i386 reference-output test. The complete byte tables,
 * actual function return masks, wide predicates and locale-bound descriptors
 * are printed without addresses; compare Linux/qemu with the relinked binary. */
#define _GNU_SOURCE 1
#define __NO_CTYPE 1
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

extern const unsigned short int **__ctype_b_loc(void);
extern const int **__ctype_tolower_loc(void);
extern const int **__ctype_toupper_loc(void);
extern size_t __ctype_get_mb_cur_max(void);
extern wctype_t (__wctype_l)(const char *, locale_t);
extern int (__iswctype_l)(wint_t, wctype_t, locale_t);
extern wint_t (__towlower_l)(wint_t, locale_t);
extern wint_t (__towupper_l)(wint_t, locale_t);
extern wctrans_t (__wctrans_l)(const char *, locale_t);
extern wint_t (__towctrans_l)(wint_t, wctrans_t, locale_t);

#define CLASSES(X) \
    X(alnum, 8) X(alpha, 0x400) X(blank, 1) X(cntrl, 2) \
    X(digit, 0x800) X(graph, 0x8000) X(lower, 0x200) X(print, 0x4000) \
    X(punct, 4) X(space, 0x2000) X(upper, 0x100) X(xdigit, 0x1000)
#define DECLARE_ALIASES(name, mask) \
    extern int (__is##name##_l)(int, locale_t); \
    extern int (__isw##name##_l)(wint_t, locale_t);
CLASSES(DECLARE_ALIASES)

typedef struct character_class {
    const char *name;
    unsigned mask;
    int (*byte)(int);
    int (*byte_l)(int, locale_t);
    int (*byte_internal_l)(int, locale_t);
    int (*wide)(wint_t);
    int (*wide_l)(wint_t, locale_t);
    int (*wide_internal_l)(wint_t, locale_t);
} character_class;
#define CLASS_ENTRY(name, mask) \
    { #name, mask, is##name, is##name##_l, __is##name##_l, \
      isw##name, isw##name##_l, __isw##name##_l },
static const character_class classes[] = { CLASSES(CLASS_ENTRY) };
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static int failures;

static unsigned ascii_bits(int c)
{
    unsigned bits = 0;
    int upper = c >= 'A' && c <= 'Z';
    int lower = c >= 'a' && c <= 'z';
    int digit = c >= '0' && c <= '9';
    if (upper) bits |= 0x100;
    if (lower) bits |= 0x200;
    if (upper || lower) bits |= 0x400;
    if (digit) bits |= 0x800;
    if (digit || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) bits |= 0x1000;
    if (c == ' ' || (c >= '\t' && c <= '\r')) bits |= 0x2000;
    if (c >= 32 && c <= 126) bits |= 0x4000;
    if (c >= 33 && c <= 126) bits |= 0x8000;
    if (c == ' ' || c == '\t') bits |= 1;
    if ((c >= 0 && c < 32) || c == 127) bits |= 2;
    if (c >= 33 && c <= 126 && !(upper || lower || digit)) bits |= 4;
    if (upper || lower || digit) bits |= 8;
    return bits;
}

static void dump_byte_tables(locale_t c_locale, locale_t utf8_locale)
{
    const unsigned short *bits = *__ctype_b_loc();
    const int *lower = *__ctype_tolower_loc(), *upper = *__ctype_toupper_loc();
    int (*lower_fn)(int) = tolower, (*upper_fn)(int) = toupper;
    int (*lower_l_fn)(int, locale_t) = tolower_l;
    int (*upper_l_fn)(int, locale_t) = toupper_l;
    int (*ascii_fn)(int) = isascii, (*toascii_fn)(int) = toascii;
    for (int c = -128; c <= 255; c++) {
        int compat = c < -1 ? c + 256 : c;
        int lo = compat >= 'A' && compat <= 'Z' ? compat + 32 : compat;
        int up = compat >= 'a' && compat <= 'z' ? compat - 32 : compat;
        unsigned expected = c >= 0 && c < 128 ? ascii_bits(c) : 0;
        printf("byte %4d %04x %4d %4d", c, bits[c], lower[c], upper[c]);
        failures += bits[c] != expected || lower[c] != lo || upper[c] != up;
        failures += lower_fn(c) != lo || upper_fn(c) != up;
        failures += lower_l_fn(c, c_locale) != lo || upper_l_fn(c, c_locale) != up;
        failures += lower_l_fn(c, utf8_locale) != lo || upper_l_fn(c, utf8_locale) != up;
        failures += ascii_fn(c) != (c >= 0 && c <= 127) || toascii_fn(c) != (c & 127);
        for (unsigned i = 0; i < COUNT(classes); i++) {
            const character_class *p = &classes[i];
            int direct = p->byte(c), explicit_c = p->byte_l(c, c_locale);
            int explicit_utf8 = p->byte_l(c, utf8_locale);
            int internal = p->byte_internal_l(c, c_locale);
            printf(" %04x:%04x:%04x:%04x", direct, explicit_c, explicit_utf8, internal);
            failures += direct != (int)(expected & p->mask);
            failures += direct != explicit_c || direct != explicit_utf8 || direct != internal;
        }
        putchar('\n');
    }
    const int outside[] = { INT_MIN, -1024, -129, 256, 1024, INT_MAX };
    for (unsigned i = 0; i < COUNT(outside); i++) {
        int c = outside[i];
        printf("outside %d %d %d %d %d\n", c, lower_fn(c), upper_fn(c), ascii_fn(c), toascii_fn(c));
        failures += lower_fn(c) != c || upper_fn(c) != c;
    }
}

static void dump_wide_character(uint32_t c, locale_t locale)
{
    int (*internal_class)(wint_t, wctype_t, locale_t) = __iswctype_l;
    wctype_t (*internal_type)(const char *, locale_t) = __wctype_l;
    wint_t (*internal_lower)(wint_t, locale_t) = __towlower_l;
    wint_t (*internal_upper)(wint_t, locale_t) = __towupper_l;
    wctrans_t (*internal_transform)(const char *, locale_t) = __wctrans_l;
    wint_t (*internal_apply)(wint_t, wctrans_t, locale_t) = __towctrans_l;
    wint_t lo = towlower(c), up = towupper(c);
    printf("wide %08x %08x %08x", (unsigned)c, (unsigned)lo, (unsigned)up);
    failures += lo != towlower_l(c, locale) || up != towupper_l(c, locale);
    failures += lo != internal_lower(c, locale) || up != internal_upper(c, locale);
    wctrans_t lower = wctrans("tolower"), upper = wctrans("toupper");
    failures += !lower || !upper;
    failures += lo != towctrans(c, lower) || up != towctrans(c, upper);
    failures += lo != towctrans_l(c, wctrans_l("tolower", locale), locale);
    failures += up != towctrans_l(c, wctrans_l("toupper", locale), locale);
    failures += lo != internal_apply(c, internal_transform("tolower", locale), locale);
    failures += up != internal_apply(c, internal_transform("toupper", locale), locale);
    for (unsigned i = 0; i < COUNT(classes); i++) {
        const character_class *p = &classes[i];
        int direct = p->wide(c), explicit_l = p->wide_l(c, locale);
        int internal = p->wide_internal_l(c, locale);
        wctype_t type = wctype(p->name), explicit_type = wctype_l(p->name, locale);
        int described = iswctype(c, type), described_l = iswctype_l(c, explicit_type, locale);
        printf(" %04x:%04x:%04x:%d:%d", direct, explicit_l, internal, described, described_l);
        failures += !type || !explicit_type || direct != explicit_l || direct != internal;
        failures += !!direct != described || !!direct != described_l;
        failures += !!direct != internal_class(c, internal_type(p->name, locale), locale);
        if (p->mask == 0x800) failures += direct != (c >= '0' && c <= '9');
    }
    putchar('\n');
}

static void dump_wide(const char *name, locale_t locale)
{
    locale_t old = uselocale(locale);
    printf("locale %s mbmax %u\n", name, (unsigned)__ctype_get_mb_cur_max());
    for (uint32_t c = 0; c <= 255; c++) dump_wide_character(c, locale);
    const uint32_t extra[] = {
        0x0100, 0x0101, 0x0130, 0x0131, 0x0178, 0x017f, 0x0301,
        0x0391, 0x03b1, 0x03c2, 0x0400, 0x0410, 0x0430, 0x0660,
        0x1680, 0x2000, 0x2007, 0x200b, 0x2028, 0x2029, 0x202f,
        0x2060, 0x20ac, 0x4e2d, 0xff10, 0xff21, 0xff41,
        0xd800, 0xdfff, 0x110000, 0xffffffff
    };
    for (unsigned i = 0; i < COUNT(extra); i++) dump_wide_character(extra[i], locale);
    printf("invalid-descriptors %d %d %d\n", !wctype("not-a-class"),
        !wctrans("not-a-transform"), iswctype('A', 0));
    failures += wctype("not-a-class") != 0 || wctrans("not-a-transform") != 0;
    failures += iswctype('A', 0) != 0;
    uselocale(old);
}

/* Descriptors retain their creator's locale, even when the active locale and
 * the nominal _l argument are different. Do not compare pointer addresses. */
static void check_descriptor_locales(locale_t c_locale, locale_t utf8_locale)
{
    wctype_t c_alpha = wctype_l("alpha", c_locale), u_alpha = wctype_l("alpha", utf8_locale);
    wctrans_t c_upper = wctrans_l("toupper", c_locale), u_upper = wctrans_l("toupper", utf8_locale);
    locale_t old = uselocale(c_locale);
    printf("bound-descriptors %d %d %d %d %08x %08x %08x %08x\n",
        iswctype(0xe9, c_alpha), iswctype(0xe9, u_alpha),
        iswctype_l(0xe9, c_alpha, utf8_locale), iswctype_l(0xe9, u_alpha, c_locale),
        (unsigned)towctrans(0xe9, c_upper), (unsigned)towctrans(0xe9, u_upper),
        (unsigned)towctrans_l(0xe9, c_upper, utf8_locale),
        (unsigned)towctrans_l(0xe9, u_upper, c_locale));
    failures += iswctype(0xe9, c_alpha) != 0 || iswctype(0xe9, u_alpha) != 1;
    failures += towctrans(0xe9, c_upper) != 0xe9 || towctrans(0xe9, u_upper) != 0xc9;
    failures += !!iswalpha_l(0xe9, c_locale) != 0 || !!iswalpha_l(0xe9, utf8_locale) != 1;
    uselocale(utf8_locale);
    failures += iswctype(0xe9, c_alpha) != 0 || iswctype(0xe9, u_alpha) != 1;
    failures += towctrans(0xe9, c_upper) != 0xe9 || towctrans(0xe9, u_upper) != 0xc9;
    uselocale(old);
}

int main(void)
{
    if (!setlocale(LC_ALL, "C")) return 1;
    locale_t c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    locale_t utf8_locale = newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t)0);
    if (!utf8_locale) utf8_locale = newlocale(LC_ALL_MASK, "C.utf8", (locale_t)0);
    if (!c_locale || !utf8_locale) {
        puts("C and C.UTF-8 locales are required");
        return 1;
    }
    dump_byte_tables(c_locale, utf8_locale);
    dump_wide("C", c_locale);
    dump_wide("UTF8", utf8_locale);
    check_descriptor_locales(c_locale, utf8_locale);
    freelocale(c_locale);
    freelocale(utf8_locale);
    printf("failures %d\n", failures);
    return failures != 0;
}
