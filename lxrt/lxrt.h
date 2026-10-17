#pragma once
/* lxrt internal interfaces. Native i686-pc-windows-msvc code; guest-visible structs use the
 * i386 SysV (glibc 2.31) layout and are pinned with static_asserts. */
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>

#include "lxabi.h"

#define LX_EXPORT __declspec(dllexport)
#define LX_NORETURN __declspec(noreturn)

/* ---- ELF32 ---------------------------------------------------------------------------- */
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf32_Ehdr;
typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; } Elf32_Phdr;
typedef struct { int32_t d_tag; uint32_t d_val; } Elf32_Dyn;
typedef struct { uint32_t st_name, st_value, st_size; unsigned char st_info, st_other; uint16_t st_shndx; } Elf32_Sym;
typedef struct { uint32_t r_offset, r_info; } Elf32_Rel;
typedef struct { uint16_t vd_version, vd_flags, vd_ndx, vd_cnt; uint32_t vd_hash, vd_aux, vd_next; } Elf32_Verdef;
typedef struct { uint32_t vda_name, vda_next; } Elf32_Verdaux;
typedef struct { uint16_t vn_version, vn_cnt; uint32_t vn_file, vn_aux, vn_next; } Elf32_Verneed;
typedef struct { uint32_t vna_hash; uint16_t vna_flags, vna_other; uint32_t vna_name, vna_next; } Elf32_Vernaux;

/* ---- module registry (loader.c) -------------------------------------------------------- */
#define LX_MAX_MODULES 64
#define LX_MAX_NEEDED 48

typedef struct lx_module {
    lx_module_desc *desc;
    HMODULE hmod;
    const char *name;           /* soname */
    char path[MAX_PATH * 3];    /* UTF-8, '/' separators: dli_fname */
    uintptr_t bias;
    uintptr_t map_start, map_end;
    const Elf32_Phdr *phdr;
    unsigned phnum;
    const Elf32_Dyn *dyn;
    const Elf32_Sym *symtab;
    const char *strtab;
    const uint32_t *hash, *gnu_hash;
    const uint16_t *versym;
    const char **vernames;      /* version index -> name (defs and needs), NULL if none */
    unsigned nvernames;
    const Elf32_Rel *rel, *jmprel;
    unsigned relsz, pltrelsz;
    uintptr_t init, fini, init_array, fini_array;
    unsigned init_arraysz, fini_arraysz;
    const char *needed[LX_MAX_NEEDED];
    unsigned nneeded;
    unsigned nsyms;
    /* TLS */
    unsigned tls_modid;
    const void *tls_image;
    unsigned tls_filesz, tls_memsz, tls_align;
    int relocated, initialized, finalized;
} lx_module;

extern lx_module lx_modules[LX_MAX_MODULES];
extern unsigned lx_nmodules;
extern lx_module *lx_scope[LX_MAX_MODULES];   /* global lookup scope order */
extern unsigned lx_nscope;

lx_module *lx_module_by_addr(uintptr_t addr);
/* Look `name` up in the global scope starting after `after` (NULL = from the start),
 * then in native providers. Returns the address; *found = 0 if nothing defines it. */
void *lx_lookup_global(const char *name, const char *version, lx_module *after, int *found);
void lx_run_finalizers(void);
/* Native data object interposed by an EXE R_386_COPY: returns the EXE copy (the live object),
 * else `native` itself. Native code must use this for guest-visible mutable globals. */
void *lx_data_addr(void *native);

/* native symbol tables (symtab.c) */
void *lx_native_lookup(const char *name, const char *version, int *found);
void lx_native_init(void);
int lx_native_owner(uintptr_t addr, char *name, size_t cap, uintptr_t *base);

