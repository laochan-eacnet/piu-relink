/* Process lifecycle: lxrt DllMain, argv/environ, __libc_start_main, exit/atexit/__cxa_*,
 * stack protector, environment variables. */
#include "lxrt.h"

#include <stdlib.h>
#include <string.h>
#include <shellapi.h>

char **lx_environ;
uint32_t lx_canary_value;
static int env_count, env_cap;
static SRWLOCK env_lock = SRWLOCK_INIT;

void lx_log_open(void);
void lx_stdio_init(void);
void lx_stdio_flush_all(void);
void lx_thread_init_main(void);
void lx_thread_detach(void);
void lx_tls_thread_exit(void);

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h;
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        uint32_t v = (uint32_t)t.QuadPart ^ (uint32_t)(t.QuadPart >> 32) ^ (GetCurrentProcessId() * 2654435761u)
                     ^ (uint32_t)(uintptr_t)&t;
        v ^= v >> 13;
        v *= 0x5bd1e995;
        lx_canary_value = v & ~0xffu; /* glibc: low byte zero stops string-based leaks */
        lx_config_load();
        lx_log_open();
        lx_install_crash_handler(); /* before any guest DllMain stub runs */
        break;
    }
    case DLL_THREAD_DETACH:
        lx_thread_detach();
        lx_tls_thread_exit();
        break;
    }
    return TRUE;
}

/* ---- environment ----------------------------------------------------------------------- */

static void env_init(void)
{
    wchar_t *block = GetEnvironmentStringsW();
    env_cap = 64;
    lx_environ = lx_calloc(env_cap + 1, sizeof(char *));
    for (wchar_t *p = block; *p; p += wcslen(p) + 1) {
        if (*p == L'=')
            continue; /* "=C:=C:\..." per-drive cwd entries */
        if (env_count + 1 >= env_cap) {
            env_cap *= 2;
            lx_environ = lx_realloc(lx_environ, (env_cap + 1) * sizeof(char *));
        }
        char *s = lx_wide_to_utf8(p);
        lx_environ[env_count++] = lx_strdup(s);
        free(s);
    }
    lx_environ[env_count] = NULL;
    FreeEnvironmentStringsW(block);
}

static int env_find(const char *name, size_t n)
{
    for (int i = 0; i < env_count; i++)
        if (!strncmp(lx_environ[i], name, n) && lx_environ[i][n] == '=')
            return i;
    return -1;
}

static char *getenv_(const char *name)
{
    size_t n = strlen(name);
    AcquireSRWLockShared(&env_lock);
    int i = env_find(name, n);
    char *r = i >= 0 ? lx_environ[i] + n + 1 : NULL;
    ReleaseSRWLockShared(&env_lock);
    return r;
}

static int env_put(char *entry, size_t namelen, int overwrite)
{
    AcquireSRWLockExclusive(&env_lock);
    int i = env_find(entry, namelen);
    if (i >= 0) {
        if (overwrite)
            lx_environ[i] = entry;
    } else {
        if (env_count + 1 >= env_cap) {
            env_cap *= 2;
            lx_environ = lx_realloc(lx_environ, (env_cap + 1) * sizeof(char *));
        }
        lx_environ[env_count++] = entry;
        lx_environ[env_count] = NULL;
    }
    ReleaseSRWLockExclusive(&env_lock);
    return 0;
}

static int setenv_(const char *name, const char *value, int overwrite)
{
    if (!name || !*name || strchr(name, '='))
        return lx_set_errno(LX_EINVAL);
    size_t n = strlen(name), v = strlen(value);
    char *e = lx_malloc(n + v + 2);
    memcpy(e, name, n);
    e[n] = '=';
    memcpy(e + n + 1, value, v + 1);
    return env_put(e, n, overwrite);
}

static int putenv_(char *s)
{
    char *eq = strchr(s, '=');
    if (!eq)
        return 0;
    return env_put(s, (size_t)(eq - s), 1);
}

static int unsetenv_(const char *name)
{
    size_t n = strlen(name);
    AcquireSRWLockExclusive(&env_lock);
    int i = env_find(name, n);
    if (i >= 0) {
        memmove(&lx_environ[i], &lx_environ[i + 1], (env_count - i) * sizeof(char *));
        env_count--;
    }
    ReleaseSRWLockExclusive(&env_lock);
    return 0;
}

static int clearenv_(void)
{
    AcquireSRWLockExclusive(&env_lock);
    env_count = 0;
    lx_environ[0] = NULL;
    ReleaseSRWLockExclusive(&env_lock);
    return 0;
}

/* ---- argv / process init (called from lx_start) ---------------------------------------- */

void lx_process_init(int *argc, char ***argv)
{
    lx_paths_init();
    lx_fd_init();
    env_init();
    lx_stdio_init();
    lx_thread_init_main();
    lx_time_init();
    lx_native_init();

    int n;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    char **av = lx_calloc(n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) {
        char *s = lx_wide_to_utf8(wargv[i]);
        for (char *c = s; i == 0 && *c; c++)
            if (*c == '\\')
                *c = '/';
        av[i] = lx_strdup(s);
        free(s);
    }
    LocalFree(wargv);
    *argc = n;
    *argv = av;
}

/* ---- atexit ---------------------------------------------------------------------------- */

typedef struct { void (*fn)(void *); void *arg; void *dso; int done, on_exit; } exit_handler;
static int exit_status;
static exit_handler *handlers;
static unsigned nhandlers, caphandlers;
static CRITICAL_SECTION exit_cs;
static INIT_ONCE exit_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_exit_cs(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&exit_cs);
    return TRUE;
}

