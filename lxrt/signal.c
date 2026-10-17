/* Linux i386 signals. Explicit raises are synchronous. Unblocked timer handlers
 * run on a native timer thread; blocked deliveries run when the owner unmasks.
 * Neither path injects a guest signal frame: SA_SIGINFO gets a zero-initialized
 * 128-byte Linux siginfo and NULL ucontext. Native faults, alternate signal stacks,
 * and SA_RESTART of unrelated Win32 waits are deliberately not emulated. */
#include "lxrt.h"
#include <string.h>

#define LX_NSIG 65
#define LX_SIGKILL 9
#define LX_SIGALRM 14
#define LX_SIGSTOP 19
#define LX_SA_SIGINFO 4u
#define LX_SA_RESTART 0x10000000u
#define LX_SA_NODEFER 0x40000000u
#define LX_SA_RESETHAND 0x80000000u

typedef struct { uint32_t words[32]; } lx_sigset;
typedef struct {
    uintptr_t handler;
    lx_sigset mask;
    uint32_t flags;
    uintptr_t restorer;
} lx_sigaction;
typedef struct {
    int32_t signo, error, code;
    uint32_t pid, uid;
    unsigned char reserved[108];
} lx_siginfo;
typedef struct { int32_t sec, usec; } lx_signal_timeval;
typedef struct { lx_signal_timeval interval, value; } lx_itimerval;

static_assert(sizeof(lx_sigset) == 128, "i386 sigset_t size");
static_assert(sizeof(lx_sigaction) == 140, "i386 sigaction size");
static_assert(offsetof(lx_sigaction, handler) == 0, "sigaction handler offset");
static_assert(offsetof(lx_sigaction, mask) == 4, "sigaction mask offset");
static_assert(offsetof(lx_sigaction, flags) == 132, "sigaction flags offset");
static_assert(offsetof(lx_sigaction, restorer) == 136, "sigaction restorer offset");
static_assert(sizeof(lx_siginfo) == 128, "i386 siginfo_t size");
static_assert(offsetof(lx_siginfo, error) == 4, "siginfo errno offset");
static_assert(offsetof(lx_siginfo, code) == 8, "siginfo code offset");
static_assert(offsetof(lx_siginfo, pid) == 12, "siginfo pid offset");
static_assert(offsetof(lx_siginfo, uid) == 16, "siginfo uid offset");
static_assert(sizeof(lx_signal_timeval) == 8, "i386 timeval size");
static_assert(offsetof(lx_signal_timeval, usec) == 4, "timeval usec offset");
static_assert(sizeof(lx_itimerval) == 16, "i386 itimerval size");
static_assert(offsetof(lx_itimerval, value) == 8, "itimerval value offset");

typedef struct lx_signal_frame {
    struct lx_signal_frame *previous;
} lx_signal_frame;

typedef struct lx_signal_state {
    LONG refs;
    lx_sigset mask, pending;
    int pending_code[LX_NSIG];
    CONDITION_VARIABLE changed;
    uint64_t delivered;
    DWORD dispatch_thread;
    unsigned dispatch_depth;
    lx_signal_frame *frame;
} lx_signal_state;

static SRWLOCK signal_lock = SRWLOCK_INIT;
static lx_sigaction actions[LX_NSIG];
static INIT_ONCE state_once = INIT_ONCE_STATIC_INIT;
static DWORD state_fls = FLS_OUT_OF_INDEXES;
/* Timer callbacks use the arming thread's logical mask/pending state. This does not
 * change the callback's actual OS thread or its guest/native TLS identity. */
static __declspec(thread) lx_signal_state *callback_state;

static void state_retain(lx_signal_state *state)
{
    InterlockedIncrement(&state->refs);
}

static void state_release(lx_signal_state *state)
{
    if (state && InterlockedDecrement(&state->refs) == 0)
        HeapFree(GetProcessHeap(), 0, state);
}

