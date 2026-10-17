/* In-object glibc i386 synchronization with zero/static initializers. Blocking
 * objects are process-private: Windows address waits/SRW locks cannot replace
 * interprocess futexes. Atomic spin locks support both sharing modes.
 * Conditions default to GLIBC_2.3.2 (48B); explicit GLIBC_2.0 accesses only 12B.
 * Attributes use 4B, including the requested compact rwlockattr representation.
 * Absolute timeouts use guest int32 timespecs and lx_clock_gettime; conditions
 * honor realtime/monotonic attributes. Realtime waits recheck clock jumps every
 * second; host timeout resolution is milliseconds.
 * Robust mutexes, priority protocols, named semaphores, cancellation, and signal
 * interruption are unsupported. Mutex consistency returns EINVAL; shared object
 * initialization returns ENOTSUP (sem_init: ENOSYS); named semaphores use ENOSYS
 * and sem_open returns glibc's NULL SEM_FAILED. Pthread errors are returned
 * directly, while semaphore errors set Linux errno and return -1. */
#include "lxrt.h"
#include <limits.h>
#include <string.h>

extern int lx_clock_gettime(int clockid, void *ts);

typedef struct { int32_t sec, nsec; } lx_sync_timespec;
typedef struct { int32_t value; } lx_sync_attr;
typedef lx_sync_attr lx_mutexattr;
typedef lx_sync_attr lx_condattr;
/* glibc reserves 8 opaque rwlockattr bytes. The requested compact ABI uses only
 * the first 4; all entry points leave any trailing guest storage untouched. */
typedef lx_sync_attr lx_rwlockattr;
typedef lx_sync_attr lx_barrierattr;

typedef struct {
    SRWLOCK lock;
    uint32_t count;
    volatile LONG owner;
    int32_t kind;
    uint32_t users, reserved;
} lx_mutex;

typedef struct {
    SRWLOCK gate;
    CONDITION_VARIABLE changed;
    int32_t flags; /* low bit: shared, bit 1: monotonic, higher bits: waiters */
} lx_cond_old;
typedef struct { lx_cond_old core; uint32_t reserved[9]; } lx_cond;

typedef struct {
    SRWLOCK gate;
    CONDITION_VARIABLE changed;
    uint32_t readers, waiting_readers, waiting_writers;
    int32_t shared;
    int32_t kind; /* glibc's static nonrecursive-writer initializer uses offset 24 */
    DWORD writer;
} lx_rwlock;

typedef struct {
    SRWLOCK gate;
    volatile LONG generation;
    uint32_t count, arrived, active;
} lx_barrier;

typedef struct {
    volatile LONG value, waiters;
    uint32_t flags, reserved;
} lx_sem;

static_assert(sizeof(SRWLOCK) == 4, "i386 SRW lock size");
static_assert(sizeof(CONDITION_VARIABLE) == 4, "i386 condition variable size");
static_assert(sizeof(lx_sync_timespec) == 8, "i386 timespec size");
static_assert(offsetof(lx_sync_timespec, nsec) == 4, "i386 timespec nanoseconds");
static_assert(sizeof(lx_mutexattr) == 4, "i386 mutex attribute size");
static_assert(sizeof(lx_condattr) == 4, "i386 condition attribute size");
static_assert(sizeof(lx_rwlockattr) == 4, "requested compact rwlock attribute size");
static_assert(sizeof(lx_barrierattr) == 4, "i386 barrier attribute size");
static_assert(sizeof(lx_mutex) == 24, "i386 mutex size");
static_assert(offsetof(lx_mutex, count) == 4, "i386 mutex recursion count");
static_assert(offsetof(lx_mutex, owner) == 8, "i386 mutex owner");
static_assert(offsetof(lx_mutex, kind) == 12, "i386 mutex kind");
static_assert(sizeof(lx_cond_old) == 12, "GLIBC_2.0 condition size");
static_assert(offsetof(lx_cond_old, flags) == 8, "compact condition flags");
static_assert(sizeof(lx_cond) == 48, "GLIBC_2.3.2 condition size");
static_assert(offsetof(lx_cond, core) == 0, "condition compatibility prefix");
static_assert(sizeof(lx_rwlock) == 32, "i386 rwlock size");
static_assert(offsetof(lx_rwlock, kind) == 24, "i386 rwlock preference");
static_assert(offsetof(lx_rwlock, writer) == 28, "i386 rwlock writer");
static_assert(sizeof(lx_barrier) == 20, "i386 barrier size");
static_assert(offsetof(lx_barrier, count) == 8, "i386 barrier threshold");
static_assert(sizeof(lx_sem) == 16, "i386 semaphore size");
static_assert(offsetof(lx_sem, value) == 0, "i386 semaphore value");
static_assert(sizeof(LONG) == 4, "i386 spin lock size");