/* ---- configuration (config.c): phoenixtools.ini beside the EXE ------------------------- */
void lx_config_load(void);                                   /* DllMain, once */
/* Iterate [section] entries in file order; start with *cursor = 0. Returns 0 at the end. */
int lx_config_next(const char *section, unsigned *cursor, const char **key, const char **value);
const char *lx_config_get(const char *section, const char *key);   /* NULL if absent */
int lx_config_bool(const char *section, const char *key);           /* absent/empty = 0 */
char *lx_config_path(const char *value);   /* malloc'd; relative values resolve against the INI dir */

/* ---- diagnostics (log.c) --------------------------------------------------------------- */
void lx_log(const char *fmt, ...);
LX_NORETURN void lx_fatal(const char *fmt, ...);
void lx_install_crash_handler(void);
extern int lx_trace;           /* [debug] trace */
extern int lx_trace_missing;   /* [debug] trace_missing */
void lx_log_missing(const char *op, const char *path, int err);  /* call when lx_trace || lx_trace_missing */

/* ---- errno (errno.c) ------------------------------------------------------------------- */
int *lx_errno_location(void);
#define lx_errno (*lx_errno_location())
int lx_set_errno(int e);                 /* returns -1 */
int lx_errno_from_win32(DWORD err);
int lx_fail_win32(void);                 /* errno from GetLastError(); returns -1 */
char *lx_strerror(int e);                /* Linux message text (string.c) */

/* Linux errno values used by lxrt */
enum {
    LX_EPERM = 1, LX_ENOENT = 2, LX_ESRCH = 3, LX_EINTR = 4, LX_EIO = 5, LX_ENXIO = 6, LX_E2BIG = 7,
    LX_ENOEXEC = 8, LX_EBADF = 9, LX_ECHILD = 10, LX_EAGAIN = 11, LX_ENOMEM = 12, LX_EACCES = 13,
    LX_EFAULT = 14, LX_EBUSY = 16, LX_EEXIST = 17, LX_EXDEV = 18, LX_ENODEV = 19, LX_ENOTDIR = 20,
    LX_EISDIR = 21, LX_EINVAL = 22, LX_ENFILE = 23, LX_EMFILE = 24, LX_ENOTTY = 25, LX_EFBIG = 27,
    LX_ENOSPC = 28, LX_ESPIPE = 29, LX_EROFS = 30, LX_EMLINK = 31, LX_EPIPE = 32, LX_EDOM = 33,
    LX_ERANGE = 34, LX_EDEADLK = 35, LX_ENAMETOOLONG = 36, LX_ENOSYS = 38, LX_ENOTEMPTY = 39,
    LX_ELOOP = 40, LX_ENODATA = 61, LX_ETIME = 62, LX_EOVERFLOW = 75, LX_EILSEQ = 84,
    LX_ENOTSOCK = 88, LX_EOPNOTSUPP = 95, LX_EAFNOSUPPORT = 97, LX_EADDRINUSE = 98,
    LX_EADDRNOTAVAIL = 99, LX_ENETUNREACH = 101, LX_ECONNABORTED = 103, LX_ECONNRESET = 104,
    LX_ENOTCONN = 107, LX_ETIMEDOUT = 110, LX_ECONNREFUSED = 111, LX_EHOSTUNREACH = 113,
    LX_EALREADY = 114, LX_EINPROGRESS = 115,
};

/* ---- guest heap (mem.c) -----------------------------------------------------------------
 * Every allocation handed to guest code (and every pointer the guest may free) MUST come from
 * these: glibc i386 malloc alignment is 16, UCRT malloc's is 8. Never mix with malloc/free. */
void *lx_malloc(size_t n);
void *lx_calloc(size_t n, size_t m);
void *lx_realloc(void *p, size_t n);
void lx_free(void *p);
char *lx_strdup(const char *s);

