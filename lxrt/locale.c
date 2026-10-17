/* GNU libstdc++ locale ABI, restricted to the C and C.UTF-8 datasets.
 * POSIX aliases C; empty names and UTF-8/UTF8 suffixes select C.UTF-8.
 * Unknown names fail with ENOENT. Numeric, monetary and calendar text stays C;
 * only LC_CTYPE changes multibyte encoding. No translated message catalogs.
 * strtold_l accepts the x87 return ABI but parses at double precision, as do the
 * wide long-double parsers. setlocale queries return a canonical dataset name,
 * not glibc's composite string for mixed global categories. */
#include "lxlocale.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static INIT_ONCE locale_once = INIT_ONCE_STATIC_INIT;
static lx_locale c_locale, global_locale;
static __declspec(thread) lx_locale *thread_locale;
static const char empty[] = "";
static const char ascii_codeset[] = "ANSI_X3.4-1968";
static const char utf8_codeset[] = "UTF-8";

static BOOL CALLBACK init_locale(PINIT_ONCE once, PVOID param, PVOID *context)
{
    lx_ctype_tables(&c_locale.ctype_b, &c_locale.ctype_tolower, &c_locale.ctype_toupper);
    for (unsigned i = 0; i < 13; ++i) c_locale.names[i] = "C";
    global_locale = c_locale;
    return TRUE;
}
static void ensure_locale(void) { InitOnceExecuteOnce(&locale_once, init_locale, NULL, NULL); }
lx_locale *lx_locale_current(void)
{
    ensure_locale();
    return thread_locale ? thread_locale : &global_locale;
}
int lx_locale_utf8(const lx_locale *loc)
{
    if (!loc || loc == (lx_locale *)-1) loc = lx_locale_current();
    return strcmp(loc->names[0], "C") != 0;
}
static const char *locale_name(const char *name)
{
    if (!name) return NULL;
    if (!strcmp(name, "C") || !strcmp(name, "POSIX")) return "C";
    size_t len = strlen(name);
    if (!len || (len >= 6 && !_stricmp(name + len - 6, ".UTF-8")) ||
        (len >= 5 && !_stricmp(name + len - 5, ".UTF8"))) return "C.UTF-8";
    return NULL;
}
static lx_locale *newlocale_(int mask, const char *name, lx_locale *base)
{
    ensure_locale();
    /* GNU's internal __newlocale accepts the LC_ALL category bit as shorthand. */
    if (mask == (1 << 6)) mask = 0x1fbf;
    if ((mask & ~0x1fbf) || !name || base == (lx_locale *)-1) {
        lx_set_errno(LX_EINVAL); return NULL;
    }
    const char *canonical = mask ? locale_name(name) : "C";
    if (!canonical) { lx_set_errno(LX_ENOENT); return NULL; }
    lx_locale *out = lx_malloc(sizeof(*out));
    if (!out) return NULL;
    *out = base ? *base : c_locale;
    for (unsigned i = 0; i < 13; ++i)
        if (mask & (1u << i)) out->names[i] = canonical;
    /* POSIX transfers ownership of base on successful newlocale. */
    if (base && base != &c_locale && base != &global_locale) lx_free(base);
    return out;
}
static lx_locale *duplocale_(lx_locale *loc)
{
    ensure_locale();
    if (loc == (lx_locale *)-1) loc = &global_locale;
    if (!loc) { lx_set_errno(LX_EINVAL); return NULL; }
    lx_locale *out = lx_malloc(sizeof(*out));
    if (out) *out = *loc;
    return out;
}
static void freelocale_(lx_locale *loc)
{
    if (loc && loc != (lx_locale *)-1 && loc != &c_locale && loc != &global_locale) lx_free(loc);
}
static lx_locale *uselocale_(lx_locale *loc)
{
    lx_locale *old = thread_locale ? thread_locale : (lx_locale *)-1;
    if (loc) thread_locale = loc == (lx_locale *)-1 ? NULL : loc;
    return old;
}
static char *setlocale_(int category, const char *name)
{
    ensure_locale();
    if (category < 0 || category > 12) { lx_set_errno(LX_EINVAL); return NULL; }
    if (!name) return (char *)global_locale.names[category == 6 ? 0 : category];
    const char *canonical = locale_name(name);
    if (!canonical) { lx_set_errno(LX_ENOENT); return NULL; }
    if (category == 6) {
        for (unsigned i = 0; i < 13; ++i) global_locale.names[i] = canonical;
    } else global_locale.names[category] = canonical;
    return (char *)canonical;
}

