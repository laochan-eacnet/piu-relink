/* Deterministic i386 guest acceptance test; compare stdout under qemu-i386 and lxrt.
 * Actual setjmp (not the glibc header's _setjmp macro) is intentionally exercised.
 * No test depends on native signal frames, alternate stacks, or timer callback TLS. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#undef setjmp
typedef void (*test_sighandler)(int);
/* glibc hides the legacy declaration in its GNU/XOPEN2K8 header mode. */
extern test_sighandler bsd_signal(int, test_sighandler);


extern void checked_jump(struct __jmp_buf_tag *, int)
    __asm__("__longjmp_chk") __attribute__((noreturn));

_Static_assert(sizeof(sigset_t) == 128, "i386 sigset_t size");
_Static_assert(sizeof(struct sigaction) == 140, "i386 sigaction size");
_Static_assert(offsetof(struct sigaction, sa_handler) == 0, "handler offset");
_Static_assert(offsetof(struct sigaction, sa_mask) == 4, "mask offset");
_Static_assert(offsetof(struct sigaction, sa_flags) == 132, "flags offset");
_Static_assert(offsetof(struct sigaction, sa_restorer) == 136, "restorer offset");
_Static_assert(sizeof(siginfo_t) == 128, "i386 siginfo_t size");
_Static_assert(sizeof(jmp_buf) == 156, "i386 jmp_buf size");
_Static_assert(offsetof(struct __jmp_buf_tag, __mask_was_saved) == 24, "jump flag offset");
_Static_assert(offsetof(struct __jmp_buf_tag, __saved_mask) == 28, "jump mask offset");
_Static_assert(sizeof(struct timeval) == 8, "i386 timeval size");
_Static_assert(sizeof(struct itimerval) == 16, "i386 itimerval size");

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL signals.c:%d\n", __LINE__); \
        return 0; \
    } \
} while (0)

static volatile sig_atomic_t caught, info_ok, handler_masks_ok, nested_ok;
static volatile sig_atomic_t alarms;
static sigjmp_buf handler_escape;

__attribute__((noinline)) static void deep_jump(jmp_buf env, int value, int depth)
{
    volatile uint32_t scratch[32];
    for (unsigned i = 0; i < 32; ++i)
        scratch[i] = (uint32_t)depth * 37 + i;
    if (depth)
        deep_jump(env, value, depth - 1);
    else
        longjmp(env, value);
    /* The volatile read also prevents tail-call conversion of recursive frames. */
    caught += (sig_atomic_t)scratch[0];
}

static void basic_handler(int sig)
{
    if (sig == SIGUSR1)
        ++caught;
}

static void info_handler(int sig, siginfo_t *info, void *context)
{
    sigset_t current;
    (void)context; /* lxrt has no synthesized guest ucontext for explicit raises. */
    ++caught;
    info_ok = sig == SIGUSR1 && info && info->si_signo == sig &&
              info->si_errno == 0 && info->si_code == SI_TKILL &&
              info->si_pid == getpid() && info->si_uid == getuid();
    if (sigprocmask(SIG_SETMASK, NULL, &current) == 0)
        handler_masks_ok = sigismember(&current, SIGUSR1) == 1 &&
                           sigismember(&current, SIGUSR2) == 1;
}

static void escaping_handler(int sig)
{
    (void)sig;
    siglongjmp(handler_escape, 9);
}

static void local_jump_handler(int sig)
{
    jmp_buf local;
    volatile int marker = 17;
    int result = _setjmp(local);
    if (result == 0)
        deep_jump(local, 4, 3);
    if (sig == SIGUSR1 && result == 4 && marker == 17)
        nested_ok = 1;
}

static void alarm_handler(int sig)
{
    if (sig == SIGALRM)
        ++alarms;
}

static int jump_tests(void)
{
    jmp_buf outer, inner, checked;
    volatile int outer_marker = 11, inner_marker = 22;
    volatile int inner_seen = 0;
    int outer_result = setjmp(outer);
    if (outer_result == 0) {
        int inner_result = _setjmp(inner);
        if (inner_result == 0)
            deep_jump(inner, 7, 6);
        CHECK(inner_result == 7 && inner_marker == 22);
        inner_seen = 1;
        deep_jump(outer, 0, 5);
    }
    CHECK(outer_result == 1 && outer_marker == 11 && inner_seen == 1);
    int checked_result = _setjmp(checked);
    if (checked_result == 0)
        checked_jump(checked, 13);
    CHECK(checked_result == 13);
    int underscore_result = _setjmp(checked);
    if (underscore_result == 0)
        _longjmp(checked, 0);
    CHECK(underscore_result == 1);
    puts("jumps: ok");
    return 1;
}

