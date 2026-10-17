/* Miscellaneous glibc 2.31 interfaces. Guest callbacks use the build's aligned C frames.
 * Sorting is allocation-free heapsort; getopt implements GNU stable argv permutation.
 * Mutable libc data goes through lx_data_addr so EXE COPY relocations remain live.
 * Diagnostics use the shared guest formatter; syslog writes only to lx_log.
 * Entropy and six-character temporary-name suffixes use the system BCrypt RNG.
 * Temporary paths go through the guest mount mapping; tmpnam does not reserve a file.
 * Termios supports native console modes and [serial] COM-port descriptors (serial.c); other
 * descriptors return ENOTTY; invalid descriptors return EBADF. TCGETS/
 * TCSETS marshal the 36-byte kernel layout separately from 60-byte glibc termios.
 * Socket ioctls belong to net.c; pipe FIONREAD uses PeekNamedPipe; disk FIONREAD
 * reports signed 32-bit size-minus-offset (including narrowing) without advancing the offset.
 * Atfork registration is a no-op because process.c deliberately rejects fork. */
#include "lxrt.h"

#include <bcrypt.h>
#include <intrin.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(size_t) == 4, "i386 size_t");
_Static_assert(sizeof(unsigned long) == 4, "i386 unsigned long");

/* ---- Sorting and searching ---------------------------------------------------------- */
typedef int (*compare_fn)(const void *, const void *);
typedef int (*compare_r_fn)(const void *, const void *, void *);

static void swap_elements(unsigned char *a, unsigned char *b, size_t width)
{
    while (width--) {
        unsigned char c = *a;
        *a++ = *b;
        *b++ = c;
    }
}

static void sift_down(unsigned char *base, size_t root, size_t count, size_t width,
                      compare_r_fn compare, void *context)
{
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && compare(base + child * width, base + (child + 1) * width, context) < 0)
            child++;
        if (compare(base + root * width, base + child * width, context) >= 0)
            break;
        swap_elements(base + root * width, base + child * width, width);
        root = child;
    }
}

static void qsort_r_(void *base, size_t count, size_t width, compare_r_fn compare, void *context)
{
    if (count < 2 || !width)
        return;
    /* In-place heapsort bounds both stack usage and worst-case work without allocating. */
    for (size_t root = count / 2; root;)
        sift_down(base, --root, count, width, compare, context);
    for (size_t end = count; end > 1;) {
        --end;
        swap_elements(base, (unsigned char *)base + end * width, width);
        sift_down(base, 0, end, width, compare, context);
    }
}

struct compare_context { compare_fn compare; };
static int compare_plain(const void *a, const void *b, void *context)
{
    return ((struct compare_context *)context)->compare(a, b);
}

static void qsort_(void *base, size_t count, size_t width, compare_fn compare)
{
    struct compare_context context = { compare };
    qsort_r_(base, count, width, compare_plain, &context);
}

static void *bsearch_(const void *key, const void *base, size_t count, size_t width, compare_fn compare)
{
    size_t first = 0;
    while (count) {
        size_t half = count / 2;
        const unsigned char *item = (const unsigned char *)base + (first + half) * width;
        int order = compare(key, item);
        if (!order)
            return (void *)item;
        if (order < 0)
            count = half;
        else {
            first += half + 1;
            count -= half + 1;
        }
    }
    return NULL;
}

static void *lfind_(const void *key, const void *base, size_t *count, size_t width, compare_fn compare)
{
    for (size_t i = 0; i < *count; i++) {
        const unsigned char *item = (const unsigned char *)base + i * width;
        if (!compare(key, item))
            return (void *)item;
    }
    return NULL;
}

static void *lsearch_(const void *key, void *base, size_t *count, size_t width, compare_fn compare)
{
    void *item = lfind_(key, base, count, width, compare);
    if (!item) {
        item = (unsigned char *)base + *count * width;
        memcpy(item, key, width);
        ++*count;
    }
    return item;
}

/* ---- GNU getopt --------------------------------------------------------------------- */
typedef struct lx_option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
} lx_option;
_Static_assert(sizeof(lx_option) == 16, "i386 struct option size");
_Static_assert(offsetof(lx_option, name) == 0, "option.name");
_Static_assert(offsetof(lx_option, has_arg) == 4, "option.has_arg");
_Static_assert(offsetof(lx_option, flag) == 8, "option.flag");
_Static_assert(offsetof(lx_option, val) == 12, "option.val");

static char *optarg_storage;
static int optind_storage = 1, opterr_storage = 1, optopt_storage = '?';
#define optarg_ (*(char **)lx_data_addr(&optarg_storage))
#define optind_ (*(int *)lx_data_addr(&optind_storage))
#define opterr_ (*(int *)lx_data_addr(&opterr_storage))
#define optopt_ (*(int *)lx_data_addr(&optopt_storage))
static char *next_option;
static int first_nonoption, last_nonoption, getopt_initialized, option_order;
enum { REQUIRE_ORDER, PERMUTE, RETURN_IN_ORDER };

