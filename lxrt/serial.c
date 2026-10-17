/* Linux serial ttys over Windows COM ports.
 *
 * phoenixtools.ini maps guest device paths to ports, e.g.
 *     [serial]
 *     /dev/ttyUSB0 = COM3      ; cabinet light board (game: 9600 8N1)
 *     /dev/ttyACM0 = COM4      ; P1 RFID reader (game: 115200 8N1)
 * Unmapped paths keep the ordinary filesystem behaviour (normally ENOENT).
 *
 * A mapped open creates an LX_FD_SERIAL descriptor on an overlapped COM handle. Every fd keeps
 * its own Linux termios; tcsetattr applies baud (cflag CBAUD incl. CBAUDEX rates), CSIZE, CSTOPB,
 * PARENB/PARODD/CMSPAR, INPCK, CRTSCTS and IXON/IXOFF (VSTART/VSTOP) to the DCB; B0 drops DTR/RTS.
 * DTR and RTS are raised on open as Linux does. Reads follow the raw-mode VMIN/VTIME rules and
 * O_NONBLOCK (EAGAIN when nothing is queued); writes complete when the driver accepts the bytes.
 *
 * Not emulated: the line discipline (ICANON line editing, ECHO, ISIG, ICRNL/ONLCR/OPOST
 * translation; those flags are stored and reported but bytes pass through raw), modem-control
 * ioctls, and select/poll readiness (non-socket fds are always reported ready). */
#include "lxrt.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct lx_serial {
    lx_termios termios;
    COMMTIMEOUTS timeouts;   /* last applied to the handle */
    int timeouts_valid;
    HANDLE read_event, write_event;
    SRWLOCK read_lock, write_lock;   /* one reader and one writer at a time, like the tty layer */
};

enum { VTIME_ = 5, VMIN_ = 6, VSTART_ = 8, VSTOP_ = 9 };
enum {
    INPCK_ = 0x10, IXON_ = 0x400, IXOFF_ = 0x1000,
    CSIZE_ = 0x30, CSTOPB_ = 0x40, PARENB_ = 0x100, PARODD_ = 0x200, CBAUDEX_ = 0x1000,
};
#define CMSPAR_ 0x40000000u
#define CRTSCTS_ 0x80000000u

static struct lx_serial *serial_new(void)
{
    struct lx_serial *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeSRWLock(&s->read_lock);
    InitializeSRWLock(&s->write_lock);
    if (!s->read_event || !s->write_event) {
        lx_serial_free(s);
        return NULL;
    }
    return s;
}

void lx_serial_free(struct lx_serial *s)
{
    if (!s)
        return;
    if (s->read_event)
        CloseHandle(s->read_event);
    if (s->write_event)
        CloseHandle(s->write_event);
    free(s);
}

struct lx_serial *lx_serial_dup(const struct lx_serial *s)
{
    struct lx_serial *d = serial_new();
    if (d)
        d->termios = s->termios;   /* timeouts are re-applied on first use */
    return d;
}

/* Linux speed code -> bits per second; 0 for B0 and for unsupported codes. */
static DWORD baud_rate(uint32_t code)
{
    static const DWORD base[16] = { 0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800,
                                    9600, 19200, 38400 };
    static const DWORD extended[16] = { 0, 57600, 115200, 230400, 460800, 500000, 576000, 921600,
                                        1000000, 1152000, 1500000, 2000000, 2500000, 3000000,
                                        3500000, 4000000 };
    return code & CBAUDEX_ ? extended[code & 15] : base[code & 15];
}

static int apply(HANDLE h, const lx_termios *t)
{
    uint32_t code = t->cflag & LX_CBAUD;
    if (code && !baud_rate(code))
        return lx_set_errno(LX_EINVAL);
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb))
        return lx_fail_win32();
    if (code)
        dcb.BaudRate = baud_rate(code);
    dcb.fBinary = TRUE;
    dcb.ByteSize = (BYTE)(5 + ((t->cflag & CSIZE_) >> 4));
    dcb.StopBits = t->cflag & CSTOPB_ ? TWOSTOPBITS : ONESTOPBIT;
    if (!(t->cflag & PARENB_))
        dcb.Parity = NOPARITY;
    else if (t->cflag & CMSPAR_)
        dcb.Parity = t->cflag & PARODD_ ? MARKPARITY : SPACEPARITY;
    else
        dcb.Parity = t->cflag & PARODD_ ? ODDPARITY : EVENPARITY;
    dcb.fParity = (t->cflag & PARENB_) && (t->iflag & INPCK_);
    dcb.fOutxCtsFlow = !!(t->cflag & CRTSCTS_);
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = code ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    dcb.fRtsControl = !code ? RTS_CONTROL_DISABLE : t->cflag & CRTSCTS_ ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fOutX = !!(t->iflag & IXON_);
    dcb.fInX = !!(t->iflag & IXOFF_);
    /* Windows rejects equal XON/XOFF characters even with software flow control off; termios
     * commonly leaves c_cc zeroed, so fall back to DC1/DC3 then. */
    unsigned char xon = t->cc[VSTART_], xoff = t->cc[VSTOP_];
    if (xon == xoff) {
        xon = 0x11;
        xoff = 0x13;
    }
    dcb.XonChar = (char)xon;
    dcb.XoffChar = (char)xoff;
    dcb.fNull = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb))
        return GetLastError() == ERROR_INVALID_PARAMETER ? lx_set_errno(LX_EINVAL) : lx_fail_win32();
    return 0;
}