typedef struct lx_lconv {
    const char *strings[10];
    unsigned char values[14];
} lx_lconv;
_Static_assert(sizeof(lx_lconv) == 56, "glibc lconv size");
_Static_assert(offsetof(lx_lconv, values) == 40, "glibc lconv fractional digits");
static lx_lconv *localeconv_(void)
{
    static lx_lconv result = { { ".", "", "", "", "", "", "", "", "", "" },
        {127,127,127,127,127,127,127,127,127,127,127,127,127,127} };
    return &result;
}

static const char *const time_strings[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December",
    "AM", "PM", "%a %b %e %H:%M:%S %Y", "%m/%d/%y", "%H:%M:%S", "%I:%M:%S %p"
};
static uint32_t wide_time[44][32];
static INIT_ONCE langinfo_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_langinfo(PINIT_ONCE once, PVOID param, PVOID *context)
{
    for (unsigned i = 0; i < LX_COUNT(time_strings); ++i)
        for (unsigned j = 0; time_strings[i][j]; ++j)
            wide_time[i][j] = (unsigned char)time_strings[i][j];
    return TRUE;
}
static char *langinfo_(unsigned item, lx_locale *loc)
{
    static const uint32_t wide_empty[] = { 0 };
    static const uint32_t wide_date[] = { '%','a',' ','%','b',' ','%','e',' ','%','H',':','%','M',':','%','S',' ','%','Z',' ','%','Y',0 };
    static const char unspecified[] = { (char)255, 0 };
    static const char digits[10][2] = {"0","1","2","3","4","5","6","7","8","9"};
    static const uint32_t wide_digits[10][2] = {{'0',0},{'1',0},{'2',0},{'3',0},{'4',0},{'5',0},{'6',0},{'7',0},{'8',0},{'9',0}};
    unsigned cat = item >> 16, index = item & 65535;
    ensure_locale();
    if (!loc || loc == (lx_locale *)-1) loc = lx_locale_current();
    const char *codeset = strcmp(loc->names[cat < 13 ? cat : 0], "C") ? utf8_codeset : ascii_codeset;
    if (index == 65535 && cat < 13) return (char *)loc->names[cat];
    switch (cat) {
    case 0:
        if (index == 0) return (char *)(loc->ctype_b - 128);
        if (index == 1) return (char *)(loc->ctype_toupper - 128);
        if (index == 3) return (char *)(loc->ctype_tolower - 128);
        if (index == 10) return "upper\0lower\0alpha\0digit\0xdigit\0space\0print\0graph\0blank\0cntrl\0punct\0alnum\0";
        if (index == 11) return "toupper\0tolower\0";
        if (index == 13) return (char *)(uintptr_t)(lx_locale_utf8(loc) ? 6 : 1);
        if (index == 14) return (char *)codeset;
        if (index == 19 || index == 30) return (char *)1;
        if (index >= 20 && index <= 29) return (char *)digits[index - 20];
        if (index >= 31 && index <= 40) return (char *)wide_digits[index - 31];
        if (index >= 41 && index <= 50) return (char *)digits[index - 41];
        if (index >= 51 && index <= 60) return (char *)(uintptr_t)('0' + index - 51);
        break;
    case 1:
        if (index == 0) return ".";
        if (index == 3) return (char *)(uintptr_t)'.';
        if (index == 4) return NULL;
        if (index == 5) return (char *)codeset;
        break;
    case 2:
        if (index < LX_COUNT(time_strings)) return (char *)time_strings[index];
        if (index >= 52 && index <= 95) {
            InitOnceExecuteOnce(&langinfo_once, init_langinfo, NULL, NULL);
            return (char *)wide_time[index - 52];
        }
        if (index >= 96 && index <= 100) return (char *)wide_empty;
        if (index == 101) return "\7";
        if (index == 102) return (char *)(uintptr_t)19971130;
        if (index == 103 || index == 104 || index == 105) return "\1";
        if (index == 108) return "%a %b %e %H:%M:%S %Z %Y";
        if (index == 109) return (char *)wide_date;
        if (index == 110) return (char *)codeset;
        if (index >= 111 && index < 123) return (char *)time_strings[26 + index - 111];
        if (index >= 123 && index < 135) {
            InitOnceExecuteOnce(&langinfo_once, init_langinfo, NULL, NULL);
            return (char *)wide_time[26 + index - 123];
        }
        if (index >= 135 && index < 147) return (char *)time_strings[14 + index - 135];
        if (index >= 147 && index < 159) {
            InitOnceExecuteOnce(&langinfo_once, init_langinfo, NULL, NULL);
            return (char *)wide_time[14 + index - 147];
        }
        break;
    case 4:
        if ((index >= 7 && index <= 14) || (index >= 16 && index <= 21) || (index >= 24 && index <= 37)) return (char *)unspecified;
        if (index == 43 || index == 44) return NULL;
        if (index == 45) return (char *)codeset;
        if (index == 15) return "-";
        break;
    case 5:
        if (index == 0) return "^[yY]";
        if (index == 1) return "^[nN]";
        if (index == 4) return (char *)codeset;
        break;
    }
    return (char *)empty;
}
static char *nl_langinfo_(unsigned item) { return langinfo_(item, NULL); }
static int strcoll_l_(const char *a, const char *b, lx_locale *loc) { return strcmp(a, b); }
static size_t strxfrm_l_(char *d, const char *s, size_t n, lx_locale *loc)
{
    size_t len = strlen(s);
    if (n) memcpy(d, s, len < n ? len + 1 : n);
    return len;
}
static int wcscoll_l_(const uint32_t *a, const uint32_t *b, lx_locale *loc) { return lx_wcscmp(a,b); }
static size_t wcsxfrm_l_(uint32_t *d, const uint32_t *s, size_t n, lx_locale *loc) { return lx_wcsxfrm(d,s,n); }
extern size_t lx_strftime(char *, size_t, const char *, const void *);
static size_t strftime_l_(char *s, size_t n, const char *f, const void *tm, lx_locale *loc) { return lx_strftime(s,n,f,tm); }
/* Encoding the format as UTF-8 preserves all non-ASCII wide literals. C date names
 * are ASCII, so the formatter output can be decoded without consulting thread locale. */