static int posixly_correct(void)
{
    char **environment = *(char ***)lx_data_addr(&lx_environ);
    if (environment)
        for (char **p = environment; *p; p++)
            if (!strncmp(*p, "POSIXLY_CORRECT=", 16))
                return 1;
    return 0;
}

static void reverse_arguments(char **argv, int first, int end)
{
    while (first < --end) {
        char *p = argv[first];
        argv[first++] = argv[end];
        argv[end] = p;
    }
}

static void exchange_arguments(char **argv)
{
    int options = optind_ - last_nonoption;
    reverse_arguments(argv, first_nonoption, last_nonoption);
    reverse_arguments(argv, last_nonoption, optind_);
    reverse_arguments(argv, first_nonoption, optind_);
    first_nonoption += options;
    last_nonoption = optind_;
}

static int not_option(const char *arg) { return arg[0] != '-' || !arg[1]; }

static void option_diagnostic(const char *program, const char *detail, const char *option)
{
    if (opterr_)
        lx_log("%s: %s '%s'", program, detail, option);
}

/* -2 means a single-dash long-only candidate must instead be parsed as short options. */
static int parse_long_option(int argc, char **argv, const char *shortopts, const lx_option *options,
                             int *index, int long_only, const char *name, int fallback)
{
    const char *equal = strchr(name, '=');
    size_t length = equal ? (size_t)(equal - name) : strlen(name);
    int match = -1, ambiguous = 0;
    for (int i = 0; options && options[i].name; i++) {
        if (strncmp(options[i].name, name, length))
            continue;
        if (strlen(options[i].name) == length) {
            match = i;
            ambiguous = 0;
            break;
        }
        if (match < 0)
            match = i;
        else if (long_only || options[match].has_arg != options[i].has_arg ||
                 options[match].flag != options[i].flag || options[match].val != options[i].val)
            ambiguous = 1;
    }
    if (match < 0 && fallback)
        return -2;
    next_option = NULL;
    ++optind_;
    if (match < 0 || ambiguous) {
        if (shortopts[0] != ':')
            option_diagnostic(argv[0], ambiguous ? "option is ambiguous" : "unrecognized option", name);
        optopt_ = 0;
        return '?';
    }
    const lx_option *option = &options[match];
    if (equal) {
        if (!option->has_arg) {
            if (shortopts[0] != ':')
                option_diagnostic(argv[0], "option does not allow an argument", name);
            optopt_ = option->val;
            return '?';
        }
        optarg_ = (char *)equal + 1;
    } else if (option->has_arg == 1) {
        if (optind_ == argc) {
            if (shortopts[0] != ':')
                option_diagnostic(argv[0], "option requires an argument", name);
            optopt_ = option->val;
            return shortopts[0] == ':' ? ':' : '?';
        }
        optarg_ = argv[optind_++];
    }
    if (index)
        *index = match;
    if (option->flag) {
        *option->flag = option->val;
        return 0;
    }
    return option->val;
}

static int getopt_internal(int argc, char *const original_argv[], const char *optstring,
                           const lx_option *options, int *index, int long_only)
{
    char **argv = (char **)original_argv;
    optarg_ = NULL;
    if (!getopt_initialized || optind_ == 0) {
        optind_ = optind_ ? optind_ : 1;
        first_nonoption = last_nonoption = optind_;
        next_option = NULL;
        option_order = optstring[0] == '-' ? RETURN_IN_ORDER :
                       optstring[0] == '+' || posixly_correct() ? REQUIRE_ORDER : PERMUTE;
        getopt_initialized = 1;
    }
    if (optstring[0] == '-' || optstring[0] == '+')
        optstring++;
    if (argc < 1)
        return -1;
    if (!next_option || !*next_option) {
        next_option = NULL;
        if (first_nonoption > optind_)
            first_nonoption = optind_;
        if (last_nonoption > optind_)
            last_nonoption = optind_;
        if (option_order == PERMUTE) {
            if (first_nonoption != last_nonoption && last_nonoption != optind_)
                exchange_arguments(argv);
            else if (last_nonoption != optind_)
                first_nonoption = optind_;
            while (optind_ < argc && not_option(argv[optind_]))
                ++optind_;
            last_nonoption = optind_;
        }
        if (optind_ < argc && !strcmp(argv[optind_], "--")) {
            ++optind_;
            if (first_nonoption != last_nonoption && last_nonoption != optind_)
                exchange_arguments(argv);
            else if (first_nonoption == last_nonoption)
                first_nonoption = optind_;
            last_nonoption = argc;
            optind_ = argc;
        }
        if (optind_ >= argc) {
            if (first_nonoption != last_nonoption)
                optind_ = first_nonoption;
            return -1;
        }
        if (not_option(argv[optind_])) {
            if (option_order == REQUIRE_ORDER)
                return -1;
            optarg_ = argv[optind_++];
            return 1;
        }
        next_option = argv[optind_] + 1;
        if (options && (next_option[0] == '-' ||
                        (long_only && (next_option[1] || !strchr(optstring, next_option[0]))))) {
            int double_dash = next_option[0] == '-';
            int result = parse_long_option(argc, argv, optstring, options, index, long_only,
                                           next_option + double_dash,
                                           !double_dash && strchr(optstring, next_option[0]) != NULL);
            if (result != -2)
                return result;
        }
    }
    unsigned char c = (unsigned char)*next_option++;
    const char *spec = strchr(optstring, c);
    if (!*next_option)
        ++optind_;
    if (!spec || c == ':' || c == ';') {
        if (opterr_ && optstring[0] != ':')
            lx_log("%s: invalid option -- '%c'", argv[0], c);
        optopt_ = c;
        return '?';
    }
    if (c == 'W' && spec[1] == ';' && options) {
        const char *name;
        if (*next_option)
            name = next_option;
        else if (optind_ < argc)
            name = argv[optind_++];
        else {
            optopt_ = c;
            if (opterr_ && optstring[0] != ':')
                lx_log("%s: option requires an argument -- '%c'", argv[0], c);
            return optstring[0] == ':' ? ':' : '?';
        }
        /* parse_long_option consumes the argument slot containing the candidate. */
        if (*next_option)
            ++optind_;
        --optind_;
        return parse_long_option(argc, argv, optstring, options, index, long_only, name, 0);
    }
    if (spec[1] == ':') {
        if (*next_option) {
            optarg_ = next_option;
            ++optind_;
        } else if (spec[2] != ':') {
            if (optind_ == argc) {
                optopt_ = c;
                if (opterr_ && optstring[0] != ':')
                    lx_log("%s: option requires an argument -- '%c'", argv[0], c);
                return optstring[0] == ':' ? ':' : '?';
            }
            optarg_ = argv[optind_++];
        }
        next_option = NULL;
    }
    return c;
}

