/* glibc byte streams over lxrt descriptors. No UCRT FILE crosses the guest ABI.
 * FILE is deliberately opaque (audited guests do not inspect glibc FILE fields).
 * Normal, line and unbuffered modes share descriptor positioning and pushback;
 * growable streams publish lx_malloc-owned storage at fflush/fclose.
 * popen/pclose report ENOSYS: cmd.exe does not implement Linux shell semantics. */
#include "lxstdio.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define LX_EOF (-1)
#define STREAM_BUFSIZ 8192
#define WEOF32 UINT32_MAX

struct lx_FILE {
    CRITICAL_SECTION lock;
    struct lx_FILE *next;
    int fd, readable, writable, append, mode, direction, eof, error, orientation;
    unsigned char *buf;
    size_t cap, rpos, rend, wlen;
    int owns_buf;
    unsigned char small[1];
    unsigned char *push;
    size_t npush, pushcap;
    unsigned char pushsmall[16];
    int memory, owns_data;
    unsigned char *data;
    size_t data_cap, data_len, data_pos;
    size_t saved_pos;
    unsigned char saved_byte;
    int saved_valid;
    char **publish_data;
    size_t *publish_size;
};

lx_FILE *lx_stdin_storage, *lx_stdout_storage, *lx_stderr_storage;
static lx_FILE *streams;
static SRWLOCK streams_lock = SRWLOCK_INIT;

void lx_flockfile(lx_FILE *f) { EnterCriticalSection(&f->lock); }
void lx_funlockfile(lx_FILE *f) { LeaveCriticalSection(&f->lock); }
static int ftrylockfile_(lx_FILE *f) { return TryEnterCriticalSection(&f->lock) ? 0 : 1; }

static lx_FILE *new_stream(int fd, int rd, int wr, int append)
{
    lx_FILE *f = lx_calloc(1, sizeof *f);
    if (!f) return NULL;
    InitializeCriticalSection(&f->lock);
    f->fd = fd; f->readable = rd; f->writable = wr; f->append = append;
    f->mode = 0; f->buf = f->small; f->cap = 1;
    f->push = f->pushsmall; f->pushcap = sizeof f->pushsmall;
    lx_fd *d = fd >= 0 ? lx_fd_get(fd) : NULL;
    if (d && d->kind == LX_FD_CONSOLE) f->mode = 1;
    AcquireSRWLockExclusive(&streams_lock);
    f->next = streams; streams = f;
    ReleaseSRWLockExclusive(&streams_lock);
    return f;
}

static void ensure_buffer(lx_FILE *f)
{
    if (f->buf == f->small && f->mode != 2) {
        unsigned char *b = lx_malloc(STREAM_BUFSIZ);
        if (b) { f->buf = b; f->cap = STREAM_BUFSIZ; f->owns_buf = 1; }
    }
}

static int parse_mode(const char *s, int *flags, int *rd, int *wr)
{
    if (!s || !*s) return lx_set_errno(LX_EINVAL);
    *rd = *s == 'r'; *wr = *s == 'w' || *s == 'a';
    if (!*rd && !*wr) return lx_set_errno(LX_EINVAL);
    *flags = *s == 'w' ? LX_O_CREAT | LX_O_TRUNC : *s == 'a' ? LX_O_CREAT | LX_O_APPEND : 0;
    for (const char *p = s + 1; *p && *p != ','; ++p) {
        if (*p == '+') *rd = *wr = 1;
        else if (*p == 'x') *flags |= LX_O_EXCL;
        else if (*p == 'e') *flags |= LX_O_CLOEXEC;
        /* b, c and m are accepted glibc mode modifiers. */
    }
    *flags |= *rd && *wr ? LX_O_RDWR : *wr ? LX_O_WRONLY : LX_O_RDONLY;
    return 0;
}

static void restore_memstream(lx_FILE *f)
{
    if (f->saved_valid) {
        f->data[f->saved_pos] = f->saved_byte;
        f->saved_valid = 0;
    }
}

static void publish(lx_FILE *f)
{
    if (f->memory == 2) {
        restore_memstream(f);
        f->saved_pos = f->data_pos;
        f->saved_byte = f->data[f->data_pos];
        f->saved_valid = 1;
        f->data[f->data_pos] = 0;
        *f->publish_data = (char *)f->data;
        *f->publish_size = f->data_pos;
    }
}

static long backend_read(lx_FILE *f, void *p, size_t n)
{
    if (n > INT32_MAX) n = INT32_MAX;
    if (!f->memory) return lx_read(f->fd, p, (unsigned long)n);
    size_t left = f->data_pos < f->data_len ? f->data_len - f->data_pos : 0;
    if (n > left) n = left;
    if (n) memcpy(p, f->data + f->data_pos, n);
    f->data_pos += n;
    return (long)n;
}

