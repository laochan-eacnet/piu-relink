/* i386 pthread ABI over native threads. pthread_t is an opaque registry pointer.
 * CreateThread commits the entire requested stack (8 MiB by default), and guest
 * entry uses Linux x87 precision plus 16-byte stack alignment. Supplied POSIX
 * stacks switch the guest call's stack and TIB bounds, not the Windows allocation.
 * Thread exit runs C++ thread_local destructors, then up to four pthread-key
 * destructor passes. DLL_THREAD_DETACH covers foreign/native-created threads.
 * pthread_exit calls ExitThread without DWARF unwinding. Main-thread exit cleanup
 * is not required. Keys use generations so deleted/reused keys cannot see old data.
 * Signals are record-only (signal.c); pthread_kill(0) checks existence, other valid
 * signals return ENOSYS for live threads or ESRCH for missing/exited threads.
 * Scheduling supports SCHED_OTHER; explicit realtime policies fail EPERM and
 * process contention scope fails ENOTSUP. Windows retains its native guard page.
 * Futex WAIT/WAKE (including PRIVATE) use registered WaitOnAddress tokens to avoid
 * losing wakes between comparison and sleep. WAKE counts may include a waiter
 * concurrently timing out. Other unrecognized syscalls fail ENOSYS, logging once. */
#include "lxrt.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define LX_KEYS 1024
#define LX_STACK_DEFAULT (8u * 1024u * 1024u)
#define LX_STACK_MIN 16384u

extern int lx_signal_mask(int how, const void *set, void *old);
typedef struct {
    int detached, policy, priority, inherit, scope;
    unsigned guard;
    void *stack;
    unsigned stacksize, reserved;
} lx_attr;
_Static_assert(sizeof(lx_attr) == 36, "i386 pthread_attr_t");
_Static_assert(sizeof(LONG) == 4, "i386 pthread_once_t");
_Static_assert(sizeof(void *) == 4, "i386 pthread_t");
typedef struct { int32_t sec, nsec; } lx_ts;
_Static_assert(sizeof(lx_ts) == 8, "i386 timespec");
typedef struct lx_cxa_dtor {
    void (*fn)(void *);
    void *arg;
    struct lx_cxa_dtor *next;
} lx_cxa_dtor;
typedef struct lx_thread {
    struct lx_thread *next;
    HANDLE handle;
    DWORD id;
    int detached, joining, exited, foreign;
    void *(*start)(void *);
    void *arg, *result;
    lx_attr attr;
    char name[16];
    unsigned char sigmask[128];
} lx_thread;
typedef struct { unsigned generation; int used; void (*dtor)(void *); } lx_key;
typedef struct { void *value; unsigned generation; } lx_key_value;
static SRWLOCK thread_lock = SRWLOCK_INIT, key_lock = SRWLOCK_INIT;
static lx_thread *threads;
static lx_key keys[LX_KEYS];
static __declspec(thread) lx_thread *current;
static __declspec(thread) lx_key_value *values;
static __declspec(thread) lx_cxa_dtor *cxa_dtors;
static __declspec(thread) int cleanup_done;

static void attr_default(lx_attr *a)
{
    memset(a, 0, sizeof(*a));
    a->guard = 4096;
    a->stacksize = LX_STACK_DEFAULT;
}
static lx_thread **find_thread(lx_thread *t)
{
    lx_thread **p = &threads;
    while (*p && *p != t) p = &(*p)->next;
    return p;
}
static lx_thread *self_record(void)
{
    if (current) return current;
    lx_thread *t = calloc(1, sizeof(*t));
    if (!t) lx_fatal("pthread_self: out of memory");
    t->id = GetCurrentThreadId();
    t->foreign = t->detached = 1;
    attr_default(&t->attr);
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    t->attr.stack = tib->StackLimit;
    t->attr.stacksize = (unsigned)((char *)tib->StackBase - (char *)tib->StackLimit);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &t->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    AcquireSRWLockExclusive(&thread_lock);
    t->next = threads; threads = t;
    ReleaseSRWLockExclusive(&thread_lock);
    current = t;
    return t;
}
void lx_thread_init_main(void) { self_record()->detached = 0; }