/* ---- strings / paths (path.c) ---------------------------------------------------------- */
wchar_t *lx_utf8_to_wide(const char *s);          /* malloc'd */
char *lx_wide_to_utf8(const wchar_t *w);          /* malloc'd */
/* Map a guest path to a Windows path (malloc'd, wide). Returns NULL + errno on failure. */
wchar_t *lx_map_path(const char *guest);
void lx_paths_init(void);
extern char lx_exe_dir[MAX_PATH * 3];             /* UTF-8, '/' separators, no trailing '/' */
extern char lx_cwd[MAX_PATH * 3];                 /* guest-visible cwd */
char *lx_absolute_path(const char *guest);        /* malloc'd normalized absolute guest path */
char *lx_guest_path(const wchar_t *host);         /* malloc'd guest path for a host path (reverse mount) */
int lx_host_immutable(const wchar_t *host);       /* inside an [immutable] mount (phoenixtools.ini)? */

/* ---- metadata cache for immutable mounts (metacache.c) ---------------------------------
 * get: 1 if cached; *value is a malloc'd copy (free()), *err the cached Linux errno (0 = ok). */
enum { LX_META_STAT = 1, LX_META_LSTAT = 2, LX_META_DIR = 3 };
int lx_meta_get(int kind, const wchar_t *host, void **value, size_t *size, int *err);
void lx_meta_put(int kind, const wchar_t *host, const void *value, size_t size, int err);
/* Borrow the cached value (valid for the process lifetime); NULL if not cached. */
const void *lx_meta_peek(int kind, const wchar_t *host, size_t *size, int *err);

/* ---- process (start.c) ----------------------------------------------------------------- */
LX_NORETURN void lx_exit(int status);
LX_NORETURN void lx_chk_fail(void);
extern char **lx_environ;
extern uint32_t lx_canary_value;

/* ---- file descriptors (fd.c) ----------------------------------------------------------- */
enum lx_fd_kind { LX_FD_NONE, LX_FD_FILE, LX_FD_CONSOLE, LX_FD_PIPE, LX_FD_DIR, LX_FD_SOCKET, LX_FD_NULL, LX_FD_MEM,
                  LX_FD_RANDOM /* /dev/urandom, /dev/random */, LX_FD_ZERO /* /dev/zero */,
                  LX_FD_SERIAL /* [serial] mapping to a Windows COM port (serial.c) */ };
struct lx_serial;
typedef struct lx_fd {
    int kind;
    HANDLE h;
    uintptr_t sock;
    int flags;       /* Linux O_* flags */
    int cloexec;
    char *mem;       /* LX_FD_MEM: synthesized contents (/proc files), malloc'd */
    size_t memlen, mempos;
    wchar_t *wpath;  /* host path the fd was opened with (fstat, fchdir, realpath) */
    struct lx_serial *serial;  /* LX_FD_SERIAL: termios + I/O state, owned by this fd */
} lx_fd;
lx_fd *lx_fd_get(int fd);                          /* NULL + EBADF */
int lx_fd_alloc(int kind, HANDLE h, int flags);    /* lowest free >= 0 */
int lx_fd_alloc_mem(char *data, size_t len, int flags);
int lx_fd_install_socket(uintptr_t s);
void lx_fd_init(void);
int lx_open(const char *path, int flags, int mode);
long lx_read(int fd, void *buf, unsigned long n);
long lx_write(int fd, const void *buf, unsigned long n);
int lx_close(int fd);
int64_t lx_lseek64(int fd, int64_t off, int whence);

/* Linux open flags (i386) */
#define LX_O_ACCMODE 3
#define LX_O_RDONLY 0
#define LX_O_WRONLY 1
#define LX_O_RDWR 2
#define LX_O_CREAT 0100
#define LX_O_EXCL 0200
#define LX_O_NOCTTY 0400
#define LX_O_TRUNC 01000
#define LX_O_APPEND 02000
#define LX_O_NONBLOCK 04000
#define LX_O_LARGEFILE 0100000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW 0400000
#define LX_O_CLOEXEC 02000000