static VOID CALLBACK state_destroy(void *value)
{
    state_release((lx_signal_state *)value);
}

static BOOL CALLBACK state_init(PINIT_ONCE once, void *parameter, void **context)
{
    state_fls = FlsAlloc(state_destroy);
    return state_fls != FLS_OUT_OF_INDEXES;
}

static lx_signal_state *current_state(void)
{
    lx_signal_state *state;
    if (callback_state)
        return callback_state;
    if (!InitOnceExecuteOnce(&state_once, state_init, NULL, NULL)) {
        lx_fail_win32();
        return NULL;
    }
    state = FlsGetValue(state_fls);
    if (state)
        return state;
    state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
    if (!state) {
        lx_set_errno(LX_ENOMEM);
        return NULL;
    }
    state->refs = 1;
    InitializeConditionVariable(&state->changed);
    if (!FlsSetValue(state_fls, state)) {
        DWORD error = GetLastError();
        state_release(state);
        lx_set_errno(lx_errno_from_win32(error));
        return NULL;
    }
    return state;
}

static int valid_signal(int sig)
{
    return sig > 0 && sig < LX_NSIG;
}

static uint32_t signal_bit(int sig)
{
    return UINT32_C(1) << ((sig - 1) & 31);
}

static int contains(const lx_sigset *set, int sig)
{
    return (set->words[(sig - 1) >> 5] & signal_bit(sig)) != 0;
}

static void add_signal(lx_sigset *set, int sig)
{
    set->words[(sig - 1) >> 5] |= signal_bit(sig);
}

static void delete_signal(lx_sigset *set, int sig)
{
    set->words[(sig - 1) >> 5] &= ~signal_bit(sig);
}

static void remove_unblockable(lx_sigset *set)
{
    delete_signal(set, LX_SIGKILL);
    delete_signal(set, LX_SIGSTOP);
}

static int default_ignored(int sig)
{
    return sig == 17 || sig == 18 || sig == 23 || sig == 28;
}

/* No lock is held while guest code runs: handlers may raise another signal, change
 * dispositions, or siglongjmp out. Same-state callbacks on other OS threads queue
 * behind the active callback rather than concurrently overwriting its saved mask. */
static void deliver_pending(lx_signal_state *state)
{
    for (;;) {
        lx_sigaction action;
        lx_sigset saved_mask;
        lx_siginfo info;
        lx_signal_frame frame;
        DWORD thread = GetCurrentThreadId();
        int sig, code;
        AcquireSRWLockExclusive(&signal_lock);
        if (state->dispatch_depth && state->dispatch_thread != thread) {
            ReleaseSRWLockExclusive(&signal_lock);
            return;
        }
        for (sig = 1; sig < LX_NSIG; ++sig)
            if (contains(&state->pending, sig) && !contains(&state->mask, sig))
                break;
        if (sig == LX_NSIG) {
            ReleaseSRWLockExclusive(&signal_lock);
            return;
        }
        delete_signal(&state->pending, sig);
        code = state->pending_code[sig];
        action = actions[sig];
        if (action.handler == 1 || (!action.handler && default_ignored(sig))) {
            ReleaseSRWLockExclusive(&signal_lock);
            continue;
        }
        if (!action.handler) {
            ReleaseSRWLockExclusive(&signal_lock);
            lx_exit(128 + sig);
        }
        saved_mask = state->mask;
        for (unsigned i = 0; i < LX_COUNT(state->mask.words); ++i)
            state->mask.words[i] |= action.mask.words[i];
        if (!(action.flags & LX_SA_NODEFER))
            add_signal(&state->mask, sig);
        remove_unblockable(&state->mask);
        if (action.flags & LX_SA_RESETHAND)
            actions[sig].handler = 0;
        state->dispatch_thread = thread;
        ++state->dispatch_depth;
        frame.previous = state->frame;
        state->frame = &frame;
        ReleaseSRWLockExclusive(&signal_lock);

        memset(&info, 0, sizeof(info));
        info.signo = sig;
        info.code = code;
        if (code == -6) { /* Linux SI_TKILL, as used by raise(). */
            info.pid = GetCurrentProcessId();
            info.uid = 1000; /* Match the guest getuid() identity. */
        }
        if (action.flags & LX_SA_SIGINFO)
            ((void (*)(int, lx_siginfo *, void *))action.handler)(sig, &info, NULL);
        else
            ((void (*)(int))action.handler)(sig);

        AcquireSRWLockExclusive(&signal_lock);
        state->mask = saved_mask;
        state->frame = frame.previous;
        if (--state->dispatch_depth == 0)
            state->dispatch_thread = 0;
        ++state->delivered;
        WakeAllConditionVariable(&state->changed);
        ReleaseSRWLockExclusive(&signal_lock);
    }
}

