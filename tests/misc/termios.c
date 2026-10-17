#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

_Static_assert(sizeof(struct termios) == 60, "i386 glibc termios size");
_Static_assert(offsetof(struct termios, c_iflag) == 0, "termios.c_iflag");
_Static_assert(offsetof(struct termios, c_oflag) == 4, "termios.c_oflag");
_Static_assert(offsetof(struct termios, c_cflag) == 8, "termios.c_cflag");
_Static_assert(offsetof(struct termios, c_lflag) == 12, "termios.c_lflag");
_Static_assert(offsetof(struct termios, c_line) == 16, "termios.c_line");
_Static_assert(offsetof(struct termios, c_cc) == 17, "termios.c_cc");
_Static_assert(offsetof(struct termios, c_ispeed) == 52, "termios.c_ispeed");
_Static_assert(offsetof(struct termios, c_ospeed) == 56, "termios.c_ospeed");

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
        return 1; \
    } \
} while (0)
#define FAILS(call, expected_errno) do { \
    errno = 0; \
    CHECK((call) == -1); \
    CHECK(errno == (expected_errno)); \
} while (0)

static int baud_and_raw(void)
{
    struct termios state;
    memset(&state, 0, sizeof state);
    state.c_cflag = CS7 | PARENB | CREAD;
    CHECK(cfsetospeed(&state, B9600) == 0);
    CHECK(cfgetospeed(&state) == B9600 && cfgetispeed(&state) == B9600);
    CHECK(state.c_ospeed == B9600);
    CHECK((state.c_cflag & (CSIZE | PARENB | CREAD)) == (CS7 | PARENB | CREAD));
    CHECK(cfsetispeed(&state, B0) == 0);
    CHECK(cfgetispeed(&state) == B0 && cfgetospeed(&state) == B9600);
    CHECK(state.c_ispeed == B0);
    CHECK(cfsetospeed(&state, B115200) == 0);
    CHECK(cfgetospeed(&state) == B115200 && cfgetispeed(&state) == B0);
    CHECK(cfsetispeed(&state, B57600) == 0);
    CHECK(cfgetispeed(&state) == B57600 && cfgetospeed(&state) == B57600);
    CHECK(state.c_ispeed == B57600);
    struct termios before = state;
    FAILS(cfsetospeed(&state, (speed_t)123456), EINVAL);
    CHECK(memcmp(&state, &before, sizeof state) == 0);
    FAILS(cfsetispeed(&state, (speed_t)123456), EINVAL);
    CHECK(memcmp(&state, &before, sizeof state) == 0);

    state.c_iflag |= IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF;
    state.c_oflag = OPOST | ONLCR;
    state.c_lflag = ECHO | ECHONL | ICANON | ISIG | IEXTEN | NOFLSH;
    state.c_cc[VMIN] = 8;
    state.c_cc[VTIME] = 9;
    cfmakeraw(&state);
    CHECK((state.c_iflag & (IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON)) == 0);
    CHECK((state.c_iflag & IXOFF) == IXOFF);
    CHECK((state.c_oflag & OPOST) == 0 && (state.c_oflag & ONLCR) == ONLCR);
    CHECK((state.c_lflag & (ECHO | ECHONL | ICANON | ISIG | IEXTEN)) == 0);
    CHECK((state.c_lflag & NOFLSH) == NOFLSH);
    CHECK((state.c_cflag & (CSIZE | PARENB)) == CS8);
    CHECK(cfgetospeed(&state) == B57600);
    CHECK(state.c_cc[VMIN] == 1 && state.c_cc[VTIME] == 0);
    return 0;
}

static int regular_file(void)
{
    char path[] = "/tmp/lx-termios-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    struct termios state, before;
    memset(&state, 0xa5, sizeof state);
    before = state;
    FAILS(tcgetattr(fd, &state), ENOTTY);
    CHECK(memcmp(&state, &before, sizeof state) == 0);
    FAILS(tcsetattr(fd, TCSANOW, &state), ENOTTY);
    FAILS(tcsetattr(fd, TCSADRAIN, &state), ENOTTY);
    FAILS(tcsetattr(fd, TCSAFLUSH, &state), ENOTTY);
    FAILS(tcflush(fd, TCIFLUSH), ENOTTY);
    FAILS(tcflush(fd, TCOFLUSH), ENOTTY);
    FAILS(tcflush(fd, TCIOFLUSH), ENOTTY);
    FAILS(tcdrain(fd), ENOTTY);
    FAILS(ioctl(fd, TCGETS, &state), ENOTTY);
    CHECK(memcmp(&state, &before, sizeof state) == 0);
    FAILS(ioctl(fd, TCSETS, &state), ENOTTY);
    FAILS(ioctl(fd, TCSETSW, &state), ENOTTY);
    FAILS(ioctl(fd, TCSETSF, &state), ENOTTY);
    FAILS(ioctl(fd, TCFLSH, TCIFLUSH), ENOTTY);
    FAILS(ioctl(fd, 0xc0105500ul, &state), ENOTTY); /* Unsupported raw USB control. */
    int available = 0x12345678;
    CHECK(ioctl(fd, FIONREAD, &available) == 0 && available == 0);
    CHECK(write(fd, "abcde", 5) == 5);
    CHECK(ioctl(fd, FIONREAD, &available) == 0 && available == 0);
    CHECK(lseek(fd, 2, SEEK_SET) == 2);
    CHECK(ioctl(fd, FIONREAD, &available) == 0 && available == 3);
    CHECK(lseek(fd, 0, SEEK_CUR) == 2);
    char contents[2];
    CHECK(read(fd, contents, sizeof contents) == sizeof contents);
    CHECK(memcmp(contents, "cd", sizeof contents) == 0);
    CHECK(ioctl(fd, FIONREAD, &available) == 0 && available == 1);
    CHECK(lseek(fd, 32, SEEK_SET) == 32);
    CHECK(ioctl(fd, FIONREAD, &available) == 0 && available == -27);
    CHECK(lseek(fd, 0, SEEK_CUR) == 32);
    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
    FAILS(tcgetattr(fd, &state), EBADF);
    FAILS(tcsetattr(fd, TCSANOW, &state), EBADF);
    FAILS(tcflush(fd, TCIFLUSH), EBADF);
    FAILS(tcdrain(fd), EBADF);
    FAILS(ioctl(fd, TCGETS, &state), EBADF);
    available = 0x12345678;
    FAILS(ioctl(-1, FIONREAD, &available), EBADF);
    CHECK(available == 0x12345678);
    return 0;
}

static int pipe_available(void)
{
    int descriptors[2];
    CHECK(pipe(descriptors) == 0);
    int available = -1;
    CHECK(ioctl(descriptors[0], FIONREAD, &available) == 0 && available == 0);
    CHECK(write(descriptors[1], "abcde", 5) == 5);
    CHECK(ioctl(descriptors[0], FIONREAD, &available) == 0 && available == 5);
    char data[5];
    CHECK(read(descriptors[0], data, 2) == 2);
    CHECK(ioctl(descriptors[0], FIONREAD, &available) == 0 && available == 3);
    CHECK(read(descriptors[0], data + 2, 3) == 3);
    CHECK(memcmp(data, "abcde", 5) == 0);
    CHECK(close(descriptors[1]) == 0);
    CHECK(ioctl(descriptors[0], FIONREAD, &available) == 0 && available == 0);
    CHECK(close(descriptors[0]) == 0);
    return 0;
}

int main(void)
{
    if (baud_and_raw() || regular_file() || pipe_available())
        return 1;
    puts("misc termios: ok");
    return 0;
}