/* Recompute absolute deadlines after every wake. Realtime waits are bounded to
 * one second per host wait so changes to the wall clock are observed as well. */
static int deadline_timeout(const lx_sync_timespec *deadline, int clockid, DWORD *timeout)
{
    lx_sync_timespec now;
    int saved_errno, error;
    int64_t ns, ms;
    if (!deadline || deadline->nsec < 0 || deadline->nsec >= 1000000000)
        return LX_EINVAL;
    saved_errno = lx_errno;
    if (lx_clock_gettime(clockid, &now)) {
        error = lx_errno;
        lx_errno = saved_errno;
        return error;
    }
    ns = ((int64_t)deadline->sec - now.sec) * 1000000000 + deadline->nsec - now.nsec;
    if (ns <= 0)
        return LX_ETIMEDOUT;
    ms = (ns + 999999) / 1000000;
    if (clockid == 0 && ms > 1000)
        ms = 1000;
    if (ms >= INFINITE)
        ms = INFINITE - 1;
    *timeout = (DWORD)ms;
    return 0;
}

static int cv_wait(CONDITION_VARIABLE *cv, SRWLOCK *gate,
                   const lx_sync_timespec *deadline, int clockid)
{
    for (;;) {
        DWORD timeout = INFINITE;
        int error;
        if (deadline && (error = deadline_timeout(deadline, clockid, &timeout)))
            return error;
        if (SleepConditionVariableSRW(cv, gate, timeout, 0))
            return 0; /* POSIX permits spurious wakeups. */
        if (GetLastError() != ERROR_TIMEOUT)
            return lx_errno_from_win32(GetLastError());
        /* A finite host wait may only be a slice of the absolute deadline. */
    }
}

static int attr_init(lx_sync_attr *attr)
{
    attr->value = 0;
    return 0;
}

static int attr_destroy(lx_sync_attr *attr)
{
    (void)attr;
    return 0;
}

static int attr_setshared(lx_sync_attr *attr, int shared, int mask)
{
    if (shared != 0 && shared != 1)
        return LX_EINVAL;
    attr->value = (attr->value & ~mask) | (shared ? mask : 0);
    return 0;
}

static int attr_getshared(const lx_sync_attr *attr, int *shared, int mask)
{
    *shared = (attr->value & mask) != 0;
    return 0;
}

/* Mutex attributes: type bits 0..1, protocol bits 2..3, shared bit 4. */
static int lx_pthread_mutexattr_settype(lx_mutexattr *attr, int kind)
{
    if (kind < 0 || kind > 3)
        return LX_EINVAL;
    attr->value = (attr->value & ~3) | kind;
    return 0;
}

static int lx_pthread_mutexattr_gettype(const lx_mutexattr *attr, int *kind)
{
    *kind = attr->value & 3;
    return 0;
}

static int lx_pthread_mutexattr_setpshared(lx_mutexattr *attr, int shared)
{
    return attr_setshared(attr, shared, 16);
}

static int lx_pthread_mutexattr_getpshared(const lx_mutexattr *attr, int *shared)
{
    return attr_getshared(attr, shared, 16);
}

static int lx_pthread_mutexattr_setprotocol(lx_mutexattr *attr, int protocol)
{
    if (protocol < 0 || protocol > 2)
        return LX_EINVAL;
    attr->value = (attr->value & ~12) | (protocol << 2);
    return 0;
}

static int lx_pthread_mutexattr_getprotocol(const lx_mutexattr *attr, int *protocol)
{
    *protocol = (attr->value >> 2) & 3;
    return 0;
}

static int lx_pthread_mutex_init(lx_mutex *mutex, const lx_mutexattr *attr)
{
    int flags = attr ? attr->value : 0;
    if (flags & ~31)
        return LX_EINVAL;
    if (flags & 28)
        return LX_EOPNOTSUPP;
    memset(mutex, 0, sizeof(*mutex));
    mutex->kind = flags & 3;
    return 0;
}

static int mutex_kind(const lx_mutex *mutex)
{
    if (mutex->kind < 0)
        return LX_EINVAL;
    if (mutex->kind > 3)
        return LX_EOPNOTSUPP; /* No robust or priority-inheritance state. */
    return 0;
}

static int lx_pthread_mutex_destroy(lx_mutex *mutex)
{
    int error = mutex_kind(mutex);
    if (error)
        return error;
    if (!TryAcquireSRWLockExclusive(&mutex->lock))
        return LX_EBUSY;
    mutex->kind = -1;
    ReleaseSRWLockExclusive(&mutex->lock);
    return 0;
}

