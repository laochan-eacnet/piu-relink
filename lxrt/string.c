/* <string.h>, <strings.h>, basic <stdlib.h> conversions and fortify *_chk variants.
 * Plain C semantics identical between glibc and UCRT are forwarded to the static CRT. */
#include "lxrt.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LX_NORETURN void chk_fail(void)
{
    lx_fatal("*** buffer overflow detected ***: terminated");
}

/* UCRT errno -> Linux errno after strto* calls */
static void sync_errno(void)
{
    if (errno == ERANGE)
        lx_errno = LX_ERANGE;
    else if (errno == EINVAL)
        lx_errno = LX_EINVAL;
}

static void *memcpy_(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *memmove_(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *memset_(void *d, int c, size_t n) { return memset(d, c, n); }
static int memcmp_(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void *memchr_(const void *s, int c, size_t n) { return (void *)memchr(s, c, n); }
static size_t strlen_(const char *s) { return strlen(s); }
static size_t strnlen_(const char *s, size_t n) { return strnlen(s, n); }
static char *strcpy_(char *d, const char *s) { return strcpy(d, s); }
static char *strncpy_(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static char *strcat_(char *d, const char *s) { return strcat(d, s); }
static char *strncat_(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static int strcmp_(const char *a, const char *b) { return strcmp(a, b); }
static int strncmp_(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static char *strchr_(const char *s, int c) { return (char *)strchr(s, c); }
static char *strrchr_(const char *s, int c) { return (char *)strrchr(s, c); }
static char *strstr_(const char *h, const char *n) { return (char *)strstr(h, n); }
static size_t strspn_(const char *s, const char *a) { return strspn(s, a); }
static size_t strcspn_(const char *s, const char *a) { return strcspn(s, a); }
static char *strpbrk_(const char *s, const char *a) { return (char *)strpbrk(s, a); }
static int strcoll_(const char *a, const char *b) { return strcmp(a, b); }   /* C locale */
static size_t strxfrm_(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n)
        strncpy(d, s, n);
    return len;
}

static void *mempcpy_(void *d, const void *s, size_t n) { return (char *)memcpy(d, s, n) + n; }

static void *memrchr_(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--)
        if (*--p == (unsigned char)c)
            return (void *)p;
    return NULL;
}

static void *rawmemchr_(const void *s, int c)
{
    const unsigned char *p = s;
    while (*p != (unsigned char)c)
        p++;
    return (void *)p;
}

static void *memmem_(const void *h, size_t hl, const void *n, size_t nl)
{
    if (!nl)
        return (void *)h;
    for (const char *p = h; hl >= nl; p++, hl--)
        if (!memcmp(p, n, nl))
            return (void *)p;
    return NULL;
}

static char *stpcpy_(char *d, const char *s)
{
    size_t n = strlen(s);
    memcpy(d, s, n + 1);
    return d + n;
}

static char *stpncpy_(char *d, const char *s, size_t n)
{
    size_t l = strnlen(s, n);
    memcpy(d, s, l);
    memset(d + l, 0, n - l);
    return d + l;
}

static char *strchrnul_(const char *s, int c)
{
    while (*s && *s != (char)c)
        s++;
    return (char *)s;
}

static int strcasecmp_(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}

static int strncasecmp_(const char *a, const char *b, size_t n)
{
    for (; n; a++, b++, n--) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

static char *strcasestr_(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++)
        if (!strncasecmp_(h, n, nl))
            return (char *)h;
    return nl ? NULL : (char *)h;
}

static char *strtok_r_(char *s, const char *delim, char **save)
{
    if (!s)
        s = *save;
    s += strspn(s, delim);
    if (!*s) {
        *save = s;
        return NULL;
    }
    char *end = s + strcspn(s, delim);
    if (*end)
        *end++ = 0;
    *save = end;
    return s;
}

static char *strtok_(char *s, const char *delim)
{
    static __declspec(thread) char *save;
    return strtok_r_(s, delim, &save);
}

static char *strsep_(char **sp, const char *delim)
{
    char *s = *sp;
    if (!s)
        return NULL;
    char *end = s + strcspn(s, delim);
    if (*end) {
        *end = 0;
        *sp = end + 1;
    } else {
        *sp = NULL;
    }
    return s;
}

static void bzero_(void *s, size_t n) { memset(s, 0, n); }
static void bcopy_(const void *s, void *d, size_t n) { memmove(d, s, n); }
static int bcmp_(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static char *index_(const char *s, int c) { return (char *)strchr(s, c); }
static char *rindex_(const char *s, int c) { return (char *)strrchr(s, c); }

static int ffs_(int i)
{
    unsigned long idx;
    return _BitScanForward(&idx, (unsigned long)i) ? (int)idx + 1 : 0;
}


static char *strsignal_(int sig)
{
    static __declspec(thread) char buf[32];
    snprintf(buf, sizeof buf, "Signal %d", sig);
    return buf;
}

/* ---- strerror (Linux numbering) -------------------------------------------------------- */
static const char *const err_msgs[] = {
    [0] = "Success", [1] = "Operation not permitted", [2] = "No such file or directory",
    [3] = "No such process", [4] = "Interrupted system call", [5] = "Input/output error",
    [6] = "No such device or address", [7] = "Argument list too long", [8] = "Exec format error",
    [9] = "Bad file descriptor", [10] = "No child processes", [11] = "Resource temporarily unavailable",
    [12] = "Cannot allocate memory", [13] = "Permission denied", [14] = "Bad address",
    [15] = "Block device required", [16] = "Device or resource busy", [17] = "File exists",
    [18] = "Invalid cross-device link", [19] = "No such device", [20] = "Not a directory",
    [21] = "Is a directory", [22] = "Invalid argument", [23] = "Too many open files in system",
    [24] = "Too many open files", [25] = "Inappropriate ioctl for device", [26] = "Text file busy",
    [27] = "File too large", [28] = "No space left on device", [29] = "Illegal seek",
    [30] = "Read-only file system", [31] = "Too many links", [32] = "Broken pipe",
    [33] = "Numerical argument out of domain", [34] = "Numerical result out of range",
    [35] = "Resource deadlock avoided", [36] = "File name too long", [37] = "No locks available",
    [38] = "Function not implemented", [39] = "Directory not empty",
    [40] = "Too many levels of symbolic links", [61] = "No data available", [62] = "Timer expired",
    [75] = "Value too large for defined data type", [84] = "Invalid or incomplete multibyte or wide character",
    [88] = "Socket operation on non-socket", [95] = "Operation not supported",
    [97] = "Address family not supported by protocol", [98] = "Address already in use",
    [99] = "Cannot assign requested address", [101] = "Network is unreachable",
    [103] = "Software caused connection abort", [104] = "Connection reset by peer",
    [107] = "Transport endpoint is not connected", [110] = "Connection timed out",
    [111] = "Connection refused", [113] = "No route to host", [114] = "Operation already in progress",
    [115] = "Operation now in progress",
};

char *lx_strerror(int e)
{
    if (e >= 0 && e < (int)LX_COUNT(err_msgs) && err_msgs[e])
        return (char *)err_msgs[e];
    static __declspec(thread) char buf[32];
    snprintf(buf, sizeof buf, "Unknown error %d", e);
    return buf;
}

/* GNU strerror_r returns char* (glibc default with _GNU_SOURCE); __xpg_strerror_r returns int. */
static char *strerror_r_(int e, char *buf, size_t n)
{
    (void)buf;
    (void)n;
    return lx_strerror(e);
}

static int xpg_strerror_r_(int e, char *buf, size_t n)
{
    const char *m = lx_strerror(e);
    if (!n)
        return LX_ERANGE;
    strncpy(buf, m, n - 1);
    buf[n - 1] = 0;
    return strlen(m) >= n ? LX_ERANGE : 0;
}

static char *strerror_l_(int e, void *loc)
{
    (void)loc;
    return lx_strerror(e);
}

/* ---- numeric conversions ---------------------------------------------------------------- */
static long strtol_(const char *s, char **e, int b) { errno = 0; long r = strtol(s, e, b); sync_errno(); return r; }
static unsigned long strtoul_(const char *s, char **e, int b) { errno = 0; unsigned long r = strtoul(s, e, b); sync_errno(); return r; }
static long long strtoll_(const char *s, char **e, int b) { errno = 0; long long r = strtoll(s, e, b); sync_errno(); return r; }
static unsigned long long strtoull_(const char *s, char **e, int b) { errno = 0; unsigned long long r = strtoull(s, e, b); sync_errno(); return r; }
static double strtod_(const char *s, char **e) { errno = 0; double r = strtod(s, e); sync_errno(); return r; }
static float strtof_(const char *s, char **e) { errno = 0; float r = strtof(s, e); sync_errno(); return r; }
static int atoi_(const char *s) { return atoi(s); }
static long atol_(const char *s) { return atol(s); }
static long long atoll_(const char *s) { return atoll(s); }
static double atof_(const char *s) { return atof(s); }

static int abs_(int x) { return x < 0 ? -x : x; }
static long labs_(long x) { return x < 0 ? -x : x; }
static long long llabs_(long long x) { return x < 0 ? -x : x; }

/* ---- fortify ---------------------------------------------------------------------------- */
static void *memcpy_chk_(void *d, const void *s, size_t n, size_t dl) { if (n > dl) chk_fail(); return memcpy(d, s, n); }
static void *memmove_chk_(void *d, const void *s, size_t n, size_t dl) { if (n > dl) chk_fail(); return memmove(d, s, n); }
static void *mempcpy_chk_(void *d, const void *s, size_t n, size_t dl) { if (n > dl) chk_fail(); return (char *)memcpy(d, s, n) + n; }
static void *memset_chk_(void *d, int c, size_t n, size_t dl) { if (n > dl) chk_fail(); return memset(d, c, n); }
static char *strcpy_chk_(char *d, const char *s, size_t dl) { if (strlen(s) >= dl) chk_fail(); return strcpy(d, s); }
static char *stpcpy_chk_(char *d, const char *s, size_t dl) { if (strlen(s) >= dl) chk_fail(); return stpcpy_(d, s); }
static char *strncpy_chk_(char *d, const char *s, size_t n, size_t dl) { if (n > dl) chk_fail(); return strncpy(d, s, n); }
static char *strcat_chk_(char *d, const char *s, size_t dl) { if (strlen(d) + strlen(s) >= dl) chk_fail(); return strcat(d, s); }
static char *strncat_chk_(char *d, const char *s, size_t n, size_t dl)
{
    if (strlen(d) + strnlen(s, n) >= dl)
        chk_fail();
    return strncat(d, s, n);
}

LX_SYMTAB(string,
    LX_SYM("memcpy", memcpy_), LX_SYM("memmove", memmove_), LX_SYM("memset", memset_),
    LX_SYM("memcmp", memcmp_), LX_SYM("memchr", memchr_), LX_SYM("memrchr", memrchr_),
    LX_SYM("rawmemchr", rawmemchr_), LX_SYM("__rawmemchr", rawmemchr_), LX_SYM("memmem", memmem_),
    LX_SYM("mempcpy", mempcpy_), LX_SYM("__mempcpy", mempcpy_),
    LX_SYM("strlen", strlen_), LX_SYM("strnlen", strnlen_), LX_SYM("__strnlen", strnlen_),
    LX_SYM("strcpy", strcpy_), LX_SYM("strncpy", strncpy_), LX_SYM("stpcpy", stpcpy_),
    LX_SYM("__stpcpy", stpcpy_), LX_SYM("stpncpy", stpncpy_),
    LX_SYM("strcat", strcat_), LX_SYM("strncat", strncat_),
    LX_SYM("strcmp", strcmp_), LX_SYM("strncmp", strncmp_), LX_SYM("strcoll", strcoll_),
    LX_SYM("strxfrm", strxfrm_),
    LX_SYM("strchr", strchr_), LX_SYM("strrchr", strrchr_), LX_SYM("strchrnul", strchrnul_),
    LX_SYM("strstr", strstr_), LX_SYM("strcasestr", strcasestr_),
    LX_SYM("strspn", strspn_), LX_SYM("strcspn", strcspn_), LX_SYM("strpbrk", strpbrk_),
    LX_SYM("strcasecmp", strcasecmp_), LX_SYM("strncasecmp", strncasecmp_),
    LX_SYM("__strcasecmp", strcasecmp_),
    LX_SYM("strtok", strtok_), LX_SYM("strtok_r", strtok_r_), LX_SYM("__strtok_r", strtok_r_),
    LX_SYM("strsep", strsep_),
    LX_SYM("bzero", bzero_), LX_SYM("__bzero", bzero_), LX_SYM("bcopy", bcopy_), LX_SYM("bcmp", bcmp_),
    LX_SYM("index", index_), LX_SYM("rindex", rindex_), LX_SYM("ffs", ffs_),
    LX_SYM("strerror", lx_strerror), LX_SYM("strerror_r", strerror_r_),
    LX_SYM("__strerror_r", strerror_r_), LX_SYM("__xpg_strerror_r", xpg_strerror_r_),
    LX_SYM("strerror_l", strerror_l_), LX_SYM("strsignal", strsignal_),
    LX_SYM("strtol", strtol_), LX_SYM("strtoul", strtoul_), LX_SYM("strtoll", strtoll_),
    LX_SYM("strtoull", strtoull_), LX_SYM("strtoq", strtoll_), LX_SYM("strtouq", strtoull_),
    LX_SYM("__isoc23_strtol", strtol_), LX_SYM("__isoc23_strtoul", strtoul_),
    LX_SYM("__strtol_internal", strtol_), LX_SYM("__strtoul_internal", strtoul_),
    LX_SYM("strtoimax", strtoll_), LX_SYM("strtoumax", strtoull_),
    LX_SYM("strtod", strtod_), LX_SYM("strtof", strtof_),
    LX_SYM("atoi", atoi_), LX_SYM("atol", atol_), LX_SYM("atoll", atoll_), LX_SYM("atof", atof_),
    LX_SYM("abs", abs_), LX_SYM("labs", labs_), LX_SYM("llabs", llabs_),
    LX_SYM("__memcpy_chk", memcpy_chk_), LX_SYM("__memmove_chk", memmove_chk_),
    LX_SYM("__mempcpy_chk", mempcpy_chk_), LX_SYM("__memset_chk", memset_chk_),
    LX_SYM("__strcpy_chk", strcpy_chk_), LX_SYM("__stpcpy_chk", stpcpy_chk_),
    LX_SYM("__strncpy_chk", strncpy_chk_), LX_SYM("__strcat_chk", strcat_chk_),
    LX_SYM("__strncat_chk", strncat_chk_),
);
