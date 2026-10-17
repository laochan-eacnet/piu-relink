#define _GNU_SOURCE

#include "core/runtime.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

void hook_glx_swap_buffers(void *display, unsigned long drawable)
{
    static int initialized;
    static struct timespec deadline;
    const int64_t interval = 1000000000LL / 60;
    struct timespec now;
    int64_t deadline_ns;
    int64_t now_ns;

    if (!pt_real_glx_swap_buffers)
        pt_real_glx_swap_buffers = dlsym(RTLD_NEXT, "glXSwapBuffers");
    if (pt_real_glx_swap_buffers)
        pt_real_glx_swap_buffers(display, drawable);
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return;

    now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    if (!initialized) {
        deadline_ns = now_ns + interval;
        initialized = 1;
    } else {
        deadline_ns = (int64_t)deadline.tv_sec * 1000000000LL +
                      deadline.tv_nsec + interval;
        if (deadline_ns < now_ns - 250000000LL)
            deadline_ns = now_ns + interval;
        while (deadline_ns <= now_ns)
            deadline_ns += interval;
    }
    deadline.tv_sec = (time_t)(deadline_ns / 1000000000LL);
    deadline.tv_nsec = (long)(deadline_ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                           &deadline, NULL) == EINTR) {
    }
}
