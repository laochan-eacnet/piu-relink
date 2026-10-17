/* Linux file descriptors over Win32 handles. fds 0-2 are the process std handles. */
#include "lxrt.h"

#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_FDS 4096
static lx_fd fds[MAX_FDS];
static SRWLOCK fd_lock = SRWLOCK_INIT;

static int kind_of_handle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return LX_FD_NULL;
    switch (GetFileType(h)) {
    case FILE_TYPE_CHAR: {
        DWORD mode;
        return GetConsoleMode(h, &mode) ? LX_FD_CONSOLE : LX_FD_FILE;
    }
    case FILE_TYPE_PIPE: return LX_FD_PIPE;
    default: return LX_FD_FILE;
    }
}

/* Without a console or redirection (GUI-subsystem EXE started from Explorer) stdout/stderr go to
 * "<exe>.log" beside the EXE, appended, so guest and lxrt diagnostics are never lost. */
static HANDLE open_session_log(void)
{
    wchar_t path[MAX_PATH + 8];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return INVALID_HANDLE_VALUE;
    wchar_t *dot = wcsrchr(path, L'.');
    wcscpy(dot && dot > wcsrchr(path, L'\\') ? dot : path + n, L".log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SYSTEMTIME t;
        char line[96];
        DWORD w;
        GetLocalTime(&t);
        int len = snprintf(line, sizeof line, "---- %04u-%02u-%02u %02u:%02u:%02u session start ----\n",
                           t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        WriteFile(h, line, (DWORD)len, &w, NULL);
    }
    return h;
}

void lx_fd_init(void)
{
    static const DWORD std[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    HANDLE log = NULL;
    SetConsoleOutputCP(CP_UTF8);
    for (int i = 0; i < 3; i++) {
        HANDLE h = GetStdHandle(std[i]);
        if (i && (!h || h == INVALID_HANDLE_VALUE)) {
            if (!log)
                log = open_session_log();
            if (log != INVALID_HANDLE_VALUE) {
                h = log;
                SetStdHandle(std[i], h);
            }
        }
        fds[i].kind = kind_of_handle(h);
        fds[i].h = h;
        fds[i].flags = i == 0 ? LX_O_RDONLY : LX_O_WRONLY;
    }
}

lx_fd *lx_fd_get(int fd)
{
    if (fd < 0 || fd >= MAX_FDS || fds[fd].kind == LX_FD_NONE) {
        lx_set_errno(LX_EBADF);
        return NULL;
    }
    return &fds[fd];
}

static int alloc_slot(void)
{
    for (int i = 0; i < MAX_FDS; i++)
        if (fds[i].kind == LX_FD_NONE)
            return i;
    return -1;
}

int lx_fd_alloc(int kind, HANDLE h, int flags)
{
    AcquireSRWLockExclusive(&fd_lock);
    int fd = alloc_slot();
    if (fd >= 0) {
        memset(&fds[fd], 0, sizeof fds[fd]);
        fds[fd].kind = kind;
        fds[fd].h = h;
        fds[fd].flags = flags;
        fds[fd].cloexec = !!(flags & LX_O_CLOEXEC);
    }
    ReleaseSRWLockExclusive(&fd_lock);
    if (fd < 0)
        return lx_set_errno(LX_EMFILE);
    return fd;
}

int lx_fd_alloc_mem(char *data, size_t len, int flags)
{
    int fd = lx_fd_alloc(LX_FD_MEM, NULL, flags);
    if (fd >= 0) {
        fds[fd].mem = data;
        fds[fd].memlen = len;
    } else {
        free(data);
    }
    return fd;
}

int lx_fd_install_socket(uintptr_t s)
{
    int fd = lx_fd_alloc(LX_FD_SOCKET, NULL, LX_O_RDWR);
    if (fd >= 0)
        fds[fd].sock = s;
    return fd;
}

int lx_open(const char *path, int flags, int mode)
{
    int special = lx_open_special(path, flags);
    if (special != -2)
        return special;
    int serial = lx_serial_open(path, flags);
    if (serial != -2)
        return serial;
    wchar_t *w = lx_map_path(path);
    if (!w)
        return -1;
    DWORD access = 0;
    switch (flags & LX_O_ACCMODE) {
    case LX_O_RDONLY: access = GENERIC_READ; break;
    case LX_O_WRONLY: access = GENERIC_WRITE; break;
    default: access = GENERIC_READ | GENERIC_WRITE; break;
    }
    DWORD disp;
    if ((flags & LX_O_CREAT) && (flags & LX_O_EXCL))
        disp = CREATE_NEW;
    else if ((flags & LX_O_CREAT) && (flags & LX_O_TRUNC))
        disp = CREATE_ALWAYS;
    else if (flags & LX_O_CREAT)
        disp = OPEN_ALWAYS;
    else if (flags & LX_O_TRUNC)
        disp = TRUNCATE_EXISTING;
    else
        disp = OPEN_EXISTING;
    DWORD attrs = GetFileAttributesW(w);
    int is_dir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    if ((flags & LX_O_DIRECTORY) && !is_dir) {
        free(w);
        int err = attrs == INVALID_FILE_ATTRIBUTES ? LX_ENOENT : LX_ENOTDIR;
        if (lx_trace || lx_trace_missing)
            lx_log_missing("opendir", path, err);
        return lx_set_errno(err);
    }
    if (is_dir && (flags & LX_O_ACCMODE) != LX_O_RDONLY) {
        free(w);
        return lx_set_errno(LX_EISDIR);
    }
    DWORD fattr = FILE_ATTRIBUTE_NORMAL;
    if (is_dir) {
        access = GENERIC_READ;
        fattr = FILE_FLAG_BACKUP_SEMANTICS;
    } else if ((flags & LX_O_CREAT) && !(mode & 0222)) {
        fattr = FILE_ATTRIBUTE_READONLY;
    }
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           disp, fattr, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        free(w);
        int err = e == ERROR_FILE_EXISTS ? LX_EEXIST : lx_errno_from_win32(e);
        if (lx_trace || lx_trace_missing)
            lx_log_missing("open", path, err);
        return lx_set_errno(err);
    }
    int fd = lx_fd_alloc(is_dir ? LX_FD_DIR : kind_of_handle(h), h, flags);
    if (fd < 0) {
        CloseHandle(h);
        free(w);
        return -1;
    }
    fds[fd].wpath = w;
    if (lx_trace)
        lx_log("open(%s, %#o) = %d", path, flags, fd);
    return fd;
}

long lx_read(int fd, void *buf, unsigned long n)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    DWORD got = 0;
    switch (f->kind) {
    case LX_FD_NULL:
        return 0;
    case LX_FD_ZERO:
        memset(buf, 0, n);
        return (long)n;
    case LX_FD_RANDOM:
        if (BCryptGenRandom(NULL, buf, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            return lx_set_errno(LX_EIO);
        return (long)n;
    case LX_FD_MEM: {
        size_t left = f->memlen - f->mempos;
        if (n > left)
            n = (unsigned long)left;
        memcpy(buf, f->mem + f->mempos, n);
        f->mempos += n;
        return (long)n;
    }
    case LX_FD_DIR:
        return lx_set_errno(LX_EISDIR);
    case LX_FD_SOCKET:
        return lx_socket_read(f->sock, buf, n);
    case LX_FD_SERIAL:
        return lx_serial_read(f, buf, n);
    case LX_FD_CONSOLE: {
        if (!ReadFile(f->h, buf, n, &got, NULL))
            return lx_fail_win32();
        /* CRLF -> LF for line-oriented console input */
        char *c = buf;
        DWORD o = 0;
        for (DWORD i = 0; i < got; i++)
            if (c[i] != '\r')
                c[o++] = c[i];
        return (long)o;
    }
    default:
        if (!ReadFile(f->h, buf, n, &got, NULL)) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF)
                return 0;
            return lx_set_errno(lx_errno_from_win32(e));
        }
        return (long)got;
    }
}

