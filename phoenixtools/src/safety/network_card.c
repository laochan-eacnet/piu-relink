#define _GNU_SOURCE

#include "safety/network_card.h"

#include "core/runtime.h"
#include "phoenix/patch_engine.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CARD_UID_MAX 17u
#define CARD_FILE_MAX 256u

struct card_reader {
    pthread_mutex_t mutex;
    unsigned char uid[CARD_UID_MAX];
    unsigned int length;
    int held;
    char path[PHOENIXTOOLS_PATH_MAX];
};

static struct card_reader readers[2] = {
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
    {.mutex = PTHREAD_MUTEX_INITIALIZER}
};

/* Called with the untouched frame of piu's two-player card consumer. */
__attribute__((visibility("hidden")))
int pt_network_card_event(unsigned char *frame)
{
    struct card_reader *reader;
    uint32_t player;
    uint32_t length;

    if (!pt_config_get()->virtual_card_reader)
        return 0;
    memcpy(&player, frame + 0x10, sizeof(player));
    if (player >= 2u)
        return 0;
    reader = &readers[player];
    pthread_mutex_lock(&reader->mutex);
    length = reader->length;
    if (!length) {
        pthread_mutex_unlock(&reader->mutex);
        return 0;
    }
    memset(frame + 0x4c, 0, 0x84);
    memcpy(frame + 0x4c, reader->uid, length);
    memcpy(frame + 0xcc, &length, sizeof(length));
    reader->length = 0;
    pthread_mutex_unlock(&reader->mutex);
    pt_log("network", "injected P%u reader event (%u UID bytes)", player + 1u, length);
    return 1;
}

__attribute__((naked)) static void card_boundary(void)
{
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "lea 36(%esp), %eax\n\t"
        "push %eax\n\t"
        "call pt_network_card_event\n\t"
        "add $4, %esp\n\t"
        "test %eax, %eax\n\t"
        "jz 1f\n\t"
        "popal\n\t"
        "popfl\n\t"
        "push $0x0807e6f8\n\t"
        "ret\n"
        "1:\n\t"
        "popal\n\t"
        "popfl\n\t"
        /* No emulated scan: advance the player, never dequeue a real reader. */
        "push $0x0807e64f\n\t"
        "ret\n\t");
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ascii_space(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/* Fixed-size, nonblocking regular-file reads: no unbounded text or FIFO waits. */
static const char *read_card(struct card_reader *reader)
{
    unsigned char text[CARD_FILE_MAX + 1u];
    unsigned char uid[CARD_UID_MAX];
    struct stat status;
    size_t used = 0;
    size_t first = 0;
    size_t last;
    size_t index;
    int fd;

    if (!pt_real_open || !reader->path[0])
        return "card reader is not initialized";
    fd = pt_real_open(reader->path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return "cannot open card file";
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        close(fd);
        return "card file must be a readable regular file";
    }
    while (used < sizeof(text)) {
        ssize_t count = read(fd, text + used, sizeof(text) - used);

        if (count < 0) {
            close(fd);
            return "cannot read card file";
        }
        if (!count)
            break;
        used += (size_t)count;
    }
    close(fd);
    if (used > CARD_FILE_MAX)
        return "card file exceeds 256 bytes";
    last = used;
    while (first < last && ascii_space(text[first]))
        ++first;
    while (last > first && ascii_space(text[last - 1u]))
        --last;
    if (last - first < 2u || last - first > CARD_UID_MAX * 2u ||
        ((last - first) & 1u))
        return "UID must contain 2-34 hexadecimal digits in complete byte pairs";
    for (index = 0; index < (last - first) / 2u; ++index) {
        int high = hex_digit(text[first + index * 2u]);
        int low = hex_digit(text[first + index * 2u + 1u]);

        if (high < 0 || low < 0)
            return "UID contains a non-hexadecimal character";
        uid[index] = (unsigned char)((high << 4) | low);
    }
    reader->length = (unsigned int)((last - first) / 2u);
    memcpy(reader->uid, uid, reader->length);
    return NULL;
}

void pt_network_card_key(unsigned int player, int pressed)
{
    struct card_reader *reader;
    const char *error;
    unsigned int length;

    if (!pt_config_get()->virtual_card_reader || player >= 2u)
        return;
    reader = &readers[player];
    pthread_mutex_lock(&reader->mutex);
    if (!pressed) {
        reader->held = 0;
        pthread_mutex_unlock(&reader->mutex);
        return;
    }
    if (reader->held) {
        pthread_mutex_unlock(&reader->mutex);
        return;
    }
    reader->held = 1;
    /* A failed explicit scan also replaces (clears) an older pending UID. */
    reader->length = 0;
    error = read_card(reader);
    length = reader->length;
    pthread_mutex_unlock(&reader->mutex);
    if (error)
        pt_log("network", "P%u card scan failed: %s: %s; fix the file and press again",
               player + 1u, reader->path, error);
    else
        pt_log("network", "queued P%u card scan (%u UID bytes)", player + 1u, length);
}

int pt_network_card_initialize(void)
{
    static const unsigned char expected[] = {
        0x8b, 0x44, 0x24, 0x10, 0x8b, 0x14, 0x85, 0x48, 0x01, 0x7c, 0x09
    };
    char config_directory[PHOENIXTOOLS_PATH_MAX];
    char directory[PHOENIXTOOLS_PATH_MAX];
    char *slash;
    size_t index;
    size_t directory_length;
    int drive_absolute;
    unsigned int player;

    if (!pt_config_get()->virtual_card_reader)
        return 0;
    if (!pt_config_get()->loaded_from[0]) {
        pt_log("network", "card-reader emulation requires a loaded INI");
        return -1;
    }
    strcpy(config_directory, pt_config_get()->loaded_from);
    for (index = 0; config_directory[index]; ++index)
        if (config_directory[index] == '\\')
            config_directory[index] = '/';
    drive_absolute = ((config_directory[0] >= 'A' && config_directory[0] <= 'Z') ||
                      (config_directory[0] >= 'a' && config_directory[0] <= 'z')) &&
                     config_directory[1] == ':' && config_directory[2] == '/';
    slash = strrchr(config_directory, '/');
    if (slash)
        slash[slash == config_directory ||
              (drive_absolute && slash == config_directory + 2) ? 1 : 0] = '\0';
    else
        strcpy(config_directory, ".");
    /* POSIX realpath would prepend CWD to D:/; lxrt accepts that absolute form. */
    if (drive_absolute)
        strcpy(directory, config_directory);
    /* Anchor relative paths without following an INI-file symlink. */
    else if (!realpath(config_directory, directory)) {
        pt_log("network", "cannot locate the loaded INI directory for card-reader emulation");
        return -1;
    }
    directory_length = strlen(directory);
    for (player = 0; player < 2u; ++player) {
        struct card_reader *reader = &readers[player];
        int written = snprintf(reader->path, sizeof(reader->path), "%s%scard%u.txt",
                               directory, directory[directory_length - 1u] == '/' ?
                               "" : "/", player + 1u);

        if (written < 0 || (size_t)written >= sizeof(reader->path)) {
            pt_log("network", "INI directory is too long for card file paths");
            return -1;
        }
        reader->length = 0;
        reader->held = 0;
    }
    if (pt_patch_jump((void *)0x0807e671u, card_boundary, expected, sizeof(expected)))
        return -1;
    pt_log("network", "virtual card reader enabled: P1=%s P2=%s (INI-relative files)",
           pt_config_get()->input.card1, pt_config_get()->input.card2);
    return 0;
}
