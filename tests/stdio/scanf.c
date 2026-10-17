/* Deterministic glibc/native byte-diff coverage; long double padding is ignored. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <locale.h>
#include <wchar.h>
#include <inttypes.h>

/* glibc headers redirect C99 entry points; retain direct legacy coverage too. */
extern int old_sscanf(const char *, const char *, ...) __asm__("sscanf");
extern int old_fscanf(FILE *, const char *, ...) __asm__("fscanf");
extern int old_scanf(const char *, ...) __asm__("scanf");
extern int old_vsscanf(const char *, const char *, va_list) __asm__("vsscanf");
extern int old_vfscanf(FILE *, const char *, va_list) __asm__("vfscanf");
extern int old_vscanf(const char *, va_list) __asm__("vscanf");

static void bytes(const void *p, unsigned n)
{
    const unsigned char *s = p;
    unsigned i;
    for (i = 0; i < n; ++i) printf("%02x", (unsigned)s[i]);
}
static void wide(const wchar_t *p, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) printf("%s%08x", i ? "," : "", (unsigned)p[i]);
}
static FILE *source(const char *text)
{
    FILE *f = tmpfile();
    if (!f) { puts("tmpfile failed"); exit(1); }
    fputs(text, f);
    rewind(f);
    return f;
}
static int via_vsscanf(int legacy, const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = legacy ? old_vsscanf(s, fmt, ap) : vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
static int via_vfscanf(int legacy, FILE *f, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = legacy ? old_vfscanf(f, fmt, ap) : vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}
static int via_vscanf(int legacy, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = legacy ? old_vscanf(fmt, ap) : vscanf(fmt, ap);
    va_end(ap);
    return r;
}
static void entry_points(void)
{
    int i, r, n, value;
    char c;
    for (i = 0; i < 4; ++i) {
        value = n = -1;
        if (i == 0) r = sscanf(" 123x", "%d%n", &value, &n);
        else if (i == 1) r = old_sscanf(" 123x", "%d%n", &value, &n);
        else r = via_vsscanf(i == 3, " 123x", "%d%n", &value, &n);
        printf("entry-string %d %d %d %d\n", i, r, value, n);
    }
    for (i = 0; i < 4; ++i) {
        FILE *f = source(" 123x");
        value = n = -1;
        if (i == 0) r = fscanf(f, "%d%n", &value, &n);
        else if (i == 1) r = old_fscanf(f, "%d%n", &value, &n);
        else r = via_vfscanf(i == 3, f, "%d%n", &value, &n);
        printf("entry-file %d %d %d %d %d\n", i, r, value, n, fgetc(f));
        fclose(f);
    }
    for (i = 0; i < 4; ++i) {
        c = '?';
        ungetc('A' + i, stdin);
        if (i == 0) r = scanf("%c", &c);
        else if (i == 1) r = old_scanf("%c", &c);
        else r = via_vscanf(i == 3, "%c", &c);
        printf("entry-stdin %d %d %d\n", i, r, (unsigned char)c);
    }
}
static void integer_tables(void)
{
    static const char *const text[] = {
        "", " ", "x", "+", "-", "+x", "--1", "0", "-0", "0129", "0x", "0xz", "0x2aZ",
        "-0XfF!", "2147483647", "2147483648", "-2147483648", "-2147483649", "4294967295",
        "4294967296", "9223372036854775807", "9223372036854775808", "-9223372036854775808",
        "-9223372036854775809", "18446744073709551615", "18446744073709551616", "-18446744073709551615",
        "9999999999999999999999999999999999999999999999999!", "  +123rest"
    };
    static const char *const formats[] = { "%d%n", "%i%n", "%u%n", "%o%n", "%x%n", "%X%n", "%lld%n", "%llu%n" };
    unsigned i, j;
    for (j = 0; j < sizeof(formats) / sizeof(formats[0]); ++j) {
        for (i = 0; i < sizeof(text) / sizeof(text[0]); ++i) {
            uint64_t v = UINT64_C(0xa5a5a5a5a5a5a5a5);
            int n = -1, r, e;
            FILE *f = source(text[i]);
            errno = 0;
            r = fscanf(f, formats[j], &v, &n);
            e = errno;
            printf("integer %u %u %d %d %d %d ", j, i, r, n, e, fgetc(f));
            bytes(&v, j >= 6 ? 8 : 4);
            putchar('\n');
            fclose(f);
        }
    }
    {
        signed char hh = 0;
        short h = 0;
        long l = 0;
        long long ll = 0;
        intmax_t j = 0;
        size_t z = 0;
        ptrdiff_t t = 0;
        int r = sscanf("-129 -32769 -1234567 -1234567890123 -9223372036854775807 4294967295 -123", "%hhd %hd %ld %lld %jd %zu %td", &hh, &h, &l, &ll, &j, &z, &t);
        printf("length-signed %d %d %d %ld %lld %jd %zu %td\n", r, hh, h, l, ll, j, z, t);
    }
    {
        unsigned char hh = 0;
        unsigned short h = 0;
        unsigned long l = 0;
        unsigned long long ll = 0;
        uintmax_t j = 0;
        int r = sscanf("511 131071 4294967295 18446744073709551615 0xffffffffffffffff", "%hhu %hu %lu %llu %jx", &hh, &h, &l, &ll, &j);
        printf("length-unsigned %d %u %u %lu %llu %ju\n", r, hh, h, l, ll, j);
    }
    {
        int a = 0, b = 0, n = -1, r;
        signed char hh = -1;
        short h = -1;
        long l = -1;
        long long ll = -1;
        intmax_t j = -1;
        size_t z = (size_t)-1;
        ptrdiff_t t = -1;
        r = sscanf(" 12345 6789", "%*2d%3d %2d%hhn%hn%ln%lln%jn%zn%tn%n", &a, &b, &hh, &h, &l, &ll, &j, &z, &t, &n);
        printf("length-count %d %d %d %d %d %ld %lld %jd %zu %td %d\n", r, a, b, hh, h, l, ll, j, z, t, n);
    }
    {
        int a = -1, b = -1, n = -1;
        int r = sscanf("10 20", "%2$d %1$d%3$n", &a, &b, &n);
        printf("positional %d %d %d %d\n", r, a, b, n);
    }
}
static void failure_and_pointer_tables(void)
{
    static const char *const formats[] = {
        "%n", "%d", "%*d", "%*d%d", "%n%d", "%*n%d", " ", "X", "%%", "%c", "%s", "%[a]", "%dX", "%d%%", "%d %d", "%0d%n", "%1d%n", "%2d%n"
    };
    static const char *const text[] = { "", " ", "1", "1 ", "1X", "x", " %", "+1" };
    static const char *const pointers[] = { "0", "0x1234", "0XdeadBEEF", "(nil)", "(NIL)", "(nix)", "+(nil)", "-1", "0x", "0xg", "4294967296" };
    unsigned i, j;
    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        for (j = 0; j < sizeof(text) / sizeof(text[0]); ++j) {
            uint32_t a[16], b[16], c[16];
            int r;
            memset(a, 0xa5, sizeof(a)); memset(b, 0xa5, sizeof(b)); memset(c, 0xa5, sizeof(c));
            r = sscanf(text[j], formats[i], a, b, c);
            printf("failure %u %u %d ", i, j, r);
            bytes(a, 8); putchar(' '); bytes(b, 8); putchar(' '); bytes(c, 8); putchar('\n');
        }
    }
    for (i = 0; i < sizeof(pointers) / sizeof(pointers[0]); ++i) {
        FILE *f = source(pointers[i]);
        void *p = (void *)(uintptr_t)0xa5a5a5a5u;
        int r, n = -1, e;
        errno = 0;
        r = fscanf(f, "%p%n", &p, &n);
        e = errno;
        printf("pointer %u %d %08x %d %d %d\n", i, r, (unsigned)(uintptr_t)p, n, e, fgetc(f));
        fclose(f);
    }
}
static void float_tables(void)
{
    static const char *const text[] = {
        "", " ", "x", "+", "-.", ".", ".e2", "0", "-0", "1.5!", "+.125!", "3.141592653589793238462643383279502884!",
        "1e2!", "1e!", "1e+!", "1e-!", "1e++2", "0x", "0x.", "0xp1", "0x.p1", "0x1.8p+2!", "-0X.8P-2!", "0xabc!", "0x1p-!",
        "inf!", "-INFINITY!", "infinite!", "infi!", "nan(payload)", "-NaN!", "naX!",
        "16777217!", "9007199254740993!", "18446744073709551617!",
        "1.000000000000000000108420217248550443400745280086994171142578125!",
        "1.17549435082228750796873653722224568e-38!", "1.40129846432481707092372958328991613e-45!",
        "2.2250738585072014e-308!", "4.9406564584124654e-324!", "1e-9999!", "1e9999!",
        "3.36210314311209350626267781732175260e-4932!", "3.64519953188247460252840593361941982e-4951!",
        "1.18973149535723176502126385303097021e4932!", "0x1.fffffffffffffffep16383!", "0x1p-16445!",
        "0x1p-149!", "0x1p-1074!", "0x1p128!", "0x1p1024!", "0x1p16384!",
        "123456789012345678901234567890123456789012345678901234567890.0e-30!"
    };
    static const char conversions[] = "fFeEgGaA";
    unsigned i, type;
    for (type = 0; type < 3; ++type) {
        for (i = 0; i < sizeof(text) / sizeof(text[0]); ++i) {
            char format[8];
            unsigned pos = 0;
            union { float f; double d; long double ld; } value;
            FILE *f = source(text[i]);
            int r, n = -1, e;
            memset(&value, 0xa5, sizeof(value));
            format[pos++] = '%';
            if (type) format[pos++] = type == 1 ? 'l' : 'L';
            format[pos++] = conversions[i % 8]; format[pos++] = '%'; format[pos++] = 'n'; format[pos] = 0;
            errno = 0;
            r = fscanf(f, format, &value, &n);
            e = errno;
            printf("float %u %u %d %d %d %d ", type, i, r, n, e, fgetc(f));
            bytes(&value, type == 0 ? 4 : type == 1 ? 8 : 10);
            putchar('\n');
            fclose(f);
        }
    }
    {
        static const char *const formats[] = { "%1f%n", "%2f%n", "%3f%n", "%4f%n", "%5f%n", "%6f%n", "%7f%n", "%*f%n%c" };
        unsigned j;
        for (i = 0; i < 7; ++i) {
            for (j = 0; j < 3; ++j) {
                static const char *const values[] = { "-12.3e+4!", "0x1.8p2!", "infinity!" };
                float v = 42;
                int n = -1, r;
                FILE *f = source(values[j]);
                r = fscanf(f, formats[i], &v, &n);
                printf("float-width %u %u %d %d %d ", i, j, r, n, fgetc(f));
                bytes(&v, 4); putchar('\n');
                fclose(f);
            }
        }
        {
            int n = -1, r;
            char c = '?';
            r = sscanf("1e+!", formats[7], &n, &c);
            printf("float-suppress %d %d %d\n", r, n, (unsigned char)c);
        }
    }
    {
        unsigned short old, cw;
        unsigned rounding;
        __asm__ volatile("fnstcw %0" : "=m"(old));
        for (rounding = 0; rounding < 4; ++rounding) {
            static const char *const values[] = { "1.000000059604644775390625", "-1.000000059604644775390625", "0x1p-150", "-0x1p-150", "1e9999", "-1e9999" };
            cw = (unsigned short)((old & ~0xc00u) | (rounding << 10));
            __asm__ volatile("fldcw %0" : : "m"(cw));
            for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
                float v = 42;
                int r, e;
                errno = 0;
                r = sscanf(values[i], "%f", &v);
                e = errno;
                printf("float-round %u %u %d %d ", rounding, i, r, e); bytes(&v, 4); putchar('\n');
            }
        }
        __asm__ volatile("fldcw %0" : : "m"(old));
    }
}
static void string_tables(void)
{
    static const struct { const char *text, *format; } cases[] = {
        { " hello world", "%s%n" }, { " hello world", "%3s%n" }, { "abc def", "%c%n" }, { "abc def", "%3c%n" },
        { "a", "%3c%n" }, { " ", "%c%n" }, { "abc123!", "%[a-z]%n" }, { "abc123!", "%3[^!]%n" },
        { "]a-b!", "%[]a-b]%n" }, { "-abc!", "%[-a-c]%n" }, { "z-a!", "%[z-a]%n" },
        { "abc]rest", "%[^]]%n" }, { "123abc", "%[a-z]%n" }, { "", "%s%n" }, { "", "%c%n" }, { "", "%[a]%n" },
        { "abc 123", "%*s %3c%n" }, { "abc123!", "%*[a-z]%3c%n" }
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char value[32];
        int n = -1, r;
        FILE *f = source(cases[i].text);
        memset(value, 0xa5, sizeof(value));
        r = fscanf(f, cases[i].format, value, &n);
        printf("string %u %d %d %d ", i, r, n, fgetc(f)); bytes(value, 12); putchar('\n');
        fclose(f);
    }
    {
        char *s = NULL, *set = NULL, *c = NULL;
        int n = -1, r;
        r = sscanf("alpha 12345XYZ", "%ms %3m[0-9]%2mc%n", &s, &set, &c, &n);
        printf("allocate %d %d %s %s ", r, n, s ? s : "NULL", set ? set : "NULL");
        if (c) bytes(c, 2); else printf("NULL");
        putchar('\n');
        free(s); free(set); free(c);
    }
    {
        char *s = NULL;
        int r = old_sscanf("legacy!", "%as", &s);
        printf("allocate-legacy %d %s\n", r, s ? s : "NULL");
        free(s);
    }
    {
        char *s = (char *)(uintptr_t)1;
        int r = sscanf("", "%ms", &s);
        printf("allocate-eof %d %d\n", r, s == NULL);
        if ((uintptr_t)s > 1) free(s);
        s = (char *)(uintptr_t)1;
        r = sscanf("x", "%m[a]", &s);
        printf("allocate-mismatch %d %d\n", r, s == NULL);
        if ((uintptr_t)s > 1) free(s);
    }
    {
        char *s = NULL;
        int r = sscanf("abc", "%*ms");
        printf("allocate-suppress %d %d\n", r, s == NULL);
    }
    {
        char input[4097], *s = NULL;
        int r, n = -1;
        for (i = 0; i < 4096; ++i) input[i] = 'a' + (i % 26);
        input[4096] = 0;
        r = sscanf(input, "%ms%n", &s, &n);
        printf("allocate-long %d %d %d %d\n", r, n, s ? (int)strlen(s) : -1, s ? memcmp(s, input, sizeof(input)) : -1);
        free(s);
    }
}
static void wide_tables(void)
{
    static const struct { const char *text, *format; unsigned output; } cases[] = {
        { "abc def", "%ls%n", 4 }, { "abc def", "%2ls%n", 3 }, { "abc", "%lc%n", 2 }, { "abc", "%2lc%n", 3 },
        { "abc123", "%l[a-z]%n", 4 }, { "\xc3\xa9\xe2\x82\xacZ!", "%2ls%n", 3 },
        { "\xf0\x9f\x98\x80Z", "%lc%n", 2 }, { "\xc3\xa9Z", "%2lc%n", 3 },
        { "\xc3\xa9Z", "%l[^Z]%n", 2 }, { "\xc3\xa9Z", "%2l[^Z]%n", 2 },
        { "\xc3", "%ls%n", 2 }, { "\xc3X", "%lc%n", 2 }, { "", "%lc%n", 2 }, { "a", "%3lc%n", 4 }
    };
    unsigned i;
    const char *locale = setlocale(LC_ALL, "C.UTF-8");
    printf("wide-locale %d\n", locale != NULL);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        wchar_t value[16];
        int r, n = -1, e;
        FILE *f = source(cases[i].text);
        memset(value, 0xa5, sizeof(value));
        errno = 0;
        r = fscanf(f, cases[i].format, value, &n);
        e = errno;
        printf("wide %u %d %d %d %d ", i, r, n, e, fgetc(f)); wide(value, cases[i].output); putchar('\n');
        fclose(f);
    }
    {
        wchar_t *s = NULL, *set = NULL;
        int n = -1, r;
        r = sscanf("\xc3\xa9\xe2\x82\xac Zed", "%mls %ml[A-Za-z]%n", &s, &set, &n);
        printf("wide-allocate %d %d ", r, n);
        if (s) wide(s, 3); else printf("NULL");
        putchar(' ');
        if (set) wide(set, 4); else printf("NULL");
        putchar('\n');
        free(s); free(set);
    }
    setlocale(LC_ALL, "C");
    {
        wchar_t value = (wchar_t)0xa5a5a5a5u;
        int r, e;
        errno = 0;
        r = sscanf("\xc3\xa9", "%lc", &value);
        e = errno;
        printf("wide-C %d %d %08x\n", r, e, (unsigned)value);
    }
}
int main(void)
{
    entry_points();
    integer_tables();
    failure_and_pointer_tables();
    float_tables();
    string_tables();
    wide_tables();
    return 0;
}
