#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
extern "C" int touch_tls();
extern "C" int tls_destroyed();
static void *worker(void *arg)
{
    if (touch_tls() != 42 || touch_tls() != 42) abort();
    if (arg) pthread_exit(arg);
    return arg;
}
int main()
{
    pthread_t threads[8];
    for (int i = 0; i < 8; ++i) if (pthread_create(&threads[i], NULL, worker, (void *)(i & 1))) return 1;
    for (int i = 0; i < 8; ++i) if (pthread_join(threads[i], NULL)) return 1;
    if (tls_destroyed() != 8) { puts("C++ thread_local destructors: FAIL"); return 1; }
    puts("C++ thread_local destructors: pass");
    return 0;
}
