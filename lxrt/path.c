/* Guest (Linux) path -> Windows path.
 *
 * Rules, in order:
 *   1. "X:/..." or "X:\..." (drive paths, e.g. dladdr names of relinked modules) pass through.
 *   2. Relative paths are joined to the guest cwd; "." and ".." are folded lexically.
 *   3. Absolute paths: longest-prefix match against [mounts] in phoenixtools.ini (beside the EXE):
 *          /piu/data = D:\piu\data
 *      Built-in defaults: /dev/null -> NUL, /tmp -> %TEMP%, / -> <exe dir>/rootfs.
 */
#include "lxrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char lx_exe_dir[MAX_PATH * 3];
char lx_cwd[MAX_PATH * 3] = "/";

typedef struct { char *guest; size_t glen; char *host; wchar_t *whost; size_t whlen; int immutable; } mount;
static mount mounts[64];
static unsigned nmounts;
static SRWLOCK cwd_lock = SRWLOCK_INIT;

wchar_t *lx_utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc((n ? n : 1) * sizeof(wchar_t));
    if (!n || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        w[0] = 0;
    return w;
}

char *lx_wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n ? n : 1);
    if (!n || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
        s[0] = 0;
    return s;
}

static void slashes(char *s)
{
    for (; *s; s++)
        if (*s == '\\')
            *s = '/';
}

