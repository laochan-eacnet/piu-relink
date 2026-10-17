#include <stddef.h>

int fixture_anchor(void)
{
    return 99;
}

int libusb_init(void **context)
{
    if (context)
        *context = (void *)0x1234;
    return 42;
}

long libusb_get_device_list(void *context, void ***list)
{
    static void *devices[1];
    (void)context;
    if (list)
        *list = devices;
    return 43;
}

void usb_init(void)
{
}

int usb_find_busses(void)
{
    return 24;
}
