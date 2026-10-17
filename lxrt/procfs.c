/* Synthesized special files: /dev/urandom, /dev/random, /dev/zero, /proc/self/maps (from the module registry and VirtualQuery),
 * /proc/self/exe-style queries, /proc/cpuinfo, /proc/meminfo, /proc/net/route and /sys/class/net/<if>/<attr> (net.c),
 * /sys/class/dmi/id/{board,sys,bios}_* (Windows BIOS registry values). */
#include "lxrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *buf; size_t len, cap; } sbuf;

static void sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(b->buf ? b->buf + b->len : NULL, b->buf ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (b->buf && b->len + (size_t)n < b->cap) {
            b->len += (size_t)n;
            return;
        }
        b->cap = (b->cap + (size_t)n + 1) * 2;
        b->buf = realloc(b->buf, b->cap);
    }
}

static void prot_str(DWORD p, char out[5])
{
    int r = 0, w = 0, x = 0;
    switch (p & 0xff) {
    case PAGE_READONLY: r = 1; break;
    case PAGE_READWRITE: case PAGE_WRITECOPY: r = w = 1; break;
    case PAGE_EXECUTE: x = 1; break;
    case PAGE_EXECUTE_READ: r = x = 1; break;
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: r = w = x = 1; break;
    }
    out[0] = r ? 'r' : '-';
    out[1] = w ? 'w' : '-';
    out[2] = x ? 'x' : '-';
    out[3] = 'p';
    out[4] = 0;
}

/* One line per protection run inside each registered module (the parts phoenixtools checks),
 * named with the module's Linux-ish path. */
char *lx_proc_self_maps(size_t *len)
{
    sbuf b = {0};
    for (unsigned i = 0; i < lx_nmodules; i++) {
        lx_module *m = &lx_modules[i];
        uintptr_t a = m->map_start;
        while (a < m->map_end) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void *)a, &mbi, sizeof mbi))
                break;
            uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (end > m->map_end)
                end = m->map_end;
            if (mbi.State == MEM_COMMIT) {
                char p[5];
                prot_str(mbi.Protect, p);
                sb_printf(&b, "%08x-%08x %s %08x 00:00 %u %s\n", (unsigned)a, (unsigned)end, p,
                          (unsigned)(a - m->map_start), 1000 + i, m->path);
            }
            a = end;
        }
    }
    sb_printf(&b, "%s", "");
    *len = b.len;
    return b.buf;
}

/* REG_SZ value under HKLM\HARDWARE\DESCRIPTION\System\<key>, leading/trailing blanks trimmed. */
static int hardware_string(const char *key, const char *value, char *out, DWORD cap)
{
    char sub[128];
    snprintf(sub, sizeof sub, "HARDWARE\\DESCRIPTION\\System\\%s", key);
    DWORD n = cap;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, sub, value, RRF_RT_REG_SZ, NULL, out, &n) != ERROR_SUCCESS || !out[0])
        return 0;
    char *s = out;
    while (*s == ' ') s++;
    memmove(out, s, strlen(s) + 1);
    for (size_t l = strlen(out); l && out[l - 1] == ' '; ) out[--l] = 0;
    return out[0] != 0;
}

static char *proc_cpuinfo(size_t *len)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    char model[128];
    if (!hardware_string("CentralProcessor\\0", "ProcessorNameString", model, sizeof model))
        strcpy(model, "lxrt virtual CPU");
    sbuf b = {0};
    for (DWORD i = 0; i < si.dwNumberOfProcessors; i++)
        sb_printf(&b, "processor\t: %lu\nvendor_id\t: GenuineIntel\nmodel name\t: %s\n"
                  "cpu MHz\t\t: 2000.000\nflags\t\t: fpu tsc cx8 cmov mmx fxsr sse sse2 sse3 ssse3\n\n", i, model);
    *len = b.len;
    return b.buf;
}

static char *sys_dmi(const char *attr, size_t *len)
{
    static const struct { const char *attr, *value; } map[] = {
        {"board_vendor", "BaseBoardManufacturer"}, {"board_name", "BaseBoardProduct"},
        {"board_version", "BaseBoardVersion"}, {"sys_vendor", "SystemManufacturer"},
        {"product_name", "SystemProductName"}, {"bios_vendor", "BIOSVendor"},
        {"bios_version", "BIOSVersion"},
    };
    char text[256];
    for (unsigned i = 0; i < LX_COUNT(map); i++) {
        if (strcmp(attr, map[i].attr))
            continue;
        if (!hardware_string("BIOS", map[i].value, text, sizeof text))
            break;
        sbuf b = {0};
        sb_printf(&b, "%s\n", text);
        *len = b.len;
        return b.buf;
    }
    lx_set_errno(LX_ENOENT);
    return NULL;
}

static char *proc_meminfo(size_t *len)
{
    MEMORYSTATUSEX ms = { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    sbuf b = {0};
    sb_printf(&b, "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\n",
              ms.ullTotalPhys / 1024, ms.ullAvailPhys / 1024, ms.ullAvailPhys / 1024);
    *len = b.len;
    return b.buf;
}

int lx_open_special(const char *path, int flags)
{
    char *data = NULL;
    size_t len = 0;
    if (!path)
        return -2;
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random"))
        return lx_fd_alloc(LX_FD_RANDOM, NULL, flags);
    if (!strcmp(path, "/dev/zero"))
        return lx_fd_alloc(LX_FD_ZERO, NULL, flags);
    if (!strncmp(path, "/sys/class/net/", 15) || !strcmp(path, "/proc/net/route")) {
        if (!(data = lx_net_special(path, &len)))
            return -1;
    } else if (!strncmp(path, "/sys/class/dmi/id/", 18)) {
        if (!(data = sys_dmi(path + 18, &len)))
            return -1;
    } else if (strncmp(path, "/proc/", 6))
        return -2;
    else if (!strcmp(path, "/proc/self/maps"))
        data = lx_proc_self_maps(&len);
    else if (!strcmp(path, "/proc/cpuinfo"))
        data = proc_cpuinfo(&len);
    else if (!strcmp(path, "/proc/meminfo"))
        data = proc_meminfo(&len);
    else
        return lx_set_errno(LX_ENOENT);
    if ((flags & LX_O_ACCMODE) != LX_O_RDONLY) {
        free(data);
        return lx_set_errno(LX_EACCES);
    }
    return lx_fd_alloc_mem(data, len, flags);
}
