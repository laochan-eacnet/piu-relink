#include "compat/libusb1_abi.h"
#include "compat/usb_forward.h"
#include "compat/virtual_lxio_bridge.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static struct pt_libusb_transfer *pending_transfer;
static int cancel_pending;

int libusb_control_transfer(void *handle, uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index,
                            unsigned char *data, uint16_t length,
                            unsigned int timeout)
{
    PT_USB_RETURN_REAL(int (*)(void *, uint8_t, uint8_t, uint16_t, uint16_t,
                              unsigned char *, uint16_t, unsigned int),
                       "libusb_control_transfer", PT_LIBUSB_ERROR_NO_DEVICE,
                       (handle, request_type, request, value, index, data,
                        length, timeout));
    (void)request;
    (void)value;
    (void)index;
    (void)timeout;
    if (handle != pt_virtual_lxio_handle())
        return PT_LIBUSB_ERROR_NO_DEVICE;
    return request_type & 0x80u ? pt_virtual_lxio_read(data, length)
                                : pt_virtual_lxio_write(data, length);
}

int libusb_interrupt_transfer(void *handle, unsigned char endpoint,
                              unsigned char *data, int length,
                              int *transferred, unsigned int timeout)
{
    int result;

    PT_USB_RETURN_REAL(int (*)(void *, unsigned char, unsigned char *, int,
                              int *, unsigned int),
                       "libusb_interrupt_transfer", PT_LIBUSB_ERROR_NO_DEVICE,
                       (handle, endpoint, data, length, transferred, timeout));
    (void)timeout;
    if (handle != pt_virtual_lxio_handle())
        return PT_LIBUSB_ERROR_NO_DEVICE;
    result = endpoint & 0x80u ? pt_virtual_lxio_read(data, length)
                              : pt_virtual_lxio_write(data, length);
    if (transferred)
        *transferred = result;
    usleep(1000);
    return 0;
}

void *libusb_alloc_transfer(int iso_packets)
{
    struct pt_libusb_transfer *transfer;

    PT_USB_RETURN_REAL(void *(*)(int), "libusb_alloc_transfer", NULL,
                       (iso_packets));
    (void)iso_packets;
    transfer = calloc(1, sizeof(*transfer));
    return transfer;
}

void libusb_free_transfer(void *opaque_transfer)
{
    PT_USB_CALL_REAL(void (*)(void *), "libusb_free_transfer",
                     (opaque_transfer));
    if (pending_transfer == opaque_transfer)
        pending_transfer = NULL;
    free(opaque_transfer);
}

int libusb_submit_transfer(void *opaque_transfer)
{
    struct pt_libusb_transfer *transfer = opaque_transfer;

    PT_USB_RETURN_REAL(int (*)(void *), "libusb_submit_transfer", -12,
                       (opaque_transfer));
    if (!transfer || transfer->device_handle != pt_virtual_lxio_handle())
        return PT_LIBUSB_ERROR_NO_DEVICE;
    pending_transfer = transfer;
    cancel_pending = 0;
    return 0;
}

int libusb_cancel_transfer(void *opaque_transfer)
{
    PT_USB_RETURN_REAL(int (*)(void *), "libusb_cancel_transfer", -12,
                       (opaque_transfer));
    if (opaque_transfer == pending_transfer)
        cancel_pending = 1;
    return 0;
}

int libusb_handle_events(void *context)
{
    struct pt_libusb_transfer *transfer;
    pt_transfer_callback callback;

    PT_USB_RETURN_REAL(int (*)(void *), "libusb_handle_events", -12,
                       (context));
    (void)context;
    usleep(1000);
    transfer = pending_transfer;
    if (!transfer)
        return 0;
    pending_transfer = NULL;
    transfer->actual_length = 0;
    if (cancel_pending) {
        cancel_pending = 0;
        transfer->status = PT_LIBUSB_TRANSFER_CANCELLED;
    } else {
        transfer->actual_length = pt_virtual_lxio_read(transfer->buffer,
                                                       transfer->length);
        transfer->status = PT_LIBUSB_TRANSFER_COMPLETED;
    }
    callback = transfer->callback;
    if (callback)
        callback(transfer);
    return 0;
}