static int add_handler(void (*fn)(void *), void *arg, void *dso, int on_exit)
{
    InitOnceExecuteOnce(&exit_once, init_exit_cs, NULL, NULL);
    EnterCriticalSection(&exit_cs);
    if (nhandlers == caphandlers) {
        caphandlers = caphandlers ? caphandlers * 2 : 64;
        handlers = realloc(handlers, caphandlers * sizeof *handlers);
    }
    handlers[nhandlers++] = (exit_handler){ fn, arg, dso, 0, on_exit };
    LeaveCriticalSection(&exit_cs);
    return 0;
}

static int cxa_atexit_(void (*fn)(void *), void *arg, void *dso)
{
    return add_handler(fn, arg, dso, 0);
}

static int atexit_(void (*fn)(void))
{
    return add_handler((void (*)(void *))(void *)fn, NULL, NULL, 0);
}

static int on_exit_(void (*fn)(int, void *), void *arg)
{
    return add_handler((void (*)(void *))(void *)fn, arg, NULL, 1);
}

/* Run handlers (reverse order) belonging to `dso` (NULL = all). */
static void cxa_finalize_(void *dso)
{
    InitOnceExecuteOnce(&exit_once, init_exit_cs, NULL, NULL);
    EnterCriticalSection(&exit_cs);
    for (unsigned i = nhandlers; i-- > 0;) {
        exit_handler *h = &handlers[i];
        if (h->done || (dso && h->dso != dso))
            continue;
        h->done = 1;
        exit_handler run = *h;
        LeaveCriticalSection(&exit_cs);
        if (run.on_exit)
            ((void (*)(int, void *))(void *)run.fn)(exit_status, run.arg);
        else
            run.fn(run.arg);
        EnterCriticalSection(&exit_cs);
        if (i > nhandlers)
            i = nhandlers;
    }
    LeaveCriticalSection(&exit_cs);
}

/* thread_local destructors (libstdc++ __cxa_thread_atexit_impl): see thread.c */

static volatile LONG exiting;

void lx_exit(int status)
{
    if (InterlockedExchange(&exiting, 1)) {
        /* exit() from an atexit handler or a second thread: just leave */
        lx_stdio_flush_all();
        ExitProcess((UINT)status);
    }
    exit_status = status;
    cxa_finalize_(NULL);
    lx_run_finalizers();
    lx_stdio_flush_all();
    if (lx_trace)
        lx_log("exit(%d)", status);
    ExitProcess((UINT)status);
}

static void exit_(int status) { lx_exit(status); }

static void _exit_(int status)
{
    if (lx_trace)
        lx_log("_exit(%d) from %p", status, __builtin_return_address(0));
    TerminateProcess(GetCurrentProcess(), (UINT)status);
    for (;;)
        ;
}

static void abort_(void)
{
    lx_log("abort() called");
    lx_stdio_flush_all();
    TerminateProcess(GetCurrentProcess(), 134);
    for (;;)
        ;
}

typedef int (*main_fn)(int, char **, char **);

static int libc_start_main_(main_fn main, int argc, char **argv, void (*init)(int, char **, char **),
                            void (*fini)(void), void (*rtld_fini)(void), void *stack_end)
{
    (void)stack_end;
    if (rtld_fini)
        cxa_atexit_((void (*)(void *))rtld_fini, NULL, NULL);
    if (fini)
        cxa_atexit_((void (*)(void *))fini, NULL, NULL);
    if (init)
        init(argc, argv, lx_environ);
    if (lx_trace)
        lx_log("calling main");
    lx_exit(main(argc, argv, lx_environ));
}

static void stack_chk_fail_(void)
{
    lx_fatal("*** stack smashing detected ***: terminated (caller %p)", __builtin_return_address(0));
}

static void assert_fail_(const char *expr, const char *file, unsigned line, const char *func)
{
    lx_log("%s:%u: %s: Assertion `%s' failed.", file, line, func ? func : "?", expr);
    abort_();
}

static LX_NORETURN void fortify_fail_(const char *msg)
{
    lx_fatal("*** %s ***: terminated", msg);
}

LX_NORETURN void lx_chk_fail(void)
{
    fortify_fail_("buffer overflow detected");
}

static char *secure_getenv_(const char *name) { return getenv_(name); }

LX_SYMTAB(start,
    LX_SYM("__libc_start_main", libc_start_main_),
    LX_SYM("exit", exit_),
    LX_SYM("_exit", _exit_),
    LX_SYM("_Exit", _exit_),
    LX_SYM("abort", abort_),
    LX_SYM("atexit", atexit_),
    LX_SYM("on_exit", on_exit_),
    LX_SYM("__cxa_atexit", cxa_atexit_),
    LX_SYM("__cxa_finalize", cxa_finalize_),
    LX_SYM("__stack_chk_fail", stack_chk_fail_),
    LX_SYM("__stack_chk_fail_local", stack_chk_fail_),
    LX_SYM("__assert_fail", assert_fail_),
    LX_SYM("__chk_fail", lx_chk_fail),
    LX_SYM("__fortify_fail", fortify_fail_),
    LX_SYM("getenv", getenv_),
    LX_SYM("secure_getenv", secure_getenv_),
    LX_SYM("__libc_secure_getenv", secure_getenv_),
    LX_SYM("setenv", setenv_),
    LX_SYM("putenv", putenv_),
    LX_SYM("unsetenv", unsetenv_),
    LX_SYM("clearenv", clearenv_),
    LX_SYM("environ", &lx_environ),
    LX_SYM("__environ", &lx_environ),
    LX_SYM("_environ", &lx_environ),
    LX_SYM("__gmon_start__", NULL),
);