static long backend_write(lx_FILE *f, const void *p, size_t n)
{
    if (n > INT32_MAX) n = INT32_MAX;
    if (!f->memory) return lx_write(f->fd, p, (unsigned long)n);
    if (f->append) f->data_pos = f->data_len;
    if (f->memory == 2) {
        restore_memstream(f);
        if (n > SIZE_MAX - f->data_pos - 1) return lx_set_errno(LX_ENOMEM);
        size_t need = f->data_pos + n + 1;
        if (need > f->data_cap) {
            size_t cap = f->data_cap;
            while (cap < need) { if (cap > SIZE_MAX / 2) { cap = need; break; } cap *= 2; }
            unsigned char *b = lx_realloc(f->data, cap);
            if (!b) return -1;
            f->data = b; f->data_cap = cap;
        }
        if (f->data_pos > f->data_len) memset(f->data + f->data_len, 0, f->data_pos - f->data_len);
    } else {
        size_t left = f->data_pos < f->data_cap ? f->data_cap - f->data_pos : 0;
        if (n > left) {
            if (left <= 1) return lx_set_errno(LX_ENOSPC);
            n = left;
        }
        if (!n) return lx_set_errno(LX_ENOSPC);
    }
    memcpy(f->data + f->data_pos, p, n);
    f->data_pos += n;
    if (f->data_pos > f->data_len) {
        f->data_len = f->data_pos;
        if (f->memory == 2) f->data[f->data_len] = 0;
    }
    if (f->memory == 1 && f->data_cap) {
        size_t at = f->data_pos < f->data_cap ? f->data_pos : f->data_cap - 1;
        if (f->data_pos >= f->data_len) f->data[at] = 0;
    }
    return (long)n;
}

static int64_t backend_seek(lx_FILE *f, int64_t off, int whence)
{
    if (!f->memory) return lx_lseek64(f->fd, off, whence);
    if (whence < 0 || whence > 2) return lx_set_errno(LX_EINVAL);
    if (f->memory == 2) restore_memstream(f);
    int64_t base = whence == 0 ? 0 : whence == 1 || f->memory == 2 ? (int64_t)f->data_pos : (int64_t)f->data_len;
    if ((off < 0 && off < -base) || (off > 0 && off > INT64_MAX - base)) return lx_set_errno(LX_EINVAL);
    int64_t pos = base + off;
    if (pos < 0 || (uint64_t)pos >= SIZE_MAX || (f->memory == 1 && (uint64_t)pos > f->data_cap))
        return lx_set_errno(LX_EINVAL);
    /* Dynamic streams can seek beyond their currently allocated buffer. */
    if (f->memory == 2 && (uint64_t)pos >= f->data_cap) {
        unsigned char *b = lx_realloc(f->data, (size_t)pos + 1);
        if (!b) return -1;
        memset(b + f->data_cap, 0, (size_t)pos + 1 - f->data_cap);
        f->data = b; f->data_cap = (size_t)pos + 1;
    }
    f->data_pos = (size_t)pos;
    return pos;
}

static int flush_write(lx_FILE *f)
{
    size_t done = 0;
    while (done < f->wlen) {
        long n = backend_write(f, f->buf + done, f->wlen - done);
        if (n <= 0) {
            if (!n) lx_set_errno(LX_EIO);
            f->error = 1;
            f->wlen = 0;
            return LX_EOF;
        }
        done += n;
        if (f->memory == 1 && done < f->wlen) {
            /* A short fmemopen cookie write is an error, not a retry: glibc
             * preserves errno when some bytes fit into its fixed buffer. */
            f->error = 1; f->wlen = 0; return LX_EOF;
        }
    }
    f->wlen = 0;
    if (f->memory == 2) publish(f);
    return 0;
}

static int discard_read(lx_FILE *f)
{
    size_t unread = f->rend - f->rpos + f->npush;
    if (unread && backend_seek(f, -(int64_t)unread, 1) < 0) return -1;
    f->rpos = f->rend = f->npush = 0;
    return 0;
}