/* The tty a Linux serial driver presents before anyone configures it: 9600 8N1, cooked mode. */
static void linux_defaults(lx_termios *t)
{
    static const unsigned char controls[17] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0 };
    memset(t, 0, sizeof *t);
    t->iflag = 0x500;    /* ICRNL | IXON */
    t->oflag = 5;        /* OPOST | ONLCR */
    t->cflag = 0x4bd;    /* B9600 | CS8 | CREAD | HUPCL */
    t->lflag = 0x8a3b;   /* ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN */
    memcpy(t->cc, controls, sizeof controls);
    t->ispeed = t->ospeed = 13;
}

static wchar_t *port_name(const char *value)
{
    if (!strncmp(value, "\\\\.\\", 4))
        return lx_utf8_to_wide(value);
    size_t n = strlen(value);
    char *full = malloc(n + 5);
    memcpy(full, "\\\\.\\", 4);
    memcpy(full + 4, value, n + 1);
    wchar_t *w = lx_utf8_to_wide(full);
    free(full);
    return w;
}

int lx_serial_open(const char *path, int flags)
{
    unsigned cursor = 0;
    const char *key, *value;
    if (!path || !lx_config_next("serial", &cursor, &key, &value))
        return -2;
    char *abs = lx_absolute_path(path);
    const char *port = NULL;
    unsigned index = 0;
    for (cursor = 0; lx_config_next("serial", &cursor, &key, &value);)
        if (*value && !strcmp(key, abs)) {
            port = value;
            index = cursor - 1;
        }
    free(abs);
    if (!port)
        return -2;

    wchar_t *name = port_name(port);
    DWORD access = (flags & LX_O_ACCMODE) == LX_O_RDONLY ? GENERIC_READ
                 : (flags & LX_O_ACCMODE) == LX_O_WRONLY ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    HANDLE h = CreateFileW(name, access, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        free(name);
        int err = e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? LX_EBUSY : lx_errno_from_win32(e);
        /* The game re-probes closed readers continuously: report each mapping's failure once
         * per distinct error. */
        static DWORD reported[256];
        if (index >= LX_COUNT(reported) || InterlockedExchange((volatile LONG *)&reported[index], (LONG)e) != (LONG)e)
            lx_log("serial: open %s -> %s failed (Win32 %lu)", path, port, e);
        return lx_set_errno(err);
    }
    struct lx_serial *s = serial_new();
    if (!s) {
        CloseHandle(h);
        free(name);
        return lx_set_errno(LX_ENOMEM);
    }
    linux_defaults(&s->termios);
    SetupComm(h, 4096, 4096);
    if (apply(h, &s->termios) < 0) {
        int err = lx_errno;
        lx_log("serial: %s -> %s rejected the default line settings", path, port);
        lx_serial_free(s);
        CloseHandle(h);
        free(name);
        return lx_set_errno(err);
    }
    int fd = lx_fd_alloc(LX_FD_SERIAL, h, flags);
    if (fd < 0) {
        lx_serial_free(s);
        CloseHandle(h);
        free(name);
        return -1;
    }
    lx_fd *f = lx_fd_get(fd);
    f->serial = s;
    f->wpath = name;
    lx_log("serial: %s -> %s (fd %d)", path, port, fd);
    return fd;
}

/* Overlapped transfer on the per-direction event; waits for completion. */
static int transfer(HANDLE h, HANDLE event, int write, void *buf, DWORD n, DWORD *done)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = event;
    ResetEvent(event);
    BOOL ok = write ? WriteFile(h, buf, n, NULL, &ov) : ReadFile(h, buf, n, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING)
        return 0;
    return GetOverlappedResult(h, &ov, done, TRUE);
}

static int set_timeouts(lx_fd *f, DWORD interval, DWORD multiplier, DWORD constant)
{
    struct lx_serial *s = f->serial;
    COMMTIMEOUTS t = { interval, multiplier, constant, 0, 0 };
    if (s->timeouts_valid && !memcmp(&t, &s->timeouts, sizeof t))
        return 1;
    if (!SetCommTimeouts(f->h, &t))
        return 0;
    s->timeouts = t;
    s->timeouts_valid = 1;
    return 1;
}

static DWORD queued_bytes(HANDLE h, int *ok)
{
    COMSTAT st;
    DWORD errors;
    *ok = ClearCommError(h, &errors, &st);
    return *ok ? st.cbInQue : 0;
}