static void queue_signal(lx_signal_state *state, int sig, int code)
{
    AcquireSRWLockExclusive(&signal_lock);
    if (actions[sig].handler == 1 ||
        (!actions[sig].handler && default_ignored(sig))) {
        ReleaseSRWLockExclusive(&signal_lock);
        return;
    }
    if (!contains(&state->pending, sig))
        state->pending_code[sig] = code;
    add_signal(&state->pending, sig);
    ReleaseSRWLockExclusive(&signal_lock);
    deliver_pending(state);
}

int lx_raise(int sig)
{
    lx_signal_state *state;
    if (sig == 0)
        return 0; /* Linux raise(0) probes this thread without delivering. */
    if (!valid_signal(sig))
        return lx_set_errno(LX_EINVAL);
    state = current_state();
    if (!state)
        return -1;
    queue_signal(state, sig, -6);
    return 0;
}

static int sigaction_(int sig, const lx_sigaction *action, lx_sigaction *old)
{
    lx_sigaction replacement;
    if (!valid_signal(sig) || (action && (sig == LX_SIGKILL || sig == LX_SIGSTOP)))
        return lx_set_errno(LX_EINVAL);
    /* Read first, so action == old is supported. */
    if (action) {
        replacement = *action;
        if (replacement.handler == (uintptr_t)-1)
            return lx_set_errno(LX_EINVAL);
    }
    AcquireSRWLockExclusive(&signal_lock);
    if (old)
        *old = actions[sig];
    if (action)
        actions[sig] = replacement;
    ReleaseSRWLockExclusive(&signal_lock);
    return 0;
}

typedef void (*lx_sighandler)(int);

static lx_sighandler install_handler(int sig, lx_sighandler handler, uint32_t flags)
{
    lx_sigaction action = {0}, old;
    action.handler = (uintptr_t)handler;
    action.flags = flags;
    if (sigaction_(sig, &action, &old) < 0)
        return (lx_sighandler)(uintptr_t)-1;
    return (lx_sighandler)old.handler;
}

static lx_sighandler signal_(int sig, lx_sighandler handler)
{
    return install_handler(sig, handler, LX_SA_RESTART);
}

static lx_sighandler sysv_signal_(int sig, lx_sighandler handler)
{
    return install_handler(sig, handler, LX_SA_RESETHAND | LX_SA_NODEFER);
}

static int sigemptyset_(lx_sigset *set)
{
    if (!set)
        return lx_set_errno(LX_EFAULT);
    memset(set, 0, sizeof(*set));
    return 0;
}

static int sigfillset_(lx_sigset *set)
{
    if (!set)
        return lx_set_errno(LX_EFAULT);
    memset(set, 0xff, sizeof(*set));
    return 0;
}

static int sigaddset_(lx_sigset *set, int sig)
{
    if (!set)
        return lx_set_errno(LX_EFAULT);
    if (!valid_signal(sig))
        return lx_set_errno(LX_EINVAL);
    add_signal(set, sig);
    return 0;
}