static int fflush_unlocked_(lx_FILE *f)
{
    if (!f) {
        int result = 0;
        AcquireSRWLockShared(&streams_lock);
        for (lx_FILE *s = streams; s; s = s->next) {
            lx_flockfile(s);
            if (s->direction == 2 && flush_write(s) < 0) result = LX_EOF;
            lx_funlockfile(s);
        }
        ReleaseSRWLockShared(&streams_lock);
        return result;
    }
    if (f->direction == 2) return flush_write(f);
    if (f->direction == 1) {
        int saved = lx_errno;
        if (discard_read(f) < 0 && lx_errno != LX_ESPIPE) return LX_EOF;
        lx_errno = saved;
    }
    if (f->memory == 2) publish(f);
    return 0;
}
static int fflush_(lx_FILE *f)
{
    if (!f) return fflush_unlocked_(NULL);
    lx_flockfile(f); int r = fflush_unlocked_(f); lx_funlockfile(f); return r;
}
void lx_stdio_flush_all(void) { fflush_(NULL); }
void lx_stdio_init(void)
{
    lx_stdin = new_stream(0, 1, 0, 0);
    lx_stdout = new_stream(1, 0, 1, 0);
    lx_stderr = new_stream(2, 0, 1, 0);
    if (!lx_stdin || !lx_stdout || !lx_stderr) lx_fatal("cannot allocate standard streams");
    lx_stderr->mode = 2;
}

static int prepare_read(lx_FILE *f)
{
    if (!f->readable) { f->error = 1; return lx_set_errno(LX_EBADF); }
    if (f->direction == 2 && flush_write(f) < 0) return -1;
    f->direction = 1;
    if (!f->orientation) f->orientation = -1;
    return 0;
}
static int prepare_write(lx_FILE *f)
{
    if (!f->writable) { f->error = 1; return lx_set_errno(LX_EBADF); }
    if (f->direction == 1 && discard_read(f) < 0) {
        /* A consumed pipe needs no positioning; unread pipe input cannot be lost. */
        f->error = 1; return -1;
    }
    f->direction = 2;
    if (!f->orientation) f->orientation = -1;
    return 0;
}

int lx_fgetc_unlocked(lx_FILE *f)
{
    if (prepare_read(f) < 0) return LX_EOF;
    if (f->npush) return f->push[--f->npush];
    if (f->rpos == f->rend) {
        if (f->eof) return LX_EOF;
        ensure_buffer(f);
        long n = backend_read(f, f->buf, f->cap);
        if (n <= 0) { if (n < 0) f->error = 1; else f->eof = 1; return LX_EOF; }
        f->rpos = 0; f->rend = (size_t)n;
    }
    return f->buf[f->rpos++];
}
static int fgetc_(lx_FILE *f) { lx_flockfile(f); int c = lx_fgetc_unlocked(f); lx_funlockfile(f); return c; }
static int getchar_(void) { return fgetc_(lx_stdin); }
static int getchar_unlocked_(void) { return lx_fgetc_unlocked(lx_stdin); }
int lx_ungetc_unlocked(int c, lx_FILE *f)
{
    if (c == LX_EOF) return LX_EOF;
    if (prepare_read(f) < 0) return LX_EOF;
    if (f->npush == f->pushcap) {
        size_t cap = f->pushcap * 2;
        unsigned char *p = lx_malloc(cap);
        if (!p) return LX_EOF;
        memcpy(p, f->push, f->npush);
        if (f->push != f->pushsmall) lx_free(f->push);
        f->push = p; f->pushcap = cap;
    }
    f->push[f->npush++] = (unsigned char)c;
    f->eof = 0;
    return (unsigned char)c;
}
static int ungetc_(int c, lx_FILE *f) { lx_flockfile(f); int r = lx_ungetc_unlocked(c, f); lx_funlockfile(f); return r; }

static size_t fread_unlocked_(void *p, size_t size, size_t count, lx_FILE *f)
{
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) { lx_set_errno(LX_EOVERFLOW); f->error = 1; return 0; }
    if (prepare_read(f) < 0) return 0;
    ensure_buffer(f);
    size_t total = size * count, done = 0;
    unsigned char *dst = p;
    while (done < total) {
        if (f->npush) { dst[done++] = f->push[--f->npush]; continue; }
        size_t avail = f->rend - f->rpos;
        if (avail) {
            if (avail > total - done) avail = total - done;
            memcpy(dst + done, f->buf + f->rpos, avail); f->rpos += avail; done += avail;
        } else if (!f->eof && total - done >= f->cap) {
            size_t n = total - done;
            if (n > INT32_MAX) n = INT32_MAX;
            if (n >= f->cap) n -= n % f->cap;
            long got = backend_read(f, dst + done, n);
            if (got <= 0) { if (got < 0) f->error = 1; else f->eof = 1; break; }
            done += (size_t)got;
        } else {
            int c = lx_fgetc_unlocked(f);
            if (c == LX_EOF) break;
            dst[done++] = (unsigned char)c;
        }
    }
    return done / size;
}
static size_t fread_(void *p, size_t s, size_t n, lx_FILE *f)
{ lx_flockfile(f); size_t r = fread_unlocked_(p, s, n, f); lx_funlockfile(f); return r; }