long lx_write(int fd, const void *buf, unsigned long n)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    DWORD put = 0;
    switch (f->kind) {
    case LX_FD_NULL:
    case LX_FD_ZERO:
    case LX_FD_RANDOM:
        return (long)n;
    case LX_FD_MEM:
    case LX_FD_DIR:
        return lx_set_errno(LX_EBADF);
    case LX_FD_SOCKET:
        return lx_socket_write(f->sock, buf, n);
    case LX_FD_SERIAL:
        return lx_serial_write(f, buf, n);
    default:
        if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY && fd > 2)
            return lx_set_errno(LX_EBADF);
        if (f->flags & LX_O_APPEND) {
            LARGE_INTEGER z = {0};
            SetFilePointerEx(f->h, z, NULL, FILE_END);
        }
        if (!WriteFile(f->h, buf, n, &put, NULL)) {
            DWORD e = GetLastError();
            return lx_set_errno(e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE ? LX_EPIPE : lx_errno_from_win32(e));
        }
        return (long)put;
    }
}

int lx_close(int fd)
{
    AcquireSRWLockExclusive(&fd_lock);
    if (fd < 0 || fd >= MAX_FDS || fds[fd].kind == LX_FD_NONE) {
        ReleaseSRWLockExclusive(&fd_lock);
        return lx_set_errno(LX_EBADF);
    }
    lx_fd f = fds[fd];
    memset(&fds[fd], 0, sizeof fds[fd]);
    ReleaseSRWLockExclusive(&fd_lock);
    switch (f.kind) {
    case LX_FD_MEM:
        free(f.mem);
        break;
    case LX_FD_SOCKET: {
        lx_socket_close(f.sock);
        break;
    }
    default:
        if (fd > 2 && f.h && f.h != INVALID_HANDLE_VALUE)
            CloseHandle(f.h);
    }
    lx_serial_free(f.serial);
    free(f.wpath);
    return 0;
}