static int getopt_(int argc, char *const argv[], const char *options)
{
    return getopt_internal(argc, argv, options, NULL, NULL, 0);
}
static int getopt_long_(int argc, char *const argv[], const char *shortopts,
                         const lx_option *options, int *index)
{
    return getopt_internal(argc, argv, shortopts, options, index, 0);
}
static int getopt_long_only_(int argc, char *const argv[], const char *shortopts,
                              const lx_option *options, int *index)
{
    return getopt_internal(argc, argv, shortopts, options, index, 1);
}

/* ---- Unix pathname components ------------------------------------------------------- */
static char *basename_(const char *path)
{
    const char *last = strrchr(path, '/');
    return (char *)(last ? last + 1 : path);
}

static char *xpg_basename_(char *path)
{
    if (!path || !*path)
        return ".";
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
        path[--length] = 0;
    char *last = strrchr(path, '/');
    return last && last[1] ? last + 1 : path;
}

static char *dirname_(char *path)
{
    if (!path || !*path)
        return ".";
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
        --length;
    while (length && path[length - 1] != '/')
        --length;
    if (!length)
        return ".";
    while (length > 1 && path[length - 1] == '/' && path[length - 2] == '/')
        --length;
    if (length == 1 && path[0] == '/' && path[1] == '/' && path[2] != '/')
        length = 2;
    if (length > 1 && path[length - 1] == '/' && path[length - 2] != '/')
        --length;
    path[length] = 0;
    return path;
}

/* ---- Diagnostics -------------------------------------------------------------------- */
struct message {
    char local[512];
    char *text;
    size_t length, capacity;
};

static int message_sink(void *context, const char *text, size_t length)
{
    struct message *m = context;
    if (length > SIZE_MAX - m->length - 1)
        return lx_set_errno(LX_EOVERFLOW);
    size_t required = m->length + length + 1;
    if (required > m->capacity) {
        size_t capacity = m->capacity <= SIZE_MAX / 2 ? m->capacity * 2 : SIZE_MAX;
        if (capacity < required)
            capacity = required;
        char *p;
        if (m->text == m->local) {
            p = lx_malloc(capacity);
            if (p)
                memcpy(p, m->text, m->length);
        } else
            p = lx_realloc(m->text, capacity);
        if (!p)
            return -1;
        m->text = p;
        m->capacity = capacity;
    }
    memcpy(m->text + m->length, text, length);
    m->length += length;
    m->text[m->length] = 0;
    return 0;
}

static void message_format(struct message *m, const char *format, va_list ap)
{
    m->text = m->local;
    m->length = 0;
    m->capacity = sizeof m->local;
    m->local[0] = 0;
    if (format)
        lx_vformat(message_sink, m, format, ap);
}

static void message_free(struct message *m)
{
    if (m->text != m->local)
        lx_free(m->text);
}