size_t lx_fwrite_unlocked(const void *p, size_t size, size_t count, lx_FILE *f)
{
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) { lx_set_errno(LX_EOVERFLOW); f->error = 1; return 0; }
    if (prepare_write(f) < 0) return 0;
    ensure_buffer(f);
    size_t total = size * count, done = 0;
    const unsigned char *src = p;
    if (f->mode == 2) {
        while (done < total) {
            long n = backend_write(f, src + done, total - done);
            if (n <= 0) { f->error = 1; break; }
            done += n;
            if (f->memory == 1 && done < total) { f->error = 1; break; }
        }
        return done / size;
    }
    while (done < total) {
        if (f->wlen == f->cap && flush_write(f) < 0) break;
        if (!f->wlen && f->mode == 0 && total - done >= f->cap) {
            size_t n = total - done;
            if (n > INT32_MAX) n = INT32_MAX;
            if (n >= f->cap) n -= n % f->cap;
            long wrote = backend_write(f, src + done, n);
            if (wrote <= 0) { f->error = 1; break; }
            done += (size_t)wrote;
            if (f->memory == 1 && (size_t)wrote < n) { f->error = 1; break; }
            continue;
        }
        size_t n = f->cap - f->wlen;
        if (n > total - done) n = total - done;
        if (f->mode == 1) {
            const unsigned char *end = memchr(src + done, '\n', n);
            if (end) n = (size_t)(end - src - done) + 1;
        }
        memcpy(f->buf + f->wlen, src + done, n); f->wlen += n; done += n;
        if (f->mode == 1 && src[done - 1] == '\n' && flush_write(f) < 0) break;
    }
    return done / size;
}
static size_t fwrite_(const void *p, size_t s, size_t n, lx_FILE *f)
{ lx_flockfile(f); size_t r = lx_fwrite_unlocked(p, s, n, f); lx_funlockfile(f); return r; }
int lx_fputc_unlocked(int c, lx_FILE *f)
{ unsigned char b = (unsigned char)c; return lx_fwrite_unlocked(&b, 1, 1, f) == 1 ? b : LX_EOF; }
static int fputc_(int c, lx_FILE *f) { lx_flockfile(f); int r = lx_fputc_unlocked(c, f); lx_funlockfile(f); return r; }
static int putchar_(int c) { return fputc_(c, lx_stdout); }
static int putchar_unlocked_(int c) { return lx_fputc_unlocked(c, lx_stdout); }
static char *fgets_unlocked_(char *s, int n, lx_FILE *f)
{
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = lx_fgetc_unlocked(f);
        if (c < 0) {
            if (f->error && !f->eof && lx_errno != LX_EAGAIN) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i && n > 1) return NULL;
    s[i] = 0; return s;
}
static char *fgets_(char *s, int n, lx_FILE *f)
{ lx_flockfile(f); char *r = fgets_unlocked_(s, n, f); lx_funlockfile(f); return r; }
static int fputs_unlocked_(const char *s, lx_FILE *f)
{ size_t n = strlen(s); return lx_fwrite_unlocked(s, 1, n, f) == n ? 1 : LX_EOF; }
static int fputs_(const char *s, lx_FILE *f)
{ lx_flockfile(f); int r = fputs_unlocked_(s, f); lx_funlockfile(f); return r; }
static int puts_(const char *s)
{
    lx_flockfile(lx_stdout);
    int r = fputs_unlocked_(s, lx_stdout) < 0 || lx_fputc_unlocked('\n', lx_stdout) < 0 ? LX_EOF : 1;
    lx_funlockfile(lx_stdout); return r;
}

