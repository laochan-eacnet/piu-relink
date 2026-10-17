#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* Default: portable sanity checks with identical, value-independent output on lxrt and
 * Linux/qemu. --sandbox additionally checks lxrt-only process/privilege contracts, guarded
 * by synthetic uname identity. Every exec request names a deliberately nonexistent file. */
_Static_assert(sizeof(struct sysinfo) == 64, "i386 sysinfo");
_Static_assert(offsetof(struct sysinfo, totalram) == 16, "sysinfo RAM offset");
_Static_assert(offsetof(struct sysinfo, mem_unit) == 52, "sysinfo unit offset");
_Static_assert(sizeof(struct utsname) == 390, "i386 utsname");
_Static_assert(offsetof(struct utsname, machine) == 260, "utsname machine offset");
_Static_assert(offsetof(struct utsname, domainname) == 325, "utsname domain offset");
_Static_assert(sizeof(struct passwd) == 28, "i386 passwd");
_Static_assert(offsetof(struct passwd, pw_uid) == 8, "passwd uid offset");
_Static_assert(offsetof(struct passwd, pw_dir) == 20, "passwd home offset");
_Static_assert(sizeof(struct rlimit) == 8, "i386 rlimit");
_Static_assert(offsetof(struct rlimit, rlim_max) == 4, "rlimit maximum offset");
_Static_assert(sizeof(struct rlimit64) == 16, "i386 rlimit64");
_Static_assert(offsetof(struct rlimit64, rlim_max) == 8, "rlimit64 maximum offset");
_Static_assert(_Alignof(struct rlimit64) == 4, "i386 rlimit64 alignment");
_Static_assert(_SC_CLK_TCK == 2 && _SC_OPEN_MAX == 4 && _SC_PAGESIZE == 30, "Linux sysconf numbers");
_Static_assert(_SC_GETPW_R_SIZE_MAX == 70 && _SC_NPROCESSORS_CONF == 83, "Linux sysconf numbers");
_Static_assert(_SC_NPROCESSORS_ONLN == 84 && _SC_PHYS_PAGES == 85 && _SC_AVPHYS_PAGES == 86,
               "Linux sysconf numbers");

static int failures;
#define CHECK(expression) do { \
    if (!(expression)) { \
        printf("process:%d: %s (errno=%d)\n", __LINE__, #expression, errno); \
        ++failures; \
    } \
} while (0)

