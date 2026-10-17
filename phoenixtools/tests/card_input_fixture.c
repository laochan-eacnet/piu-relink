#define _GNU_SOURCE
#include "card_input_fixture.h"
#include "config/config_internal.h"

char root[PATH_MAX];
char card_paths[2][PATH_MAX];
void *display = (void *)(uintptr_t)0x1234u;
unsigned int open_calls, fopen_calls;
unsigned int conversion_calls, card_name_calls, lookup_calls, log_calls;
unsigned int lxio_calls, lxio_resets;

static void increment(unsigned int *counter)
{
    __atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
}

void pt_log(const char *component, const char *format, ...)
{
    (void)component;
    (void)format;
    increment(&log_calls);
}

int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    va_list args;

    increment(&open_calls);
    if (flags & O_CREAT) {
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return __real_open(path, flags, mode);
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    increment(&fopen_calls);
    return __real_fopen(path, mode);
}


int (*pt_real_open)(const char *, int, ...) = __wrap_open;

void write_bytes(const char *path, const void *bytes, size_t length)
{
    const unsigned char *cursor = bytes;
    int fd = __real_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    assert(fd >= 0);
    while (length) {
        ssize_t count = write(fd, cursor, length);
        assert(count > 0);
        cursor += count;
        length -= (size_t)count;
    }
    assert(close(fd) == 0);
}

void write_text(const char *path, const char *text)
{
    write_bytes(path, text, strlen(text));
}

static void path_join(char *destination, size_t capacity, const char *suffix)
{
    int length = snprintf(destination, capacity, "%s/%s", root, suffix);
    assert(length >= 0 && (size_t)length < capacity);
}

void load_config(int cards, int lxio, const char *bindings)
{
    char contents[2048];
    char flag[64] = "";
    int length;

    /* A negative value deliberately omits the option to test safe defaults. */
    if (cards >= 0)
        assert(snprintf(flag, sizeof(flag), "virtual_card_reader=%s\n",
                        cards ? "true" : "false") > 0);
    length = snprintf(contents, sizeof(contents),
        "[input]\nvirtual_lxio=%s\n%s%s"
        "[paths]\nruntime_dir=../runtime\n",
        lxio ? "true" : "false", flag, bindings);
    assert(length >= 0 && (size_t)length < sizeof(contents));
    write_bytes("target/settings.ini", contents, (size_t)length);
    pt_config_load_path("configured/phoenixtools.ini");
    assert(pt_config_get()->virtual_card_reader == (cards > 0));
    assert(pt_config_get()->virtual_lxio == lxio);
}

void setup(int cards, int lxio, const char *bindings)
{
    char template[] = "/tmp/phoenixtools-card-XXXXXX";
    char *directory = mkdtemp(template);

    assert(directory && strlen(directory) < sizeof(root));
    strcpy(root, directory);
    assert(chdir(root) == 0);
    assert(mkdir("configured", 0700) == 0);
    assert(mkdir("target", 0700) == 0);
    assert(mkdir("runtime", 0700) == 0);
    assert(mkdir("elsewhere", 0700) == 0);
    assert(symlink("../target/settings.ini", "configured/phoenixtools.ini") == 0);
    path_join(card_paths[0], sizeof(card_paths[0]), "configured/card1.txt");
    path_join(card_paths[1], sizeof(card_paths[1]), "configured/card2.txt");
    load_config(cards, lxio, bindings);
    open_calls = fopen_calls = 0;
    conversion_calls = card_name_calls = lookup_calls = log_calls = 0;
}

void cleanup(void)
{
    static const char *const files[] = {
        "configured/card1.txt", "configured/card2.txt",
        "configured/phoenixtools.ini", "target/settings.ini",
        "target/card1.txt", "target/card2.txt", "runtime/card1.txt",
        "runtime/card2.txt", "elsewhere/card1.txt", "elsewhere/card2.txt",
        "card1.txt", "card2.txt"
    };
    size_t index;

    assert(chdir(root) == 0);
    for (index = 0; index < sizeof(files) / sizeof(files[0]); ++index)
        if (unlink(files[index]) != 0)
            assert(errno == ENOENT);
    assert(rmdir("configured") == 0);
    assert(rmdir("target") == 0);
    assert(rmdir("runtime") == 0);
    assert(rmdir("elsewhere") == 0);
    assert(chdir("/") == 0);
    assert(rmdir(root) == 0);
}

void explicit_scan(unsigned int player)
{
    unsigned int before = open_calls;

    pt_network_card_key(player, 0);
    assert(open_calls == before);
    pt_network_card_key(player, 1);
    assert(open_calls == before + 1u);
}

void enabled_setup(int lxio, const char *bindings)
{
    struct stat status;

    setup(1, lxio, bindings);
    map_native_inputs();
    map_native_card();
    assert(pt_network_card_initialize() == 0);
    assert(stat(card_paths[0], &status) == -1 && errno == ENOENT);
    assert(stat(card_paths[1], &status) == -1 && errno == ENOENT);
    assert(pt_bindings_resolve(display) == 0);
    expect_frame(0, NULL, 0, 1);
    expect_frame(1, NULL, 0, 1);
    expect_frame(2, NULL, 0, 0);
    expect_frame(UINT_MAX, NULL, 0, 0);
    pt_network_card_key(2, 1);
    pt_network_card_key(UINT_MAX, 1);
    assert(open_calls == 0);
}