static lx_FILE *fdopen_(int fd, const char *mode)
{
    int flags, rd, wr;
    if (parse_mode(mode, &flags, &rd, &wr) < 0) return NULL;
    lx_fd *d = lx_fd_get(fd);
    if (!d) return NULL;
    int access = d->flags & LX_O_ACCMODE;
    if ((rd && access == LX_O_WRONLY) || (wr && access == LX_O_RDONLY)) { lx_set_errno(LX_EINVAL); return NULL; }
    if (flags & LX_O_APPEND) d->flags |= LX_O_APPEND;
    lx_FILE *f = new_stream(fd, rd, wr, !!(flags & LX_O_APPEND));
    if (f && f->append && !rd) { int saved = lx_errno; lx_lseek64(fd, 0, 2); lx_errno = saved; }
    return f;
}
static lx_FILE *fopen_(const char *path, const char *mode)
{
    int flags, rd, wr;
    if (parse_mode(mode, &flags, &rd, &wr) < 0) return NULL;
    int fd = lx_open(path, flags | LX_O_LARGEFILE, 0666);
    if (fd < 0) return NULL;
    lx_FILE *f = new_stream(fd, rd, wr, !!(flags & LX_O_APPEND));
    if (!f) lx_close(fd);
    if (f && f->append && !rd) lx_lseek64(fd, 0, 2);
    return f;
}
static int fileno_unlocked_(lx_FILE *f) { return f->fd < 0 ? lx_set_errno(LX_EBADF) : f->fd; }
static int fileno_(lx_FILE *f) { lx_flockfile(f); int r = fileno_unlocked_(f); lx_funlockfile(f); return r; }
static int fclose_(lx_FILE *f)
{
    AcquireSRWLockExclusive(&streams_lock);
    lx_FILE **p = &streams;
    while (*p && *p != f) p = &(*p)->next;
    if (*p) *p = f->next;
    lx_flockfile(f);
    ReleaseSRWLockExclusive(&streams_lock);
    int r = f->direction == 2 ? flush_write(f) : 0;
    if (f->memory == 2) publish(f);
    if (f->fd >= 0 && lx_close(f->fd) < 0) r = LX_EOF;
    if (f->owns_buf) lx_free(f->buf);
    if (f->owns_data) lx_free(f->data);
    if (f->push != f->pushsmall) lx_free(f->push);
    lx_funlockfile(f); DeleteCriticalSection(&f->lock); lx_free(f);
    return r;
}
static lx_FILE *freopen_(const char *path, const char *mode, lx_FILE *f)
{
    int flags, rd, wr;
    if (parse_mode(mode, &flags, &rd, &wr) < 0) return NULL;
    lx_flockfile(f);
    fflush_unlocked_(f);
    int fd;
    if (!path) {
        lx_fd *d = lx_fd_get(f->fd);
        if (!d || !d->wpath) { lx_funlockfile(f); lx_set_errno(LX_EINVAL); return NULL; }
        char *utf = lx_guest_path(d->wpath);
        if (!utf) { lx_funlockfile(f); return NULL; }
        fd = lx_open(utf, flags | LX_O_LARGEFILE, 0666);
        free(utf);
    } else fd = lx_open(path, flags | LX_O_LARGEFILE, 0666);
    int oldfd = f->fd;
    if (fd >= 0 && oldfd >= 0 && oldfd != fd) {
        int found;
        int (*dup2_fn)(int, int) = (int (*)(int, int))lx_native_lookup("dup2", NULL, &found);
        if (!found || dup2_fn(fd, oldfd) < 0) {
            int e = lx_errno;
            lx_close(fd); fd = -1; lx_errno = e;
        } else { lx_close(fd); fd = oldfd; }
    }
    if (fd < 0 && oldfd >= 0) lx_close(oldfd);
    f->fd = fd;
    if (fd < 0) { f->error = 1; lx_funlockfile(f); return NULL; }
    if (f->owns_data) lx_free(f->data);
    f->data = NULL; f->owns_data = f->memory = 0;
    f->readable = rd; f->writable = wr; f->append = !!(flags & LX_O_APPEND);
    f->rpos = f->rend = f->wlen = f->npush = 0;
    f->direction = f->orientation = f->eof = f->error = 0;
    if (f->append && !rd) lx_lseek64(fd, 0, 2);
    lx_funlockfile(f); return f;
}

static int setvbuf_(lx_FILE *f, char *buf, int mode, size_t n)
{
    if (mode < 0 || mode > 2) return lx_set_errno(LX_EINVAL);
    lx_flockfile(f);
    if (fflush_unlocked_(f) < 0) { lx_funlockfile(f); return -1; }
    unsigned char *b = (unsigned char *)buf;
    int own = 0;
    if (mode == 2) { b = f->small; n = 1; }
    else if (!b) { if (!n) n = STREAM_BUFSIZ; b = lx_malloc(n); own = 1; }
    else if (!n) { lx_funlockfile(f); return lx_set_errno(LX_EINVAL); }
    if (!b) { lx_funlockfile(f); return -1; }
    if (f->owns_buf) lx_free(f->buf);
    f->buf = b; f->cap = n; f->owns_buf = own; f->mode = mode;
    f->rpos = f->rend = f->wlen = 0;
    lx_funlockfile(f); return 0;
}
static void setbuf_(lx_FILE *f, char *b) { setvbuf_(f, b, b ? 0 : 2, STREAM_BUFSIZ); }
static void setbuffer_(lx_FILE *f, char *b, size_t n) { setvbuf_(f, b, b ? 0 : 2, n); }
static void setlinebuf_(lx_FILE *f) { setvbuf_(f, NULL, 1, 0); }