static const char *program_name(void)
{
    const char *name = lx_nmodules && lx_modules[0].name ? lx_modules[0].name : "program";
    const char *slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

static void diagnostic_text(const char *text)
{
    size_t remaining = strlen(text);
    while (remaining) {
        long n = lx_write(2, text, (unsigned long)remaining);
        if (n <= 0)
            break;
        text += n;
        remaining -= (size_t)n;
    }
}

static void warning_v(int with_errno, const char *format, va_list ap)
{
    int saved_errno = lx_errno;
    struct message message;
    message_format(&message, format, ap);
    diagnostic_text(program_name());
    diagnostic_text(": ");
    diagnostic_text(message.text);
    if (with_errno) {
        if (format)
            diagnostic_text(": ");
        diagnostic_text(lx_strerror(saved_errno));
    }
    diagnostic_text("\n");
    message_free(&message);
    lx_errno = saved_errno;
}

static void vwarn_(const char *format, va_list ap) { warning_v(1, format, ap); }
static void vwarnx_(const char *format, va_list ap) { warning_v(0, format, ap); }
static void warn_(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vwarn_(format, ap);
    va_end(ap);
}
static void warnx_(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vwarnx_(format, ap);
    va_end(ap);
}
static LX_NORETURN void verr_(int status, const char *format, va_list ap)
{
    vwarn_(format, ap);
    lx_exit(status);
}
static LX_NORETURN void verrx_(int status, const char *format, va_list ap)
{
    vwarnx_(format, ap);
    lx_exit(status);
}
static LX_NORETURN void err_(int status, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    verr_(status, format, ap);
}
static LX_NORETURN void errx_(int status, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    verrx_(status, format, ap);
}

static unsigned error_message_count_storage;
static int error_one_per_line_storage;
typedef void (*error_progname_fn)(void);
static error_progname_fn error_print_progname_storage;
#define error_message_count_ (*(unsigned *)lx_data_addr(&error_message_count_storage))
#define error_one_per_line_ (*(int *)lx_data_addr(&error_one_per_line_storage))
#define error_print_progname_ (*(error_progname_fn *)lx_data_addr(&error_print_progname_storage))
static const char *last_error_file;
static unsigned last_error_line;

static void error_v(int status, int error_number, int at_line, const char *file, unsigned line,
                     const char *format, va_list ap)
{
    int saved_errno = lx_errno;
    if (at_line && error_one_per_line_) {
        if (last_error_line == line &&
            (last_error_file == file || (last_error_file && file && !strcmp(last_error_file, file))))
            return;
        last_error_file = file;
        last_error_line = line;
    }
    struct message message;
    message_format(&message, format, ap);
    lx_stdio_flush_all();
    if (error_print_progname_)
        error_print_progname_();
    else {
        diagnostic_text(program_name());
        diagnostic_text(at_line ? ":" : ": ");
    }
    if (at_line) {
        if (file) {
            char number[32];
            snprintf(number, sizeof number, ":%u: ", line);
            diagnostic_text(file);
            diagnostic_text(number);
        } else
            diagnostic_text(" ");
    }
    diagnostic_text(message.text);
    if (error_number) {
        diagnostic_text(": ");
        diagnostic_text(lx_strerror(error_number));
    }
    diagnostic_text("\n");
    ++error_message_count_;
    message_free(&message);
    lx_errno = saved_errno;
    if (status)
        lx_exit(status);
}

static void error_(int status, int error_number, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    error_v(status, error_number, 0, NULL, 0, format, ap);
    va_end(ap);
}
static void error_at_line_(int status, int error_number, const char *file, unsigned line,
                           const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    error_v(status, error_number, 1, file, line, format, ap);
    va_end(ap);
}

static SRWLOCK syslog_lock = SRWLOCK_INIT;
static const char *syslog_ident;
static int syslog_options, syslog_facility = 8, syslog_mask = 255;

static void openlog_(const char *ident, int options, int facility)
{
    AcquireSRWLockExclusive(&syslog_lock);
    if (ident)
        syslog_ident = ident;
    syslog_options = options;
    if (!(facility & ~0x3f8))
        syslog_facility = facility;
    ReleaseSRWLockExclusive(&syslog_lock);
}
static void closelog_(void)
{
    AcquireSRWLockExclusive(&syslog_lock);
    syslog_ident = NULL;
    syslog_options = 0;
    ReleaseSRWLockExclusive(&syslog_lock);
}
static int setlogmask_(int mask)
{
    AcquireSRWLockExclusive(&syslog_lock);
    int previous = syslog_mask;
    if (mask)
        syslog_mask = mask;
    ReleaseSRWLockExclusive(&syslog_lock);
    return previous;
}
static void vsyslog_(int priority, const char *format, va_list ap)
{
    int saved_errno = lx_errno;
    AcquireSRWLockShared(&syslog_lock);
    if (!(syslog_mask & (1 << (priority & 7)))) {
        ReleaseSRWLockShared(&syslog_lock);
        return;
    }
    struct message message;
    message_format(&message, format, ap);
    if (!(priority & 0x3f8))
        priority |= syslog_facility;
    const char *ident = syslog_ident ? syslog_ident : program_name();
    if (syslog_options & 1) /* LOG_PID */
        lx_log("syslog <%d> %s[%lu]: %s", priority, ident, GetCurrentProcessId(), message.text);
    else
        lx_log("syslog <%d> %s: %s", priority, ident, message.text);
    message_free(&message);
    ReleaseSRWLockShared(&syslog_lock);
    lx_errno = saved_errno;
}
static void syslog_(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog_(priority, format, ap);
    va_end(ap);
}

/* ---- Entropy and guest-mapped temporary paths --------------------------------------- */
static long getrandom_(void *buffer, size_t length, unsigned flags)
{
    if (flags & ~3u)
        return lx_set_errno(LX_EINVAL);
    if (!length)
        return 0;
    if (!buffer)
        return lx_set_errno(LX_EFAULT);
    if (length > LONG_MAX)
        length = LONG_MAX;
    if (BCryptGenRandom(NULL, buffer, (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return lx_set_errno(LX_EIO);
    return (long)length;
}
static int getentropy_(void *buffer, size_t length)
{
    if (length > 256)
        return lx_set_errno(LX_EIO);
    return getrandom_(buffer, length, 0) < 0 ? -1 : 0;
}

static int random_suffix(char *suffix)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char random[16];
    size_t used = sizeof random;
    for (unsigned i = 0; i < 6;) {
        if (used == sizeof random) {
            if (getrandom_(random, sizeof random, 0) < 0)
                return -1;
            used = 0;
        }
        unsigned value = random[used++];
        if (value < 248)
            suffix[i++] = alphabet[value % 62];
    }
    return 0;
}

static char *template_suffix(char *path)
{
    if (!path) {
        lx_set_errno(LX_EFAULT);
        return NULL;
    }
    size_t length = strlen(path);
    if (length < 6 || memcmp(path + length - 6, "XXXXXX", 6)) {
        lx_set_errno(LX_EINVAL);
        return NULL;
    }
    return path + length - 6;
}

static int mkostemp_(char *path, int flags)
{
    char *suffix = template_suffix(path);
    if (!suffix)
        return -1;
    int saved_errno = lx_errno;
    for (unsigned attempt = 0; attempt < 238328; attempt++) {
        if (random_suffix(suffix) < 0)
            return -1;
        int fd = lx_open(path, (flags & ~LX_O_ACCMODE) | LX_O_RDWR | LX_O_CREAT | LX_O_EXCL, 0600);
        if (fd >= 0) {
            lx_errno = saved_errno;
            return fd;
        }
        if (lx_errno != LX_EEXIST)
            return -1;
    }
    return lx_set_errno(LX_EEXIST);
}
static int mkstemp_(char *path) { return mkostemp_(path, 0); }

static char *mkdtemp_(char *path)
{
    char *suffix = template_suffix(path);
    if (!suffix)
        return NULL;
    int saved_errno = lx_errno;
    for (unsigned attempt = 0; attempt < 238328; attempt++) {
        if (random_suffix(suffix) < 0)
            return NULL;
        wchar_t *host = lx_map_path(path);
        if (!host)
            return NULL;
        BOOL created = CreateDirectoryW(host, NULL);
        DWORD error = created ? 0 : GetLastError();
        free(host);
        if (created) {
            lx_errno = saved_errno;
            return path;
        }
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            lx_set_errno(lx_errno_from_win32(error));
            return NULL;
        }
    }
    lx_set_errno(LX_EEXIST);
    return NULL;
}

static char *tmpnam_(char *buffer)
{
    static char internal[20]; /* glibc i386 L_tmpnam */
    char candidate[20] = "/tmp/lxXXXXXX";
    int saved_errno = lx_errno;
    for (unsigned attempt = 0; attempt < 238328; attempt++) {
        if (random_suffix(candidate + 7) < 0)
            return NULL;
        wchar_t *host = lx_map_path(candidate);
        if (!host)
            return NULL;
        DWORD attributes = GetFileAttributesW(host);
        DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        free(host);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (error != ERROR_FILE_NOT_FOUND) {
                lx_set_errno(lx_errno_from_win32(error));
                return NULL;
            }
            char *result = buffer ? buffer : internal;
            strcpy(result, candidate);
            lx_errno = saved_errno;
            return result;
        }
    }
    lx_set_errno(LX_EEXIST);
    return NULL;
}

/* ---- Small system interfaces -------------------------------------------------------- */
static int sched_cpucount_(size_t size, const void *set)
{
    const unsigned char *bytes = set;
    int count = 0;
    /* glibc counts complete cpu_set_t words, including when size is not word-aligned. */
    size &= ~(sizeof(uint32_t) - 1);
    for (size_t i = 0; i < size; i++) {
        unsigned value = bytes[i];
        while (value) {
            value &= value - 1;
            ++count;
        }
    }
    return count;
}
static int register_atfork_(void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso)
{
    (void)prepare; (void)parent; (void)child; (void)dso;
    return 0; /* This runtime deliberately does not implement fork. */
}
static int pthread_atfork_(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    return register_atfork_(prepare, parent, child, NULL);
}
static const char *gnu_get_libc_version_(void) { return "2.31"; }

static size_t confstr_(int name, char *buffer, size_t length)
{
    const char *value;
    switch (name) {
    case 0: value = "/bin:/usr/bin"; break;
    case 1: value = "POSIX_V6_ILP32_OFF32\nPOSIX_V6_ILP32_OFFBIG"; break;
    case 2: value = "glibc 2.31"; break;
    case 3: value = "NPTL 2.31"; break;
    case 4: value = "XBS5_ILP32_OFF32\nXBS5_ILP32_OFFBIG"; break;
    case 5: value = "POSIX_V7_ILP32_OFF32\nPOSIX_V7_ILP32_OFFBIG"; break;
    case 1000: case 1003: value = "-D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64"; break;
    case 1004: case 1007: value = "-D_LARGEFILE64_SOURCE"; break;
    case 1001: case 1002: case 1005: case 1006: value = ""; break;
    case 1148: case 1149: value = "POSIXLY_CORRECT=1"; break;
    default:
        if (name >= 1100 && name <= 1147) {
            int environment = (name - 1100) % 16;
            if (environment >= 8 || environment % 4 >= 2)
                value = ""; /* No LP64 or LPBIG environment; libraries need no flags. */
            else if (environment % 4 == 1)
                value = "-m32";
            else
                value = environment == 4 ? "-m32 -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64" : "-m32";
        } else {
            lx_set_errno(LX_EINVAL);
            return 0;
        }
    }
    size_t required = strlen(value) + 1;
    if (length && buffer) {
        size_t copied = required < length ? required : length;
        memcpy(buffer, value, copied);
        buffer[copied - 1] = 0;
    }
    return required;
}

static unsigned long getauxval_(unsigned long type)
{
    switch (type) {
    case 6: return 4096; /* AT_PAGESZ, the guest page size */
    case 16: { /* x86 Linux AT_HWCAP uses CPUID.1:EDX feature positions. */
        int cpu[4];
        __cpuid(cpu, 1);
        return (unsigned long)cpu[3];
    }
    case 26: return 0; /* No guest FSGSBASE / ring-3 MWAIT contract. */
    default:
        lx_set_errno(LX_ENOENT);
        return 0;
    }
}

/* ---- Linux termios and ioctl -------------------------------------------------------- */
typedef struct lx_kernel_termios {
    uint32_t iflag, oflag, cflag, lflag;
    unsigned char line, cc[19];
} lx_kernel_termios;
_Static_assert(sizeof(lx_kernel_termios) == 36, "i386 Linux TCGETS size");
_Static_assert(offsetof(lx_kernel_termios, iflag) == 0, "kernel termios.c_iflag");
_Static_assert(offsetof(lx_kernel_termios, oflag) == 4, "kernel termios.c_oflag");
_Static_assert(offsetof(lx_kernel_termios, cflag) == 8, "kernel termios.c_cflag");
_Static_assert(offsetof(lx_kernel_termios, lflag) == 12, "kernel termios.c_lflag");
_Static_assert(offsetof(lx_kernel_termios, line) == 16, "kernel termios.c_line");
_Static_assert(offsetof(lx_kernel_termios, cc) == 17, "kernel termios.c_cc");

#define CBAUD_ LX_CBAUD
#define IBAUD0_ 0x80000000u

static unsigned cfgetospeed_(const lx_termios *termios) { return termios->cflag & CBAUD_; }
static unsigned cfgetispeed_(const lx_termios *termios)
{
    return termios->iflag & IBAUD0_ ? 0 : termios->cflag & CBAUD_;
}
static int valid_speed(unsigned speed) { return !(speed & ~CBAUD_); }
static int cfsetospeed_(lx_termios *termios, unsigned speed)
{
    if (!valid_speed(speed))
        return lx_set_errno(LX_EINVAL);
    termios->ospeed = speed;
    termios->cflag = (termios->cflag & ~CBAUD_) | speed;
    return 0;
}
static int cfsetispeed_(lx_termios *termios, unsigned speed)
{
    if (!valid_speed(speed))
        return lx_set_errno(LX_EINVAL);
    termios->ispeed = speed;
    if (!speed)
        termios->iflag |= IBAUD0_;
    else {
        termios->iflag &= ~IBAUD0_;
        termios->cflag = (termios->cflag & ~CBAUD_) | speed;
    }
    return 0;
}
static void cfmakeraw_(lx_termios *termios)
{
    termios->iflag &= ~(1u | 2u | 8u | 32u | 64u | 128u | 256u | 1024u);
    termios->oflag &= ~1u;
    termios->lflag &= ~(8u | 64u | 2u | 1u | 32768u);
    termios->cflag = (termios->cflag & ~(48u | 256u)) | 48u;
    termios->cc[6] = 1; /* VMIN */
    termios->cc[5] = 0; /* VTIME */
}

static lx_fd *console_fd(int fd, DWORD *mode, int *input)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return NULL;
    if (f->kind != LX_FD_CONSOLE || !GetConsoleMode(f->h, mode)) {
        lx_set_errno(LX_ENOTTY);
        return NULL;
    }
    DWORD events;
    *input = GetNumberOfConsoleInputEvents(f->h, &events) != 0;
    return f;
}