static int sigdelset_(lx_sigset *set, int sig)
{
    if (!set)
        return lx_set_errno(LX_EFAULT);
    if (!valid_signal(sig))
        return lx_set_errno(LX_EINVAL);
    delete_signal(set, sig);
    return 0;
}

static int sigismember_(const lx_sigset *set, int sig)
{
    if (!set)
        return lx_set_errno(LX_EFAULT);
    if (!valid_signal(sig))
        return lx_set_errno(LX_EINVAL);
    return contains(set, sig);
}

/* Called before a nonlocal jump changes ESP. Discard only callback bookkeeping
 * for frames actually bypassed, including nested handlers; a jump within the
 * current handler must leave its eventual normal-return bookkeeping intact. */
void lx_signal_longjmp(uintptr_t target_stack)
{
    lx_signal_state *state = callback_state;
    if (!state && state_fls != FLS_OUT_OF_INDEXES)
        state = FlsGetValue(state_fls);
    if (!state)
        return;
    AcquireSRWLockExclusive(&signal_lock);
    if (state->dispatch_thread == GetCurrentThreadId()) {
        while (state->frame && (uintptr_t)state->frame < target_stack) {
            state->frame = state->frame->previous;
            --state->dispatch_depth;
            ++state->delivered;
        }
        if (!state->dispatch_depth)
            state->dispatch_thread = 0;
        WakeAllConditionVariable(&state->changed);
    }
    ReleaseSRWLockExclusive(&signal_lock);
}

int lx_signal_mask(int how, const void *set_ptr, void *old_ptr)
{
    lx_signal_state *state;
    lx_sigset set;
    if (set_ptr) {
        if (how < 0 || how > 2)
            return lx_set_errno(LX_EINVAL);
        set = *(const lx_sigset *)set_ptr;
    }
    state = current_state();
    if (!state)
        return -1;
    AcquireSRWLockExclusive(&signal_lock);
    if (old_ptr)
        *(lx_sigset *)old_ptr = state->mask;
    if (set_ptr) {
        for (unsigned i = 0; i < LX_COUNT(set.words); ++i) {
            if (how == 0)
                state->mask.words[i] |= set.words[i];
            else if (how == 1)
                state->mask.words[i] &= ~set.words[i];
            else
                state->mask.words[i] = set.words[i];
        }
        remove_unblockable(&state->mask);
    }
    ReleaseSRWLockExclusive(&signal_lock);
    if (set_ptr)
        deliver_pending(state);
    return 0;
}

static int pthread_sigmask_(int how, const void *set, void *old)
{
    int saved_errno = lx_errno;
    int result = lx_signal_mask(how, set, old);
    int error = result < 0 ? lx_errno : 0;
    lx_errno = saved_errno;
    return error;
}

static int wait_for_signal(const lx_sigset *temporary_mask)
{
    lx_signal_state *state = current_state();
    lx_sigset saved;
    uint64_t delivered;
    if (!state)
        return -1;
    AcquireSRWLockExclusive(&signal_lock);
    saved = state->mask;
    delivered = state->delivered;
    if (temporary_mask) {
        state->mask = *temporary_mask;
        remove_unblockable(&state->mask);
    }
    ReleaseSRWLockExclusive(&signal_lock);
    deliver_pending(state);
    AcquireSRWLockExclusive(&signal_lock);
    while (state->delivered == delivered) {
        if (!SleepConditionVariableSRW(&state->changed, &signal_lock, INFINITE, 0)) {
            DWORD error = GetLastError();
            if (temporary_mask)
                state->mask = saved;
            ReleaseSRWLockExclusive(&signal_lock);
            return lx_set_errno(lx_errno_from_win32(error));
        }
    }
    if (temporary_mask)
        state->mask = saved;
    ReleaseSRWLockExclusive(&signal_lock);
    if (temporary_mask)
        deliver_pending(state);
    return lx_set_errno(LX_EINTR);
}