static int64_t ftello64_unlocked(lx_FILE *f)
{
    int64_t p = backend_seek(f, 0, f->append && f->direction == 2 && f->wlen ? 2 : 1);
    if (p < 0) return -1;
    if (f->direction == 1) p -= (int64_t)(f->rend - f->rpos + f->npush);
    if (f->direction == 2) p += f->wlen;
    return p;
}
static int64_t ftello64_(lx_FILE *f) { lx_flockfile(f); int64_t p = ftello64_unlocked(f); lx_funlockfile(f); return p; }
static long ftell_(lx_FILE *f)
{
    int64_t p = ftello64_(f);
    if (p > INT32_MAX || p < INT32_MIN) return lx_set_errno(LX_EOVERFLOW);
    return (long)p;
}
static int fseeko64_(lx_FILE *f, int64_t off, int whence)
{
    lx_flockfile(f);
    if (f->direction == 2 && flush_write(f) < 0) { lx_funlockfile(f); return -1; }
    if (whence == 1 && f->direction == 1) off -= (int64_t)(f->rend - f->rpos + f->npush);
    if (backend_seek(f, off, whence) < 0) { lx_funlockfile(f); return -1; }
    f->rpos = f->rend = f->wlen = f->npush = 0; f->eof = 0; f->direction = 0;
    lx_funlockfile(f); return 0;
}
static int fseek_(lx_FILE *f, long off, int whence) { return fseeko64_(f, off, whence); }
static void rewind_(lx_FILE *f) { lx_flockfile(f); fseeko64_(f, 0, 0); f->error = 0; lx_funlockfile(f); }
#pragma pack(push, 4)
typedef struct { int32_t pos; int32_t state[2]; } lx_fpos;
typedef struct { int64_t pos; int32_t state[2]; } lx_fpos64;
#pragma pack(pop)
static_assert(sizeof(lx_fpos) == 12, "i386 fpos_t");
static_assert(sizeof(lx_fpos64) == 16, "i386 fpos64_t");
static int fgetpos_(lx_FILE *f, lx_fpos *p) { long n = ftell_(f); if (n < 0) return -1; p->pos = n; p->state[0] = p->state[1] = 0; return 0; }
static int fgetpos64_(lx_FILE *f, lx_fpos64 *p) { int64_t n = ftello64_(f); if (n < 0) return -1; p->pos = n; p->state[0] = p->state[1] = 0; return 0; }
static int fsetpos_(lx_FILE *f, const lx_fpos *p) { return fseeko64_(f, p->pos, 0); }
static int fsetpos64_(lx_FILE *f, const lx_fpos64 *p) { return fseeko64_(f, p->pos, 0); }
static int feof_unlocked_(lx_FILE *f) { return f->eof != 0; }
int lx_ferror_unlocked(lx_FILE *f) { return f->error != 0; }
static void clearerr_unlocked_(lx_FILE *f) { f->eof = f->error = 0; }
static int feof_(lx_FILE *f) { lx_flockfile(f); int r = feof_unlocked_(f); lx_funlockfile(f); return r; }
static int ferror_(lx_FILE *f) { lx_flockfile(f); int r = lx_ferror_unlocked(f); lx_funlockfile(f); return r; }
static void clearerr_(lx_FILE *f) { lx_flockfile(f); clearerr_unlocked_(f); lx_funlockfile(f); }