static void run_destructors(void)
{
    if (cleanup_done) return;
    cleanup_done = 1;
    /* glibc runs C++ thread_local destructors before pthread key destructors. */
    while (cxa_dtors) {
        lx_cxa_dtor *d = cxa_dtors;
        cxa_dtors = d->next;
        void (*fn)(void *) = d->fn;
        void *arg = d->arg;
        free(d);
        fn(arg);
    }
    if (!values) return;
    for (unsigned round = 0; round < 4; ++round) {
        int called = 0;
        for (unsigned k = 0; k < LX_KEYS; ++k) {
            if (!values[k].value) continue;
            AcquireSRWLockShared(&key_lock);
            void (*dtor)(void *) = keys[k].used && keys[k].generation == values[k].generation ? keys[k].dtor : NULL;
            ReleaseSRWLockShared(&key_lock);
            void *v = values[k].value;
            values[k].value = NULL;
            if (dtor) { called = 1; dtor(v); }
        }
        if (!called) break;
    }
    free(values); values = NULL;
}
void lx_thread_detach(void)
{
    run_destructors();
    lx_thread *t = current;
    if (!t) return;
    AcquireSRWLockExclusive(&thread_lock);
    t->exited = 1;
    if (t->detached) {
        lx_thread **p = find_thread(t);
        if (*p) *p = t->next;
        CloseHandle(t->handle);
        free(t);
    }
    current = NULL;
    ReleaseSRWLockExclusive(&thread_lock);
}
static LX_NORETURN void lx_pthread_exit(void *result)
{
    lx_thread *t = self_record();
    t->result = result;
    lx_thread_detach();
    /* No DWARF unwinding through native frames. DLL_THREAD_DETACH frees guest TLS. */
    ExitThread(0);
}
/* A user-supplied POSIX stack cannot be passed to CreateThread. The native stack is
 * still fully committed; switch only the guest invocation, preserving the TIB bounds. */
