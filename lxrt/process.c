/* Linux process/system identity and deliberately non-launching process control.
 * No API in this group starts a host program, mounts a host disk, or reboots Windows.
 * system(command) reports the shell-style status 127 << 8; system(NULL) reports no shell.
 * Mount success is an explicit piu compatibility no-op, restricted to three exact targets.
 * IDs are fixed at uid/gid 1000; credential/session setters do not change host security.
 * Resource-limit setters retain validated guest metadata, not host-enforced quotas.
 * sysinfo clamps byte counts to uint32, reports no load averages and one guest process;
 * swap is estimated from Windows commit limits. Unsupported sysconf names are indeterminate,
 * while unknown/reserved numbers fail EINVAL. Only Linux/Linux32 personalities are accepted.
 */
#include "lxrt.h"

#include <intrin.h>
#include <limits.h>
#include <string.h>
#include <tlhelp32.h>

extern int lx_raise(int sig);

#pragma pack(push, 4)
typedef struct {
    int32_t uptime;
    uint32_t loads[3];
    uint32_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t totalhigh, freehigh, mem_unit;
    char reserved[8];
} lx_sysinfo;

typedef struct {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
} lx_utsname;

typedef struct { uint32_t cur, max; } lx_rlimit;
typedef struct { uint64_t cur, max; } lx_rlimit64;

typedef struct {
    char *name, *passwd;
    uint32_t uid, gid;
    char *gecos, *dir, *shell;
} lx_passwd;
#pragma pack(pop)

static_assert(sizeof(lx_sysinfo) == 64, "i386 sysinfo size");
static_assert(offsetof(lx_sysinfo, uptime) == 0, "i386 sysinfo uptime");
static_assert(offsetof(lx_sysinfo, loads) == 4, "i386 sysinfo loads");
static_assert(offsetof(lx_sysinfo, totalram) == 16, "i386 sysinfo totalram");
static_assert(offsetof(lx_sysinfo, freeram) == 20, "i386 sysinfo freeram");
static_assert(offsetof(lx_sysinfo, sharedram) == 24, "i386 sysinfo sharedram");
static_assert(offsetof(lx_sysinfo, bufferram) == 28, "i386 sysinfo bufferram");
static_assert(offsetof(lx_sysinfo, totalswap) == 32, "i386 sysinfo totalswap");
static_assert(offsetof(lx_sysinfo, freeswap) == 36, "i386 sysinfo freeswap");
static_assert(offsetof(lx_sysinfo, procs) == 40, "i386 sysinfo procs");
static_assert(offsetof(lx_sysinfo, pad) == 42, "i386 sysinfo pad");
static_assert(offsetof(lx_sysinfo, totalhigh) == 44, "i386 sysinfo totalhigh");
static_assert(offsetof(lx_sysinfo, freehigh) == 48, "i386 sysinfo freehigh");
static_assert(offsetof(lx_sysinfo, mem_unit) == 52, "i386 sysinfo mem_unit");
static_assert(offsetof(lx_sysinfo, reserved) == 56, "i386 sysinfo padding");
static_assert(sizeof(lx_utsname) == 390, "i386 utsname size");
static_assert(offsetof(lx_utsname, sysname) == 0, "i386 utsname sysname");
static_assert(offsetof(lx_utsname, nodename) == 65, "i386 utsname nodename");
static_assert(offsetof(lx_utsname, release) == 130, "i386 utsname release");
static_assert(offsetof(lx_utsname, version) == 195, "i386 utsname version");
static_assert(offsetof(lx_utsname, machine) == 260, "i386 utsname machine");
static_assert(offsetof(lx_utsname, domainname) == 325, "i386 utsname domainname");
static_assert(sizeof(lx_rlimit) == 8, "i386 rlimit size");
static_assert(offsetof(lx_rlimit, cur) == 0, "i386 rlimit current");
static_assert(offsetof(lx_rlimit, max) == 4, "i386 rlimit maximum");
static_assert(sizeof(lx_rlimit64) == 16, "i386 rlimit64 size");
static_assert(offsetof(lx_rlimit64, cur) == 0, "i386 rlimit64 current");
static_assert(offsetof(lx_rlimit64, max) == 8, "i386 rlimit64 maximum");
static_assert(_Alignof(lx_rlimit64) == 4, "i386 rlimit64 alignment");
static_assert(sizeof(lx_passwd) == 28, "i386 passwd size");
static_assert(offsetof(lx_passwd, name) == 0, "i386 passwd name");
static_assert(offsetof(lx_passwd, passwd) == 4, "i386 passwd password");
static_assert(offsetof(lx_passwd, uid) == 8, "i386 passwd uid");
static_assert(offsetof(lx_passwd, gid) == 12, "i386 passwd gid");
static_assert(offsetof(lx_passwd, gecos) == 16, "i386 passwd gecos");
static_assert(offsetof(lx_passwd, dir) == 20, "i386 passwd directory");
static_assert(offsetof(lx_passwd, shell) == 24, "i386 passwd shell");