static int mutex_acquire(lx_mutex *mutex, const lx_sync_timespec *deadline, int try_only)
{
    DWORD tid = GetCurrentThreadId();
    int error = mutex_kind(mutex);
    if (error)
        return error;
    if ((DWORD)InterlockedCompareExchange(&mutex->owner, 0, 0) == tid) {
        if (mutex->kind == 1) {
            if (mutex->count == UINT32_MAX)
                return LX_EAGAIN;
            ++mutex->count;
            return 0;
        }
        if (try_only)
            return LX_EBUSY;
        if (mutex->kind == 2)
            return LX_EDEADLK;
    }
    for (;;) {
        LONG owner;
        DWORD timeout = INFINITE;
        if (TryAcquireSRWLockExclusive(&mutex->lock)) {
            mutex->count = 1;
            InterlockedExchange(&mutex->owner, (LONG)tid);
            return 0;
        }
        if (try_only)
            return LX_EBUSY;
        if (deadline && (error = deadline_timeout(deadline, 0, &timeout)))
            return error;
        owner = InterlockedCompareExchange(&mutex->owner, 0, 0);
        if (!owner) {
            YieldProcessor(); /* An owner is between SRW acquisition/publication or release. */
            continue;
        }
        if (!WaitOnAddress(&mutex->owner, &owner, sizeof(owner), timeout) &&
            GetLastError() != ERROR_TIMEOUT)
            return lx_errno_from_win32(GetLastError());
    }
}

static int lx_pthread_mutex_lock(lx_mutex *mutex)
{
    return mutex_acquire(mutex, NULL, 0);
}

static int lx_pthread_mutex_trylock(lx_mutex *mutex)
{
    return mutex_acquire(mutex, NULL, 1);
}

static int lx_pthread_mutex_timedlock(lx_mutex *mutex, const lx_sync_timespec *deadline)
{
    if (!deadline)
        return LX_EINVAL;
    return mutex_acquire(mutex, deadline, 0);
}

static int lx_pthread_mutex_unlock(lx_mutex *mutex)
{
    int error = mutex_kind(mutex);
    if (error)
        return error;
    if ((DWORD)InterlockedCompareExchange(&mutex->owner, 0, 0) != GetCurrentThreadId())
        return LX_EPERM;
    if (mutex->kind == 1 && mutex->count > 1) {
        --mutex->count;
        return 0;
    }
    mutex->count = 0;
    InterlockedExchange(&mutex->owner, 0);
    ReleaseSRWLockExclusive(&mutex->lock);
    WakeByAddressAll((void *)&mutex->owner);
    return 0;
}

static int lx_pthread_mutex_consistent(lx_mutex *mutex)
{
    (void)mutex;
    return LX_EINVAL; /* Only nonrobust mutexes are supported. */
}

static int lx_pthread_condattr_setpshared(lx_condattr *attr, int shared)
{
    return attr_setshared(attr, shared, 1);
}

static int lx_pthread_condattr_getpshared(const lx_condattr *attr, int *shared)
{
    return attr_getshared(attr, shared, 1);
}

static int lx_pthread_condattr_setclock(lx_condattr *attr, int clockid)
{
    if (clockid != 0 && clockid != 1)
        return LX_EINVAL;
    attr->value = (attr->value & ~2) | (clockid << 1);
    return 0;
}

static int lx_pthread_condattr_getclock(const lx_condattr *attr, int *clockid)
{
    *clockid = (attr->value >> 1) & 1;
    return 0;
}

static int cond_init(void *object, size_t size, const lx_condattr *attr)
{
    lx_cond_old *cond = object;
    int flags = attr ? attr->value : 0;
    if (flags & ~3)
        return LX_EINVAL;
    if (flags & 1)
        return LX_EOPNOTSUPP;
    memset(object, 0, size);
    cond->flags = flags;
    return 0;
}

static int lx_pthread_cond_init(lx_cond *cond, const lx_condattr *attr)
{
    return cond_init(cond, sizeof(*cond), attr);
}

static int lx_pthread_cond_init_old(lx_cond_old *cond, const lx_condattr *attr)
{
    return cond_init(cond, sizeof(*cond), attr);
}

/* These four common entry points only access the 12-byte prefix. The old ABI
 * must not read or write the storage belonging to an adjacent guest object. */
static int lx_pthread_cond_destroy(lx_cond_old *cond)
{
    int error = 0;
    AcquireSRWLockExclusive(&cond->gate);
    if (cond->flags < 0)
        error = LX_EINVAL;
    else if (cond->flags >> 2)
        error = LX_EBUSY;
    else
        cond->flags = -1;
    ReleaseSRWLockExclusive(&cond->gate);
    return error;
}