static int tcgetattr_(int fd, lx_termios *termios)
{
    lx_fd *f = lx_fd_get(fd);
    if (f && f->kind == LX_FD_SERIAL)
        return lx_serial_tcgetattr(f, termios);
    DWORD mode;
    int input;
    if (!console_fd(fd, &mode, &input))
        return -1;
    if (!termios)
        return lx_set_errno(LX_EFAULT);
    memset(termios, 0, sizeof *termios);
    termios->iflag = 0x500; /* ICRNL | IXON */
    termios->oflag = input || (mode & ENABLE_PROCESSED_OUTPUT) ? 5 : 0; /* OPOST | ONLCR */
    termios->cflag = 0xbf; /* B38400 | CS8 | CREAD, no serial device implied. */
    termios->lflag = 0x8030; /* IEXTEN | ECHOE | ECHOK */
    if (!input || (mode & ENABLE_PROCESSED_INPUT))
        termios->lflag |= 1;
    if (!input || (mode & ENABLE_LINE_INPUT))
        termios->lflag |= 2;
    if (!input || (mode & ENABLE_ECHO_INPUT))
        termios->lflag |= 8;
    static const unsigned char controls[19] = {
        3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0
    };
    memcpy(termios->cc, controls, sizeof controls);
    termios->ispeed = termios->ospeed = 15;
    return 0;
}