static int fork_(void)
{
    lx_log("fork caller=%p: child processes are unsupported", _ReturnAddress());
    return lx_set_errno(LX_ENOSYS);
}

static int vfork_(void)
{
    lx_log("vfork caller=%p: child processes are unsupported", _ReturnAddress());
    return lx_set_errno(LX_ENOSYS);
}

/* Log arguments separately so quoting/truncation never changes execution behavior.
 * Logs are bounded; even a command with more arguments is always refused, never launched. */
static void log_exec_vector(const char *api, const char *path, char *const *argv)
{
    lx_log("%s command=%s: execution disabled", api, path ? path : "(null)");
    if (!argv)
        return;
    unsigned i = 0;
    for (; i < 16 && argv[i]; ++i)
        lx_log("  argv[%u]=%s", i, argv[i]);
    if (i == 16)
        lx_log("  [argument log truncated after 16 entries]");
}

static int exec_vector(const char *api, const char *path, char *const *argv)
{
    log_exec_vector(api, path, argv);
    return lx_set_errno(path ? LX_ENOENT : LX_EFAULT);
}

static int execv_(const char *path, char *const *argv)
{
    return exec_vector("execv", path, argv);
}

static int execvp_(const char *file, char *const *argv)
{
    return exec_vector("execvp", file, argv);
}

static int execve_(const char *path, char *const *argv, char *const *envp)
{
    (void)envp;
    return exec_vector("execve", path, argv);
}

static int exec_list(const char *api, const char *path, const char *arg, va_list ap)
{
    lx_log("%s command=%s: execution disabled", api, path ? path : "(null)");
    unsigned i = 0;
    for (; i < 16 && arg; ++i) {
        lx_log("  argv[%u]=%s", i, arg);
        arg = va_arg(ap, const char *);
    }
    if (i == 16 && arg)
        lx_log("  [argument log truncated after 16 entries]");
    return lx_set_errno(path ? LX_ENOENT : LX_EFAULT);
}

static int execl_(const char *path, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    int result = exec_list("execl", path, arg, ap);
    va_end(ap);
    return result;
}

static int execle_(const char *path, const char *arg, ...)
{
    /* envp follows the argument terminator, but cannot affect a refused launch. */
    va_list ap;
    va_start(ap, arg);
    int result = exec_list("execle", path, arg, ap);
    va_end(ap);
    return result;
}

static int system_(const char *command)
{
    lx_log("system command=%s: no shell is launched", command ? command : "(null)");
    return command ? 127 << 8 : 0;
}

static int wait_result(int pid, int options)
{
    const uint32_t allowed = 1u | 2u | 8u | 0x20000000u | 0x40000000u | 0x80000000u;
    if ((uint32_t)options & ~allowed)
        return lx_set_errno(LX_EINVAL);
    if (pid == INT_MIN)
        return lx_set_errno(LX_ESRCH);
    return lx_set_errno(LX_ECHILD);
}