static int cond_wait(lx_cond_old *cond, lx_mutex *mutex, const lx_sync_timespec *deadline)
{
    int error, lock_error;
    if (deadline && (deadline->nsec < 0 || deadline->nsec >= 1000000000))
        return LX_EINVAL;
    AcquireSRWLockExclusive(&cond->gate);
    if (cond->flags < 0) {
        ReleaseSRWLockExclusive(&cond->gate);
        return LX_EINVAL;
    }
    if (cond->flags > INT32_MAX - 4) {
        ReleaseSRWLockExclusive(&cond->gate);
        return LX_EAGAIN;
    }
    cond->flags += 4;
    error = lx_pthread_mutex_unlock(mutex);
    if (!error)
        error = cv_wait(&cond->changed, &cond->gate, deadline, (cond->flags >> 1) & 1);
    else {
        cond->flags -= 4;
        ReleaseSRWLockExclusive(&cond->gate);
        return error;
    }
    cond->flags -= 4;
    ReleaseSRWLockExclusive(&cond->gate);
    lock_error = lx_pthread_mutex_lock(mutex);
    return lock_error ? lock_error : error;
}

static int lx_pthread_cond_wait(lx_cond_old *cond, lx_mutex *mutex)
{
    return cond_wait(cond, mutex, NULL);
}

static int lx_pthread_cond_timedwait(lx_cond_old *cond, lx_mutex *mutex,
                                     const lx_sync_timespec *deadline)
{
    if (!deadline)
        return LX_EINVAL;
    return cond_wait(cond, mutex, deadline);
}

static int lx_pthread_cond_signal(lx_cond_old *cond)
{
    int error = 0;
    AcquireSRWLockExclusive(&cond->gate);
    if (cond->flags < 0)
        error = LX_EINVAL;
    else
        WakeConditionVariable(&cond->changed);
    ReleaseSRWLockExclusive(&cond->gate);
    return error;
}

static int lx_pthread_cond_broadcast(lx_cond_old *cond)
{
    int error = 0;
    AcquireSRWLockExclusive(&cond->gate);
    if (cond->flags < 0)
        error = LX_EINVAL;
    else
        WakeAllConditionVariable(&cond->changed);
    ReleaseSRWLockExclusive(&cond->gate);
    return error;
}

/* Compact rwlock attributes: preference bits 0..1, shared bit 2. */
static int lx_pthread_rwlockattr_setpshared(lx_rwlockattr *attr, int shared)
{
    return attr_setshared(attr, shared, 4);
}

static int lx_pthread_rwlockattr_getpshared(const lx_rwlockattr *attr, int *shared)
{
    return attr_getshared(attr, shared, 4);
}

static int lx_pthread_rwlockattr_setkind_np(lx_rwlockattr *attr, int kind)
{
    if (kind < 0 || kind > 2)
        return LX_EINVAL;
    attr->value = (attr->value & ~3) | kind;
    return 0;
}

static int lx_pthread_rwlockattr_getkind_np(const lx_rwlockattr *attr, int *kind)
{
    *kind = attr->value & 3;
    return 0;
}

static int lx_pthread_rwlock_init(lx_rwlock *rwlock, const lx_rwlockattr *attr)
{
    int flags = attr ? attr->value : 0;
    if ((flags & ~7) || (flags & 3) == 3)
        return LX_EINVAL;
    if (flags & 4)
        return LX_EOPNOTSUPP;
    memset(rwlock, 0, sizeof(*rwlock));
    rwlock->kind = flags & 3;
    return 0;
}

static int lx_pthread_rwlock_destroy(lx_rwlock *rwlock)
{
    int error = 0;
    AcquireSRWLockExclusive(&rwlock->gate);
    if (rwlock->kind < 0 || rwlock->kind > 2)
        error = LX_EINVAL;
    else if (rwlock->readers || rwlock->writer || rwlock->waiting_readers || rwlock->waiting_writers)
        error = LX_EBUSY;
    else
        rwlock->kind = -1;
    ReleaseSRWLockExclusive(&rwlock->gate);
    return error;
}

