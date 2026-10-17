#define _GNU_SOURCE
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <getopt.h>
#include <gnu/libc-version.h>
#include <limits.h>
#include <sched.h>
#include <search.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/random.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static unsigned comparator_calls;
static int compare_int(const void *a, const void *b)
{
    ++comparator_calls;
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

struct sort_context { int direction; unsigned calls; uint32_t cookie; };
static int compare_context(const void *a, const void *b, void *argument)
{
    struct sort_context *context = argument;
    if (context->cookie != UINT32_C(0xa1639e5b))
        abort();
    ++context->calls;
    int x = *(const int *)a, y = *(const int *)b;
    return context->direction * ((x > y) - (x < y));
}

struct record { int key; unsigned char payload[9]; };
static int compare_record(const void *a, const void *b)
{
    const struct record *x = a, *y = b;
    return (x->key > y->key) - (x->key < y->key);
}

static int algorithms(void)
{
    enum { COUNT = 257 };
    int values[COUNT];
    for (int i = 0; i < COUNT; i++)
        values[i] = (i * 73 + 19) % COUNT - 128;
    comparator_calls = 0;
    qsort(values, COUNT, sizeof values[0], compare_int);
    CHECK(comparator_calls > 0);
    for (int i = 0; i < COUNT; i++)
        CHECK(values[i] == i - 128);

    /* A volatile function pointer prevents glibc's inline bsearch bypassing lxrt. */
    void *(*volatile search)(const void *, const void *, size_t, size_t,
                            int (*)(const void *, const void *)) = bsearch;
    for (int key = -128; key <= 128; key++) {
        int *found = search(&key, values, COUNT, sizeof values[0], compare_int);
        CHECK(found == &values[key + 128]);
    }
    int missing = 129;
    CHECK(search(&missing, values, COUNT, sizeof values[0], compare_int) == NULL);
    CHECK(search(&missing, values, 0, sizeof values[0], compare_int) == NULL);

    struct sort_context context = { -1, 0, UINT32_C(0xa1639e5b) };
    qsort_r(values, COUNT, sizeof values[0], compare_context, &context);
    CHECK(context.calls > 0);
    for (int i = 0; i < COUNT; i++)
        CHECK(values[i] == 128 - i);
    qsort(values, 0, sizeof values[0], compare_int);
    qsort(values, 1, sizeof values[0], compare_int);
    CHECK(values[0] == 128);

    int equal[] = { 3, -4, 3, INT_MAX, 0, INT_MIN, -4, 3 };
    const int expected[] = { INT_MIN, -4, -4, 0, 3, 3, 3, INT_MAX };
    qsort(equal, sizeof equal / sizeof equal[0], sizeof equal[0], compare_int);
    CHECK(memcmp(equal, expected, sizeof equal) == 0);

    struct record records[17];
    for (unsigned i = 0; i < 17; i++) {
        records[i].key = (int)(16 - i);
        memset(records[i].payload, (int)(16 - i), sizeof records[i].payload);
    }
    qsort(records, 17, sizeof records[0], compare_record);
    for (unsigned i = 0; i < 17; i++) {
        CHECK(records[i].key == (int)i);
        for (unsigned j = 0; j < sizeof records[i].payload; j++)
            CHECK(records[i].payload[j] == i);
    }

    int table[5] = { 4, 8, 12 };
    size_t count = 3;
    int key = 8;
    CHECK(lfind(&key, table, &count, sizeof table[0], compare_int) == &table[1]);
    CHECK(lsearch(&key, table, &count, sizeof table[0], compare_int) == &table[1]);
    CHECK(count == 3);
    key = 7;
    CHECK(lfind(&key, table, &count, sizeof table[0], compare_int) == NULL);
    CHECK(lsearch(&key, table, &count, sizeof table[0], compare_int) == &table[3]);
    CHECK(count == 4 && table[3] == 7);
    return 0;
}

static int options(void)
{
    CHECK(unsetenv("POSIXLY_CORRECT") == 0);
    opterr = 0;
    int flag = 0;
    const struct option long_options[] = {
        { "alpha", no_argument, NULL, 'a' },
        { "beta", required_argument, NULL, 'b' },
        { "color", optional_argument, NULL, 'c' },
        { "flag", no_argument, &flag, 37 },
        { "alpine", no_argument, NULL, 'p' },
        { NULL, 0, NULL, 0 }
    };
    char *argv[] = { "algorithms", "first", "--alpha", "-b", "value", "second",
                     "--color=blue", "--flag", "-ac", "--", "-a", "third", NULL };
    enum { ARGC = 12 };
    int index = -1;
    optind = 0;
    CHECK(getopt_long(ARGC, argv, "ab:c::", long_options, &index) == 'a');
    CHECK(index == 0 && optarg == NULL);
    CHECK(getopt_long(ARGC, argv, "ab:c::", long_options, &index) == 'b');
    CHECK(strcmp(optarg, "value") == 0);
    CHECK(getopt_long(ARGC, argv, "ab:c::", long_options, &index) == 'c');
    CHECK(index == 2 && strcmp(optarg, "blue") == 0);
    CHECK(getopt_long(ARGC, argv, "ab:c::", long_options, &index) == 0);
    CHECK(index == 3 && flag == 37 && optarg == NULL);
    CHECK(getopt_long(ARGC, argv, "ab:c::", long_options, &index) == 'a');
    CHECK(getopt_long(ARGC, argv, "ab:c::", long_options, &index) == 'c');
    CHECK(optarg == NULL);
    CHECK(getopt_long(ARGC, argv, "ab:c::", long_options, &index) == -1);
    CHECK(optind == 8);
    CHECK(strcmp(argv[8], "first") == 0 && strcmp(argv[9], "second") == 0);
    CHECK(strcmp(argv[10], "-a") == 0 && strcmp(argv[11], "third") == 0);

    /* Reset must discard a partially consumed short-option cluster. */
    char *cluster[] = { "algorithms", "-abc", NULL };
    optind = 0;
    CHECK(getopt(2, cluster, "abc") == 'a');
    optind = 0;
    CHECK(getopt(2, cluster, "abc") == 'a');
    CHECK(getopt(2, cluster, "abc") == 'b');
    CHECK(getopt(2, cluster, "abc") == 'c');
    CHECK(getopt(2, cluster, "abc") == -1 && optind == 2);

    char *ambiguous[] = { "algorithms", "--al", "--alph", "--beta", NULL };
    optind = 0;
    CHECK(getopt_long(4, ambiguous, ":ab:", long_options, &index) == '?');
    CHECK(optopt == 0 && optarg == NULL);
    CHECK(getopt_long(4, ambiguous, ":ab:", long_options, &index) == 'a');
    CHECK(index == 0);
    CHECK(getopt_long(4, ambiguous, ":ab:", long_options, &index) == ':');
    CHECK(optopt == 'b');
    CHECK(getopt_long(4, ambiguous, ":ab:", long_options, &index) == -1);

    char *long_only[] = { "algorithms", "-alpha", "-bvalue", "-color", "operand", NULL };
    optind = 0;
    CHECK(getopt_long_only(5, long_only, "ab:c::", long_options, &index) == 'a');
    CHECK(getopt_long_only(5, long_only, "ab:c::", long_options, &index) == 'b');
    CHECK(strcmp(optarg, "value") == 0);
    CHECK(getopt_long_only(5, long_only, "ab:c::", long_options, &index) == 'c');
    CHECK(optarg == NULL);
    CHECK(getopt_long_only(5, long_only, "ab:c::", long_options, &index) == -1);
    CHECK(optind == 4 && strcmp(long_only[optind], "operand") == 0);

    char *require_order[] = { "algorithms", "operand", "-a", NULL };
    optind = 0;
    CHECK(getopt(3, require_order, "+a") == -1 && optind == 1);
    optind = 0;
    CHECK(getopt(3, require_order, "-a") == 1);
    CHECK(strcmp(optarg, "operand") == 0);
    CHECK(getopt(3, require_order, "-a") == 'a');
    CHECK(getopt(3, require_order, "-a") == -1);
    CHECK(setenv("POSIXLY_CORRECT", "1", 1) == 0);
    optind = 0;
    CHECK(getopt(3, require_order, "a") == -1 && optind == 1);
    CHECK(unsetenv("POSIXLY_CORRECT") == 0);

    char *w_options[] = { "algorithms", "-W", "beta=word", "-Walpha", NULL };
    optind = 0;
    CHECK(getopt_long(4, w_options, "W;", long_options, &index) == 'b');
    CHECK(strcmp(optarg, "word") == 0);
    CHECK(getopt_long(4, w_options, "W;", long_options, &index) == 'a');
    CHECK(getopt_long(4, w_options, "W;", long_options, &index) == -1);
    return 0;
}

static int configuration(void)
{
    CHECK(strcmp(gnu_get_libc_version(), "2.31") == 0);
    size_t required = confstr(_CS_GNU_LIBC_VERSION, NULL, 0);
    CHECK(required == sizeof "glibc 2.31");
    char short_buffer[5];
    CHECK(confstr(_CS_GNU_LIBC_VERSION, short_buffer, sizeof short_buffer) == required);
    CHECK(strcmp(short_buffer, "glib") == 0);
    errno = 0;
    CHECK(confstr(-1, NULL, 0) == 0 && errno == EINVAL);
    CHECK(getauxval(AT_PAGESZ) == 4096);
    errno = 0;
    CHECK(getauxval(0xfffffffful) == 0 && errno == ENOENT);
    return 0;
}

static unsigned progname_calls;
static void test_progname(void) { ++progname_calls; }

static int error_data(void)
{
    unsigned initial_count = error_message_count;
    int initial_one_per_line = error_one_per_line;
    void (*initial_progname)(void) = error_print_progname;
    error_print_progname = test_progname;
    error_one_per_line = 1;
    progname_calls = 0;
    errno = EACCES;
    error(0, 0, "misc diagnostic data test");
    CHECK(errno == EACCES);
    error_at_line(0, 0, "misc-test", 17, "first");
    error_at_line(0, 0, "misc-test", 17, "suppressed");
    error_at_line(0, 0, "misc-test", 18, "second");
    CHECK(progname_calls == 3 && error_message_count == initial_count + 3);
    error_one_per_line = initial_one_per_line;
    error_print_progname = initial_progname;
    return 0;
}

extern char *__xpg_basename(char *);
extern char *dirname(char *);

static int path_components(void)
{
    static const struct { const char *path, *base, *directory; } cases[] = {
        { "", ".", "." }, { "/", "/", "/" }, { "//", "/", "//" },
        { "///", "/", "/" }, { "file", "file", "." },
        { "file///", "file", "." }, { "/a/b", "b", "/a" },
        { "/a///b///", "b", "/a" }, { "//a", "a", "//" }
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char mutable[32];
        strcpy(mutable, cases[i].path);
        CHECK(strcmp(__xpg_basename(mutable), cases[i].base) == 0);
        strcpy(mutable, cases[i].path);
        CHECK(strcmp(dirname(mutable), cases[i].directory) == 0);
    }
    CHECK(strcmp(__xpg_basename(NULL), ".") == 0);
    CHECK(strcmp(dirname(NULL), ".") == 0);
    CHECK(strcmp(basename("/a/b/"), "") == 0);
    CHECK(strcmp(basename("plain"), "plain") == 0);
    return 0;
}

static int entropy_and_temporary(void)
{
    unsigned char random[258];
    memset(random, 0xa5, sizeof random);
    CHECK(getentropy(random + 1, 256) == 0);
    CHECK(random[0] == 0xa5 && random[257] == 0xa5);
    unsigned char before[258];
    memcpy(before, random, sizeof before);
    errno = 0;
    CHECK(getentropy(random, 257) == -1 && errno == EIO);
    CHECK(memcmp(random, before, sizeof random) == 0);
    CHECK(getrandom(random, 0, 0) == 0);
    CHECK(getrandom(random, 32, GRND_NONBLOCK) == 32);
    errno = 0;
    CHECK(getrandom(random, 1, ~3u) == -1 && errno == EINVAL);
    char invalid[] = "/tmp/lx-invalid";
    errno = 0;
    CHECK(mkstemp(invalid) == -1 && errno == EINVAL);
    char path[] = "/tmp/lx-misc-XXXXXX";
    int fd = mkostemp(path, O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR);
    CHECK(write(fd, "temporary", 9) == 9);
    CHECK(close(fd) == 0 && unlink(path) == 0);
    char directory[] = "/tmp/lx-misc-dir-XXXXXX";
    CHECK(mkdtemp(directory) == directory);
    fd = open(directory, O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(rmdir(directory) == 0);
    char name[L_tmpnam];
    CHECK(tmpnam(name) == name);
    fd = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(name) == 0);
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus);
    CPU_SET(31, &cpus);
    CPU_SET(127, &cpus);
    CHECK(CPU_COUNT(&cpus) == 3);
    return 0;
}

int main(void)
{
    if (algorithms() || options() || configuration() || error_data() ||
        path_components() || entropy_and_temporary())
        return 1;
    puts("misc algorithms: ok");
    return 0;
}