static long getdelim_(char **line, size_t *size, int delim, lx_FILE *f)
{
    if (!line || !size) return lx_set_errno(LX_EINVAL);
    lx_flockfile(f);
    if (!*line || !*size) {
        char *p = lx_malloc(120);
        if (!p) { lx_funlockfile(f); return -1; }
        *line = p; *size = 120;
    }
    size_t n = 0;
    for (;;) {
        int c = lx_fgetc_unlocked(f);
        if (c < 0) break;
        if (n + 1 >= *size) {
            if (*size > INT32_MAX / 2) { lx_set_errno(LX_EOVERFLOW); f->error = 1; n = 0; break; }
            size_t cap = *size * 2;
            char *p = lx_realloc(*line, cap);
            if (!p) { n = 0; break; }
            *line = p; *size = cap;
        }
        (*line)[n++] = (char)c;
        if (c == (unsigned char)delim) break;
    }
    (*line)[n] = 0;
    lx_funlockfile(f); return n ? (long)n : -1;
}
static long getline_(char **p, size_t *n, lx_FILE *f) { return getdelim_(p, n, '\n', f); }
static lx_FILE *fmemopen_(void *buf, size_t size, const char *mode)
{
    int flags, rd, wr;
    if (parse_mode(mode, &flags, &rd, &wr) < 0) return NULL;
    if (size > INT32_MAX) { lx_set_errno(LX_EINVAL); return NULL; }
    lx_FILE *f = new_stream(-1, rd, wr, !!(flags & LX_O_APPEND));
    if (!f) return NULL;
    f->memory = 1; f->data_cap = size;
    f->data = buf ? buf : lx_calloc(size ? size : 1, 1);
    f->owns_data = !buf;
    if (!f->data) { fclose_(f); return NULL; }
    if (flags & LX_O_TRUNC) { f->data_len = 0; f->data[0] = 0; }
    else if (flags & LX_O_APPEND) {
        unsigned char *end = memchr(f->data, 0, size);
        f->data_len = end ? (size_t)(end - f->data) : size;
        f->data_pos = f->data_len;
    } else f->data_len = size;
    return f;
}
static lx_FILE *open_memstream_(char **ptr, size_t *size)
{
    lx_FILE *f = new_stream(-1, 0, 1, 0);
    if (!f) return NULL;
    f->data = lx_calloc(64, 1);
    if (!f->data) { fclose_(f); return NULL; }
    f->memory = 2; f->data_cap = 64; f->publish_data = ptr; f->publish_size = size;
    return f;
}
static lx_FILE *tmpfile_(void)
{
    wchar_t dir[MAX_PATH + 1], path[MAX_PATH + 1];
    if (!GetTempPathW(LX_COUNT(dir), dir) || !GetTempFileNameW(dir, L"lxr", 0, path)) { lx_fail_win32(); return NULL; }
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); DeleteFileW(path); lx_set_errno(lx_errno_from_win32(e)); return NULL; }
    int fd = lx_fd_alloc(LX_FD_FILE, h, LX_O_RDWR);
    if (fd < 0) { CloseHandle(h); return NULL; }
    lx_FILE *f = new_stream(fd, 1, 1, 0);
    if (!f) lx_close(fd);
    return f;
}
static void perror_(const char *prefix)
{
    int e = lx_errno;
    const char *s = lx_strerror(e);
    lx_flockfile(lx_stderr);
    if (prefix && *prefix) { fputs_unlocked_(prefix, lx_stderr); fputs_unlocked_(": ", lx_stderr); }
    fputs_unlocked_(s, lx_stderr); lx_fputc_unlocked('\n', lx_stderr);
    lx_funlockfile(lx_stderr); lx_errno = e;
}
/* Linux shell pipelines require process emulation, deliberately not a cmd.exe translation. */
static lx_FILE *popen_(const char *command, const char *mode) { lx_set_errno(LX_ENOSYS); return NULL; }
static int pclose_(lx_FILE *f) { return lx_set_errno(LX_ENOSYS); }
static size_t fread_chk_(void *p, size_t cap, size_t s, size_t n, lx_FILE *f)
{ if (s && n > cap / s) lx_fatal("*** buffer overflow detected ***"); return fread_(p, s, n, f); }

uint32_t lx_fgetwc_unlocked(lx_FILE *f)
{
    if (f->orientation < 0) return WEOF32;
    f->orientation = 1;
    char b[4]; int c = lx_fgetc_unlocked(f);
    if (c < 0) return WEOF32;
    b[0] = (char)c;
    uint32_t wc; int n = lx_utf8_decode((const unsigned char *)b, 1, &wc);
    for (int i = 1; n == -2 && i < 4; ++i) {
        c = lx_fgetc_unlocked(f);
        if (c < 0) { f->error = 1; lx_set_errno(LX_EILSEQ); return WEOF32; }
        b[i] = (char)c; n = lx_utf8_decode((const unsigned char *)b, (size_t)i + 1, &wc);
    }
    if (n < 0) { f->error = 1; lx_set_errno(LX_EILSEQ); return WEOF32; }
    return wc;
}
uint32_t lx_fputwc_unlocked(uint32_t wc, lx_FILE *f)
{
    if (f->orientation < 0) return WEOF32;
    f->orientation = 1;
    char b[4]; int n = lx_utf8_encode(wc, b);
    if (n < 0) { f->error = 1; lx_set_errno(LX_EILSEQ); return WEOF32; }
    return lx_fwrite_unlocked(b, 1, n, f) == (size_t)n ? wc : WEOF32;
}
static uint32_t fgetwc_(lx_FILE *f) { lx_flockfile(f); uint32_t c = lx_fgetwc_unlocked(f); lx_funlockfile(f); return c; }
static uint32_t fputwc_(uint32_t wc, lx_FILE *f) { lx_flockfile(f); uint32_t c = lx_fputwc_unlocked(wc, f); lx_funlockfile(f); return c; }
static uint32_t ungetwc_(uint32_t wc, lx_FILE *f)
{
    if (wc == WEOF32) return WEOF32;
    char b[4]; int n = lx_utf8_encode(wc, b);
    if (n < 0) { lx_set_errno(LX_EILSEQ); return WEOF32; }
    lx_flockfile(f);
    if (f->orientation < 0) { lx_funlockfile(f); return WEOF32; }
    f->orientation = 1;
    for (int i = n - 1; i >= 0; --i) if (lx_ungetc_unlocked((unsigned char)b[i], f) < 0) { lx_funlockfile(f); return WEOF32; }
    lx_funlockfile(f); return wc;
}
static int fwide_(lx_FILE *f, int mode)
{
    lx_flockfile(f);
    if (!f->orientation && mode) f->orientation = mode > 0 ? 1 : -1;
    int r = f->orientation; lx_funlockfile(f); return r;
}