static int rwlock_acquire(lx_rwlock *rwlock, int write, int try_only,
                          const lx_sync_timespec *deadline)
{
    DWORD tid = GetCurrentThreadId();
    uint32_t *waiting = write ? &rwlock->waiting_writers : &rwlock->waiting_readers;
    int error = 0, registered = 0;
    AcquireSRWLockExclusive(&rwlock->gate);
    if (rwlock->kind < 0 || rwlock->kind > 2 || rwlock->shared) {
        error = LX_EINVAL;
        goto out;
    }
    for (;;) {
        int blocked = rwlock->writer || (write ? rwlock->readers != 0 :
            (rwlock->kind == 2 && rwlock->waiting_writers != 0));
        if (!blocked) {
            if (write)
                rwlock->writer = tid;
            else if (rwlock->readers == UINT32_MAX) {
                error = LX_EAGAIN;
                goto out;
            } else
                ++rwlock->readers;
            break;
        }
        if (try_only) {
            error = LX_EBUSY;
            break;
        }
        if (rwlock->writer == tid) {
            error = LX_EDEADLK;
            break;
        }
        if (!registered) {
            if (*waiting == UINT32_MAX) {
                error = LX_EAGAIN;
                break;
            }
            ++*waiting;
            registered = 1;
        }
        error = cv_wait(&rwlock->changed, &rwlock->gate, deadline, 0);
        if (error)
            break;
    }
out:
    if (registered) {
        --*waiting;
        /* A timed-out final preferred writer must release waiting readers. */
        if (write && !rwlock->waiting_writers && !rwlock->writer)
            WakeAllConditionVariable(&rwlock->changed);
    }
    ReleaseSRWLockExclusive(&rwlock->gate);
    return error;
}

static int lx_pthread_rwlock_rdlock(lx_rwlock *rwlock)
{
    return rwlock_acquire(rwlock, 0, 0, NULL);
}

static int lx_pthread_rwlock_wrlock(lx_rwlock *rwlock)
{
    return rwlock_acquire(rwlock, 1, 0, NULL);
}

static int lx_pthread_rwlock_tryrdlock(lx_rwlock *rwlock)
{
    return rwlock_acquire(rwlock, 0, 1, NULL);
}

static int lx_pthread_rwlock_trywrlock(lx_rwlock *rwlock)
{
    return rwlock_acquire(rwlock, 1, 1, NULL);
}

static int lx_pthread_rwlock_timedrdlock(lx_rwlock *rwlock, const lx_sync_timespec *deadline)
{
    if (!deadline)
        return LX_EINVAL;
    return rwlock_acquire(rwlock, 0, 0, deadline);
}

static int lx_pthread_rwlock_timedwrlock(lx_rwlock *rwlock, const lx_sync_timespec *deadline)
{
    if (!deadline)
        return LX_EINVAL;
    return rwlock_acquire(rwlock, 1, 0, deadline);
}

static int lx_pthread_rwlock_unlock(lx_rwlock *rwlock)
{
    int error = 0;
    AcquireSRWLockExclusive(&rwlock->gate);
    if (rwlock->kind < 0 || rwlock->kind > 2)
        error = LX_EINVAL;
    else if (rwlock->writer) {
        if (rwlock->writer != GetCurrentThreadId())
            error = LX_EPERM;
        else
            rwlock->writer = 0;
    } else if (rwlock->readers)
        --rwlock->readers;
    else
        error = LX_EPERM;
    if (!error && !rwlock->writer)
        WakeAllConditionVariable(&rwlock->changed);
    ReleaseSRWLockExclusive(&rwlock->gate);
    return error;
}

static int lx_pthread_barrierattr_setpshared(lx_barrierattr *attr, int shared)
{
    return attr_setshared(attr, shared, 1);
}

static int lx_pthread_barrierattr_getpshared(const lx_barrierattr *attr, int *shared)
{
    return attr_getshared(attr, shared, 1);
}

static int lx_pthread_barrier_init(lx_barrier *barrier, const lx_barrierattr *attr, unsigned count)
{
    int flags = attr ? attr->value : 0;
    if (!count || count >= INT32_MAX || (flags & ~1))
        return LX_EINVAL;
    if (flags & 1)
        return LX_EOPNOTSUPP;
    memset(barrier, 0, sizeof(*barrier));
    barrier->count = count;
    return 0;
}

static int lx_pthread_barrier_destroy(lx_barrier *barrier)
{
    int error = 0;
    AcquireSRWLockExclusive(&barrier->gate);
    if (!barrier->count)
        error = LX_EINVAL;
    else if (barrier->active)
        error = LX_EBUSY;
    else
        barrier->count = 0;
    ReleaseSRWLockExclusive(&barrier->gate);
    return error;
}