static int sigsuspend_(const lx_sigset *mask)
{
    lx_sigset copy;
    if (!mask)
        return lx_set_errno(LX_EFAULT);
    copy = *mask;
    return wait_for_signal(&copy);
}

static int pause_(void)
{
    return wait_for_signal(NULL);
}

static int sigrtmin_(void) { return 34; }
static int sigrtmax_(void) { return 64; }

typedef struct {
    uint64_t due, interval;
    lx_signal_state *owner;
} lx_interval_timer;

static lx_interval_timer timers[3];
static INIT_ONCE timer_once = INIT_ONCE_STATIC_INIT;
static HANDLE timer_changed;

/* ITIMER_REAL is elapsed time; VIRTUAL/PROF use process user/user+kernel CPU time.
 * CPU timers are sampled at 10ms while armed. All three dispatch on the timer thread. */
static uint64_t timer_now(int which)
{
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER k, u;
    if (which == 0)
        return GetTickCount64() * UINT64_C(1000);
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        lx_fatal("GetProcessTimes failed for interval timer: %lu", GetLastError());
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (u.QuadPart + (which == 2 ? k.QuadPart : 0)) / 10;
}

static DWORD WINAPI timer_thread(void *parameter)
{
    static const int timer_signals[3] = {14, 26, 27};
    for (;;) {
        DWORD wait = INFINITE;
        int fired = -1;
        lx_signal_state *owner = NULL;
        AcquireSRWLockExclusive(&signal_lock);
        for (int i = 0; i < 3; ++i) {
            uint64_t now, remaining;
            if (!timers[i].due)
                continue;
            now = timer_now(i);
            if (now >= timers[i].due) {
                fired = i;
                owner = timers[i].owner;
                state_retain(owner);
                if (timers[i].interval) {
                    uint64_t elapsed = now - timers[i].due;
                    timers[i].due += (elapsed / timers[i].interval + 1) * timers[i].interval;
                } else {
                    timers[i].due = 0;
                    timers[i].owner = NULL;
                    state_release(owner);
                }
                break;
            }
            remaining = (timers[i].due - now + 999) / 1000;
            if (i != 0 && remaining > 10)
                remaining = 10;
            if (remaining >= INFINITE)
                remaining = INFINITE - 1;
            if ((DWORD)remaining < wait)
                wait = (DWORD)remaining;
        }
        ReleaseSRWLockExclusive(&signal_lock);
        if (fired >= 0) {
            lx_signal_state *previous = callback_state;
            callback_state = owner;
            queue_signal(owner, timer_signals[fired], 128); /* SI_KERNEL */
            callback_state = previous;
            state_release(owner);
        } else {
            WaitForSingleObject(timer_changed, wait);
        }
    }
}

static BOOL CALLBACK timer_init(PINIT_ONCE once, void *parameter, void **context)
{
    HANDLE thread;
    timer_changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!timer_changed)
        return FALSE;
    thread = CreateThread(NULL, 0, timer_thread, NULL, 0, NULL);
    if (!thread) {
        DWORD error = GetLastError();
        CloseHandle(timer_changed);
        timer_changed = NULL;
        SetLastError(error);
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}

static void timer_snapshot(int which, lx_itimerval *value)
{
    uint64_t remaining = 0;
    if (timers[which].due) {
        uint64_t now = timer_now(which);
        if (timers[which].due > now)
            remaining = timers[which].due - now;
    }
    value->interval.sec = (int32_t)(timers[which].interval / 1000000);
    value->interval.usec = (int32_t)(timers[which].interval % 1000000);
    value->value.sec = (int32_t)(remaining / 1000000);
    value->value.usec = (int32_t)(remaining % 1000000);
}

static int getitimer_(int which, lx_itimerval *value)
{
    if (which < 0 || which > 2)
        return lx_set_errno(LX_EINVAL);
    if (!value)
        return lx_set_errno(LX_EFAULT);
    AcquireSRWLockExclusive(&signal_lock);
    timer_snapshot(which, value);
    ReleaseSRWLockExclusive(&signal_lock);
    return 0;
}

