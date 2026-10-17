/* M2 acceptance: threads + mutex/cond + __thread in a .so + qsort callback + preload/RTLD_NEXT
 * + dladdr + COPY relocation + dl_iterate_phdr. Output is deterministic. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tls_incr(void);
int tls_get(void);
int *tls_public_addr(void);
int table_sum(void);
extern int shared_table[4];

#define NTHREADS 4
#define ITERS 1000
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int finished, total;

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    for (int i = 0; i < ITERS; i++)
        tls_incr();
    int mine = tls_get();
    int *pub = tls_public_addr();
    *pub += id;                      /* each thread has its own copy of the initialized image */
    pthread_mutex_lock(&lock);
    total += mine;
    finished++;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    return (void *)(long)(mine == ITERS && *pub == 7 + id);
}

static int cmp_desc(const void *a, const void *b)
{
    return *(const int *)b - *(const int *)a;
}

static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    int *n = data;
    (void)size;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_GNU_EH_FRAME || info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            (*n)++;
            break;
        }
    return 0;
}

int main(void)
{
    pthread_t t[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        pthread_create(&t[i], NULL, worker, (void *)(long)(i + 1));
    pthread_mutex_lock(&lock);
    while (finished < NTHREADS)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    int ok = 1;
    for (int i = 0; i < NTHREADS; i++) {
        void *r;
        pthread_join(t[i], &r);
        ok &= (int)(long)r;
    }
    printf("threads: total=%d per-thread-tls=%s main-tls=%d\n", total, ok ? "ok" : "BAD", tls_get());

    int v[] = { 5, 3, 9, 1, 7 };
    qsort(v, 5, sizeof v[0], cmp_desc);
    printf("qsort: %d %d %d %d %d\n", v[0], v[1], v[2], v[3], v[4]);

    shared_table[0] = 100;           /* exe owns the COPY; the .so must see this write */
    printf("copy: sum=%d\n", table_sum());

    FILE *f = fopen("/tmp/lxrt_binder_test.txt", "w");
    if (f) {
        fputs("x\n", f);
        fclose(f);
    }
    /* the preload module is not linked into the program: reach it through the global scope */
    int *hook_calls = dlsym(RTLD_DEFAULT, "hook_calls");
    const char *(*hook_module)(void) = (const char *(*)(void))dlsym(RTLD_DEFAULT, "hook_module");
    printf("preload: fopen hooked %d time(s), hook module %s\n", hook_calls ? *hook_calls : -1,
           hook_module ? hook_module() : "(none)");

    Dl_info di;
    if (dladdr((void *)&main, &di))
        printf("dladdr(main): sym=%s\n", di.dli_sname ? di.dli_sname : "(none)");
    void *self_sym = dlsym(RTLD_DEFAULT, "table_sum");
    printf("dlsym(RTLD_DEFAULT, table_sum) %s\n", self_sym == (void *)table_sum ? "matches" : "MISMATCH");

    int with_dyn = 0;
    dl_iterate_phdr(phdr_cb, &with_dyn);
    printf("dl_iterate_phdr: %s\n", with_dyn >= 2 ? "ok" : "BAD");
    return ok ? 0 : 1;
}