static int lx_pthread_barrier_wait(lx_barrier *barrier)
{
    LONG generation;
    int error = 0;
    AcquireSRWLockExclusive(&barrier->gate);
    if (!barrier->count || barrier->active == UINT32_MAX) {
        error = barrier->count ? LX_EAGAIN : LX_EINVAL;
        ReleaseSRWLockExclusive(&barrier->gate);
        return error;
    }
    generation = barrier->generation;
    ++barrier->active;
    if (++barrier->arrived == barrier->count) {
        barrier->arrived = 0;
        InterlockedIncrement(&barrier->generation);
        --barrier->active;
        ReleaseSRWLockExclusive(&barrier->gate);
        WakeByAddressAll((void *)&barrier->generation);
        return -1; /* PTHREAD_BARRIER_SERIAL_THREAD, not an errno failure. */
    }
    ReleaseSRWLockExclusive(&barrier->gate);
    for (;;) {
        if (InterlockedCompareExchange(&barrier->generation, 0, 0) != generation)
            break;
        if (!WaitOnAddress(&barrier->generation, &generation, sizeof(generation), INFINITE)) {
            error = lx_errno_from_win32(GetLastError());
            break;
        }
    }
    AcquireSRWLockExclusive(&barrier->gate);
    if (error && barrier->generation == generation)
        --barrier->arrived;
    --barrier->active;
    ReleaseSRWLockExclusive(&barrier->gate);
    return error;
}

static int lx_pthread_spin_init(volatile LONG *spin, int shared)
{
    if (shared != 0 && shared != 1)
        return LX_EINVAL;
    InterlockedExchange(spin, 0);
    return 0;
}

static int lx_pthread_spin_destroy(volatile LONG *spin)
{
    LONG previous = InterlockedCompareExchange(spin, -1, 0);
    return previous == 0 ? 0 : previous == 1 ? LX_EBUSY : LX_EINVAL;
}

static int lx_pthread_spin_trylock(volatile LONG *spin)
{
    LONG previous = InterlockedCompareExchange(spin, 1, 0);
    return previous == 0 ? 0 : previous == 1 ? LX_EBUSY : LX_EINVAL;
}

static int lx_pthread_spin_lock(volatile LONG *spin)
{
    unsigned spins = 0;
    for (;;) {
        LONG previous = InterlockedCompareExchange(spin, 1, 0);
        if (!previous)
            return 0;
        if (previous != 1)
            return LX_EINVAL;
        YieldProcessor();
        if (++spins == 256) {
            SwitchToThread();
            spins = 0;
        }
    }
}

static int lx_pthread_spin_unlock(volatile LONG *spin)
{
    return InterlockedCompareExchange(spin, 0, 1) == 1 ? 0 : LX_EINVAL;
}

static int lx_sem_init(lx_sem *sem, int shared, unsigned value)
{
    if (value > INT32_MAX)
        return lx_set_errno(LX_EINVAL);
    if (shared)
        return lx_set_errno(LX_ENOSYS);
    memset(sem, 0, sizeof(*sem));
    sem->value = (LONG)value;
    return 0;
}

static int lx_sem_destroy(lx_sem *sem)
{
    if (InterlockedCompareExchange(&sem->value, 0, 0) < 0)
        return lx_set_errno(LX_EINVAL);
    if (InterlockedCompareExchange(&sem->waiters, 0, 0))
        return lx_set_errno(LX_EBUSY);
    InterlockedExchange(&sem->value, -1);
    return 0;
}

/* Return an errno value internally; semaphore entry points alone set errno. */
static int sem_take(lx_sem *sem)
{
    LONG value = InterlockedCompareExchange(&sem->value, 0, 0);
    for (;;) {
        LONG previous;
        if (value < 0)
            return LX_EINVAL;
        if (!value)
            return LX_EAGAIN;
        previous = InterlockedCompareExchange(&sem->value, value - 1, value);
        if (previous == value)
            return 0;
        value = previous;
    }
}

static int lx_sem_trywait(lx_sem *sem)
{
    int error = sem_take(sem);
    return error ? lx_set_errno(error) : 0;
}

static int sem_wait(lx_sem *sem, const lx_sync_timespec *deadline)
{
    int error = sem_take(sem);
    if (error != LX_EAGAIN)
        return error ? lx_set_errno(error) : 0;
    if (InterlockedIncrement(&sem->waiters) <= 0) {
        InterlockedDecrement(&sem->waiters);
        return lx_set_errno(LX_EAGAIN);
    }
    for (;;) {
        LONG zero = 0;
        DWORD timeout = INFINITE;
        error = sem_take(sem);
        if (error != LX_EAGAIN)
            break;
        if (deadline && (error = deadline_timeout(deadline, 0, &timeout)))
            break;
        if (!WaitOnAddress(&sem->value, &zero, sizeof(zero), timeout) &&
            GetLastError() != ERROR_TIMEOUT) {
            error = lx_errno_from_win32(GetLastError());
            break;
        }
    }
    InterlockedDecrement(&sem->waiters);
    return error ? lx_set_errno(error) : 0;
}

