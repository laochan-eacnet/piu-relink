/* Diagnostics: lx_log/lx_fatal to stderr (and the [debug] log file), crash reports that translate
 * addresses into guest module + ELF vaddr. */
#include "lxrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int lx_trace, lx_trace_missing;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static SRWLOCK log_lock = SRWLOCK_INIT;
static ULONGLONG start_ms;

void lx_log_open(void)
{
    const char *log = lx_config_get("debug", "log");
    if (log && *log) {
        char *path = lx_config_path(log);
        wchar_t *w = lx_utf8_to_wide(path);
        log_file = CreateFileW(w, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        free(w);
        free(path);
    }
    lx_trace = lx_config_bool("debug", "trace");
    lx_trace_missing = lx_config_bool("debug", "trace_missing");
    start_ms = GetTickCount64();
}

/* Failed path lookups (stat/open). [debug] trace logs every one; trace_missing logs each
 * distinct (operation, path) once with a timestamp, cheap enough to leave on during play. */
void lx_log_missing(const char *op, const char *path, int err)
{
    static uint64_t seen[1 << 16];
    static SRWLOCK seen_lock = SRWLOCK_INIT;
    if (!lx_trace) {
        uint64_t h = 1469598103934665603ull;
        for (const char *c = op; *c; c++)
            h = (h ^ (unsigned char)*c) * 1099511628211ull;
        for (const char *c = path ? path : ""; *c; c++)
            h = (h ^ (unsigned char)*c) * 1099511628211ull;
        h |= 1;
        AcquireSRWLockExclusive(&seen_lock);
        unsigned i = (unsigned)h & 0xffff, probes = 0;
        while (seen[i] && seen[i] != h && ++probes < 64)
            i = (i + 1) & 0xffff;
        int fresh = seen[i] != h;
        if (fresh && probes < 64)
            seen[i] = h;
        ReleaseSRWLockExclusive(&seen_lock);
        if (!fresh)
            return;
    }
    lx_log("missing +%.3fs %s(%s): errno %d", (GetTickCount64() - start_ms) / 1000.0, op,
           path ? path : "(null)", err);
}

static void emit(const char *buf, int len)
{
    DWORD w;
    AcquireSRWLockExclusive(&log_lock);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE)
        WriteFile(err, buf, len, &w, NULL);
    if (log_file != INVALID_HANDLE_VALUE)
        WriteFile(log_file, buf, len, &w, NULL);
    ReleaseSRWLockExclusive(&log_lock);
}

static void vlog(const char *prefix, const char *fmt, va_list ap)
{
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[lxrt %lu] %s", GetCurrentThreadId(), prefix);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    if (m < 0 || m > (int)(sizeof buf - n - 2))
        m = (int)(sizeof buf - n - 2);
    n += m;
    buf[n++] = '\n';
    emit(buf, n);
}

void lx_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void lx_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("FATAL: ", fmt, ap);
    va_end(ap);
    TerminateProcess(GetCurrentProcess(), 134);
    for (;;)
        ;
}

/* "module+0xoff (vaddr 0x...)" for an address */
static void describe(uintptr_t a, char *out, size_t cap)
{
    lx_module *m = lx_module_by_addr(a);
    if (m) {
        snprintf(out, cap, "%s vaddr %#x", m->name, (unsigned)(a - m->bias));
        return;
    }
    char name[MAX_PATH * 3];
    uintptr_t base;
    if (lx_native_owner(a, name, sizeof name, &base)) {
        const char *s = strrchr(name, '/');
        snprintf(out, cap, "%s+%#x", s ? s + 1 : name, (unsigned)(a - base));
        return;
    }
    HMODULE h = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &h) && h) {
        char p[MAX_PATH];
        GetModuleFileNameA(h, p, sizeof p);
        const char *s = strrchr(p, '\\');
        snprintf(out, cap, "%s+%#x", s ? s + 1 : p, (unsigned)(a - (uintptr_t)h));
        return;
    }
    snprintf(out, cap, "?");
}

static int known_code(uintptr_t a)
{
    char name[MAX_PATH * 3];
    uintptr_t base;
    return lx_module_by_addr(a) || lx_native_owner(a, name, sizeof name, &base);
}

static void report(EXCEPTION_POINTERS *ep, const char *what)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const CONTEXT *c = ep->ContextRecord;
    char where[512];
    describe(c->Eip, where, sizeof where);
    lx_log("%s: exception %#lx at %#lx (%s)", what, er->ExceptionCode, c->Eip, where);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        lx_log("  %s address %#lx", er->ExceptionInformation[0] == 8 ? "execute" :
               er->ExceptionInformation[0] ? "write" : "read", (unsigned long)er->ExceptionInformation[1]);
    lx_log("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    /* Values on the stack that point into guest modules or lxrt/providers (heuristic backtrace). */
    const uintptr_t *sp = (const uintptr_t *)c->Esp;
    MEMORY_BASIC_INFORMATION mbi;
    int shown = 0;
    if (VirtualQuery(sp, &mbi, sizeof mbi)) {
        const uintptr_t *end = (const uintptr_t *)((char *)mbi.BaseAddress + mbi.RegionSize);
        for (; sp < end && shown < 32; sp++) {
            if (!known_code(*sp))
                continue;
            describe(*sp, where, sizeof where);
            lx_log("  stack %p: %s", (void *)sp, where);
            shown++;
        }
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    report(ep, "FATAL");
    TerminateProcess(GetCurrentProcess(), 139);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Guest code and lxrt/providers never install SEH handlers, so a fault there is fatal; report it
 * first-chance (system DLLs such as GL drivers may replace the unhandled-exception filter).
 * Faults inside other DLLs are left to their own handlers. */
static LONG WINAPI first_chance(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!known_code(ep->ContextRecord->Eip)) {
        if (lx_trace)
            report(ep, "first-chance (foreign code)");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    report(ep, "FATAL");
    TerminateProcess(GetCurrentProcess(), 139);
    return EXCEPTION_CONTINUE_SEARCH;
}

void lx_install_crash_handler(void)
{
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, first_chance);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}
