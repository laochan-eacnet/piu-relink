/* M2: LD_PRELOAD-style interposer: fopen forwards via dlsym(RTLD_NEXT). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int hook_calls;

FILE *fopen(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    if (!real)
        real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    hook_calls++;
    return real ? real(path, mode) : NULL;
}

const char *hook_module(void)
{
    Dl_info info;
    if (!dladdr((void *)&hook_module, &info) || !info.dli_fname)
        return "?";
    const char *s = strrchr(info.dli_fname, '/');
    return s ? s + 1 : info.dli_fname;
}

__attribute__((constructor)) static void hook_init(void)
{
    fprintf(stderr, "preload constructor ran\n");
}