int64_t lx_lseek64(int fd, int64_t off, int whence)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    if (whence < 0 || whence > 2)
        return lx_set_errno(LX_EINVAL);
    if (f->kind == LX_FD_MEM) {
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->mempos : (int64_t)f->memlen;
        if (base + off < 0)
            return lx_set_errno(LX_EINVAL);
        f->mempos = (size_t)(base + off);
        if (f->mempos > f->memlen)
            f->mempos = f->memlen;
        return base + off;
    }
    if (f->kind != LX_FD_FILE && f->kind != LX_FD_DIR)
        return lx_set_errno(LX_ESPIPE);
    LARGE_INTEGER d, r;
    d.QuadPart = off;
    if (!SetFilePointerEx(f->h, d, &r, whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END))
        return lx_fail_win32();
    return r.QuadPart;
}

/* ---- guest entry points ---------------------------------------------------------------- */

static int open_(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & LX_O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return lx_open(path, flags, mode);
}

static int creat_(const char *path, int mode)
{
    return lx_open(path, LX_O_CREAT | LX_O_WRONLY | LX_O_TRUNC, mode);
}

static long read_(int fd, void *b, unsigned long n) { return lx_read(fd, b, n); }
static long write_(int fd, const void *b, unsigned long n) { return lx_write(fd, b, n); }
static int close_(int fd) { return lx_close(fd); }

static long lseek_(int fd, long off, int whence)
{
    int64_t r = lx_lseek64(fd, off, whence);
    if (r > 0x7fffffff)
        return lx_set_errno(LX_EOVERFLOW);
    return (long)r;
}

static int64_t lseek64_(int fd, int64_t off, int whence)
{
    return lx_lseek64(fd, off, whence);
}

static long pread64_(int fd, void *buf, unsigned long n, int64_t off)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    if (f->kind != LX_FD_FILE)
        return lx_set_errno(LX_ESPIPE);
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got;
    if (!ReadFile(f->h, buf, n, &got, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF)
            return 0;
        return lx_fail_win32();
    }
    return (long)got;
}

