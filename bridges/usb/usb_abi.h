#pragma once
/* Linux i386 libusb ABI. Native builds must remain 32-bit; these objects are
 * read and written directly by guest code. The 0.1 descriptor trees have the
 * same layout as 1.0 (only pointer constness differs), so share their types. */
#include <stddef.h>
#include <stdint.h>
#include "usb_backend.h"

_Static_assert(sizeof(void *) == 4, "lxusb requires an i386 build");
typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;
typedef struct libusb_device_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} libusb_device_descriptor;
typedef struct libusb_endpoint_descriptor {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval, bRefresh, bSynchAddress;
    unsigned char *extra;
    int extra_length;
} libusb_endpoint_descriptor;
typedef struct libusb_interface_descriptor {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
    uint8_t bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
    libusb_endpoint_descriptor *endpoint;
    unsigned char *extra;
    int extra_length;
} libusb_interface_descriptor;
typedef struct libusb_interface {
    libusb_interface_descriptor *altsetting;
    int num_altsetting;
} libusb_interface;
typedef struct libusb_config_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
    libusb_interface *interface;
    unsigned char *extra;
    int extra_length;
} libusb_config_descriptor;

enum { LIBUSB_TRANSFER_TYPE_CONTROL, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS,
       LIBUSB_TRANSFER_TYPE_BULK, LIBUSB_TRANSFER_TYPE_INTERRUPT, LIBUSB_TRANSFER_TYPE_BULK_STREAM };
enum { LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_ERROR, LIBUSB_TRANSFER_TIMED_OUT,
       LIBUSB_TRANSFER_CANCELLED, LIBUSB_TRANSFER_STALL, LIBUSB_TRANSFER_NO_DEVICE,
       LIBUSB_TRANSFER_OVERFLOW };
enum { LIBUSB_TRANSFER_SHORT_NOT_OK = 1, LIBUSB_TRANSFER_FREE_BUFFER = 2,
       LIBUSB_TRANSFER_FREE_TRANSFER = 4, LIBUSB_TRANSFER_ADD_ZERO_PACKET = 8 };
typedef struct libusb_iso_packet_descriptor {
    unsigned int length, actual_length;
    int status;
} libusb_iso_packet_descriptor;
typedef struct libusb_transfer {
    libusb_device_handle *dev_handle;
    uint8_t flags, endpoint, type;
    unsigned int timeout;
    int status, length, actual_length;
    void (*callback)(struct libusb_transfer *);
    void *user_data;
    unsigned char *buffer;
    int num_iso_packets;
    libusb_iso_packet_descriptor iso_packet_desc[];
} libusb_transfer;

typedef libusb_device_descriptor usb_device_descriptor;
typedef libusb_endpoint_descriptor usb_endpoint_descriptor;
typedef libusb_interface_descriptor usb_interface_descriptor;
typedef libusb_interface usb_interface;
typedef libusb_config_descriptor usb_config_descriptor;
#define LXUSB_PATH_MAX 4096
typedef struct usb_bus usb_bus;
typedef struct usb_device {
    struct usb_device *next, *prev;
    char filename[LXUSB_PATH_MAX + 1];
    usb_bus *bus;
    usb_device_descriptor descriptor;
    usb_config_descriptor *config;
    void *dev;
    uint8_t devnum, num_children;
    struct usb_device **children;
} usb_device;
struct usb_bus {
    usb_bus *next, *prev;
    char dirname[LXUSB_PATH_MAX + 1];
    usb_device *devices;
    uint32_t location;
    usb_device *root_dev;
};