static int waitpid_(int pid, int *status, int options)
{
    (void)status;
    return wait_result(pid, options);
}

static int wait_(int *status)
{
    return waitpid_(-1, status, 0);
}

static int wait4_(int pid, int *status, int options, void *usage)
{
    (void)usage;
    return waitpid_(pid, status, options);
}

static int wait3_(int *status, int options, void *usage)
{
    return wait4_(-1, status, options, usage);
}

int lx_getpid(void) { return (int)GetCurrentProcessId(); }
int lx_gettid(void) { return (int)GetCurrentThreadId(); }

int lx_getppid(void)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return lx_fail_win32();
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof entry);
    entry.dwSize = sizeof entry;
    if (!Process32FirstW(snapshot, &entry)) {
        DWORD error = GetLastError();
        CloseHandle(snapshot);
        return lx_set_errno(lx_errno_from_win32(error));
    }
    DWORD self = GetCurrentProcessId();
    do {
        if (entry.th32ProcessID == self) {
            int parent = (int)entry.th32ParentProcessID;
            CloseHandle(snapshot);
            return parent;
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return 0; /* No visible parent/process snapshot entry. */
}

static uint32_t uid_(void) { return 1000; }
static int setid_(uint32_t id) { return id == UINT32_MAX ? lx_set_errno(LX_EINVAL) : 0; }
static int setsid_(void) { return lx_getpid(); }
static int getpgrp_(void) { return lx_getpid(); }

static int setpgid_(int pid, int pgid)
{
    if (pid < 0 || pgid < 0)
        return lx_set_errno(LX_EINVAL);
    if (pid && pid != lx_getpid())
        return lx_set_errno(LX_ESRCH);
    return 0; /* No host job/session manipulation. */
}

int lx_kill(int pid, int sig)
{
    if (sig < 0 || sig > 64)
        return lx_set_errno(LX_EINVAL);
    if (pid != lx_getpid())
        return lx_set_errno(LX_ESRCH);
    return sig ? lx_raise(sig) : 0;
}

static SRWLOCK priority_lock = SRWLOCK_INIT;
static int process_nice;

static int priority_target(int which, uint32_t who)
{
    if (which < 0 || which > 2)
        return lx_set_errno(LX_EINVAL);
    if (who && who != (which == 2 ? 1000u : (uint32_t)lx_getpid()))
        return lx_set_errno(LX_ESRCH);
    return 0;
}

static int getpriority_(int which, uint32_t who)
{
    if (priority_target(which, who) < 0)
        return -1;
    AcquireSRWLockShared(&priority_lock);
    int result = process_nice;
    ReleaseSRWLockShared(&priority_lock);
    return result; /* A valid -1, like glibc, does not set errno. */
}

static int set_nice_locked(int value)
{
    if (value < -20)
        value = -20;
    if (value > 19)
        value = 19;
    DWORD priority = value < -10 ? HIGH_PRIORITY_CLASS :
                     value < 0 ? ABOVE_NORMAL_PRIORITY_CLASS :
                     value == 0 ? NORMAL_PRIORITY_CLASS :
                     value < 10 ? BELOW_NORMAL_PRIORITY_CLASS : IDLE_PRIORITY_CLASS;
    if (!SetPriorityClass(GetCurrentProcess(), priority))
        return lx_fail_win32();
    process_nice = value;
    return 0;
}

static int setpriority_(int which, uint32_t who, int value)
{
    if (priority_target(which, who) < 0)
        return -1;
    AcquireSRWLockExclusive(&priority_lock);
    int result = set_nice_locked(value);
    ReleaseSRWLockExclusive(&priority_lock);
    return result;
}

static int nice_(int increment)
{
    AcquireSRWLockExclusive(&priority_lock);
    int64_t sum = (int64_t)process_nice + increment;
    int value = sum < -20 ? -20 : sum > 19 ? 19 : (int)sum;
    int result = set_nice_locked(value) < 0 ? -1 : process_nice;
    ReleaseSRWLockExclusive(&priority_lock);
    return result;
}

typedef HRESULT (WINAPI *set_thread_description_fn)(HANDLE, PCWSTR);
static INIT_ONCE thread_name_once = INIT_ONCE_STATIC_INIT;
static set_thread_description_fn set_thread_description;
static __declspec(thread) char thread_name[16] = "piu";

static BOOL CALLBACK find_thread_description(PINIT_ONCE once, PVOID arg, PVOID *context)
{
    (void)once;
    (void)arg;
    (void)context;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel)
        set_thread_description = (set_thread_description_fn)GetProcAddress(kernel, "SetThreadDescription");
    return TRUE;
}