long lx_serial_read(lx_fd *f, void *buf, unsigned long n)
{
    struct lx_serial *s = f->serial;
    if ((f->flags & LX_O_ACCMODE) == LX_O_WRONLY)
        return lx_set_errno(LX_EBADF);
    if (!n)
        return 0;
    AcquireSRWLockExclusive(&s->read_lock);
    unsigned vmin = s->termios.cc[VMIN_], vtime = s->termios.cc[VTIME_];
    int nonblock = !!(f->flags & LX_O_NONBLOCK), ok;
    DWORD want = n > MAXDWORD ? MAXDWORD : (DWORD)n, got = 0;
    if (nonblock || (!vmin && !vtime)) {
        /* Return whatever is queued, possibly nothing. */
        ok = set_timeouts(f, MAXDWORD, 0, 0);
    } else if (!vmin) {
        /* Wait up to VTIME deciseconds for the first byte, then return what is queued. */
        ok = set_timeouts(f, MAXDWORD, MAXDWORD, vtime * 100);
    } else {
        /* At least VMIN bytes (or n, if smaller). Already-queued data beyond that is returned too;
         * VTIME > 0 adds the inter-byte timer that starts after the first byte. */
        DWORD queued = queued_bytes(f->h, &ok);
        if (ok && queued >= (vmin < want ? vmin : want)) {
            ok = set_timeouts(f, MAXDWORD, 0, 0);
        } else if (ok) {
            if (vmin < want)
                want = vmin;
            ok = set_timeouts(f, vtime ? vtime * 100 : 0, 0, 0);
        }
    }
    if (ok)
        ok = transfer(f->h, s->read_event, 0, buf, want, &got);
    ReleaseSRWLockExclusive(&s->read_lock);
    if (!ok) {
        DWORD e = GetLastError();
        return lx_set_errno(e == ERROR_OPERATION_ABORTED || e == ERROR_BAD_COMMAND || e == ERROR_DEVICE_NOT_CONNECTED
                                ? LX_EIO : lx_errno_from_win32(e));
    }
    if (!got && nonblock)
        return lx_set_errno(LX_EAGAIN);
    return (long)got;
}

long lx_serial_write(lx_fd *f, const void *buf, unsigned long n)
{
    struct lx_serial *s = f->serial;
    if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY)
        return lx_set_errno(LX_EBADF);
    if (!n)
        return 0;
    AcquireSRWLockExclusive(&s->write_lock);
    DWORD put = 0;
    int ok = transfer(f->h, s->write_event, 1, (void *)buf, n > MAXDWORD ? MAXDWORD : (DWORD)n, &put);
    ReleaseSRWLockExclusive(&s->write_lock);
    if (!ok) {
        DWORD e = GetLastError();
        return lx_set_errno(e == ERROR_OPERATION_ABORTED || e == ERROR_BAD_COMMAND || e == ERROR_DEVICE_NOT_CONNECTED
                                ? LX_EIO : lx_errno_from_win32(e));
    }
    return (long)put;
}

int lx_serial_tcgetattr(lx_fd *f, lx_termios *t)
{
    if (!t)
        return lx_set_errno(LX_EFAULT);
    *t = f->serial->termios;
    return 0;
}

/* Wait until the driver's transmit queue is empty. Not FlushFileBuffers: port drivers may reject
 * IRP_MJ_FLUSH_BUFFERS (the virtual null-modem pair used by tests/serial does). */
int lx_serial_tcdrain(lx_fd *f)
{
    for (;;) {
        COMSTAT st;
        DWORD errors;
        if (!ClearCommError(f->h, &errors, &st))
            return lx_fail_win32();
        if (!st.cbOutQue)
            return 0;
        Sleep(1);
    }
}

int lx_serial_tcflush(lx_fd *f, int queue)
{
    static const DWORD purge[3] = { PURGE_RXCLEAR, PURGE_TXCLEAR, PURGE_RXCLEAR | PURGE_TXCLEAR };
    if (queue < 0 || queue > 2)
        return lx_set_errno(LX_EINVAL);
    return PurgeComm(f->h, purge[queue]) ? 0 : lx_fail_win32();
}

int lx_serial_tcsetattr(lx_fd *f, int action, const lx_termios *t)
{
    if (action < 0 || action > 2)
        return lx_set_errno(LX_EINVAL);
    if (!t)
        return lx_set_errno(LX_EFAULT);
    if (action >= 1 && lx_serial_tcdrain(f) < 0)    /* TCSADRAIN, TCSAFLUSH */
        return -1;
    if (action == 2 && lx_serial_tcflush(f, 0) < 0)  /* TCSAFLUSH discards pending input */
        return -1;
    if (apply(f->h, t) < 0)
        return -1;
    lx_termios *cur = &f->serial->termios;
    *cur = *t;
    cur->ispeed = cur->ospeed = t->cflag & LX_CBAUD;
    return 0;
}

int lx_serial_queued(lx_fd *f)
{
    int ok;
    DWORD n = queued_bytes(f->h, &ok);
    if (!ok)
        return lx_fail_win32();
    return n > INT_MAX ? INT_MAX : (int)n;
}