static void add_mount(const char *guest, const char *host)
{
    if (nmounts >= LX_COUNT(mounts))
        return;
    mount *m = &mounts[nmounts++];
    m->guest = _strdup(guest);
    m->glen = strlen(guest);
    while (m->glen > 1 && m->guest[m->glen - 1] == '/')
        m->guest[--m->glen] = 0;
    /* canonical host path ("a/../b" folded) so host-side prefix checks are exact */
    wchar_t *wh = lx_utf8_to_wide(host), full[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(wh, MAX_PATH * 2, full, NULL);
    free(wh);
    m->whost = _wcsdup(n && n < MAX_PATH * 2 ? full : L"");
    m->whlen = wcslen(m->whost);
    while (m->whlen > 3 && (m->whost[m->whlen - 1] == L'\\' || m->whost[m->whlen - 1] == L'/'))
        m->whost[--m->whlen] = 0;
    m->host = lx_wide_to_utf8(m->whost);
    slashes(m->host);
}

/* Is this host path inside a mount declared [immutable] in phoenixtools.ini (read-only game data whose
 * metadata may be cached for the process lifetime)? */
int lx_host_immutable(const wchar_t *host)
{
    for (unsigned i = 0; i < nmounts; i++) {
        const mount *m = &mounts[i];
        if (m->immutable && m->whlen && !_wcsnicmp(host, m->whost, m->whlen) &&
            (host[m->whlen] == L'\\' || host[m->whlen] == L'/' || !host[m->whlen]))
            return 1;
    }
    return 0;
}

static int is_drive_path(const char *p)
{
    return ((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':' && (p[2] == '/' || p[2] == '\\' || !p[2]);
}

void lx_paths_init(void)
{
    wchar_t w[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH);
    w[n < MAX_PATH ? n : MAX_PATH - 1] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, lx_exe_dir, sizeof lx_exe_dir, NULL, NULL);
    slashes(lx_exe_dir);
    char *s = strrchr(lx_exe_dir, '/');
    if (s)
        *s = 0;

    unsigned cursor = 0;
    const char *k, *v;
    while (lx_config_next("mounts", &cursor, &k, &v)) {
        char host[MAX_PATH * 3];
        if (is_drive_path(v) || v[0] == '/' || v[0] == '\\')
            snprintf(host, sizeof host, "%s", v);
        else
            snprintf(host, sizeof host, "%s/%s", lx_exe_dir, v);  /* relative to exe dir */
        add_mount(k, host);
    }
    for (cursor = 0; lx_config_next("immutable", &cursor, &k, &v);)
        for (unsigned i = 0; i < nmounts; i++)
            if (!strcmp(mounts[i].guest, k))
                mounts[i].immutable = v[0] == '1' || (v[0] | 0x20) == 'y' || (v[0] | 0x20) == 't';
    if ((v = lx_config_get("process", "cwd")))
        snprintf(lx_cwd, sizeof lx_cwd, "%s", v);
    char tmp[MAX_PATH * 3];
    wchar_t wt[MAX_PATH];
    if (GetTempPathW(MAX_PATH, wt)) {
        WideCharToMultiByte(CP_UTF8, 0, wt, -1, tmp, sizeof tmp, NULL, NULL);
        add_mount("/tmp", tmp);
    }
    snprintf(tmp, sizeof tmp, "%s/rootfs", lx_exe_dir);
    add_mount("/", tmp);
}

/* Lexically normalize an absolute '/'-path in place ("." / ".." / "//"). */
static void normalize(char *p, size_t root_len)
{
    char *out = p + root_len, *in = p + root_len;
    while (*in) {
        while (*in == '/')
            in++;
        if (!*in)
            break;
        char *seg = in;
        while (*in && *in != '/')
            in++;
        size_t len = (size_t)(in - seg);
        if (len == 1 && seg[0] == '.')
            continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (out > p + root_len && out[-1] != '/')
                out--;
            if (out > p + root_len)
                out--;
            continue;
        }
        if (out == p + root_len && root_len && p[root_len - 1] == '/')
            ;
        else
            *out++ = '/';
        memmove(out, seg, len);
        out += len;
    }
    if (out == p + root_len && !(root_len && p[root_len - 1] == '/'))
        *out++ = '/';
    *out = 0;
}

/* Absolute guest path (UTF-8, normalized, malloc'd) for `guest`. */
char *lx_absolute_path(const char *guest)
{
    size_t gl = strlen(guest);
    char *buf;
    if (guest[0] == '/' || is_drive_path(guest)) {
        buf = malloc(gl + 2);
        memcpy(buf, guest, gl + 1);
    } else {
        AcquireSRWLockShared(&cwd_lock);
        size_t cl = strlen(lx_cwd);
        buf = malloc(cl + gl + 3);
        memcpy(buf, lx_cwd, cl);
        buf[cl] = '/';
        memcpy(buf + cl + 1, guest, gl + 1);
        ReleaseSRWLockShared(&cwd_lock);
    }
    slashes(buf);
    int drive = is_drive_path(buf);
    if (drive && !buf[2]) {   /* "D:" -> "D:/" (buffer has room: gl + 2 >= 4) */
        buf[2] = '/';
        buf[3] = 0;
    }
    normalize(buf, drive ? 3 : 1);
    return buf;
}

/* Reverse of lx_map_path: host path -> guest path via the longest matching mount host prefix;
 * paths outside every mount stay drive paths with '/' separators. */
char *lx_guest_path(const wchar_t *host)
{
    char *h = lx_wide_to_utf8(host);
    slashes(h);
    const mount *best = NULL;
    size_t best_len = 0;
    for (unsigned i = 0; i < nmounts; i++) {
        size_t hl = strlen(mounts[i].host);
        if (!_strnicmp(h, mounts[i].host, hl) && (h[hl] == '/' || !h[hl]) && hl > best_len) {
            best = &mounts[i];
            best_len = hl;
        }
    }
    if (!best)
        return h;
    const char *rest = h + best_len;
    size_t gl = best->glen == 1 ? 0 : best->glen;
    char *g = malloc(gl + strlen(rest) + 2);
    memcpy(g, best->guest, gl);
    strcpy(g + gl, *rest ? rest : (gl ? "" : "/"));
    free(h);
    return g;
}

wchar_t *lx_map_path(const char *guest)
{
    if (!guest) {
        lx_set_errno(LX_EFAULT);
        return NULL;
    }
    if (!*guest) {
        lx_set_errno(LX_ENOENT);
        return NULL;
    }
    char *abs = lx_absolute_path(guest);
    if (!strcmp(abs, "/dev/null")) {
        free(abs);
        return lx_utf8_to_wide("NUL");
    }
    char *host;
    if (is_drive_path(abs)) {
        host = abs;
    } else {
        const mount *best = NULL;
        for (unsigned i = 0; i < nmounts; i++) {
            const mount *m = &mounts[i];
            int match = m->glen == 1 ? 1 :
                (!strncmp(abs, m->guest, m->glen) && (abs[m->glen] == '/' || !abs[m->glen]));
            if (match && (!best || m->glen > best->glen))
                best = m;
        }
        if (!best) {
            free(abs);
            lx_set_errno(LX_ENOENT);
            return NULL;
        }
        const char *rest = abs + (best->glen == 1 ? 0 : best->glen);
        host = malloc(strlen(best->host) + strlen(rest) + 2);
        strcpy(host, best->host);
        if (*rest && *rest != '/')
            strcat(host, "/");
        strcat(host, rest);
        free(abs);
    }
    for (char *c = host; *c; c++)
        if (*c == '/')
            *c = '\\';
    wchar_t *w = lx_utf8_to_wide(host);
    free(host);
    return w;
}

/* chdir/getcwd operate on the guest-visible cwd string. */
int lx_chdir(const char *path)
{
    wchar_t *w = lx_map_path(path);
    if (!w)
        return -1;
    DWORD a = GetFileAttributesW(w);
    free(w);
    if (a == INVALID_FILE_ATTRIBUTES)
        return lx_fail_win32();
    if (!(a & FILE_ATTRIBUTE_DIRECTORY))
        return lx_set_errno(LX_ENOTDIR);
    char *abs = lx_absolute_path(path);
    AcquireSRWLockExclusive(&cwd_lock);
    snprintf(lx_cwd, sizeof lx_cwd, "%s", abs);
    ReleaseSRWLockExclusive(&cwd_lock);
    free(abs);
    return 0;
}

char *lx_getcwd(char *buf, size_t size)
{
    AcquireSRWLockShared(&cwd_lock);
    size_t n = strlen(lx_cwd) + 1;
    if (!buf) {
        buf = lx_malloc(size > n ? size : n);
        size = size > n ? size : n;
    }
    if (n > size) {
        ReleaseSRWLockShared(&cwd_lock);
        lx_set_errno(LX_ERANGE);
        return NULL;
    }
    memcpy(buf, lx_cwd, n);
    ReleaseSRWLockShared(&cwd_lock);
    return buf;
}

LX_SYMTAB(path,
    LX_SYM("chdir", lx_chdir),
    LX_SYM("getcwd", lx_getcwd),
);