size_t lx_wcsftime(uint32_t *out, size_t n, const uint32_t *format, const void *tm)
{
    size_t chars = lx_wcslen(format);
    if (!n || chars > (SIZE_MAX - 1) / 4 || n > (SIZE_MAX - 1) / 4) return 0;
    char local_format[256], local_output[512];
    char *fmt = chars * 4 + 1 <= sizeof(local_format) ? local_format : malloc(chars * 4 + 1);
    char *buf = n * 4 + 1 <= sizeof(local_output) ? local_output : malloc(n * 4 + 1);
    if (!fmt || !buf) { if (fmt != local_format) free(fmt); if (buf != local_output) free(buf); lx_set_errno(LX_ENOMEM); return 0; }
    size_t pos = 0, result = 0;
    for (size_t i = 0; i < chars; ++i) {
        int len = lx_utf8_encode(format[i], fmt + pos);
        if (len < 0) goto done;
        pos += len;
    }
    fmt[pos] = 0;
    size_t bytes = lx_strftime(buf, n * 4 + 1, fmt, tm);
    if (!bytes && *fmt) goto done;
    pos = 0;
    while (pos < bytes) {
        uint32_t cp;
        int len = lx_utf8_decode((const unsigned char *)buf + pos, bytes - pos, &cp);
        if (len < 0 || result + 1 >= n) { result = 0; goto done; }
        out[result++] = cp; pos += len;
    }
    out[result] = 0;
 done:
    if (fmt != local_format) free(fmt);
    if (buf != local_output) free(buf);
    return result;
}
static size_t wcsftime_l_(uint32_t *s, size_t n, const uint32_t *f, const void *tm, lx_locale *loc) { return lx_wcsftime(s,n,f,tm); }
static double strtod_l_(const char *s, char **end, lx_locale *loc)
{
    errno = 0; double value = strtod(s,end);
    if (errno == ERANGE) lx_set_errno(LX_ERANGE);
    return value;
}
static float strtof_l_(const char *s, char **end, lx_locale *loc)
{
    errno = 0; float value = strtof(s,end);
    if (errno == ERANGE) lx_set_errno(LX_ERANGE);
    return value;
}
/* MSVC long double is double. A naked tail call leaves the parsed double in ST0,
 * which is also the SysV i386 long-double return register (no value narrowing). */
