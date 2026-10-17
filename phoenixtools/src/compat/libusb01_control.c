#include "compat/libusb01_device.h"
#include "compat/usb_forward.h"

#include <string.h>

int usb_control_msg(void *handle, int request_type, int request, int value,
                    int index, char *bytes, int size, int timeout)
{
    PT_USB_RETURN_REAL(int (*)(void *, int, int, int, int, char *, int, int),
                       "usb_control_msg", -1,
                       (handle, request_type, request, value, index, bytes,
                        size, timeout));
    (void)value;
    (void)index;
    (void)timeout;
    if (handle != pt_usb01_virtual_handle() || !bytes || size < 0)
        return -1;

    /* Phoenix opens both legacy 1010 and modern 1020 devices. The 1020 report
     * is the sole synthetic input source; 1010 stays neutral to avoid duplicate
     * panel events through its different decoder layout. */
    if (request_type == 0xc0 && request == 0xae) {
        memset(bytes, 0xff, (size_t)size);
        return size;
    }
    if (request_type == 0x40 && request == 0xae)
        return size;
    return -1;
}