static int prctl_(int option, ...)
{
    if (option != 15 && option != 16)
        return lx_set_errno(LX_EINVAL);
    va_list ap;
    va_start(ap, option);
    char *arg = va_arg(ap, char *);
    va_end(ap);
    if (!arg)
        return lx_set_errno(LX_EFAULT);
    if (option == 16) {
        memcpy(arg, thread_name, sizeof thread_name);
        return 0;
    }
    char name[16] = {0};
    size_t len = strnlen(arg, sizeof name - 1);
    memcpy(name, arg, len);
    wchar_t wide[16];
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, LX_COUNT(wide)))
        return lx_fail_win32();
    InitOnceExecuteOnce(&thread_name_once, find_thread_description, NULL, NULL);
    if (!set_thread_description)
        return lx_set_errno(LX_ENOSYS);
    HRESULT result = set_thread_description(GetCurrentThread(), wide);
    if (FAILED(result)) {
        if (HRESULT_FACILITY(result) == FACILITY_WIN32)
            return lx_set_errno(lx_errno_from_win32(HRESULT_CODE(result)));
        return lx_set_errno(LX_EIO);
    }
    memcpy(thread_name, name, sizeof name);
    return 0;
}

static int get_nprocs_(void)
{
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count ? (int)count : lx_fail_win32();
}

static int get_nprocs_conf_(void)
{
    DWORD count = GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS);
    return count ? (int)count : lx_fail_win32();
}

static int memory_status(MEMORYSTATUSEX *status)
{
    memset(status, 0, sizeof *status);
    status->dwLength = sizeof *status;
    return GlobalMemoryStatusEx(status) ? 0 : lx_fail_win32();
}

static long memory_pages(int available)
{
    MEMORYSTATUSEX memory;
    if (memory_status(&memory) < 0)
        return -1;
    uint64_t pages = (available ? memory.ullAvailPhys : memory.ullTotalPhys) / 4096;
    return pages > LONG_MAX ? LONG_MAX : (long)pages;
}

static long get_phys_pages_(void) { return memory_pages(0); }
static long get_avphys_pages_(void) { return memory_pages(1); }
static int getpagesize_(void) { return 4096; }

/* Limits are guest bookkeeping, not host job-object limits. The descriptor ceiling is real;
 * other limits are reported/stored but not enforced by Windows resource accounting. */
static SRWLOCK limits_lock = SRWLOCK_INIT;
static lx_rlimit64 limits[16] = {
    {UINT64_MAX, UINT64_MAX}, {UINT64_MAX, UINT64_MAX}, {UINT64_MAX, UINT64_MAX},
    {8u * 1024u * 1024u, UINT64_MAX}, {0, UINT64_MAX}, {UINT64_MAX, UINT64_MAX},
    {4096, 4096}, {4096, 4096}, {65536, 65536}, {UINT64_MAX, UINT64_MAX},
    {UINT64_MAX, UINT64_MAX}, {4096, 4096}, {819200, 819200}, {40, 40}, {0, 0},
    {UINT64_MAX, UINT64_MAX}
};