static long pwrite64_(int fd, const void *buf, unsigned long n, int64_t off)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    if (f->kind != LX_FD_FILE)
        return lx_set_errno(LX_ESPIPE);
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD put;
    if (!WriteFile(f->h, buf, n, &put, &ov))
        return lx_fail_win32();
    return (long)put;
}

static long pread_(int fd, void *buf, unsigned long n, long off) { return pread64_(fd, buf, n, off); }
static long pwrite_(int fd, const void *buf, unsigned long n, long off) { return pwrite64_(fd, buf, n, off); }

typedef struct { void *iov_base; size_t iov_len; } lx_iovec;

static long writev_(int fd, const lx_iovec *iov, int cnt)
{
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        if (!iov[i].iov_len)
            continue;
        long r = lx_write(fd, iov[i].iov_base, (unsigned long)iov[i].iov_len);
        if (r < 0)
            return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

static long readv_(int fd, const lx_iovec *iov, int cnt)
{
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        long r = lx_read(fd, iov[i].iov_base, (unsigned long)iov[i].iov_len);
        if (r < 0)
            return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

static int dup_impl(int fd, int min, int exact)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    if (f->kind == LX_FD_SOCKET)
        return lx_set_errno(LX_EOPNOTSUPP); /* sockets have no refcounted duplicate; unused by piu */
    HANDLE h = f->h;
    if (h && h != INVALID_HANDLE_VALUE &&
        !DuplicateHandle(GetCurrentProcess(), f->h, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return lx_fail_win32();
    struct lx_serial *serial = NULL;
    if (f->kind == LX_FD_SERIAL && !(serial = lx_serial_dup(f->serial))) {
        CloseHandle(h);
        return lx_set_errno(LX_ENOMEM);
    }
    AcquireSRWLockExclusive(&fd_lock);
    int nfd = -1;
    if (exact >= 0) {
        nfd = exact;
    } else {
        for (int i = min; i < MAX_FDS; i++)
            if (fds[i].kind == LX_FD_NONE) {
                nfd = i;
                break;
            }
    }
    lx_fd old = {0};
    if (nfd >= 0) {
        old = fds[nfd];
        fds[nfd] = *f;
        fds[nfd].h = h;
        fds[nfd].cloexec = 0;
        fds[nfd].wpath = f->wpath ? _wcsdup(f->wpath) : NULL;
        if (f->kind == LX_FD_MEM) {
            fds[nfd].mem = malloc(f->memlen ? f->memlen : 1);
            memcpy(fds[nfd].mem, f->mem, f->memlen);
        }
        fds[nfd].serial = serial;
    }
    ReleaseSRWLockExclusive(&fd_lock);
    if (nfd < 0) {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        lx_serial_free(serial);
        return lx_set_errno(LX_EMFILE);
    }
    if (old.kind != LX_FD_NONE) {
        if (old.h && old.h != INVALID_HANDLE_VALUE && nfd > 2)
            CloseHandle(old.h);
        free(old.mem);
        free(old.wpath);
        lx_serial_free(old.serial);
    }
    if (nfd <= 2) {
        static const DWORD std[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
        SetStdHandle(std[nfd], h);
    }
    return nfd;
}

static int dup_(int fd) { return dup_impl(fd, 0, -1); }

static int dup2_(int fd, int nfd)
{
    if (nfd < 0 || nfd >= MAX_FDS)
        return lx_set_errno(LX_EBADF);
    if (fd == nfd)
        return lx_fd_get(fd) ? nfd : -1;
    return dup_impl(fd, 0, nfd);
}

enum { F_DUPFD = 0, F_GETFD = 1, F_SETFD = 2, F_GETFL = 3, F_SETFL = 4, F_GETLK = 5, F_SETLK = 6,
       F_SETLKW = 7, F_GETLK64 = 12, F_SETLK64 = 13, F_SETLKW64 = 14, F_DUPFD_CLOEXEC = 1030 };

static int fcntl_(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int r = dup_impl(fd, (int)arg, -1);
        if (r >= 0 && cmd == F_DUPFD_CLOEXEC)
            fds[r].cloexec = 1;
        return r;
    }
    case F_GETFD: return f->cloexec ? 1 : 0;
    case F_SETFD: f->cloexec = (int)(arg & 1); return 0;
    case F_GETFL: return f->flags & ~LX_O_CLOEXEC;
    case F_SETFL:
        f->flags = (f->flags & LX_O_ACCMODE) | ((int)arg & (LX_O_APPEND | LX_O_NONBLOCK));
        if (f->kind == LX_FD_SOCKET) {
            lx_socket_set_nonblock(f->sock, !!(arg & LX_O_NONBLOCK));
        }
        return 0;
    case F_GETLK:
    case F_GETLK64: {
        /* report "unlocked": l_type = F_UNLCK (2) at offset 0 of struct flock(64) */
        short *l_type = (short *)arg;
        *l_type = 2;
        return 0;
    }
    case F_SETLK:
    case F_SETLKW:
    case F_SETLK64:
    case F_SETLKW64:
        return 0; /* advisory locks: single game process */
    default:
        return lx_set_errno(LX_EINVAL);
    }
}

static int isatty_(int fd)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return 0;
    if (f->kind != LX_FD_CONSOLE && f->kind != LX_FD_SERIAL)
        return lx_set_errno(LX_ENOTTY), 0;
    return 1;
}

static int fsync_(int fd)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    if (f->kind == LX_FD_FILE)
        FlushFileBuffers(f->h);
    return 0;
}