static void test_system_information(const struct utsname *name, int sandbox)
{
    CHECK(!strcmp(name->sysname, "Linux"));
    CHECK(name->release[0] != 0 && memchr(name->release, 0, sizeof name->release) != NULL);
    CHECK(name->machine[0] != 0 && memchr(name->machine, 0, sizeof name->machine) != NULL);
    CHECK(name->nodename[0] != 0 && memchr(name->nodename, 0, sizeof name->nodename) != NULL);

    long page_size = sysconf(30);
    CHECK(page_size > 0 && !(page_size & (page_size - 1)));
    CHECK(getpagesize() == page_size);
    CHECK(sysconf(2) == 100);
    CHECK(sysconf(4) > 0);
    CHECK(sysconf(_SC_GETPW_R_SIZE_MAX) >= 1024);
    CHECK(sysconf(_SC_LOGIN_NAME_MAX) >= 4);
    long online = sysconf(84), configured = sysconf(83);
    CHECK(online > 0 && configured >= online);
    CHECK(get_nprocs() > 0 && get_nprocs_conf() > 0);
    CHECK(sysconf(85) > 0 && sysconf(86) >= 0);
    CHECK(get_phys_pages() > 0 && get_avphys_pages() >= 0);
    errno = 0;
    CHECK(sysconf(-1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(sysconf(200) == -1 && errno == EINVAL); /* Reserved confname.h gap. */

    struct {
        uint32_t before;
        struct sysinfo info;
        uint32_t after;
    } guarded;
    memset(&guarded, 0xa5, sizeof guarded);
    guarded.before = 0x12345678;
    guarded.after = 0x87654321;
    int result = sysinfo(&guarded.info);
    CHECK(result == 0);
    CHECK(guarded.before == 0x12345678 && guarded.after == 0x87654321);
    if (result != 0)
        return;
    CHECK(guarded.info.mem_unit != 0);
    CHECK(guarded.info.totalram > 0 && guarded.info.freeram <= guarded.info.totalram);
    CHECK(guarded.info.uptime >= 0 && guarded.info.procs > 0);
    CHECK((uint64_t)guarded.info.totalram * guarded.info.mem_unit >= (uint64_t)page_size);
    if (sandbox) {
        CHECK(!strcmp(name->release, "5.4.0"));
        CHECK(!strcmp(name->machine, "i686"));
        CHECK(!strcmp(name->domainname, "(none)"));
        CHECK(page_size == 4096 && sysconf(4) == 4096);
        errno = EDOM;
        CHECK(sysconf(_SC_TZNAME_MAX) == -1 && errno == EDOM);
        CHECK(guarded.info.mem_unit == 1 && guarded.info.procs == 1);
        CHECK(guarded.info.freeswap <= guarded.info.totalswap);
        CHECK(guarded.info.loads[0] == 0 && guarded.info.loads[1] == 0 && guarded.info.loads[2] == 0);
        CHECK(guarded.info.sharedram == 0 && guarded.info.bufferram == 0);
        CHECK(guarded.info.totalhigh == 0 && guarded.info.freehigh == 0);
        for (size_t i = 0; i < sizeof guarded.info._f; ++i)
            CHECK(guarded.info._f[i] == 0);
    }
}

static int within_buffer(const char *pointer, const char *buffer, size_t length)
{
    return (uintptr_t)pointer >= (uintptr_t)buffer &&
           (uintptr_t)pointer < (uintptr_t)buffer + length;
}

static void test_passwd_shape(void)
{
    uid_t uid = getuid();
    struct passwd *record = getpwuid(uid);
    CHECK(record != NULL);
    if (!record)
        return;
    CHECK(record->pw_uid == uid);
    CHECK(record->pw_name != NULL && record->pw_name[0] != 0);
    CHECK(record->pw_passwd != NULL && record->pw_gecos != NULL);
    CHECK(record->pw_dir != NULL && record->pw_dir[0] != 0);
    CHECK(record->pw_shell != NULL);
    if (!record->pw_name)
        return;
    char name[256];
    size_t length = strlen(record->pw_name);
    CHECK(length < sizeof name);
    if (length >= sizeof name)
        return;
    memcpy(name, record->pw_name, length + 1);
    CHECK(getpwnam(name) != NULL);

    char buffer[4096];
    struct passwd value;
    struct passwd *result = NULL;
    CHECK(getpwuid_r(uid, &value, buffer, sizeof buffer, &result) == 0 && result == &value);
    if (result == &value) {
        CHECK(value.pw_uid == uid && !strcmp(value.pw_name, name));
        CHECK(within_buffer(value.pw_name, buffer, sizeof buffer));
        CHECK(within_buffer(value.pw_dir, buffer, sizeof buffer));
    }
    result = NULL;
    CHECK(getpwnam_r(name, &value, buffer, sizeof buffer, &result) == 0 && result == &value);
    if (result == &value)
        CHECK(value.pw_uid == uid);
    result = (struct passwd *)(uintptr_t)1;
    CHECK(getpwnam_r(name, &value, buffer, 1, &result) == ERANGE && result == NULL);
    result = (struct passwd *)(uintptr_t)1;
    CHECK(getpwuid_r(UINT32_MAX, &value, buffer, sizeof buffer, &result) == 0 && result == NULL);
}

static void test_passwd(void)
{
    CHECK(getuid() == 1000 && geteuid() == 1000 && getgid() == 1000 && getegid() == 1000);
    struct passwd *record = getpwuid(1000);
    CHECK(record != NULL);
    if (record) {
        CHECK(!strcmp(record->pw_name, "piu"));
        CHECK(record->pw_uid == 1000 && record->pw_gid == 1000);
        CHECK(record->pw_dir && record->pw_dir[0]);
        CHECK(record->pw_dir[0] == '/' || (strlen(record->pw_dir) > 2 && record->pw_dir[1] == ':'));
        CHECK(!strcmp(record->pw_shell, "/bin/sh"));
    }
    CHECK(getpwnam("piu") != NULL);
    errno = EDOM;
    CHECK(getpwuid(UINT32_MAX) == NULL && errno == EDOM);
    CHECK(getpwnam("definitely-missing-piu-user") == NULL && errno == EDOM);
    CHECK(!strcmp(getlogin(), "piu"));
    char login[4];
    CHECK(getlogin_r(login, sizeof login) == 0 && !strcmp(login, "piu"));
    CHECK(getlogin_r(login, 3) == ERANGE);

    char buffer[1024];
    struct passwd value;
    struct passwd *result = (struct passwd *)(uintptr_t)1;
    errno = EDOM;
    CHECK(getpwuid_r(1000, &value, buffer, sizeof buffer, &result) == 0 && result == &value);
    CHECK(errno == EDOM);
    if (result == &value) {
        CHECK(!strcmp(value.pw_name, "piu"));
        CHECK(value.pw_uid == 1000 && value.pw_gid == 1000);
        CHECK(within_buffer(value.pw_name, buffer, sizeof buffer));
        CHECK(within_buffer(value.pw_passwd, buffer, sizeof buffer));
        CHECK(within_buffer(value.pw_gecos, buffer, sizeof buffer));
        CHECK(within_buffer(value.pw_dir, buffer, sizeof buffer));
        CHECK(within_buffer(value.pw_shell, buffer, sizeof buffer));
    }
    result = (struct passwd *)(uintptr_t)1;
    CHECK(getpwnam_r("piu", &value, buffer, 1, &result) == ERANGE && result == NULL);
    result = (struct passwd *)(uintptr_t)1;
    CHECK(getpwnam_r("definitely-missing-piu-user", &value, buffer, sizeof buffer, &result) == 0);
    CHECK(result == NULL);
    result = NULL;
    CHECK(getpwnam_r("piu", &value, buffer, sizeof buffer, &result) == 0 && result == &value);
    if (result == &value && record)
        CHECK(!strcmp(value.pw_dir, record->pw_dir));
}

static void test_limit_shapes(void)
{
    static const int resources[] = {RLIMIT_STACK, RLIMIT_NOFILE};
    for (size_t i = 0; i < sizeof resources / sizeof resources[0]; ++i) {
        struct rlimit value;
        struct rlimit64 wide;
        int narrow_ok = getrlimit(resources[i], &value);
        int wide_ok = getrlimit64(resources[i], &wide);
        CHECK(narrow_ok == 0 && wide_ok == 0);
        if (narrow_ok != 0 || wide_ok != 0)
            continue;
        CHECK(value.rlim_cur > 0 && value.rlim_cur <= value.rlim_max);
        CHECK(wide.rlim_cur > 0 && wide.rlim_cur <= wide.rlim_max);
        CHECK(value.rlim_cur == (wide.rlim_cur > UINT32_MAX ? RLIM_INFINITY : wide.rlim_cur));
        CHECK(value.rlim_max == (wide.rlim_max > UINT32_MAX ? RLIM_INFINITY : wide.rlim_max));
    }
}

static void test_missing_exec(void)
{
    const char *missing = "/definitely-missing-piu-executable";
    char *args[] = {(char *)missing, "test-argument", NULL};
    char *environment[] = {"LXRT_PROCESS_TEST=1", NULL};
    errno = 0;
    CHECK(execl(missing, missing, "test-argument", (char *)NULL) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(execle(missing, missing, "test-argument", (char *)NULL, environment) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(execv(missing, args) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(execvp(missing, args) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(execve(missing, args, environment) == -1 && errno == ENOENT);
}

static void test_limits(void)
{
    struct rlimit limit;
    struct rlimit64 wide;
    CHECK(getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur == 8u * 1024u * 1024u);
    CHECK(getrlimit64(RLIMIT_STACK, &wide) == 0 && wide.rlim_cur == 8u * 1024u * 1024u);
    CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur == 4096 && limit.rlim_max == 4096);
    CHECK(getrlimit64(RLIMIT_NOFILE, &wide) == 0 && wide.rlim_cur == 4096 && wide.rlim_max == 4096);
    struct rlimit64 changed = {2048, 4096};
    CHECK(setrlimit64(RLIMIT_NOFILE, &changed) == 0);
    CHECK(sysconf(_SC_OPEN_MAX) == 2048);
    CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur == 2048);
    limit.rlim_cur = limit.rlim_max = 4096;
    CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    CHECK(sysconf(_SC_OPEN_MAX) == 4096);
    changed.rlim_cur = 4097;
    changed.rlim_max = 4096;
    errno = 0;
    CHECK(setrlimit64(RLIMIT_NOFILE, &changed) == -1 && errno == EINVAL);
    changed.rlim_cur = changed.rlim_max = 8192;
    errno = 0;
    CHECK(setrlimit64(RLIMIT_NOFILE, &changed) == -1 && errno == EPERM);
    errno = 0;
    CHECK(getrlimit(16, &limit) == -1 && errno == EINVAL);
}

static volatile sig_atomic_t signal_calls;

static void record_signal(int sig)
{
    if (sig == SIGUSR1)
        ++signal_calls;
}

static void test_process_control(void)
{
    CHECK(getpid() > 0 && getpid() == getpid());
    CHECK(getppid() >= 0 && gettid() > 0);
    CHECK(getpgrp() == getpid());
    CHECK(setpgid(0, 0) == 0 && setsid() == getpid());
    CHECK(setuid(1000) == 0 && setgid(1000) == 0);
    errno = 0;
    CHECK(setpgid(-1, 0) == -1 && errno == EINVAL);
    CHECK(kill(getpid(), 0) == 0);
    errno = 0;
    CHECK(kill(-1, 0) == -1 && errno == ESRCH);
    errno = 0;
    CHECK(kill(getpid(), 65) == -1 && errno == EINVAL);
    struct sigaction action, old_action;
    memset(&action, 0, sizeof action);
    action.sa_handler = record_signal;
    CHECK(sigemptyset(&action.sa_mask) == 0);
    int installed = sigaction(SIGUSR1, &action, &old_action);
    CHECK(installed == 0);
    if (installed == 0) {
        CHECK(kill(getpid(), SIGUSR1) == 0);
        CHECK(signal_calls == 1);
        CHECK(sigaction(SIGUSR1, &old_action, NULL) == 0);
    }

    errno = 0;
    CHECK(fork() == -1 && errno == ENOSYS);
    errno = 0;
    CHECK(vfork() == -1 && errno == ENOSYS);
    int status = 0x13572468;
    struct rusage usage, original;
    memset(&usage, 0x5a, sizeof usage);
    original = usage;
    errno = 0;
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    CHECK(status == 0x13572468);
    errno = 0;
    CHECK(wait(&status) == -1 && errno == ECHILD);
    errno = 0;
    CHECK(wait3(&status, 0, &usage) == -1 && errno == ECHILD);
    errno = 0;
    CHECK(wait4(-1, &status, 0, &usage) == -1 && errno == ECHILD);
    CHECK(!memcmp(&usage, &original, sizeof usage) && status == 0x13572468);
    errno = 0;
    CHECK(waitpid(-1, NULL, 4) == -1 && errno == EINVAL);

    CHECK(system(NULL) == 0);
    CHECK(system("/definitely-missing-piu-executable") == (127 << 8));
    errno = 0;
    CHECK(daemon(1, 1) == -1 && errno == ENOSYS);
}

static void test_priority_and_name(void)
{
    errno = 0;
    int priority = getpriority(PRIO_PROCESS, 0);
    CHECK(priority == 0 && errno == 0);
    CHECK(setpriority(PRIO_PROCESS, 0, priority) == 0);
    CHECK(nice(0) == priority);
    errno = 0;
    CHECK(getpriority(3, 0) == -1 && errno == EINVAL);
    CHECK(prctl(PR_SET_NAME, "piu-process-test-long") == 0);
    char name[16];
    memset(name, 0xa5, sizeof name);
    CHECK(prctl(PR_GET_NAME, name) == 0);
    CHECK(!memcmp(name, "piu-process-tes", 15) && name[15] == 0);
    errno = 0;
    CHECK(prctl(-1) == -1 && errno == EINVAL);
    CHECK(personality(0xffffffffUL) == PER_LINUX);
    CHECK(personality(PER_LINUX32) == PER_LINUX);
    CHECK(personality(0xffffffffUL) == PER_LINUX32);
    CHECK(personality(PER_LINUX) == PER_LINUX32);
    errno = 0;
    CHECK(personality(ADDR_NO_RANDOMIZE) == -1 && errno == EINVAL);
}

static void test_privilege_sandbox(void)
{
    static const char *const targets[] = {"/mnt/0", "/mnt/1", "/mnt/hd"};
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; ++i) {
        CHECK(mount("/definitely-missing-piu-device", targets[i], "vfat", 0x498, NULL) == 0);
        CHECK(umount(targets[i]) == 0);
        CHECK(umount2(targets[i], MNT_DETACH) == 0);
    }
    errno = 0;
    CHECK(mount("/definitely-missing-piu-device", "/mnt/2", "vfat", 0, NULL) == -1 && errno == ENODEV);
    errno = 0;
    CHECK(umount("/mnt/2") == -1 && errno == EINVAL);
    errno = 0;
    CHECK(umount2("/mnt/0", 16) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(ioperm(0, 1, 1) == -1 && errno == EPERM);
    errno = 0;
    CHECK(iopl(3) == -1 && errno == EPERM);
    errno = 0;
    CHECK(reboot(RB_AUTOBOOT) == -1 && errno == EPERM);
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--sandbox"))) {
        puts("usage: process [--sandbox]");
        return 2;
    }
    int sandbox = argc == 2;
    struct utsname name;
    memset(&name, 0xa5, sizeof name);
    int result = uname(&name);
    CHECK(result == 0);
    if (result != 0)
        return 1;
    int is_lxrt = memchr(name.version, 0, sizeof name.version) &&
                  !strcmp(name.version, "#1 lxrt");
    test_system_information(&name, sandbox && is_lxrt);
    test_passwd_shape();
    test_limit_shapes();
    test_missing_exec();
    if (sandbox) {
        CHECK(is_lxrt);
        if (is_lxrt) {
            test_passwd();
            test_limits();
            test_process_control();
            test_priority_and_name();
            test_privilege_sandbox();
        }
    }
    if (failures)
        printf("process: %d failures\n", failures);
    else
        puts("process: ok");
    return failures ? 1 : 0;
}
