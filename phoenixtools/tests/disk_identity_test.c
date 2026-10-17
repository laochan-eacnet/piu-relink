/* Run with phoenixtools preloaded and a config whose virtual_disk lives in a scratch dir.
 * Phoenix's system-information probe: open("/dev/sda", O_RDONLY) + HDIO_GET_IDENTITY must
 * yield printable model/serial strings; writable raw opens stay blocked; the identity is
 * only answered for the virtual disk descriptor. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int field(const unsigned char *id, unsigned word, unsigned length, char *out)
{
    memcpy(out, id + word * 2u, length);
    out[length] = '\0';
    for (unsigned i = length; i && out[i - 1] == ' '; --i)
        out[i - 1] = '\0';
    for (unsigned i = 0; out[i]; ++i)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e)
            return -1;
    return out[0] ? 0 : -1;
}

int main(void)
{
    unsigned char identity[512];
    char model[41], serial[21];
    int fd = open("/dev/sda", O_RDONLY);

    if (fd < 0) {
        fprintf(stderr, "read-only /dev/sda open failed: %s\n", strerror(errno));
        return 1;
    }
    memset(identity, 0xFF, sizeof(identity));
    if (ioctl(fd, 0x030D, identity) != 0 || field(identity, 27, 40, model) != 0 ||
        field(identity, 10, 20, serial) != 0) {
        fprintf(stderr, "HDIO_GET_IDENTITY on virtual disk failed\n");
        return 2;
    }
    close(fd);
    errno = 0;
    if (open("/dev/sda", O_RDWR) != -1 || errno != EACCES) {
        fprintf(stderr, "writable raw /dev/sda open was not blocked\n");
        return 3;
    }
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || ioctl(fd, 0x030D, identity) == 0) {
        fprintf(stderr, "identity answered for a non-disk descriptor\n");
        return 4;
    }
    close(fd);
    printf("disk-identity model=\"%s\" serial=\"%s\"\n", model, serial);
    return 0;
}