static int pipe_(int p[2])
{
    HANDLE r, w;
    if (!CreatePipe(&r, &w, NULL, 65536))
        return lx_fail_win32();
    p[0] = lx_fd_alloc(LX_FD_PIPE, r, LX_O_RDONLY);
    p[1] = lx_fd_alloc(LX_FD_PIPE, w, LX_O_WRONLY);
    return 0;
}

static int pipe2_(int p[2], int flags)
{
    (void)flags;
    return pipe_(p);
}

static int ftruncate64_(int fd, int64_t len)
{
    lx_fd *f = lx_fd_get(fd);
    if (!f)
        return -1;
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = len;
    if (!SetFileInformationByHandle(f->h, FileEndOfFileInfo, &info, sizeof info))
        return lx_fail_win32();
    return 0;
}

static int ftruncate_(int fd, long len) { return ftruncate64_(fd, len); }

static void sync_(void) {}

LX_SYMTAB(fd,
    LX_SYM("open", open_),
    LX_SYM("open64", open_),
    LX_SYM("__open_2", open_),
    LX_SYM("__open64_2", open_),
    LX_SYM("creat", creat_),
    LX_SYM("creat64", creat_),
    LX_SYM("read", read_),
    LX_SYM("__read_chk", read_),
    LX_SYM("write", write_),
    LX_SYM("close", close_),
    LX_SYM("lseek", lseek_),
    LX_SYM("lseek64", lseek64_),
    LX_SYM("pread", pread_),
    LX_SYM("pread64", pread64_),
    LX_SYM("pwrite", pwrite_),
    LX_SYM("pwrite64", pwrite64_),
    LX_SYM("readv", readv_),
    LX_SYM("writev", writev_),
    LX_SYM("dup", dup_),
    LX_SYM("dup2", dup2_),
    LX_SYM("fcntl", fcntl_),
    LX_SYM("fcntl64", fcntl_),
    LX_SYM("isatty", isatty_),
    LX_SYM("fsync", fsync_),
    LX_SYM("fdatasync", fsync_),
    LX_SYM("pipe", pipe_),
    LX_SYM("pipe2", pipe2_),
    LX_SYM("ftruncate", ftruncate_),
    LX_SYM("ftruncate64", ftruncate64_),
    LX_SYM("sync", sync_),
);