LX_SYMTAB(stdio,
    LX_SYM("stdin", &lx_stdin_storage), LX_SYM("stdout", &lx_stdout_storage), LX_SYM("stderr", &lx_stderr_storage),
    LX_SYM("fopen", fopen_), LX_SYM("fopen64", fopen_), LX_SYM("freopen", freopen_), LX_SYM("freopen64", freopen_),
    LX_SYM("fdopen", fdopen_), LX_SYM("fileno", fileno_), LX_SYM("fileno_unlocked", fileno_unlocked_),
    LX_SYM("fclose", fclose_), LX_SYM("fflush", fflush_), LX_SYM("fflush_unlocked", fflush_unlocked_),
    LX_SYM("setvbuf", setvbuf_), LX_SYM("setbuf", setbuf_), LX_SYM("setbuffer", setbuffer_), LX_SYM("setlinebuf", setlinebuf_),
    LX_SYM("fread", fread_), LX_SYM("fread_unlocked", fread_unlocked_), LX_SYM("__fread_chk", fread_chk_),
    LX_SYM("fwrite", fwrite_), LX_SYM("fwrite_unlocked", lx_fwrite_unlocked),
    LX_SYM("fgetc", fgetc_), LX_SYM("getc", fgetc_), LX_SYM("fgetc_unlocked", lx_fgetc_unlocked), LX_SYM("getc_unlocked", lx_fgetc_unlocked), LX_SYM("_IO_getc", fgetc_),
    LX_SYM("getchar", getchar_), LX_SYM("getchar_unlocked", getchar_unlocked_),
    LX_SYM("fputc", fputc_), LX_SYM("putc", fputc_), LX_SYM("fputc_unlocked", lx_fputc_unlocked), LX_SYM("putc_unlocked", lx_fputc_unlocked), LX_SYM("_IO_putc", fputc_),
    LX_SYM("putchar", putchar_), LX_SYM("putchar_unlocked", putchar_unlocked_),
    LX_SYM("fgets", fgets_), LX_SYM("fgets_unlocked", fgets_unlocked_), LX_SYM("fputs", fputs_), LX_SYM("fputs_unlocked", fputs_unlocked_), LX_SYM("puts", puts_), LX_SYM("ungetc", ungetc_),
    LX_SYM("getline", getline_), LX_SYM("getdelim", getdelim_), LX_SYM("__getdelim", getdelim_),
    LX_SYM("fseek", fseek_), LX_SYM("fseeko", fseek_), LX_SYM("fseeko64", fseeko64_), LX_SYM("ftell", ftell_), LX_SYM("ftello", ftell_), LX_SYM("ftello64", ftello64_), LX_SYM("rewind", rewind_),
    LX_SYM("fgetpos", fgetpos_), LX_SYM("fgetpos64", fgetpos64_), LX_SYM("fsetpos", fsetpos_), LX_SYM("fsetpos64", fsetpos64_),
    LX_SYM("feof", feof_), LX_SYM("ferror", ferror_), LX_SYM("clearerr", clearerr_), LX_SYM("feof_unlocked", feof_unlocked_), LX_SYM("ferror_unlocked", lx_ferror_unlocked), LX_SYM("clearerr_unlocked", clearerr_unlocked_),
    LX_SYM("flockfile", lx_flockfile), LX_SYM("funlockfile", lx_funlockfile), LX_SYM("ftrylockfile", ftrylockfile_),
    LX_SYM("tmpfile", tmpfile_), LX_SYM("tmpfile64", tmpfile_), LX_SYM("fmemopen", fmemopen_), LX_SYM("open_memstream", open_memstream_), LX_SYM("perror", perror_), LX_SYM("popen", popen_), LX_SYM("pclose", pclose_),
    LX_SYM("getwc", fgetwc_), LX_SYM("fgetwc", fgetwc_), LX_SYM("putwc", fputwc_), LX_SYM("fputwc", fputwc_), LX_SYM("ungetwc", ungetwc_), LX_SYM("fwide", fwide_)
);