static int tcdrain_(int fd)
{
    lx_fd *f = lx_fd_get(fd);
    if (f && f->kind == LX_FD_SERIAL)
        return lx_serial_tcdrain(f);
    DWORD mode;
    int input;
    return console_fd(fd, &mode, &input) ? 0 : -1; /* Console WriteFile completes synchronously. */
}

static int tcflush_(int fd, int queue)
{
    lx_fd *f = lx_fd_get(fd);
    if (f && f->kind == LX_FD_SERIAL)
        return lx_serial_tcflush(f, queue);
    DWORD mode;
    int input;
    f = console_fd(fd, &mode, &input);
    if (!f)
        return -1;
    if (queue < 0 || queue > 2)
        return lx_set_errno(LX_EINVAL);
    if (input && queue != 1 && !FlushConsoleInputBuffer(f->h))
        return lx_fail_win32();
    return 0;
}

static int tcsetattr_(int fd, int action, const lx_termios *termios)
{
    lx_fd *f = lx_fd_get(fd);
    if (f && f->kind == LX_FD_SERIAL)
        return lx_serial_tcsetattr(f, action, termios);
    DWORD mode;
    int input;
    f = console_fd(fd, &mode, &input);
    if (!f)
        return -1;
    if (action < 0 || action > 2)
        return lx_set_errno(LX_EINVAL);
    if (!termios)
        return lx_set_errno(LX_EFAULT);
    if (input) {
        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        if (termios->lflag & 2)
            mode |= ENABLE_LINE_INPUT;
        if ((termios->lflag & (2 | 8)) == (2 | 8))
            mode |= ENABLE_ECHO_INPUT;
        if (termios->lflag & 1)
            mode |= ENABLE_PROCESSED_INPUT;
    } else {
        mode &= ~ENABLE_PROCESSED_OUTPUT;
        if (termios->oflag & 1)
            mode |= ENABLE_PROCESSED_OUTPUT;
    }
    if (action == 2 && input && !FlushConsoleInputBuffer(f->h))
        return lx_fail_win32();
    return SetConsoleMode(f->h, mode) ? 0 : lx_fail_win32();
}