__declspec(naked) static void *call_on_stack(void *(*fn)(void *), void *arg, void *top)
{
    __asm {
        push ebp
        mov ebp, esp
        push esi
        mov esi, esp
        mov esp, [ebp + 16]
        and esp, -16
        sub esp, 16
        mov eax, [ebp + 12]
        mov [esp], eax
        call dword ptr [ebp + 8]
        mov esp, esi
        pop esi
        pop ebp
        ret
    }
}
static DWORD WINAPI thread_entry(void *arg)
{
    lx_thread *t = arg;
    current = t;
    (void)lx_signal_mask(2, t->sigmask, NULL);
    void *result;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    static const uint16_t guest_cw = 0x037f;
    __asm__ volatile("fldcw %0" : : "m"(guest_cw));
    if (t->attr.stack) {
        void *base = tib->StackBase, *limit = tib->StackLimit;
        tib->StackLimit = t->attr.stack;
        tib->StackBase = (char *)t->attr.stack + t->attr.stacksize;
        result = call_on_stack(t->start, t->arg, tib->StackBase);
        tib->StackBase = base; tib->StackLimit = limit;
    } else {
        t->attr.stack = tib->StackLimit;
        /* The requested commit size is the advertised usable stack size. */
        result = t->start(t->arg);
    }
    lx_pthread_exit(result);
}
static int lx_pthread_create(lx_thread **out, const lx_attr *attr, void *(*start)(void *), void *arg)
{
    if (!out || !start) return LX_EINVAL;
    lx_attr a;
    if (attr) a = *attr; else attr_default(&a);
    if (a.stacksize < LX_STACK_MIN || a.stacksize > UINT_MAX - 65536u) return LX_EINVAL;
    lx_thread *parent = self_record();
    if (!a.inherit) { a.policy = parent->attr.policy; a.priority = parent->attr.priority; }
    else if (a.policy != 0) return LX_EPERM;
    lx_thread *t = calloc(1, sizeof(*t));
    if (!t) return LX_EAGAIN;
    t->attr = a; t->detached = a.detached; t->start = start; t->arg = arg;
    (void)lx_signal_mask(2, NULL, t->sigmask);
    /* dwStackSize is COMMIT, never STACK_SIZE_PARAM_IS_A_RESERVATION: Linux
     * generated code has no Windows stack probes and may jump across guard pages. */
    t->handle = CreateThread(NULL, a.stacksize, thread_entry, t, CREATE_SUSPENDED, &t->id);
    if (!t->handle) { free(t); return LX_EAGAIN; }
    AcquireSRWLockExclusive(&thread_lock);
    t->next = threads; threads = t;
    *out = t;
    ReleaseSRWLockExclusive(&thread_lock);
    ResumeThread(t->handle);
    return 0;
}
static int lx_pthread_join(lx_thread *t, void **result)
{
    if (t == self_record()) return LX_EDEADLK;
    AcquireSRWLockExclusive(&thread_lock);
    if (!*find_thread(t)) { ReleaseSRWLockExclusive(&thread_lock); return LX_ESRCH; }
    if (t->detached || t->joining) { ReleaseSRWLockExclusive(&thread_lock); return LX_EINVAL; }
    t->joining = 1;
    HANDLE h = t->handle;
    ReleaseSRWLockExclusive(&thread_lock);
    if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) return LX_ESRCH;
    AcquireSRWLockExclusive(&thread_lock);
    if (result) *result = t->result;
    *find_thread(t) = t->next;
    CloseHandle(h); free(t);
    ReleaseSRWLockExclusive(&thread_lock);
    return 0;
}
static int lx_pthread_detach(lx_thread *t)
{
    AcquireSRWLockExclusive(&thread_lock);
    if (!*find_thread(t)) { ReleaseSRWLockExclusive(&thread_lock); return LX_ESRCH; }
    if (t->detached || t->joining) { ReleaseSRWLockExclusive(&thread_lock); return LX_EINVAL; }
    t->detached = 1;
    if (t->exited) { *find_thread(t) = t->next; CloseHandle(t->handle); free(t); }
    ReleaseSRWLockExclusive(&thread_lock);
    return 0;
}
static int lx_pthread_equal(lx_thread *a, lx_thread *b) { return a == b; }
static int lx_pthread_kill(lx_thread *t, int sig)
{
    if (sig < 0 || sig > 64) return LX_EINVAL;
    AcquireSRWLockShared(&thread_lock);
    int live = *find_thread(t) && !t->exited && WaitForSingleObject(t->handle, 0) == WAIT_TIMEOUT;
    ReleaseSRWLockShared(&thread_lock);
    /* Signal delivery is not emulated; signal zero remains an existence query. */
    return !live ? LX_ESRCH : sig ? LX_ENOSYS : 0;
}
static int lx_pthread_setname(lx_thread *t, const char *name)
{
    if (strlen(name) >= 16) return LX_ERANGE;
    AcquireSRWLockExclusive(&thread_lock);
    if (!*find_thread(t)) { ReleaseSRWLockExclusive(&thread_lock); return LX_ESRCH; }
    strcpy(t->name, name);
    ReleaseSRWLockExclusive(&thread_lock);
    return 0;
}
static int lx_pthread_getname(lx_thread *t, char *name, unsigned n)
{
    AcquireSRWLockShared(&thread_lock);
    if (!*find_thread(t)) { ReleaseSRWLockShared(&thread_lock); return LX_ESRCH; }
    if (strlen(t->name) >= n) { ReleaseSRWLockShared(&thread_lock); return LX_ERANGE; }
    strcpy(name, t->name);
    ReleaseSRWLockShared(&thread_lock);
    return 0;
}
static int lx_attr_init(lx_attr *a) { attr_default(a); return 0; }
static int lx_attr_destroy(lx_attr *a) { return 0; }
static int lx_attr_setdetach(lx_attr *a, int v) { if (v != 0 && v != 1) return LX_EINVAL; a->detached = v; return 0; }
static int lx_attr_getdetach(const lx_attr *a, int *v) { *v = a->detached; return 0; }
static int lx_attr_setstacksize(lx_attr *a, unsigned v) { if (v < LX_STACK_MIN) return LX_EINVAL; a->stacksize = v; return 0; }
static int lx_attr_getstacksize(const lx_attr *a, unsigned *v) { *v = a->stacksize; return 0; }
static int lx_attr_setstack(lx_attr *a, void *p, unsigned v) { if (!p || v < LX_STACK_MIN || (uintptr_t)p + v < (uintptr_t)p) return LX_EINVAL; a->stack = p; a->stacksize = v; return 0; }
static int lx_attr_getstack(const lx_attr *a, void **p, unsigned *v) { *p = a->stack; *v = a->stacksize; return 0; }
static int priority_valid(int policy, int p) { return policy == 0 ? p == 0 : (policy == 1 || policy == 2) && p >= 1 && p <= 99; }
static int lx_attr_setpolicy(lx_attr *a, int p) { if (p < 0 || p > 2) return LX_EINVAL; a->policy = p; return 0; }
static int lx_attr_getpolicy(const lx_attr *a, int *p) { *p = a->policy; return 0; }
static int lx_attr_setparam(lx_attr *a, const int *p) { if (!priority_valid(a->policy, *p)) return LX_EINVAL; a->priority = *p; return 0; }
static int lx_attr_getparam(const lx_attr *a, int *p) { *p = a->priority; return 0; }
static int lx_attr_setinherit(lx_attr *a, int p) { if (p != 0 && p != 1) return LX_EINVAL; a->inherit = p; return 0; }
static int lx_attr_getinherit(const lx_attr *a, int *p) { *p = a->inherit; return 0; }
static int lx_attr_setscope(lx_attr *a, int p) { if (p == 1) return LX_EOPNOTSUPP; if (p != 0) return LX_EINVAL; a->scope = p; return 0; }
static int lx_attr_getscope(const lx_attr *a, int *p) { *p = a->scope; return 0; }
static int lx_attr_setguard(lx_attr *a, unsigned n) { a->guard = n; return 0; }
static int lx_attr_getguard(const lx_attr *a, unsigned *n) { *n = a->guard; return 0; }
static int lx_pthread_getattr(lx_thread *t, lx_attr *a)
{
    AcquireSRWLockShared(&thread_lock);
    if (!*find_thread(t)) { ReleaseSRWLockShared(&thread_lock); return LX_ESRCH; }
    *a = t->attr; a->detached = t->detached;
    ReleaseSRWLockShared(&thread_lock); return 0;
}
static int lx_setsched(lx_thread *t, int policy, const int *param)
{
    if (!priority_valid(policy, *param)) return LX_EINVAL;
    AcquireSRWLockExclusive(&thread_lock);
    if (!*find_thread(t)) { ReleaseSRWLockExclusive(&thread_lock); return LX_ESRCH; }
    /* Real-time Linux policies are intentionally unavailable without host privilege. */
    if (policy != 0) { ReleaseSRWLockExclusive(&thread_lock); return LX_EPERM; }
    t->attr.policy = policy; t->attr.priority = *param;
    ReleaseSRWLockExclusive(&thread_lock); return 0;
}
static int lx_getsched(lx_thread *t, int *policy, int *param)
{
    AcquireSRWLockShared(&thread_lock);
    if (!*find_thread(t)) { ReleaseSRWLockShared(&thread_lock); return LX_ESRCH; }
    *policy = t->attr.policy; *param = t->attr.priority;
    ReleaseSRWLockShared(&thread_lock); return 0;
}
static int lx_yield(void) { SwitchToThread(); return 0; }
static int lx_priority_min(int p) { if (p == 0 || p == 3 || p == 5) return 0; if (p == 1 || p == 2) return 1; return lx_set_errno(LX_EINVAL); }
static int lx_priority_max(int p) { if (p == 0 || p == 3 || p == 5) return 0; if (p == 1 || p == 2) return 99; return lx_set_errno(LX_EINVAL); }
static int lx_key_create(unsigned *key, void (*dtor)(void *))
{
    AcquireSRWLockExclusive(&key_lock);
    for (unsigned i = 0; i < LX_KEYS; ++i) if (!keys[i].used) {
        keys[i].used = 1; keys[i].dtor = dtor;
        if (++keys[i].generation == 0) ++keys[i].generation;
        *key = i; ReleaseSRWLockExclusive(&key_lock); return 0;
    }
    ReleaseSRWLockExclusive(&key_lock); return LX_EAGAIN;
}
static int lx_key_delete(unsigned k)
{
    AcquireSRWLockExclusive(&key_lock);
    if (k >= LX_KEYS || !keys[k].used) { ReleaseSRWLockExclusive(&key_lock); return LX_EINVAL; }
    keys[k].used = 0; keys[k].dtor = NULL;
    ReleaseSRWLockExclusive(&key_lock); return 0;
}
static void *lx_getspecific(unsigned k)
{
    if (k >= LX_KEYS || !values) return NULL;
    AcquireSRWLockShared(&key_lock);
    void *v = keys[k].used && keys[k].generation == values[k].generation ? values[k].value : NULL;
    ReleaseSRWLockShared(&key_lock); return v;
}
static int lx_setspecific(unsigned k, const void *v)
{
    AcquireSRWLockShared(&key_lock);
    if (k >= LX_KEYS || !keys[k].used) { ReleaseSRWLockShared(&key_lock); return LX_EINVAL; }
    unsigned generation = keys[k].generation;
    ReleaseSRWLockShared(&key_lock);
    if (!values) { values = calloc(LX_KEYS, sizeof(*values)); if (!values) return LX_ENOMEM; }
    values[k].generation = generation; values[k].value = (void *)v;
    return 0;
}
static int lx_once(volatile LONG *once, void (*init)(void))
{
    for (;;) {
        LONG v = InterlockedCompareExchange(once, 1, 0);
        if (v == 2) return 0;
        if (!v) { init(); InterlockedExchange(once, 2); WakeByAddressAll((void *)once); return 0; }
        WaitOnAddress(once, &v, sizeof(v), INFINITE);
    }
}
static int lx_cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso)
{
    lx_cxa_dtor *d = malloc(sizeof(*d));
    if (!d) return lx_set_errno(LX_ENOMEM);
    d->fn = fn; d->arg = arg; d->next = cxa_dtors; cxa_dtors = d;
    return 0;
}

