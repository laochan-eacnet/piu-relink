#include "compat/libusb01_abi.h"
#include "compat/libusb1_abi.h"
#include "compat/usb_forward.h"

#include <string.h>

static struct pt_usb01_interface_descriptor alternate_setting = {
    9, 4, 0, 0, 0, 0xff, 0, 0, 0, NULL, NULL, 0
};
static struct pt_usb01_interface interface = {&alternate_setting, 1};
static struct pt_usb01_config_descriptor configuration = {
    9, 2, 9, 1, 1, 0, 0x80, 50, &interface, NULL, 0
};
static struct pt_usb01_bus virtual_bus;
static struct pt_usb01_device virtual_device;
static int virtual_handle;

struct pt_usb01_bus *usb_busses;

static void initialize_virtual_device(void)
{
    memset(&virtual_bus, 0, sizeof(virtual_bus));
    memset(&virtual_device, 0, sizeof(virtual_device));
    strcpy(virtual_bus.directory, "phoenixtools");
    virtual_bus.devices = &virtual_device;
    strcpy(virtual_device.filename, "001");
    virtual_device.bus = &virtual_bus;
    virtual_device.descriptor.length = 18;
    virtual_device.descriptor.descriptor_type = 1;
    virtual_device.descriptor.usb_version = 0x0110;
    virtual_device.descriptor.max_packet_size = 8;
    virtual_device.descriptor.vendor_id = PT_LXIO_VID;
    virtual_device.descriptor.product_id = PT_LXIO_LEGACY_PID;
    virtual_device.descriptor.device_version = 1;
    virtual_device.descriptor.configuration_count = 1;
    virtual_device.config = &configuration;
}

void usb_init(void)
{
    PT_USB_CALL_REAL(void (*)(void), "usb_init", ());
    initialize_virtual_device();
    usb_busses = &virtual_bus;
}

int usb_find_busses(void)
{
    PT_USB_RETURN_REAL(int (*)(void), "usb_find_busses", -1, ());
    usb_busses = &virtual_bus;
    return 1;
}

int usb_find_devices(void)
{
    PT_USB_RETURN_REAL(int (*)(void), "usb_find_devices", -1, ());
    usb_busses = &virtual_bus;
    return 1;
}

void *usb_open(void *device)
{
    PT_USB_RETURN_REAL(void *(*)(void *), "usb_open", NULL, (device));
    return device == &virtual_device ? &virtual_handle : NULL;
}

int usb_close(void *handle)
{
    PT_USB_RETURN_REAL(int (*)(void *), "usb_close", -1, (handle));
    return handle == &virtual_handle ? 0 : -1;
}

int usb_claim_interface(void *handle, int interface_number)
{
    PT_USB_RETURN_REAL(int (*)(void *, int), "usb_claim_interface", -1,
                       (handle, interface_number));
    return handle == &virtual_handle && interface_number == 0 ? 0 : -1;
}

int usb_set_configuration(void *handle, int selected_configuration)
{
    PT_USB_RETURN_REAL(int (*)(void *, int), "usb_set_configuration", -1,
                       (handle, selected_configuration));
    return handle == &virtual_handle && selected_configuration == 1 ? 0 : -1;
}

int usb_set_altinterface(void *handle, int alternate)
{
    PT_USB_RETURN_REAL(int (*)(void *, int), "usb_set_altinterface", -1,
                       (handle, alternate));
    return handle == &virtual_handle && alternate == 0 ? 0 : -1;
}

int usb_reset(void *handle)
{
    PT_USB_RETURN_REAL(int (*)(void *), "usb_reset", -1, (handle));
    return handle == &virtual_handle ? 0 : -1;
}

void *pt_usb01_virtual_handle(void)
{
    return &virtual_handle;
}