static int ioctl_(int fd, unsigned long request, void *arg)
{
    int result = lx_net_ioctl(fd, request, arg);
    if (result != -2)
        return result;
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    if (request == 0x541b && f->kind == LX_FD_PIPE) { /* FIONREAD */
        if (!arg)
            return lx_set_errno(LX_EFAULT);
        DWORD available;
        if (!PeekNamedPipe(f->h, NULL, 0, NULL, &available, NULL)) {
            if (GetLastError() != ERROR_BROKEN_PIPE)
                return lx_fail_win32();
            available = 0;
        }
        *(int *)arg = available > INT_MAX ? INT_MAX : (int)available;
        return 0;
    }
    if (request == 0x541b && f->kind == LX_FD_SERIAL) { /* FIONREAD */
        if (!arg)
            return lx_set_errno(LX_EFAULT);
        int queued = lx_serial_queued(f);
        if (queued < 0)
            return -1;
        *(int *)arg = queued;
        return 0;
    }
    if (request == 0x541b && f->kind == LX_FD_FILE && GetFileType(f->h) == FILE_TYPE_DISK) {
        if (!arg)
            return lx_set_errno(LX_EFAULT);
        LARGE_INTEGER size, position, zero;
        zero.QuadPart = 0;
        if (!GetFileSizeEx(f->h, &size) || !SetFilePointerEx(f->h, zero, &position, FILE_CURRENT))
            return lx_fail_win32();
        uint64_t remaining = (uint64_t)size.QuadPart - (uint64_t)position.QuadPart;
        *(int *)arg = (int32_t)(uint32_t)remaining;
        return 0;
    }
    if (request == 0x540b) /* TCFLSH takes an integer, not a pointer. */
        return tcflush_(fd, (int)(uintptr_t)arg);
    if (request == 0x5401) { /* TCGETS uses kernel termios, not glibc's 60-byte object. */
        lx_termios termios;
        if (tcgetattr_(fd, &termios) < 0)
            return -1;
        if (!arg)
            return lx_set_errno(LX_EFAULT);
        lx_kernel_termios *kernel = arg;
        memcpy(kernel, &termios, offsetof(lx_kernel_termios, cc));
        memcpy(kernel->cc, termios.cc, sizeof kernel->cc);
        return 0;
    }
    if (request >= 0x5402 && request <= 0x5404) { /* TCSETS / TCSETSW / TCSETSF */
        if (f->kind != LX_FD_CONSOLE && f->kind != LX_FD_SERIAL)
            return lx_set_errno(LX_ENOTTY);
        if (!arg)
            return lx_set_errno(LX_EFAULT);
        lx_termios termios;
        const lx_kernel_termios *kernel = arg;
        memset(&termios, 0, sizeof termios);
        memcpy(&termios, kernel, offsetof(lx_kernel_termios, cc));
        memcpy(termios.cc, kernel->cc, sizeof kernel->cc);
        termios.ispeed = termios.ospeed = termios.cflag & CBAUD_;
        return tcsetattr_(fd, (int)(request - 0x5402), &termios);
    }
    return lx_set_errno(LX_ENOTTY);
}