extern int lx_clock_gettime(int clockid, void *ts);
extern int lx_nanosleep(const void *req, void *rem);
/* WaitOnAddress has no wake count API. Count registered waiters under one lock;
 * WAKE returns min(requested, registered), an approximation if timeout races it. */
typedef struct lx_futex_waiter { volatile LONG *addr; volatile LONG signalled; struct lx_futex_waiter *next; } lx_futex_waiter;
static SRWLOCK futex_lock = SRWLOCK_INIT;
static lx_futex_waiter *futex_waiters;
static long futex_call(volatile LONG *addr, int op, int value, const lx_ts *timeout)
{
    if (!addr || ((uintptr_t)addr & 3)) return lx_set_errno(LX_EINVAL);
    op &= ~128;
    if (op == 1) {
        if (value < 0) return lx_set_errno(LX_EINVAL);
        if (!value) return 0;
        AcquireSRWLockExclusive(&futex_lock);
        int count = 0;
        for (lx_futex_waiter *w = futex_waiters; w; w = w->next) {
            if (w->addr == addr && !w->signalled && count < value) {
                InterlockedExchange(&w->signalled, 1);
                WakeByAddressSingle((void *)&w->signalled);
                ++count;
            }
        }
        ReleaseSRWLockExclusive(&futex_lock);
        return count;
    }
    if (op != 0) return lx_set_errno(LX_ENOSYS);
    DWORD ms = INFINITE;
    if (timeout) {
        if (timeout->sec < 0 || timeout->nsec < 0 || timeout->nsec >= 1000000000) return lx_set_errno(LX_EINVAL);
        uint64_t n = (uint64_t)timeout->sec * 1000 + ((unsigned)timeout->nsec + 999999) / 1000000;
        ms = n >= INFINITE ? INFINITE - 1 : (DWORD)n;
    }
    lx_futex_waiter waiter = { addr, 0, NULL };
    AcquireSRWLockExclusive(&futex_lock);
    if (InterlockedCompareExchange(addr, 0, 0) != value) { ReleaseSRWLockExclusive(&futex_lock); return lx_set_errno(LX_EAGAIN); }
    waiter.next = futex_waiters; futex_waiters = &waiter;
    ReleaseSRWLockExclusive(&futex_lock);
    LONG pending = 0;
    BOOL ok = WaitOnAddress(&waiter.signalled, &pending, 4, ms);
    DWORD error = GetLastError();
    AcquireSRWLockExclusive(&futex_lock);
    lx_futex_waiter **p = &futex_waiters;
    while (*p != &waiter) p = &(*p)->next;
    *p = waiter.next;
    ReleaseSRWLockExclusive(&futex_lock);
    if (!ok) return lx_set_errno(error == ERROR_TIMEOUT ? LX_ETIMEDOUT : LX_EINVAL);
    return 0;
}
static long lx_syscall(long number, ...)
{
    va_list ap; va_start(ap, number);
    long result;
    if (number == 240) {
        volatile LONG *addr = va_arg(ap, volatile LONG *);
        int op = va_arg(ap, int), value = va_arg(ap, int);
        const lx_ts *timeout = NULL;
        if ((op & ~128) == 0) timeout = va_arg(ap, const lx_ts *);
        result = futex_call(addr, op, value, timeout);
    } else if (number == 224) result = GetCurrentThreadId();
    else if (number == 20) result = GetCurrentProcessId();
    else if (number == 265) {
        int id = va_arg(ap, int); void *ts = va_arg(ap, void *);
        result = lx_clock_gettime(id, ts);
    } else if (number == 162) {
        const void *req = va_arg(ap, const void *); void *rem = va_arg(ap, void *);
        result = lx_nanosleep(req, rem);
    } else {
        static volatile LONG warned;
        if (!InterlockedExchange(&warned, 1)) lx_log("syscall: unsupported i386 number %ld (further unsupported calls suppressed)\n", number);
        result = lx_set_errno(LX_ENOSYS);
    }
    va_end(ap); return result;
}