static int getrlimit64_(int resource, lx_rlimit64 *result)
{
    if ((unsigned)resource >= LX_COUNT(limits))
        return lx_set_errno(LX_EINVAL);
    if (!result)
        return lx_set_errno(LX_EFAULT);
    AcquireSRWLockShared(&limits_lock);
    *result = limits[resource];
    ReleaseSRWLockShared(&limits_lock);
    return 0;
}

static int getrlimit_(int resource, lx_rlimit *result)
{
    if (!result)
        return lx_set_errno(LX_EFAULT);
    lx_rlimit64 value;
    if (getrlimit64_(resource, &value) < 0)
        return -1;
    result->cur = value.cur > UINT32_MAX ? UINT32_MAX : (uint32_t)value.cur;
    result->max = value.max > UINT32_MAX ? UINT32_MAX : (uint32_t)value.max;
    return 0;
}

static int setrlimit64_(int resource, const lx_rlimit64 *value)
{
    if ((unsigned)resource >= LX_COUNT(limits))
        return lx_set_errno(LX_EINVAL);
    if (!value)
        return lx_set_errno(LX_EFAULT);
    lx_rlimit64 requested = *value;
    if (requested.cur > requested.max)
        return lx_set_errno(LX_EINVAL);
    AcquireSRWLockExclusive(&limits_lock);
    if (requested.max > limits[resource].max || (resource == 7 && requested.max > 4096)) {
        ReleaseSRWLockExclusive(&limits_lock);
        return lx_set_errno(LX_EPERM);
    }
    limits[resource] = requested;
    ReleaseSRWLockExclusive(&limits_lock);
    return 0;
}

static int setrlimit_(int resource, const lx_rlimit *value)
{
    if (!value)
        return lx_set_errno(LX_EFAULT);
    lx_rlimit64 wide = {
        value->cur == UINT32_MAX ? UINT64_MAX : value->cur,
        value->max == UINT32_MAX ? UINT64_MAX : value->max
    };
    return setrlimit64_(resource, &wide);
}

static long sysconf_(int name)
{
    /* Numeric values are Linux i386 bits/confname.h values, never UCRT constants. */
    switch (name) {
    case 0: return 2097152;              /* _SC_ARG_MAX */
    case 1: return 4096;                 /* _SC_CHILD_MAX */
    case 2: return 100;                  /* _SC_CLK_TCK */
    case 3: return 65536;                /* _SC_NGROUPS_MAX */
    case 4: {                           /* _SC_OPEN_MAX */
        lx_rlimit64 value;
        if (getrlimit64_(7, &value) < 0)
            return -1;
        return (long)value.cur;
    }
    case 5: return 16;                   /* _SC_STREAM_MAX */
    case 29: return 200809;              /* _SC_VERSION */
    case 30: return 4096;                /* _SC_PAGESIZE */
    case 33: return INT_MAX;             /* _SC_SEM_VALUE_MAX */
    case 60: return 1024;                /* _SC_IOV_MAX */
    case 67:                            /* _SC_THREADS */
    case 68:                            /* _SC_THREAD_SAFE_FUNCTIONS */
    case 77:                            /* _SC_THREAD_ATTR_STACKADDR */
    case 78: return 200809;              /* _SC_THREAD_ATTR_STACKSIZE */
    case 69:                            /* _SC_GETGR_R_SIZE_MAX */
    case 70: return 1024;                /* _SC_GETPW_R_SIZE_MAX */
    case 71: return 256;                 /* _SC_LOGIN_NAME_MAX */
    case 72: return 32;                  /* _SC_TTY_NAME_MAX */
    case 73: return 4;                   /* _SC_THREAD_DESTRUCTOR_ITERATIONS */
    case 74: return 1024;                /* _SC_THREAD_KEYS_MAX */
    case 75: return 16384;               /* _SC_THREAD_STACK_MIN */
    case 83: return get_nprocs_conf_();  /* _SC_NPROCESSORS_CONF */
    case 84: return get_nprocs_();       /* _SC_NPROCESSORS_ONLN */
    case 85: return get_phys_pages_();  /* _SC_PHYS_PAGES */
    case 86: return get_avphys_pages_();/* _SC_AVPHYS_PAGES */
    case 87: return INT_MAX;             /* _SC_ATEXIT_MAX */
    case 88: return 8192;                /* _SC_PASS_MAX */
    case 180: return 64;                 /* _SC_HOST_NAME_MAX */
    default:
        /* Recognized but unsupported/indeterminate queries preserve errno. The gap after
         * the cache queries is reserved, not a set of valid Linux sysconf names. */
        if ((name >= 0 && name <= 199) || (name >= 235 && name <= 248))
            return -1;
        return lx_set_errno(LX_EINVAL);
    }
}