LX_SYMTAB(misc,
    LX_SYM("qsort", qsort_), LX_SYM("qsort_r", qsort_r_), LX_SYM("bsearch", bsearch_),
    LX_SYM("lfind", lfind_), LX_SYM("lsearch", lsearch_),
    LX_SYM("getopt", getopt_), LX_SYM("getopt_long", getopt_long_),
    LX_SYM("getopt_long_only", getopt_long_only_), LX_SYM("optarg", &optarg_storage),
    LX_SYM("optind", &optind_storage), LX_SYM("opterr", &opterr_storage), LX_SYM("optopt", &optopt_storage),
    LX_SYM("basename", basename_), LX_SYM("__xpg_basename", xpg_basename_), LX_SYM("dirname", dirname_),
    LX_SYM("err", err_), LX_SYM("errx", errx_), LX_SYM("warn", warn_), LX_SYM("warnx", warnx_),
    LX_SYM("verr", verr_), LX_SYM("verrx", verrx_), LX_SYM("vwarn", vwarn_), LX_SYM("vwarnx", vwarnx_),
    LX_SYM("error", error_), LX_SYM("error_at_line", error_at_line_),
    LX_SYM("error_message_count", &error_message_count_storage),
    LX_SYM("error_one_per_line", &error_one_per_line_storage),
    LX_SYM("error_print_progname", &error_print_progname_storage),
    LX_SYM("syslog", syslog_), LX_SYM("vsyslog", vsyslog_), LX_SYM("openlog", openlog_),
    LX_SYM("closelog", closelog_), LX_SYM("setlogmask", setlogmask_),
    LX_SYM("getrandom", getrandom_), LX_SYM("getentropy", getentropy_),
    LX_SYM("mkstemp", mkstemp_), LX_SYM("mkstemp64", mkstemp_),
    LX_SYM("mkostemp", mkostemp_), LX_SYM("mkostemp64", mkostemp_),
    LX_SYM("mkdtemp", mkdtemp_), LX_SYM("tmpnam", tmpnam_),
    LX_SYM("__sched_cpucount", sched_cpucount_), LX_SYM("__register_atfork", register_atfork_),
    LX_SYM("pthread_atfork", pthread_atfork_), LX_SYM("gnu_get_libc_version", gnu_get_libc_version_),
    LX_SYM("confstr", confstr_), LX_SYM("getauxval", getauxval_), LX_SYM("ioctl", ioctl_),
    LX_SYM("tcgetattr", tcgetattr_), LX_SYM("tcsetattr", tcsetattr_),
    LX_SYM("tcflush", tcflush_), LX_SYM("tcdrain", tcdrain_),
    LX_SYM("cfgetispeed", cfgetispeed_), LX_SYM("cfgetospeed", cfgetospeed_),
    LX_SYM("cfsetispeed", cfsetispeed_), LX_SYM("cfsetospeed", cfsetospeed_), LX_SYM("cfmakeraw", cfmakeraw_)
);