static int lx_sem_wait(lx_sem *sem)
{
    return sem_wait(sem, NULL);
}

static int lx_sem_timedwait(lx_sem *sem, const lx_sync_timespec *deadline)
{
    if (!deadline)
        return lx_set_errno(LX_EINVAL);
    return sem_wait(sem, deadline);
}

static int lx_sem_post(lx_sem *sem)
{
    LONG value = InterlockedCompareExchange(&sem->value, 0, 0);
    for (;;) {
        LONG previous;
        if (value < 0)
            return lx_set_errno(LX_EINVAL);
        if (value == INT32_MAX)
            return lx_set_errno(LX_EOVERFLOW);
        previous = InterlockedCompareExchange(&sem->value, value + 1, value);
        if (previous == value)
            break;
        value = previous;
    }
    WakeByAddressSingle((void *)&sem->value);
    return 0;
}

static int lx_sem_getvalue(lx_sem *sem, int *result)
{
    LONG value = InterlockedCompareExchange(&sem->value, 0, 0);
    if (value < 0)
        return lx_set_errno(LX_EINVAL);
    *result = (int)value;
    return 0;
}

static void *lx_sem_open(const char *name, int flags, ...)
{
    (void)name;
    (void)flags;
    lx_set_errno(LX_ENOSYS);
    return NULL; /* glibc's SEM_FAILED is (sem_t *)0, not MAP_FAILED. */
}

static int lx_sem_close(lx_sem *sem)
{
    (void)sem;
    return lx_set_errno(LX_ENOSYS);
}

static int lx_sem_unlink(const char *name)
{
    (void)name;
    return lx_set_errno(LX_ENOSYS);
}

