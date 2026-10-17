/* COM-port backend regression: needs two linked ports (a null-modem pair) mapped in the
 * phoenixtools.ini written by tests/serial/run.sh:
 *     /dev/ttyUSB0 = <port A>      /dev/ttyACM0 = <port B>      /dev/ttyACM9 = COM250 (absent)
 * Opens and configures the ports exactly like the game's opener (0x0810c966), then checks data
 * in both directions and the VMIN/VTIME, O_NONBLOCK, FIONREAD, tcflush, speed and error paths. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); return 1; } } while (0)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* The game's opener: open(path, O_RDWR|O_NOCTTY|O_NONBLOCK), fcntl(F_SETFL, 0), save termios,
 * zeroed raw termios with IGNPAR and CS8|CLOCAL|CREAD at 9600 or 115200, tcflush, tcsetattr. */
static int game_open(const char *path, speed_t speed, struct termios *saved)
{
    int fd = open(path, 0x902);
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFL, 0) < 0 || tcgetattr(fd, saved) < 0)
        return -1;
    struct termios t;
    memset(&t, 0, sizeof t);
    t.c_iflag = IGNPAR;
    t.c_cflag = speed | CS8 | CLOCAL | CREAD;
    if (tcflush(fd, TCIFLUSH) < 0 || tcsetattr(fd, TCSANOW, &t) < 0)
        return -1;
    return fd;
}

/* Read exactly n bytes using VTIME so a broken link fails instead of hanging. */
static int read_all(int fd, char *buf, int n)
{
    struct termios t, old;
    tcgetattr(fd, &old);
    t = old;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 20;
    tcsetattr(fd, TCSANOW, &t);
    int got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0)
            break;
        got += (int)r;
    }
    tcsetattr(fd, TCSANOW, &old);
    return got;
}

static int transfer(int from, int to, const char *what)
{
    char buf[64];
    int n = (int)strlen(what);
    if (write(from, what, n) != n)
        return 0;
    return read_all(to, buf, n) == n && !memcmp(buf, what, n);
}

struct delayed { int fd; const char *data; };
static void *write_later(void *arg)
{
    struct delayed *d = arg;
    usleep(300000);
    write(d->fd, d->data, strlen(d->data));
    return NULL;
}

int main(void)
{
    struct termios saved_a, saved_b, t;
    int a = game_open("/dev/ttyUSB0", B9600, &saved_a);
    CHECK(a >= 0);
    int b = game_open("/dev/ttyACM0", B9600, &saved_b);
    CHECK(b >= 0);
    CHECK(isatty(a) == 1 && isatty(b) == 1);
    CHECK(saved_a.c_cflag & CREAD);                                 /* Linux defaults reported */
    CHECK(tcgetattr(a, &t) == 0 && cfgetospeed(&t) == B9600 && t.c_iflag == IGNPAR);
    CHECK((fcntl(a, F_GETFL) & O_NONBLOCK) == 0);

    /* VMIN=0/VTIME=0 with nothing queued: immediate 0, which the game's pollers rely on. */
    char buf[64];
    double start = now();
    CHECK(read(a, buf, sizeof buf) == 0);
    CHECK(now() - start < 0.2);

    /* Light-board style one-byte writes, then both directions. */
    const char *cmd = "P1O12----";
    for (const char *c = cmd; *c; c++)
        CHECK(write(a, c, 1) == 1);
    CHECK(read_all(b, buf, (int)strlen(cmd)) == (int)strlen(cmd) && !memcmp(buf, cmd, strlen(cmd)));
    CHECK(transfer(b, a, "\xaa\x03\x01\x00\x04\x01\x02\x03\x04\x07\xff"));

    /* FIONREAD and TCIFLUSH. */
    CHECK(write(a, "abc", 3) == 3);
    usleep(200000);
    int queued = -1;
    CHECK(ioctl(b, FIONREAD, &queued) == 0 && queued == 3);
    CHECK(tcflush(b, TCIFLUSH) == 0);
    CHECK(ioctl(b, FIONREAD, &queued) == 0 && queued == 0);

    /* O_NONBLOCK with nothing queued: EAGAIN. */
    CHECK(fcntl(b, F_SETFL, O_NONBLOCK) == 0);
    errno = 0;
    CHECK(read(b, buf, sizeof buf) == -1 && errno == EAGAIN);
    CHECK(fcntl(b, F_SETFL, 0) == 0);

    /* VMIN=0, VTIME=3: times out after ~0.3 s with 0. */
    CHECK(tcgetattr(b, &t) == 0);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 3;
    CHECK(tcsetattr(b, TCSANOW, &t) == 0);
    start = now();
    CHECK(read(b, buf, sizeof buf) == 0);
    double waited = now() - start;
    CHECK(waited > 0.25 && waited < 1.5);

    /* VMIN=4, VTIME=0: blocks until four bytes arrive from another thread. */
    t.c_cc[VMIN] = 4;
    t.c_cc[VTIME] = 0;
    CHECK(tcsetattr(b, TCSADRAIN, &t) == 0);
    struct delayed d = { a, "WXYZ" };
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, write_later, &d) == 0);
    start = now();
    ssize_t r = read(b, buf, sizeof buf);
    CHECK(r == 4 && !memcmp(buf, "WXYZ", 4) && now() - start > 0.2);
    pthread_join(thread, NULL);

    /* 115200 (CBAUDEX) on both ends, through a dup'd descriptor. */
    CHECK(tcgetattr(a, &t) == 0);
    cfsetospeed(&t, B115200);
    cfsetispeed(&t, B115200);
    CHECK(tcsetattr(a, TCSAFLUSH, &t) == 0);
    CHECK(tcgetattr(b, &t) == 0);
    cfsetospeed(&t, B115200);
    cfsetispeed(&t, B115200);
    t.c_cc[VMIN] = 0;
    CHECK(tcsetattr(b, TCSANOW, &t) == 0);
    CHECK(tcgetattr(a, &t) == 0 && cfgetospeed(&t) == B115200);
    int a2 = dup(a);
    CHECK(a2 >= 0 && isatty(a2));
    CHECK(transfer(a2, b, "P1G3-----"));
    CHECK(close(a2) == 0);
    CHECK(transfer(a, b, "still open"));
    CHECK(tcdrain(a) == 0);

    /* A speed code outside the Linux table is rejected and leaves the settings unchanged. */
    t.c_cflag = (t.c_cflag & ~CBAUD) | 0x1000;
    errno = 0;
    CHECK(tcsetattr(a, TCSANOW, &t) == -1 && errno == EINVAL);
    CHECK(tcgetattr(a, &t) == 0 && cfgetospeed(&t) == B115200);

    /* Restore like the game's close path. */
    CHECK(tcsetattr(a, TCSANOW, &saved_a) == 0 && close(a) == 0);
    CHECK(tcsetattr(b, TCSANOW, &saved_b) == 0 && close(b) == 0);

    /* Exclusive access: a second open of an already-open port is EBUSY. */
    a = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY);
    CHECK(a >= 0);
    errno = 0;
    CHECK(open("/dev/ttyUSB0", O_RDWR | O_NOCTTY) == -1 && errno == EBUSY);
    CHECK(close(a) == 0);

    /* Mapped to a port that does not exist, and an unmapped device path. */
    errno = 0;
    CHECK(open("/dev/ttyACM9", 0x902) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(open("/dev/ttyACM5", 0x902) == -1 && errno == ENOENT);
    puts("PASS serial");
    return 0;
}
