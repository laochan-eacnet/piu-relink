#include "compat/libusb1_abi.h"
#include "compat/usb_forward.h"
#include "compat/virtual_lxio_bridge.h"

#include <stdint.h>
#include <string.h>

static void *device_list[2];
static void *empty_device_list[1];

static const struct pt_libusb_endpoint_descriptor endpoints[2] = {
    {7, 5, 0x81, 3, PT_LXIO_REPORT_SIZE, 1, 0, 0, NULL, 0},
    {7, 5, 0x02, 3, PT_LXIO_REPORT_SIZE, 1, 0, 0, NULL, 0}
};

static const struct pt_libusb_interface_descriptor interface_descriptor = {
    9, 4, 0, 0, 2, 3, 0, 0, 0, endpoints, NULL, 0
};

static const struct pt_libusb_interface interface = {
    &interface_descriptor, 1
};

static const struct pt_libusb_config_descriptor config_descriptor = {
    9, 2, 9, 1, 1, 0, 0x80, 50, &interface, NULL, 0
};

int libusb_init(void **context)
{
    PT_USB_RETURN_REAL(int (*)(void **), "libusb_init", -12, (context));
    if (context)
        *context = NULL;
    return 0;
}

void libusb_exit(void *context)
{
    PT_USB_CALL_REAL(void (*)(void *), "libusb_exit", (context));
    (void)context;
}

long libusb_get_device_list(void *context, void ***list)
{
    PT_USB_RETURN_REAL(long (*)(void *, void ***), "libusb_get_device_list",
                       -12, (context, list));
    (void)context;
    if (!pt_virtual_lxio_enabled()) {
        if (list)
            *list = empty_device_list;
        return 0;
    }
    device_list[0] = pt_virtual_lxio_device();
    device_list[1] = NULL;
    if (list)
        *list = device_list;
    return 1;
}

void libusb_free_device_list(void **list, int unref_devices)
{
    PT_USB_CALL_REAL(void (*)(void **, int), "libusb_free_device_list",
                     (list, unref_devices));
    (void)list;
    (void)unref_devices;
}

int libusb_get_device_descriptor(void *device, void *descriptor)
{
    const struct pt_libusb_device_descriptor value = {
        18, 1, 0x0110, 0, 0, 0, 64, PT_LXIO_VID, PT_LXIO_PID, 1, 0, 0, 0, 1
    };
    PT_USB_RETURN_REAL(int (*)(void *, void *), "libusb_get_device_descriptor",
                       PT_LIBUSB_ERROR_NO_DEVICE, (device, descriptor));
    if (device != pt_virtual_lxio_device() || !descriptor)
        return PT_LIBUSB_ERROR_NO_DEVICE;
    memcpy(descriptor, &value, sizeof(value));
    return 0;
}

int libusb_get_config_descriptor(void *device, uint8_t index, void **config)
{
    PT_USB_RETURN_REAL(int (*)(void *, uint8_t, void **),
                       "libusb_get_config_descriptor",
                       PT_LIBUSB_ERROR_NO_DEVICE, (device, index, config));
    if (device != pt_virtual_lxio_device() || index != 0 || !config)
        return PT_LIBUSB_ERROR_NO_DEVICE;
    *config = (void *)&config_descriptor;
    return 0;
}

int libusb_get_active_config_descriptor(void *device, void **config)
{
    PT_USB_RETURN_REAL(int (*)(void *, void **),
                       "libusb_get_active_config_descriptor",
                       PT_LIBUSB_ERROR_NO_DEVICE, (device, config));
    return libusb_get_config_descriptor(device, 0, config);
}

void libusb_free_config_descriptor(void *config)
{
    PT_USB_CALL_REAL(void (*)(void *), "libusb_free_config_descriptor", (config));
    (void)config;
}

uint8_t libusb_get_bus_number(void *device)
{
    PT_USB_RETURN_REAL(uint8_t (*)(void *), "libusb_get_bus_number", 0,
                       (device));
    return device == pt_virtual_lxio_device() ? 1 : 0;
}

uint8_t libusb_get_device_address(void *device)
{
    PT_USB_RETURN_REAL(uint8_t (*)(void *), "libusb_get_device_address", 0,
                       (device));
    return device == pt_virtual_lxio_device() ? 1 : 0;
}

int libusb_open(void *device, void **handle)
{
    PT_USB_RETURN_REAL(int (*)(void *, void **), "libusb_open",
                       PT_LIBUSB_ERROR_NO_DEVICE, (device, handle));
    if (device != pt_virtual_lxio_device() || !handle)
        return PT_LIBUSB_ERROR_NO_DEVICE;
    *handle = pt_virtual_lxio_handle();
    return 0;
}

void libusb_close(void *handle)
{
    PT_USB_CALL_REAL(void (*)(void *), "libusb_close", (handle));
    (void)handle;
}

int libusb_claim_interface(void *handle, int interface_number)
{
    PT_USB_RETURN_REAL(int (*)(void *, int), "libusb_claim_interface",
                       PT_LIBUSB_ERROR_NO_DEVICE,
                       (handle, interface_number));
    return handle == pt_virtual_lxio_handle() && interface_number == 0 ?
           0 : PT_LIBUSB_ERROR_NO_DEVICE;
}

int libusb_release_interface(void *handle, int interface_number)
{
    PT_USB_RETURN_REAL(int (*)(void *, int), "libusb_release_interface", -12,
                       (handle, interface_number));
    (void)handle;
    (void)interface_number;
    return 0;
}

int libusb_kernel_driver_active(void *handle, int interface_number)
{
    PT_USB_RETURN_REAL(int (*)(void *, int), "libusb_kernel_driver_active", 0,
                       (handle, interface_number));
    (void)handle;
    (void)interface_number;
    return 0;
}

int libusb_detach_kernel_driver(void *handle, int interface_number)
{
    PT_USB_RETURN_REAL(int (*)(void *, int), "libusb_detach_kernel_driver", -12,
                       (handle, interface_number));
    (void)handle;
    (void)interface_number;
    return 0;
}