static uint32_t clamp_bytes(uint64_t n)
{
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

static int sysinfo_(lx_sysinfo *info)
{
    if (!info)
        return lx_set_errno(LX_EFAULT);
    MEMORYSTATUSEX memory;
    if (memory_status(&memory) < 0)
        return -1;
    lx_sysinfo value;
    memset(&value, 0, sizeof value);
    uint64_t seconds = GetTickCount64() / 1000;
    value.uptime = seconds > INT32_MAX ? INT32_MAX : (int32_t)seconds;
    value.totalram = clamp_bytes(memory.ullTotalPhys);
    value.freeram = clamp_bytes(memory.ullAvailPhys);
    /* Windows page-file figures are commit limits; subtract RAM for a swap estimate. */
    uint64_t swap = memory.ullTotalPageFile > memory.ullTotalPhys ?
                    memory.ullTotalPageFile - memory.ullTotalPhys : 0;
    uint64_t free_swap = memory.ullAvailPageFile > memory.ullAvailPhys ?
                         memory.ullAvailPageFile - memory.ullAvailPhys : 0;
    value.totalswap = clamp_bytes(swap);
    value.freeswap = clamp_bytes(free_swap < swap ? free_swap : swap);
    value.procs = 1; /* This guest process namespace cannot spawn children. */
    value.mem_unit = 1;
    *info = value;
    return 0;
}

static int uname_(lx_utsname *name)
{
    if (!name)
        return lx_set_errno(LX_EFAULT);
    lx_utsname value;
    memset(&value, 0, sizeof value);
    memcpy(value.sysname, "Linux", sizeof "Linux");
    memcpy(value.release, "5.4.0", sizeof "5.4.0");
    memcpy(value.version, "#1 lxrt", sizeof "#1 lxrt");
    memcpy(value.machine, "i686", sizeof "i686");
    memcpy(value.domainname, "(none)", sizeof "(none)");
    wchar_t host[256];
    DWORD size = LX_COUNT(host);
    if (!GetComputerNameExW(ComputerNameDnsHostname, host, &size))
        return lx_fail_win32();
    /* Convert before truncating so a hostname can never overrun the 65-byte ABI field. */
    char utf8[sizeof host * 2];
    if (!WideCharToMultiByte(CP_UTF8, 0, host, -1, utf8, sizeof utf8, NULL, NULL))
        return lx_fail_win32();
    size_t length = strlen(utf8);
    if (length > sizeof value.nodename - 1)
        length = sizeof value.nodename - 1;
    memcpy(value.nodename, utf8, length);
    *name = value;
    return 0;
}

static int piu_mount_target(const char *target)
{
    return target && (!strcmp(target, "/mnt/0") || !strcmp(target, "/mnt/1") ||
                      !strcmp(target, "/mnt/hd"));
}

static int mount_(const char *source, const char *target, const char *type,
                  unsigned long flags, const void *data)
{
    (void)data;
    lx_log("mount source=%s target=%s type=%s flags=%#lx: compatibility no-op only",
           source ? source : "(null)", target ? target : "(null)", type ? type : "(null)", flags);
    if (!target)
        return lx_set_errno(LX_EFAULT);
    return piu_mount_target(target) ? 0 : lx_set_errno(LX_ENODEV);
}

static int umount2_(const char *target, int flags)
{
    lx_log("umount2 target=%s flags=%#x: compatibility no-op only", target ? target : "(null)", flags);
    if (!target)
        return lx_set_errno(LX_EFAULT);
    if ((flags & ~15) || ((flags & 4) && (flags & 3)))
        return lx_set_errno(LX_EINVAL);
    return piu_mount_target(target) ? 0 : lx_set_errno(LX_EINVAL);
}

static int umount_(const char *target) { return umount2_(target, 0); }

static int ioperm_(unsigned long from, unsigned long count, int enable)
{
    lx_log("ioperm from=%#lx count=%lu enable=%d: denied", from, count, enable);
    return lx_set_errno(LX_EPERM);
}

static int iopl_(int level)
{
    lx_log("iopl level=%d: denied", level);
    return lx_set_errno(LX_EPERM);
}

static int reboot_(int command)
{
    lx_log("reboot command=%#x: denied; host reboot is never invoked", command);
    return lx_set_errno(LX_EPERM);
}

static int daemon_(int nochdir, int noclose)
{
    lx_log("daemon nochdir=%d noclose=%d: unsupported", nochdir, noclose);
    return lx_set_errno(LX_ENOSYS);
}

static __declspec(thread) unsigned long current_personality;

static int personality_(unsigned long persona)
{
    if (persona == UINT32_MAX)
        return (int)current_personality;
    /* The guest already has the 32-bit Linux ABI. Other ABI/VM-behavior flags are unsupported. */
    if (persona != 0 && persona != 8)
        return lx_set_errno(LX_EINVAL);
    unsigned long old = current_personality;
    current_personality = persona;
    return (int)old;
}

static char login_name[] = "piu";
static char *getlogin_(void) { return login_name; }

static int getlogin_r_(char *buffer, size_t size)
{
    if (size < sizeof login_name)
        return LX_ERANGE;
    if (!buffer)
        return LX_EFAULT;
    memcpy(buffer, login_name, sizeof login_name);
    return 0;
}

static int fill_passwd(lx_passwd *record, char *buffer, size_t size, lx_passwd **result)
{
    if (!result)
        return LX_EINVAL;
    *result = NULL;
    if (!record || !buffer)
        return LX_EINVAL;
    const char *fields[] = {"piu", "x", "piu", lx_exe_dir, "/bin/sh"};
    size_t lengths[LX_COUNT(fields)];
    size_t total = 0;
    for (unsigned i = 0; i < LX_COUNT(fields); ++i) {
        lengths[i] = strlen(fields[i]) + 1;
        total += lengths[i];
    }
    if (size < total)
        return LX_ERANGE;
    char *values[LX_COUNT(fields)];
    for (unsigned i = 0; i < LX_COUNT(fields); ++i) {
        values[i] = buffer;
        memcpy(buffer, fields[i], lengths[i]);
        buffer += lengths[i];
    }
    record->name = values[0];
    record->passwd = values[1];
    record->uid = record->gid = 1000;
    record->gecos = values[2];
    record->dir = values[3];
    record->shell = values[4];
    *result = record;
    return 0;
}

static int getpwuid_r_(uint32_t uid, lx_passwd *record, char *buffer, size_t size, lx_passwd **result)
{
    if (!result)
        return LX_EINVAL;
    *result = NULL;
    if (!record || !buffer)
        return LX_EINVAL;
    return uid == 1000 ? fill_passwd(record, buffer, size, result) : 0;
}

static int getpwnam_r_(const char *name, lx_passwd *record, char *buffer, size_t size, lx_passwd **result)
{
    if (!result)
        return LX_EINVAL;
    *result = NULL;
    if (!name || !record || !buffer)
        return LX_EINVAL;
    return !strcmp(name, "piu") ? fill_passwd(record, buffer, size, result) : 0;
}

static INIT_ONCE passwd_once = INIT_ONCE_STATIC_INIT;
static lx_passwd passwd_record;
static char passwd_buffer[sizeof lx_exe_dir + 32];

static BOOL CALLBACK initialize_passwd(PINIT_ONCE once, PVOID arg, PVOID *context)
{
    (void)once;
    (void)arg;
    (void)context;
    lx_passwd *result;
    return fill_passwd(&passwd_record, passwd_buffer, sizeof passwd_buffer, &result) == 0;
}

static lx_passwd *getpwuid_(uint32_t uid)
{
    if (uid != 1000)
        return NULL; /* A missing record is not a database error and preserves errno. */
    if (!InitOnceExecuteOnce(&passwd_once, initialize_passwd, NULL, NULL)) {
        lx_errno = LX_EIO;
        return NULL;
    }
    return &passwd_record;
}

static lx_passwd *getpwnam_(const char *name)
{
    if (!name) {
        lx_errno = LX_EINVAL;
        return NULL;
    }
    return !strcmp(name, "piu") ? getpwuid_(1000) : NULL;
}

LX_SYMTAB(process,
    LX_SYM("fork", fork_), LX_SYM("vfork", vfork_),
    LX_SYM("execl", execl_), LX_SYM("execv", execv_), LX_SYM("execvp", execvp_),
    LX_SYM("execve", execve_), LX_SYM("execle", execle_), LX_SYM("system", system_),
    LX_SYM("waitpid", waitpid_), LX_SYM("wait", wait_), LX_SYM("wait3", wait3_), LX_SYM("wait4", wait4_),
    LX_SYM("getpid", lx_getpid), LX_SYM("getppid", lx_getppid), LX_SYM("gettid", lx_gettid),
    LX_SYM("getuid", uid_), LX_SYM("geteuid", uid_), LX_SYM("getgid", uid_), LX_SYM("getegid", uid_),
    LX_SYM("setuid", setid_), LX_SYM("setgid", setid_), LX_SYM("setsid", setsid_),
    LX_SYM("setpgid", setpgid_), LX_SYM("getpgrp", getpgrp_), LX_SYM("kill", lx_kill),
    LX_SYM("getpriority", getpriority_), LX_SYM("setpriority", setpriority_), LX_SYM("nice", nice_),
    LX_SYM("prctl", prctl_), LX_SYM("sysconf", sysconf_),
    LX_SYM("get_nprocs", get_nprocs_), LX_SYM("get_nprocs_conf", get_nprocs_conf_),
    LX_SYM("get_phys_pages", get_phys_pages_), LX_SYM("get_avphys_pages", get_avphys_pages_),
    LX_SYM("getpagesize", getpagesize_), LX_SYM("sysinfo", sysinfo_), LX_SYM("uname", uname_),
    LX_SYM("getrlimit", getrlimit_), LX_SYM("setrlimit", setrlimit_),
    LX_SYM("getrlimit64", getrlimit64_), LX_SYM("setrlimit64", setrlimit64_),
    LX_SYM("mount", mount_), LX_SYM("umount", umount_), LX_SYM("umount2", umount2_),
    LX_SYM("ioperm", ioperm_), LX_SYM("iopl", iopl_), LX_SYM("reboot", reboot_),
    LX_SYM("getlogin", getlogin_), LX_SYM("getlogin_r", getlogin_r_),
    LX_SYM("getpwuid", getpwuid_), LX_SYM("getpwnam", getpwnam_),
    LX_SYM("getpwuid_r", getpwuid_r_), LX_SYM("getpwnam_r", getpwnam_r_),
    LX_SYM("daemon", daemon_), LX_SYM("personality", personality_),
);