static __declspec(naked) void strtold_l_(void)
{
    __asm { jmp strtod_l_ }
}
static char *bindtextdomain_(const char *domain, const char *directory) { return (char *)(directory ? directory : "/usr/share/locale"); }
static char *bind_codeset_(const char *domain, const char *codeset) { return (char *)(codeset ? codeset : "UTF-8"); }
static char text_domain[256] = "messages";
static char *textdomain_(const char *domain)
{
    if (domain) { size_t n = strlen(domain); if (n >= sizeof(text_domain)) { lx_set_errno(LX_EINVAL); return NULL; } strcpy(text_domain, *domain ? domain : "messages"); }
    return text_domain;
}
static char *gettext_(const char *s) { return (char *)s; }
static char *dgettext_(const char *domain, const char *s) { return (char *)s; }
static char *dcgettext_(const char *domain, const char *s, int category) { return (char *)s; }
static char *ngettext_(const char *one, const char *many, unsigned long n) { return (char *)(n == 1 ? one : many); }
static char *dngettext_(const char *domain, const char *one, const char *many, unsigned long n) { return ngettext_(one,many,n); }
static char *dcngettext_(const char *domain, const char *one, const char *many, unsigned long n, int category) { return ngettext_(one,many,n); }
#define BOTH(n, f) LX_SYM(n, f), LX_SYM("__" n, f)
LX_SYMTAB(locale,
    BOTH("newlocale",newlocale_), BOTH("duplocale",duplocale_), BOTH("freelocale",freelocale_), BOTH("uselocale",uselocale_),
    LX_SYM("setlocale",setlocale_), LX_SYM("localeconv",localeconv_), BOTH("nl_langinfo_l",langinfo_), LX_SYM("nl_langinfo",nl_langinfo_),
    BOTH("strcoll_l",strcoll_l_), BOTH("strxfrm_l",strxfrm_l_), BOTH("wcscoll_l",wcscoll_l_), BOTH("wcsxfrm_l",wcsxfrm_l_),
    BOTH("strftime_l",strftime_l_), BOTH("wcsftime_l",wcsftime_l_), BOTH("strtod_l",strtod_l_), BOTH("strtof_l",strtof_l_), BOTH("strtold_l",strtold_l_),
    LX_SYM("bindtextdomain",bindtextdomain_), LX_SYM("bind_textdomain_codeset",bind_codeset_), LX_SYM("textdomain",textdomain_),
    LX_SYM("gettext",gettext_), LX_SYM("dgettext",dgettext_), LX_SYM("dcgettext",dcgettext_), LX_SYM("ngettext",ngettext_), LX_SYM("dngettext",dngettext_), LX_SYM("dcngettext",dcngettext_)
);