static int signal_tests(void)
{
    struct sigaction action, old, saved, query;
    sigset_t blocked, previous;
    memset(&action, 0, sizeof(action));
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaddset(&action.sa_mask, SIGUSR2) == 0);
    action.sa_sigaction = info_handler;
    action.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGUSR1, &action, &saved) == 0);
    caught = info_ok = handler_masks_ok = 0;
    CHECK(raise(SIGUSR1) == 0);
    CHECK(caught == 1 && info_ok && handler_masks_ok);

    action.sa_handler = basic_handler;
    action.sa_flags = SA_RESTART;
    CHECK(sigaction(SIGUSR1, &action, &old) == 0);
    CHECK(old.sa_sigaction == info_handler && (old.sa_flags & SA_SIGINFO));
    CHECK(sigismember(&old.sa_mask, SIGUSR2) == 1);
    CHECK(sigaction(SIGUSR1, &old, &old) == 0); /* Input/output aliasing. */
    CHECK(old.sa_handler == basic_handler);

    CHECK(sigemptyset(&blocked) == 0);
    CHECK(sigaddset(&blocked, SIGUSR1) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, &previous) == 0);
    caught = 0;
    CHECK(raise(SIGUSR1) == 0 && raise(SIGUSR1) == 0);
    CHECK(caught == 0);
    CHECK(sigprocmask(SIG_UNBLOCK, &blocked, NULL) == 0);
    CHECK(caught == 1 && info_ok && handler_masks_ok); /* Standard signals coalesce. */
    CHECK(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);

    CHECK(signal(SIGUSR1, basic_handler) == (test_sighandler)info_handler);
    CHECK(sigaction(SIGUSR1, NULL, &query) == 0);
    CHECK(query.sa_flags & SA_RESTART);
    CHECK(raise(SIGUSR1) == 0 && caught == 2);
    CHECK(bsd_signal(SIGUSR1, basic_handler) == basic_handler);
    CHECK(sysv_signal(SIGUSR1, basic_handler) == basic_handler);
    CHECK(sigaction(SIGUSR1, NULL, &query) == 0);
    CHECK((query.sa_flags & (SA_RESETHAND | SA_NODEFER)) == (SA_RESETHAND | SA_NODEFER));
    CHECK(raise(SIGUSR1) == 0 && caught == 3);
    CHECK(sigaction(SIGUSR1, NULL, &query) == 0 && query.sa_handler == SIG_DFL);
    CHECK(signal(SIGUSR1, SIG_IGN) == SIG_DFL);
    CHECK(raise(SIGUSR1) == 0 && caught == 3);

    CHECK(signal(SIGUSR1, local_jump_handler) == SIG_IGN);
    CHECK(raise(SIGUSR1) == 0 && nested_ok);
    CHECK(signal(SIGUSR1, escaping_handler) == local_jump_handler);
    int escaped = sigsetjmp(handler_escape, 1);
    if (escaped == 0)
        raise(SIGUSR1);
    CHECK(escaped == 9);
    CHECK(sigprocmask(SIG_SETMASK, NULL, &blocked) == 0);
    CHECK(sigismember(&blocked, SIGUSR1) == 0);
    CHECK(signal(SIGUSR1, basic_handler) == escaping_handler);
    CHECK(raise(SIGUSR1) == 0 && caught == 4);
    CHECK(sigaction(SIGUSR1, &saved, NULL) == 0);

    const int ignored[] = {SIGCHLD, SIGURG, SIGWINCH, SIGCONT};
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    CHECK(sigemptyset(&action.sa_mask) == 0);
    for (unsigned i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        CHECK(sigaction(ignored[i], &action, &old) == 0);
        CHECK(raise(ignored[i]) == 0);
        CHECK(sigaction(ignored[i], &old, NULL) == 0);
    }
    errno = EDOM;
    CHECK(raise(0) == 0 && errno == EDOM && caught == 4);
    CHECK(raise(-1) == -1 && errno == EINVAL);
    CHECK(raise(65) == -1 && errno == EINVAL);
    CHECK(sigaction(SIGKILL, &action, NULL) == -1 && errno == EINVAL);
    puts("signals: ok");
    return 1;
}