/* ---- synthesized files (procfs.c) ------------------------------------------------------ */
char *lx_proc_self_maps(size_t *len);
int lx_open_special(const char *path, int flags);  /* -2 if `path` is not special */

/* ---- Linux termios (i386 glibc layout) ------------------------------------------------- */
typedef struct lx_termios {
    uint32_t iflag, oflag, cflag, lflag;
    unsigned char line, cc[32];
    uint32_t ispeed, ospeed;
} lx_termios;
_Static_assert(sizeof(lx_termios) == 60, "i386 glibc termios size");
_Static_assert(offsetof(lx_termios, cc) == 17, "termios.c_cc");
_Static_assert(offsetof(lx_termios, ispeed) == 52, "termios.c_ispeed");
#define LX_CBAUD 0x100fu

/* ---- serial ports (serial.c): [serial] /dev/ttyX = COMn in phoenixtools.ini ------------- */
int lx_serial_open(const char *path, int flags);   /* -2 if `path` has no [serial] mapping */
long lx_serial_read(lx_fd *f, void *buf, unsigned long n);
long lx_serial_write(lx_fd *f, const void *buf, unsigned long n);
int lx_serial_tcgetattr(lx_fd *f, lx_termios *t);
int lx_serial_tcsetattr(lx_fd *f, int action, const lx_termios *t);
int lx_serial_tcflush(lx_fd *f, int queue);
int lx_serial_tcdrain(lx_fd *f);
int lx_serial_queued(lx_fd *f);                     /* FIONREAD; -1 + errno */
struct lx_serial *lx_serial_dup(const struct lx_serial *s);
void lx_serial_free(struct lx_serial *s);

/* ---- sockets (net.c) ------------------------------------------------------------------- */
int lx_socket_close(uintptr_t s);
char *lx_net_special(const char *path, size_t *len); /* /proc/net/route, /sys/class/net/<if>/<attr>; malloc'd */
int lx_socket_set_nonblock(uintptr_t s, int on);
long lx_socket_read(uintptr_t s, void *buf, unsigned long n);         /* fd.c read() on sockets */
long lx_socket_write(uintptr_t s, const void *buf, unsigned long n);  /* fd.c write() on sockets */
int lx_net_ioctl(int fd, unsigned long req, void *arg);               /* -2: not a network request */

/* ---- threads (thread.c) ---------------------------------------------------------------- */
void lx_thread_init_main(void);
void lx_thread_detach(void);      /* DLL_THREAD_DETACH: run thread-specific destructors */
void lx_time_init(void);          /* time.c: tzname/timezone/daylight before binding */

/* ---- TLS (loader.c) -------------------------------------------------------------------- */
void *lx_tls_get_addr(unsigned modid, unsigned offset);
void lx_tls_thread_exit(void);

/* ---- stdio (stdio.c) ------------------------------------------------------------------- */
void lx_stdio_init(void);
void lx_stdio_flush_all(void);

/* ---- formatted output (format.c) ------------------------------------------------------- */
typedef int (*lx_format_sink)(void *ctx, const char *s, size_t n);
int lx_vformat(lx_format_sink sink, void *ctx, const char *fmt, va_list ap);

/* ---- logging setup (log.c) ------------------------------------------------------------- */
void lx_log_open(void);

typedef void (*lx_voidfn)(void);

/* ---- symbol table helpers ---------------------------------------------------------------
 * Each HLE source defines `const lx_sym lx_syms_<group>[]` + `const unsigned lx_nsyms_<group>`
 * and is listed in symtab.c. */
#define LX_SYM(n, f) { n, NULL, (void *)(f) }
#define LX_SYMV(n, v, f) { n, v, (void *)(f) }
#define LX_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define LX_SYMTAB(group, ...) \
    const lx_sym lx_syms_##group[] = { __VA_ARGS__ }; \
    const unsigned lx_nsyms_##group = LX_COUNT(lx_syms_##group)