static int valid_timeval(const lx_signal_timeval *value)
{
    return value->sec >= 0 && value->usec >= 0 && value->usec < 1000000;
}

static int setitimer_(int which, const lx_itimerval *value, lx_itimerval *old)
{
    lx_itimerval copy = {0};
    lx_signal_state *owner = NULL, *previous;
    uint64_t duration, interval;
    if (which < 0 || which > 2)
        return lx_set_errno(LX_EINVAL);
    /* Linux treats a NULL new value as a disarm request. */
    if (value)
        copy = *value;
    if (!valid_timeval(&copy.interval) || !valid_timeval(&copy.value))
        return lx_set_errno(LX_EINVAL);
    duration = (uint64_t)copy.value.sec * 1000000 + copy.value.usec;
    interval = (uint64_t)copy.interval.sec * 1000000 + copy.interval.usec;
    if (duration) {
        owner = current_state();
        if (!owner)
            return -1;
        if (!InitOnceExecuteOnce(&timer_once, timer_init, NULL, NULL))
            return lx_fail_win32();
        state_retain(owner);
    }
    AcquireSRWLockExclusive(&signal_lock);
    if (old)
        timer_snapshot(which, old);
    previous = timers[which].owner;
    timers[which].due = duration ? timer_now(which) + duration : 0;
    timers[which].interval = interval;
    timers[which].owner = owner;
    if (timer_changed)
        SetEvent(timer_changed);
    ReleaseSRWLockExclusive(&signal_lock);
    state_release(previous);
    return 0;
}

static unsigned alarm_(unsigned seconds)
{
    /* alarm accepts unsigned seconds, unlike setitimer's signed time_t. */
    lx_signal_state *owner = NULL, *previous;
    uint64_t duration = (uint64_t)seconds * 1000000;
    if (seconds) {
        owner = current_state();
        if (!owner)
            return 0;
        if (!InitOnceExecuteOnce(&timer_once, timer_init, NULL, NULL)) {
            lx_fail_win32();
            return 0;
        }
        state_retain(owner);
    }
    AcquireSRWLockExclusive(&signal_lock);
    /* Snapshot directly: a previous unsigned alarm can exceed signed time_t. */
    uint64_t now = timer_now(0);
    uint64_t remaining = timers[0].due > now ? timers[0].due - now : 0;
    previous = timers[0].owner;
    timers[0].due = duration ? now + duration : 0;
    timers[0].interval = 0;
    timers[0].owner = owner;
    if (timer_changed)
        SetEvent(timer_changed);
    ReleaseSRWLockExclusive(&signal_lock);
    state_release(previous);
    return (unsigned)((remaining + 999999) / 1000000);
}

LX_SYMTAB(signal,
    LX_SYM("sigaction", sigaction_),
    LX_SYM("signal", signal_),
    LX_SYM("bsd_signal", signal_),
    LX_SYM("sysv_signal", sysv_signal_),
    LX_SYM("__sysv_signal", sysv_signal_),
    LX_SYM("raise", lx_raise),
    LX_SYM("sigemptyset", sigemptyset_),
    LX_SYM("sigfillset", sigfillset_),
    LX_SYM("sigaddset", sigaddset_),
    LX_SYM("sigdelset", sigdelset_),
    LX_SYM("sigismember", sigismember_),
    LX_SYM("sigprocmask", lx_signal_mask),
    LX_SYM("pthread_sigmask", pthread_sigmask_),
    LX_SYM("sigsuspend", sigsuspend_),
    LX_SYM("pause", pause_),
    LX_SYM("__libc_current_sigrtmin", sigrtmin_),
    LX_SYM("__libc_current_sigrtmax", sigrtmax_),
    LX_SYM("alarm", alarm_),
    LX_SYM("setitimer", setitimer_),
    LX_SYM("getitimer", getitimer_),
);
