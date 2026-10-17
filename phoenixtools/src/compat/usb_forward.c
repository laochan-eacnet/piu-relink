#define _GNU_SOURCE

#include "compat/usb_forward.h"

#include "config/config.h"

#include <dlfcn.h>

void *pt_usb_next_symbol(const char *name)
{
    return dlsym(RTLD_NEXT, name);
}

int pt_virtual_usb_mode(void)
{
    return pt_config_get()->virtual_lxio;
}