static void *mask_worker(void *argument)
{
    sigset_t mask, second;
    (void)argument;
    if (pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0 ||
        sigismember(&mask, SIGUSR1) != 1 || sigismember(&mask, SIGUSR2) != 0)
        return (void *)(uintptr_t)1;
    sigemptyset(&second);
    sigaddset(&second, SIGUSR2);
    if (pthread_sigmask(SIG_BLOCK, &second, NULL) != 0 ||
        pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0 ||
        sigismember(&mask, SIGUSR1) != 1 || sigismember(&mask, SIGUSR2) != 1)
        return (void *)(uintptr_t)1;
    return NULL;
}

static int mask_tests(void)
{
    struct { uint32_t before; sigset_t set; uint32_t after; } guarded;
    sigset_t mask, original, current, second;
    sigjmp_buf env;
    pthread_t thread;
    void *thread_result;
    guarded.before = UINT32_C(0x12345678);
    guarded.after = UINT32_C(0x87654321);
    memset(&guarded.set, 0xa5, sizeof(guarded.set));
    CHECK(sigemptyset(&guarded.set) == 0);
    for (unsigned i = 0; i < sizeof(guarded.set); ++i)
        CHECK(((const unsigned char *)&guarded.set)[i] == 0);
    CHECK(sigfillset(&guarded.set) == 0);
    CHECK(sigismember(&guarded.set, 1) == 1 && sigismember(&guarded.set, 64) == 1);
    CHECK(sigdelset(&guarded.set, 64) == 0 && sigismember(&guarded.set, 64) == 0);
    CHECK(sigaddset(&guarded.set, 64) == 0 && sigismember(&guarded.set, 64) == 1);
    CHECK(guarded.before == UINT32_C(0x12345678) && guarded.after == UINT32_C(0x87654321));
    errno = 0;
    CHECK(sigaddset(&guarded.set, 0) == -1 && errno == EINVAL);
    CHECK(sigismember(&guarded.set, 65) == -1 && errno == EINVAL);
    CHECK(SIGRTMIN == 34 && SIGRTMAX == 64);

    CHECK(sigemptyset(&mask) == 0 && sigaddset(&mask, SIGUSR1) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &mask, &original) == 0);
    CHECK(sigemptyset(&second) == 0 && sigaddset(&second, SIGUSR2) == 0);
    int saved = sigsetjmp(env, 1);
    if (saved == 0) {
        CHECK(sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0);
        CHECK(sigprocmask(SIG_BLOCK, &second, NULL) == 0);
        siglongjmp(env, 3);
    }
    CHECK(saved == 3);
    CHECK(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    CHECK(sigismember(&current, SIGUSR1) == 1 && sigismember(&current, SIGUSR2) == 0);

    int actual_setjmp = setjmp(env);
    if (actual_setjmp == 0) {
        CHECK(sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0);
        longjmp(env, 5);
    }
    CHECK(actual_setjmp == 5);
    CHECK(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    CHECK(sigismember(&current, SIGUSR1) == 1);

    int unsaved = __sigsetjmp(env, 0);
    if (unsaved == 0) {
        CHECK(sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0);
        siglongjmp(env, 6);
    }
    CHECK(unsaved == 6);
    CHECK(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    CHECK(sigismember(&current, SIGUSR1) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
    int plain = _setjmp(env);
    if (plain == 0) {
        CHECK(sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0);
        _longjmp(env, 7);
    }
    CHECK(plain == 7);
    CHECK(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    CHECK(sigismember(&current, SIGUSR1) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

    errno = EDOM;
    CHECK(pthread_sigmask(77, &mask, NULL) == EINVAL && errno == EDOM);
    CHECK(pthread_sigmask(77, NULL, &current) == 0 && errno == EDOM);
    CHECK(pthread_create(&thread, NULL, mask_worker, NULL) == 0);
    CHECK(pthread_join(thread, &thread_result) == 0 && thread_result == NULL);
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
    CHECK(sigismember(&current, SIGUSR1) == 1 && sigismember(&current, SIGUSR2) == 0);
    CHECK(sigprocmask(SIG_SETMASK, &original, NULL) == 0);
    puts("masks: ok");
    return 1;
}

static int timer_tests(void)
{
    struct sigaction action;
    struct itimerval value, old, query, zero;
    sigset_t blocked, original, current;
    memset(&action, 0, sizeof(action));
    action.sa_handler = alarm_handler;
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    memset(&zero, 0, sizeof(zero));
    CHECK(alarm(5) == 0);
    unsigned previous_alarm = alarm(0);
    CHECK(previous_alarm > 0 && previous_alarm <= 5);
    for (int which = ITIMER_REAL; which <= ITIMER_PROF; ++which) {
        memset(&value, 0, sizeof(value));
        value.it_interval.tv_sec = 2;
        value.it_interval.tv_usec = 1234;
        value.it_value.tv_sec = 10;
        CHECK(setitimer(which, &value, &old) == 0);
        CHECK(old.it_value.tv_sec == 0 && old.it_value.tv_usec == 0);
        CHECK(getitimer(which, &query) == 0);
        CHECK(query.it_interval.tv_sec == 2 && query.it_interval.tv_usec == 1234);
        CHECK(query.it_value.tv_sec >= 0 && query.it_value.tv_sec <= 10);
        CHECK(query.it_value.tv_sec != 0 || query.it_value.tv_usec != 0);
        value.it_value.tv_sec = 20;
        CHECK(setitimer(which, &value, &old) == 0);
        CHECK(old.it_interval.tv_sec == 2 && old.it_interval.tv_usec == 1234);
        CHECK(old.it_value.tv_sec >= 0 && old.it_value.tv_sec <= 10);
        CHECK(setitimer(which, &zero, &old) == 0);
        CHECK(old.it_value.tv_sec >= 0 && old.it_value.tv_sec <= 20);
        CHECK(getitimer(which, &query) == 0);
        CHECK(query.it_value.tv_sec == 0 && query.it_value.tv_usec == 0);
        CHECK(query.it_interval.tv_sec == 0 && query.it_interval.tv_usec == 0);
    }
    value = zero;
    value.it_value.tv_usec = 1000000;
    errno = 0;
    CHECK(setitimer(ITIMER_REAL, &value, NULL) == -1 && errno == EINVAL);
    CHECK(getitimer(3, &query) == -1 && errno == EINVAL);

    CHECK(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGALRM) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, &original) == 0);
    alarms = 0;
    value = zero;
    value.it_value.tv_usec = 20000;
    CHECK(setitimer(ITIMER_REAL, &value, NULL) == 0);
    errno = 0;
    CHECK(sigsuspend(&original) == -1 && errno == EINTR);
    CHECK(alarms == 1);
    CHECK(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    CHECK(sigismember(&current, SIGALRM) == 1);
    CHECK(getitimer(ITIMER_REAL, &query) == 0);
    CHECK(query.it_value.tv_sec == 0 && query.it_value.tv_usec == 0);

    value.it_value.tv_usec = 20000;
    value.it_interval.tv_usec = 20000;
    CHECK(setitimer(ITIMER_REAL, &value, NULL) == 0);
    while (alarms < 3) {
        errno = 0;
        CHECK(sigsuspend(&original) == -1 && errno == EINTR);
    }
    CHECK(setitimer(ITIMER_REAL, &zero, &old) == 0);
    CHECK(old.it_interval.tv_sec == 0 && old.it_interval.tv_usec == 20000);
    CHECK(sigprocmask(SIG_SETMASK, &original, NULL) == 0);

    /* A periodic timer avoids the unavoidable alarm-before-pause race. */
    sig_atomic_t before = alarms;
    CHECK(setitimer(ITIMER_REAL, &value, NULL) == 0);
    errno = 0;
    CHECK(pause() == -1 && errno == EINTR);
    CHECK(alarms > before);
    CHECK(setitimer(ITIMER_REAL, &zero, NULL) == 0);
    /* Keep the benign handler until process exit: cancellation does not retract
     * an already generated SIGALRM, on Linux either. */
    puts("timers: ok");
    return 1;
}

int main(void)
{
    sigset_t empty, initial;
    if (sigemptyset(&empty) || sigprocmask(SIG_SETMASK, &empty, &initial))
        return 1;
    if (!jump_tests() || !signal_tests() || !mask_tests() || !timer_tests())
        return 1;
    if (sigprocmask(SIG_SETMASK, &initial, NULL))
        return 1;
    puts("signals: PASS");
    return 0;
}
