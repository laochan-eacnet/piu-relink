#define _GNU_SOURCE

#include "core/runtime.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

/* Phoenix reads the HDD model and serial for its system-information screens with
 * HDIO_GET_IDENTITY on a read-only /dev/sda descriptor, which filesystem.c opens on the
 * virtual disk image. Answer it with a fixed virtual identity; every other ioctl, and
 * HDIO_GET_IDENTITY on any other descriptor, goes to the real implementation. */

#define HDIO_GET_IDENTITY_REQUEST 0x030Du
#define ATA_IDENTIFY_SIZE 512u

/* ATA IDENTIFY strings as libata's HDIO_GET_IDENTITY returns them: readable byte order,
 * space padded. */
static void identity_string(unsigned char *identity, unsigned word, unsigned length,
                            const char *text)
{
    size_t used = strlen(text);

    memset(identity + word * 2u, ' ', length);
    memcpy(identity + word * 2u, text, used < length ? used : length);
}

/* stat64: 32-bit struct stat fails with EOVERFLOW on filesystems with 64-bit inodes. */
static int virtual_disk_fd(int fd)
{
    struct stat64 opened, image;
    const char *virtual_disk = pt_config_get()->virtual_disk;

    return *virtual_disk && fstat64(fd, &opened) == 0 && stat64(virtual_disk, &image) == 0 &&
           opened.st_dev == image.st_dev && opened.st_ino == image.st_ino;
}

int ioctl(int fd, unsigned long request, ...)
{
    static int (*real_ioctl)(int, unsigned long, void *);
    va_list arguments;
    void *argument;

    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);
    if (request == HDIO_GET_IDENTITY_REQUEST && argument && virtual_disk_fd(fd)) {
        unsigned char *identity = argument;

        memset(identity, 0, ATA_IDENTIFY_SIZE);
        identity_string(identity, 10u, 20u, "PTVD0001");                  /* serial */
        identity_string(identity, 23u, 8u, "1.0");                        /* firmware */
        identity_string(identity, 27u, 40u, "PHOENIXTOOLS VIRTUAL DISK"); /* model */
        return 0;
    }
    if (!real_ioctl)
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (!real_ioctl) {
        errno = ENOSYS;
        return -1;
    }
    return real_ioctl(fd, request, argument);
}