#define ABI_SIZE(t, n) _Static_assert(sizeof(t) == (n), #t " size")
#define ABI_OFF(t, f, n) _Static_assert(offsetof(t, f) == (n), #t "." #f " offset")
ABI_SIZE(libusb_device_descriptor, 18);
ABI_OFF(libusb_device_descriptor, bDeviceClass, 4);
ABI_OFF(libusb_device_descriptor, idVendor, 8);
ABI_OFF(libusb_device_descriptor, idProduct, 10);
ABI_OFF(libusb_device_descriptor, bcdDevice, 12);
ABI_OFF(libusb_device_descriptor, bNumConfigurations, 17);
ABI_SIZE(libusb_endpoint_descriptor, 20);
ABI_OFF(libusb_endpoint_descriptor, extra, 12);
ABI_OFF(libusb_endpoint_descriptor, bEndpointAddress, 2);
ABI_OFF(libusb_endpoint_descriptor, wMaxPacketSize, 4);
ABI_OFF(libusb_endpoint_descriptor, extra_length, 16);
ABI_SIZE(libusb_interface_descriptor, 24);
ABI_OFF(libusb_interface_descriptor, endpoint, 12);
ABI_OFF(libusb_interface_descriptor, bInterfaceNumber, 2);
ABI_OFF(libusb_interface_descriptor, bInterfaceClass, 5);
ABI_OFF(libusb_interface_descriptor, extra, 16);
ABI_OFF(libusb_interface_descriptor, extra_length, 20);
ABI_SIZE(libusb_interface, 8);
ABI_OFF(libusb_interface, num_altsetting, 4);
ABI_SIZE(libusb_config_descriptor, 24);
ABI_OFF(libusb_config_descriptor, interface, 12);
ABI_OFF(libusb_config_descriptor, bConfigurationValue, 5);
ABI_OFF(libusb_config_descriptor, extra, 16);
ABI_OFF(libusb_config_descriptor, extra_length, 20);
ABI_SIZE(libusb_transfer, 40);
ABI_OFF(libusb_transfer, dev_handle, 0);
ABI_OFF(libusb_transfer, flags, 4);
ABI_OFF(libusb_transfer, endpoint, 5);
ABI_OFF(libusb_transfer, type, 6);
ABI_OFF(libusb_transfer, timeout, 8);
ABI_OFF(libusb_transfer, status, 12);
ABI_OFF(libusb_transfer, length, 16);
ABI_OFF(libusb_transfer, actual_length, 20);
ABI_OFF(libusb_transfer, callback, 24);
ABI_OFF(libusb_transfer, user_data, 28);
ABI_OFF(libusb_transfer, buffer, 32);
ABI_OFF(libusb_transfer, num_iso_packets, 36);
ABI_OFF(libusb_transfer, iso_packet_desc, 40);
ABI_SIZE(libusb_iso_packet_descriptor, 12);
ABI_OFF(libusb_iso_packet_descriptor, length, 0);
ABI_OFF(libusb_iso_packet_descriptor, actual_length, 4);
ABI_OFF(libusb_iso_packet_descriptor, status, 8);
ABI_OFF(usb_bus, devices, 0x100c);
ABI_SIZE(usb_bus, 0x1018);
ABI_OFF(usb_device, descriptor, 0x1010);
ABI_OFF(usb_device, config, 0x1024);
ABI_OFF(usb_device, dev, 0x1028);
ABI_SIZE(usb_device, 0x1034);
#undef ABI_SIZE
#undef ABI_OFF

/* Test seam: production lazily resolves lxrt.dll!lx_guest_symbol. */
extern void *(*lxusb_guest_lookup)(const char *name);
int libusb_init(libusb_context **ctx);
void libusb_exit(libusb_context *ctx);
int libusb_get_device_list(libusb_context *ctx, libusb_device ***list);
void libusb_free_device_list(libusb_device **list, int unref_devices);
int libusb_get_device_descriptor(libusb_device *dev, libusb_device_descriptor *desc);
int libusb_get_config_descriptor(libusb_device *dev, uint8_t index, libusb_config_descriptor **out);
int libusb_get_active_config_descriptor(libusb_device *dev, libusb_config_descriptor **out);
void libusb_free_config_descriptor(libusb_config_descriptor *config);
uint8_t libusb_get_bus_number(libusb_device *dev);
uint8_t libusb_get_device_address(libusb_device *dev);
int libusb_open(libusb_device *dev, libusb_device_handle **out);
void libusb_close(libusb_device_handle *h);
int libusb_claim_interface(libusb_device_handle *h, int iface);
int libusb_release_interface(libusb_device_handle *h, int iface);
int libusb_kernel_driver_active(libusb_device_handle *h, int iface);
int libusb_detach_kernel_driver(libusb_device_handle *h, int iface);
int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t request,
                            uint16_t value, uint16_t index, unsigned char *data,
                            uint16_t length, unsigned int timeout);
int libusb_interrupt_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data,
                              int length, int *transferred, unsigned int timeout);
int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data,
                         int length, int *transferred, unsigned int timeout);
libusb_transfer *libusb_alloc_transfer(int iso_packets);
void libusb_free_transfer(libusb_transfer *t);
int libusb_submit_transfer(libusb_transfer *t);
int libusb_cancel_transfer(libusb_transfer *t);
int libusb_handle_events(libusb_context *ctx);
void usb_init(void);
int usb_find_busses(void);
int usb_find_devices(void);
libusb_device_handle *usb_open(usb_device *dev);
int usb_close(libusb_device_handle *h);
int usb_set_configuration(libusb_device_handle *h, int value);
int usb_claim_interface(libusb_device_handle *h, int iface);
int usb_set_altinterface(libusb_device_handle *h, int alt);
int usb_reset(libusb_device_handle *h);
int usb_control_msg(libusb_device_handle *h, int type, int request, int value, int index,
                    char *bytes, int size, int timeout);
