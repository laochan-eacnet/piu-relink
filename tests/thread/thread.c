#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stddef.h>

_Static_assert(sizeof(pthread_t) == 4, "pthread_t");
_Static_assert(sizeof(pthread_attr_t) == 36, "pthread_attr_t");
_Static_assert(sizeof(pthread_once_t) == 4, "pthread_once_t");
_Static_assert(sizeof(pthread_mutex_t) == 24, "pthread_mutex_t");
_Static_assert(offsetof(pthread_mutex_t, __data.__kind) == 12, "mutex kind");
_Static_assert(sizeof(pthread_cond_t) == 48, "pthread_cond_t");
_Static_assert(sizeof(pthread_rwlock_t) == 32, "pthread_rwlock_t");
_Static_assert(sizeof(pthread_barrier_t) == 20, "pthread_barrier_t");
_Static_assert(sizeof(sem_t) == 16, "sem_t");
_Static_assert(sizeof(pthread_mutexattr_t) == 4, "mutexattr");
_Static_assert(sizeof(pthread_condattr_t) == 4, "condattr");
_Static_assert(sizeof(pthread_barrierattr_t) == 4, "barrierattr");
/* glibc reserves eight opaque rwlockattr bytes; lxrt needs only its first four. */
_Static_assert(sizeof(pthread_rwlockattr_t) == 8, "rwlockattr storage");
#define CHECK(x) do { if (!(x)) { printf("FAIL thread line %d\n", __LINE__); exit(1); } } while (0)
#define N 8
static pthread_mutex_t normal = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t recursive = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_spinlock_t spin;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int nnormal, nrecursive, nspin, nonce;
static pthread_key_t key;
static int ndtor, ncxa, dtor_order;
extern int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
static void once_init(void) { ++nonce; }
static void key_dtor(void *p)
{
    CHECK(dtor_order == 2);
    __sync_fetch_and_add(&ndtor, 1);
    intptr_t n = (intptr_t)p;
    if (n < 4) CHECK(pthread_setspecific(key, (void *)(n + 1)) == 0);
}
static void cxa_dtor(void *p)
{
    CHECK((intptr_t)p == 1);
    dtor_order = 2;
    ++ncxa;
}
static void *counter(void *p)
{
    for (int i = 0; i < 2000; ++i) {
        CHECK(pthread_mutex_lock(&normal) == 0); ++nnormal; CHECK(pthread_mutex_unlock(&normal) == 0);
        CHECK(pthread_mutex_lock(&recursive) == 0); CHECK(pthread_mutex_lock(&recursive) == 0);
        ++nrecursive;
        CHECK(pthread_mutex_unlock(&recursive) == 0); CHECK(pthread_mutex_unlock(&recursive) == 0);
        CHECK(pthread_spin_lock(&spin) == 0); ++nspin; CHECK(pthread_spin_unlock(&spin) == 0);
        CHECK(pthread_once(&once, once_init) == 0);
    }
    return (void *)((intptr_t)p + 100);
}
static void *destructor_thread(void *p)
{
    CHECK(pthread_setspecific(key, (void *)1) == 0);
    CHECK(pthread_getspecific(key) == (void *)1);
    CHECK(__cxa_thread_atexit_impl(cxa_dtor, (void *)1, &dtor_order) == 0);
    dtor_order = 1;
    if (p) pthread_exit((void *)77);
    return (void *)88;
}
static pthread_cond_t available = PTHREAD_COND_INITIALIZER, consumed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int slot, full, total;
static void *consumer(void *unused)
{
    for (;;) {
        CHECK(pthread_mutex_lock(&queue_lock) == 0);
        while (!full) CHECK(pthread_cond_wait(&available, &queue_lock) == 0);
        int n = slot; full = 0;
        CHECK(pthread_cond_signal(&consumed) == 0);
        CHECK(pthread_mutex_unlock(&queue_lock) == 0);
        if (n < 0) break;
        total += n;
    }
    return NULL;
}
static struct timespec after_ms(clockid_t id, int ms)
{
    struct timespec t; CHECK(clock_gettime(id, &t) == 0);
    t.tv_nsec += ms * 1000000;
    if (t.tv_nsec >= 1000000000) { ++t.tv_sec; t.tv_nsec -= 1000000000; }
    return t;
}
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static int rwvalue, rwmirror;
static void *rw_worker(void *p)
{
    for (int i = 0; i < 300; ++i) {
        if ((intptr_t)p & 1) {
            CHECK(pthread_rwlock_wrlock(&rw) == 0); ++rwvalue; rwmirror = rwvalue;
        } else {
            CHECK(pthread_rwlock_rdlock(&rw) == 0); CHECK(rwvalue == rwmirror);
        }
        CHECK(pthread_rwlock_unlock(&rw) == 0);
    }
    return NULL;
}
static pthread_barrier_t barrier;
static int serials;
static void *barrier_worker(void *p)
{
    for (int i = 0; i < 50; ++i) {
        int rc = pthread_barrier_wait(&barrier);
        CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
        if (rc == PTHREAD_BARRIER_SERIAL_THREAD) __sync_fetch_and_add(&serials, 1);
    }
    return NULL;
}
static sem_t done;
static void *detached_worker(void *p)
{
    CHECK(sem_post(&done) == 0);
    return NULL;
}
static volatile int futex_word, futex_ready;
static void *futex_worker(void *p)
{
    __atomic_store_n(&futex_ready, 1, __ATOMIC_RELEASE);
    CHECK(syscall(SYS_futex, &futex_word, (intptr_t)p ? 128 : 0, 0, NULL) == 0);
    return NULL;
}
static void *stack_worker(void *p)
{
    volatile char block[131072];
    for (unsigned i = 0; i < sizeof(block); i += 4096) block[i] = (char)i;
    pthread_attr_t a; void *base; size_t size;
    CHECK(pthread_getattr_np(pthread_self(), &a) == 0);
    CHECK(pthread_attr_getstack(&a, &base, &size) == 0);
    CHECK((uintptr_t)&block[0] >= (uintptr_t)base);
    CHECK((uintptr_t)&block[sizeof(block)-1] < (uintptr_t)base + size);
    CHECK(pthread_attr_destroy(&a) == 0);
    char name[16];
    CHECK(pthread_setname_np(pthread_self(), "lxrt-thread") == 0);
    CHECK(pthread_getname_np(pthread_self(), name, sizeof(name)) == 0);
    CHECK(strcmp(name, "lxrt-thread") == 0);
    CHECK(pthread_equal(pthread_self(), pthread_self()));
    CHECK(pthread_kill(pthread_self(), 0) == 0);
    return p;
}
extern int old_cond_init(void *, const void *);
extern int old_cond_wait(void *, pthread_mutex_t *);
extern int old_cond_timedwait(void *, pthread_mutex_t *, const struct timespec *);
extern int old_cond_signal(void *);
extern int old_cond_broadcast(void *);
extern int old_cond_destroy(void *);
__asm__(".symver old_cond_init,pthread_cond_init@GLIBC_2.0");
__asm__(".symver old_cond_wait,pthread_cond_wait@GLIBC_2.0");
__asm__(".symver old_cond_timedwait,pthread_cond_timedwait@GLIBC_2.0");
__asm__(".symver old_cond_signal,pthread_cond_signal@GLIBC_2.0");
__asm__(".symver old_cond_broadcast,pthread_cond_broadcast@GLIBC_2.0");
__asm__(".symver old_cond_destroy,pthread_cond_destroy@GLIBC_2.0");
int main(void)
{
    pthread_t th[N]; void *ret;
    CHECK(pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE) == 0);
    for (intptr_t i = 0; i < N; ++i) CHECK(pthread_create(&th[i], NULL, counter, (void *)i) == 0);
    for (intptr_t i = 0; i < N; ++i) { CHECK(pthread_join(th[i], &ret) == 0); CHECK(ret == (void *)(i + 100)); }
    CHECK(nnormal == N*2000 && nrecursive == N*2000 && nspin == N*2000 && nonce == 1);
    CHECK(pthread_spin_destroy(&spin) == 0);
    puts("mutex recursive spin once join: pass");
    CHECK(pthread_key_create(&key, key_dtor) == 0);
    CHECK(pthread_create(&th[0], NULL, destructor_thread, NULL) == 0);
    CHECK(pthread_join(th[0], &ret) == 0 && ret == (void *)88);
    CHECK(pthread_create(&th[0], NULL, destructor_thread, (void *)1) == 0);
    CHECK(pthread_join(th[0], &ret) == 0 && ret == (void *)77);
    CHECK(ndtor == 8 && ncxa == 2); CHECK(pthread_key_delete(key) == 0);
    puts("TSD four rounds C++ order pthread_exit: pass");
    CHECK(pthread_create(&th[0], NULL, consumer, NULL) == 0);
    for (int n = 1; n <= 1001; ++n) {
        CHECK(pthread_mutex_lock(&queue_lock) == 0);
        while (full) CHECK(pthread_cond_wait(&consumed, &queue_lock) == 0);
        slot = n == 1001 ? -1 : n; full = 1;
        CHECK(pthread_cond_signal(&available) == 0); CHECK(pthread_mutex_unlock(&queue_lock) == 0);
    }
    CHECK(pthread_join(th[0], NULL) == 0); CHECK(total == 500500);
    CHECK(pthread_mutex_lock(&queue_lock) == 0);
    struct timespec deadline = after_ms(CLOCK_REALTIME, 10);
    CHECK(pthread_cond_timedwait(&available, &queue_lock, &deadline) == ETIMEDOUT);
    CHECK(pthread_mutex_unlock(&queue_lock) == 0);
    pthread_condattr_t ca; pthread_cond_t mono;
    CHECK(pthread_condattr_init(&ca) == 0); CHECK(pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_cond_init(&mono, &ca) == 0); CHECK(pthread_condattr_destroy(&ca) == 0);
    CHECK(pthread_mutex_lock(&queue_lock) == 0); deadline = after_ms(CLOCK_MONOTONIC, 10);
    CHECK(pthread_cond_timedwait(&mono, &queue_lock, &deadline) == ETIMEDOUT);
    CHECK(pthread_mutex_unlock(&queue_lock) == 0); CHECK(pthread_cond_destroy(&mono) == 0);
    struct { uint32_t before; uint32_t cond[3]; uint32_t after; } old = {0x12345678, {0}, 0x87654321};
    CHECK(old_cond_init(old.cond, NULL) == 0);
    CHECK(pthread_mutex_lock(&queue_lock) == 0); deadline = after_ms(CLOCK_REALTIME, 1);
    CHECK(old_cond_timedwait(old.cond, &queue_lock, &deadline) == ETIMEDOUT);
    CHECK(pthread_mutex_unlock(&queue_lock) == 0);
    CHECK(old_cond_signal(old.cond) == 0 && old_cond_broadcast(old.cond) == 0 && old_cond_destroy(old.cond) == 0);
    CHECK(old.before == 0x12345678 && old.after == 0x87654321);
    puts("cond producer consumer realtime monotonic legacy12: pass");
    for (intptr_t i = 0; i < N; ++i) CHECK(pthread_create(&th[i], NULL, rw_worker, (void *)i) == 0);
    for (int i = 0; i < N; ++i) CHECK(pthread_join(th[i], NULL) == 0);
    CHECK(rwvalue == 1200 && rwvalue == rwmirror);
    CHECK(pthread_rwlock_tryrdlock(&rw) == 0); CHECK(pthread_rwlock_unlock(&rw) == 0);
    CHECK(pthread_rwlock_trywrlock(&rw) == 0); CHECK(pthread_rwlock_tryrdlock(&rw) == EBUSY);
    deadline = after_ms(CLOCK_REALTIME, 1); CHECK(pthread_rwlock_timedrdlock(&rw, &deadline) != 0);
    CHECK(pthread_rwlock_unlock(&rw) == 0); CHECK(pthread_rwlock_destroy(&rw) == 0);
    CHECK(pthread_barrier_init(&barrier, NULL, N) == 0);
    for (int i = 0; i < N; ++i) CHECK(pthread_create(&th[i], NULL, barrier_worker, NULL) == 0);
    for (int i = 0; i < N; ++i) CHECK(pthread_join(th[i], NULL) == 0);
    CHECK(serials == 50); CHECK(pthread_barrier_destroy(&barrier) == 0);
    puts("rwlock barrier: pass");
    CHECK(sem_init(&done, 0, 0) == 0); errno = 0; CHECK(sem_trywait(&done) == -1 && errno == EAGAIN);
    deadline = after_ms(CLOCK_REALTIME, 1); CHECK(sem_timedwait(&done, &deadline) == -1 && errno == ETIMEDOUT);
    pthread_attr_t a; CHECK(pthread_attr_init(&a) == 0); CHECK(pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED) == 0);
    for (int i = 0; i < N; ++i) CHECK(pthread_create(&th[i], &a, detached_worker, NULL) == 0);
    CHECK(pthread_attr_destroy(&a) == 0);
    for (int i = 0; i < N; ++i) CHECK(sem_wait(&done) == 0);
    CHECK(pthread_create(&th[0], NULL, detached_worker, NULL) == 0); CHECK(pthread_detach(th[0]) == 0); CHECK(sem_wait(&done) == 0);
    int value; CHECK(sem_getvalue(&done, &value) == 0 && value == 0);
    CHECK(sem_destroy(&done) == 0);
    puts("semaphore detached threads: pass");
    for (intptr_t mode = 0; mode < 2; ++mode) {
        futex_ready = futex_word = 0;
        CHECK(pthread_create(&th[0], NULL, futex_worker, (void *)mode) == 0);
        while (!__atomic_load_n(&futex_ready, __ATOMIC_ACQUIRE)) sched_yield();
        while (syscall(SYS_futex, &futex_word, 1 | (mode ? 128 : 0), 1) == 0) sched_yield();
        CHECK(pthread_join(th[0], NULL) == 0);
    }
    futex_word = 1;
    CHECK(syscall(SYS_futex, &futex_word, 0, 0, NULL) == -1 && errno == EAGAIN);
    struct timespec relative = {0, 1000000};
    CHECK(syscall(SYS_futex, &futex_word, 128, 1, &relative) == -1 && errno == ETIMEDOUT);
    CHECK(syscall(SYS_gettid) > 0 && syscall(SYS_getpid) == getpid());
    puts("futex wait wake private EAGAIN timeout syscall IDs: pass");
    CHECK(pthread_attr_init(&a) == 0); size_t size;
    CHECK(pthread_attr_setstacksize(&a, 1024*1024) == 0); CHECK(pthread_attr_getstacksize(&a, &size) == 0 && size == 1024*1024);
    CHECK(pthread_create(&th[0], &a, stack_worker, (void *)9) == 0); CHECK(pthread_join(th[0], &ret) == 0 && ret == (void *)9);
    void *stack = malloc(1024*1024); CHECK(stack != NULL);
    CHECK(pthread_attr_setstack(&a, stack, 1024*1024) == 0);
    CHECK(pthread_create(&th[0], &a, stack_worker, (void *)10) == 0); CHECK(pthread_join(th[0], &ret) == 0 && ret == (void *)10);
    free(stack); CHECK(pthread_attr_destroy(&a) == 0);
    puts("attributes stack commit custom stack names self: pass");
    pthread_mutexattr_t ma; pthread_mutex_t err;
    CHECK(pthread_mutexattr_init(&ma) == 0); CHECK(pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK) == 0);
    CHECK(pthread_mutex_init(&err, &ma) == 0); CHECK(pthread_mutexattr_destroy(&ma) == 0);
    CHECK(pthread_mutex_lock(&err) == 0); CHECK(pthread_mutex_lock(&err) == EDEADLK);
    CHECK(pthread_mutex_trylock(&err) == EBUSY); CHECK(pthread_mutex_unlock(&err) == 0);
    CHECK(pthread_mutex_unlock(&err) == EPERM); CHECK(pthread_mutex_destroy(&err) == 0);
    CHECK(pthread_mutex_lock(&normal) == 0); CHECK(pthread_mutex_trylock(&normal) == EBUSY);
    deadline = after_ms(CLOCK_REALTIME, 1);
    CHECK(pthread_mutex_timedlock(&normal, &deadline) == ETIMEDOUT);
    CHECK(pthread_mutex_unlock(&normal) == 0);
    CHECK(pthread_mutex_destroy(&normal) == 0); CHECK(pthread_mutex_destroy(&recursive) == 0);
    CHECK(pthread_cond_destroy(&available) == 0); CHECK(pthread_cond_destroy(&consumed) == 0); CHECK(pthread_mutex_destroy(&queue_lock) == 0);
    puts("errorcheck mutex: pass");
    return 0;
}
