#include "compat/virtual_lxio_bridge.h"
#include "compat/libusb1_abi.h"
#include "input/lxio.h"

#include <string.h>

static int fake_device;
static int virtual_handle;

int pt_virtual_lxio_enabled(void)
{
    return phoenixtools_virtual_lxio_enabled();
}

int pt_virtual_lxio_read(unsigned char *data, int length)
{
    int result;

    if (!data || length <= 0)
        return 0;
    memset(data, 0xff, (size_t)length);
    result = phoenixtools_lxio_read(data, length);
    return result < 0 ? 0 : result;
}

int pt_virtual_lxio_write(const unsigned char *data, int length)
{
    return phoenixtools_lxio_write(data, length);
}

void *pt_virtual_lxio_device(void)
{
    return &fake_device;
}

void *pt_virtual_lxio_handle(void)
{
    return &virtual_handle;
}