LX_SYMTAB(thread,
    LX_SYM("pthread_create", lx_pthread_create), LX_SYM("pthread_join", lx_pthread_join),
    LX_SYM("pthread_detach", lx_pthread_detach), LX_SYM("pthread_exit", lx_pthread_exit),
    LX_SYM("pthread_self", self_record), LX_SYM("pthread_equal", lx_pthread_equal),
    LX_SYM("pthread_kill", lx_pthread_kill), LX_SYM("pthread_setname_np", lx_pthread_setname), LX_SYM("pthread_getname_np", lx_pthread_getname),
    LX_SYM("pthread_attr_init", lx_attr_init), LX_SYM("pthread_attr_destroy", lx_attr_destroy),
    LX_SYM("pthread_attr_setdetachstate", lx_attr_setdetach), LX_SYM("pthread_attr_getdetachstate", lx_attr_getdetach),
    LX_SYM("pthread_attr_setstacksize", lx_attr_setstacksize), LX_SYM("pthread_attr_getstacksize", lx_attr_getstacksize),
    LX_SYM("pthread_attr_setstack", lx_attr_setstack), LX_SYM("pthread_attr_getstack", lx_attr_getstack),
    LX_SYM("pthread_attr_setschedpolicy", lx_attr_setpolicy), LX_SYM("pthread_attr_getschedpolicy", lx_attr_getpolicy),
    LX_SYM("pthread_attr_setschedparam", lx_attr_setparam), LX_SYM("pthread_attr_getschedparam", lx_attr_getparam),
    LX_SYM("pthread_attr_setinheritsched", lx_attr_setinherit), LX_SYM("pthread_attr_getinheritsched", lx_attr_getinherit),
    LX_SYM("pthread_attr_setscope", lx_attr_setscope), LX_SYM("pthread_attr_getscope", lx_attr_getscope),
    LX_SYM("pthread_attr_setguardsize", lx_attr_setguard), LX_SYM("pthread_attr_getguardsize", lx_attr_getguard),
    LX_SYM("pthread_getattr_np", lx_pthread_getattr), LX_SYM("pthread_setschedparam", lx_setsched), LX_SYM("pthread_getschedparam", lx_getsched),
    LX_SYM("sched_yield", lx_yield), LX_SYM("pthread_yield", lx_yield),
    LX_SYM("sched_get_priority_min", lx_priority_min), LX_SYM("sched_get_priority_max", lx_priority_max),
    LX_SYM("pthread_key_create", lx_key_create), LX_SYM("__pthread_key_create", lx_key_create),
    LX_SYM("pthread_key_delete", lx_key_delete), LX_SYM("pthread_getspecific", lx_getspecific), LX_SYM("pthread_setspecific", lx_setspecific),
    LX_SYM("pthread_once", lx_once), LX_SYM("__pthread_once", lx_once),
    LX_SYM("__cxa_thread_atexit_impl", lx_cxa_thread_atexit), LX_SYM("syscall", lx_syscall)
);
