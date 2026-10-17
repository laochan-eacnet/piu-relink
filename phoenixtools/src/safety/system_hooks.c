#include "safety/safety.h"

#include "config/config.h"
#include "core/log.h"

#include <errno.h>
#include <string.h>

__attribute__((naked, noinline))
void hook_update_state(void)
{
    __asm__ volatile("mov $0x0806600A, %eax\n\tjmp *%eax\n\t");
}

__attribute__((naked, noinline))
void hook_update_entry(void)
{
    __asm__ volatile("xor %eax, %eax\n\tret\n\t");
}

__attribute__((naked, noinline))
void hook_disk_id_entry(void)
{
    __asm__ volatile("xor %eax, %eax\n\tret\n\t");
}

int hook_lock_response(void *context, unsigned char *response)
{
    const struct pt_config *config = pt_config_get();

    (void)context;
    if (!response)
        return -1;
    memset(response, 0, 32);
    if (config->hold_lock_screen)
        return -1;
    response[0] = 4;
    response[1] = (unsigned char)config->region_slot;
    if (config->verbose)
        pt_log("lock", "supplied non-secret region %s",
               pt_region_name(config->region_slot));
    return 0;
}

__attribute__((naked, noinline))
void hook_port_io_probe(void)
{
    __asm__ volatile("xor %eax, %eax\n\tret\n\t");
}

__attribute__((naked, noinline))
void hook_mixer_helper(void)
{
    __asm__ volatile("xor %eax, %eax\n\tret\n\t");
}

int ioperm(unsigned long from, unsigned long count, int turn_on)
{
    (void)from;
    (void)count;
    (void)turn_on;
    errno = EPERM;
    return -1;
}

int XSetNormalHints(void *display, unsigned long window, void *hints)
{
    (void)display;
    (void)window;
    (void)hints;
    return 1;
}