LX_SYMTAB(sync,
    LX_SYM("pthread_mutex_init", lx_pthread_mutex_init),
    LX_SYM("pthread_mutex_destroy", lx_pthread_mutex_destroy),
    LX_SYM("pthread_mutex_lock", lx_pthread_mutex_lock),
    LX_SYM("pthread_mutex_trylock", lx_pthread_mutex_trylock),
    LX_SYM("pthread_mutex_timedlock", lx_pthread_mutex_timedlock),
    LX_SYM("pthread_mutex_unlock", lx_pthread_mutex_unlock),
    LX_SYM("pthread_mutex_consistent", lx_pthread_mutex_consistent),
    LX_SYM("pthread_mutex_consistent_np", lx_pthread_mutex_consistent),
    LX_SYM("pthread_mutexattr_init", attr_init),
    LX_SYM("pthread_mutexattr_destroy", attr_destroy),
    LX_SYM("pthread_mutexattr_settype", lx_pthread_mutexattr_settype),
    LX_SYM("pthread_mutexattr_gettype", lx_pthread_mutexattr_gettype),
    LX_SYM("pthread_mutexattr_setkind_np", lx_pthread_mutexattr_settype),
    LX_SYM("pthread_mutexattr_getkind_np", lx_pthread_mutexattr_gettype),
    LX_SYM("pthread_mutexattr_setpshared", lx_pthread_mutexattr_setpshared),
    LX_SYM("pthread_mutexattr_getpshared", lx_pthread_mutexattr_getpshared),
    LX_SYM("pthread_mutexattr_setprotocol", lx_pthread_mutexattr_setprotocol),
    LX_SYM("pthread_mutexattr_getprotocol", lx_pthread_mutexattr_getprotocol),
    LX_SYM("pthread_condattr_init", attr_init),
    LX_SYM("pthread_condattr_destroy", attr_destroy),
    LX_SYM("pthread_condattr_setpshared", lx_pthread_condattr_setpshared),
    LX_SYM("pthread_condattr_getpshared", lx_pthread_condattr_getpshared),
    LX_SYM("pthread_condattr_setclock", lx_pthread_condattr_setclock),
    LX_SYM("pthread_condattr_getclock", lx_pthread_condattr_getclock),
    /* The modern version precedes the legacy version for unversioned lookup. */
    LX_SYMV("pthread_cond_init", "GLIBC_2.3.2", lx_pthread_cond_init),
    LX_SYMV("pthread_cond_init", "GLIBC_2.0", lx_pthread_cond_init_old),
    LX_SYM("pthread_cond_init", lx_pthread_cond_init),
    LX_SYMV("pthread_cond_destroy", "GLIBC_2.3.2", lx_pthread_cond_destroy),
    LX_SYMV("pthread_cond_destroy", "GLIBC_2.0", lx_pthread_cond_destroy),
    LX_SYM("pthread_cond_destroy", lx_pthread_cond_destroy),
    LX_SYMV("pthread_cond_wait", "GLIBC_2.3.2", lx_pthread_cond_wait),
    LX_SYMV("pthread_cond_wait", "GLIBC_2.0", lx_pthread_cond_wait),
    LX_SYM("pthread_cond_wait", lx_pthread_cond_wait),
    LX_SYMV("pthread_cond_timedwait", "GLIBC_2.3.2", lx_pthread_cond_timedwait),
    LX_SYMV("pthread_cond_timedwait", "GLIBC_2.0", lx_pthread_cond_timedwait),
    LX_SYM("pthread_cond_timedwait", lx_pthread_cond_timedwait),
    LX_SYMV("pthread_cond_signal", "GLIBC_2.3.2", lx_pthread_cond_signal),
    LX_SYMV("pthread_cond_signal", "GLIBC_2.0", lx_pthread_cond_signal),
    LX_SYM("pthread_cond_signal", lx_pthread_cond_signal),
    LX_SYMV("pthread_cond_broadcast", "GLIBC_2.3.2", lx_pthread_cond_broadcast),
    LX_SYMV("pthread_cond_broadcast", "GLIBC_2.0", lx_pthread_cond_broadcast),
    LX_SYM("pthread_cond_broadcast", lx_pthread_cond_broadcast),
    LX_SYM("pthread_rwlock_init", lx_pthread_rwlock_init),
    LX_SYM("pthread_rwlock_destroy", lx_pthread_rwlock_destroy),
    LX_SYM("pthread_rwlock_rdlock", lx_pthread_rwlock_rdlock),
    LX_SYM("pthread_rwlock_wrlock", lx_pthread_rwlock_wrlock),
    LX_SYM("pthread_rwlock_tryrdlock", lx_pthread_rwlock_tryrdlock),
    LX_SYM("pthread_rwlock_trywrlock", lx_pthread_rwlock_trywrlock),
    LX_SYM("pthread_rwlock_timedrdlock", lx_pthread_rwlock_timedrdlock),
    LX_SYM("pthread_rwlock_timedwrlock", lx_pthread_rwlock_timedwrlock),
    LX_SYM("pthread_rwlock_unlock", lx_pthread_rwlock_unlock),
    LX_SYM("pthread_rwlockattr_init", attr_init),
    LX_SYM("pthread_rwlockattr_destroy", attr_destroy),
    LX_SYM("pthread_rwlockattr_setpshared", lx_pthread_rwlockattr_setpshared),
    LX_SYM("pthread_rwlockattr_getpshared", lx_pthread_rwlockattr_getpshared),
    LX_SYM("pthread_rwlockattr_setkind_np", lx_pthread_rwlockattr_setkind_np),
    LX_SYM("pthread_rwlockattr_getkind_np", lx_pthread_rwlockattr_getkind_np),
    LX_SYM("pthread_barrier_init", lx_pthread_barrier_init),
    LX_SYM("pthread_barrier_destroy", lx_pthread_barrier_destroy),
    LX_SYM("pthread_barrier_wait", lx_pthread_barrier_wait),
    LX_SYM("pthread_barrierattr_init", attr_init),
    LX_SYM("pthread_barrierattr_destroy", attr_destroy),
    LX_SYM("pthread_barrierattr_setpshared", lx_pthread_barrierattr_setpshared),
    LX_SYM("pthread_barrierattr_getpshared", lx_pthread_barrierattr_getpshared),
    LX_SYM("pthread_spin_init", lx_pthread_spin_init),
    LX_SYM("pthread_spin_destroy", lx_pthread_spin_destroy),
    LX_SYM("pthread_spin_lock", lx_pthread_spin_lock),
    LX_SYM("pthread_spin_trylock", lx_pthread_spin_trylock),
    LX_SYM("pthread_spin_unlock", lx_pthread_spin_unlock),
    LX_SYM("sem_init", lx_sem_init),
    LX_SYM("sem_destroy", lx_sem_destroy),
    LX_SYM("sem_wait", lx_sem_wait),
    LX_SYM("sem_trywait", lx_sem_trywait),
    LX_SYM("sem_timedwait", lx_sem_timedwait),
    LX_SYM("sem_post", lx_sem_post),
    LX_SYM("sem_getvalue", lx_sem_getvalue),
    LX_SYM("sem_open", lx_sem_open),
    LX_SYM("sem_close", lx_sem_close),
    LX_SYM("sem_unlink", lx_sem_unlink)
);
