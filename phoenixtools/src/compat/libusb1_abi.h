#pragma once

#include <stdint.h>

#define PT_LXIO_REPORT_SIZE 16
#define PT_LXIO_VID 0x0d2fu
#define PT_LXIO_PID 0x1020u
#define PT_LIBUSB_TRANSFER_COMPLETED 0
#define PT_LIBUSB_TRANSFER_CANCELLED 3
#define PT_LIBUSB_ERROR_NO_DEVICE -4

typedef void (*pt_transfer_callback)(void *);

struct pt_libusb_device_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t manufacturer_index;
    uint8_t product_index;
    uint8_t serial_index;
    uint8_t configuration_count;
};

struct pt_libusb_endpoint_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t endpoint_address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t refresh;
    uint8_t sync_address;
    const unsigned char *extra;
    int extra_length;
};

struct pt_libusb_interface_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_index;
    const struct pt_libusb_endpoint_descriptor *endpoints;
    const unsigned char *extra;
    int extra_length;
};

struct pt_libusb_interface {
    const struct pt_libusb_interface_descriptor *alternate_settings;
    int alternate_setting_count;
};

struct pt_libusb_config_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length;
    uint8_t interface_count;
    uint8_t configuration_value;
    uint8_t configuration_index;
    uint8_t attributes;
    uint8_t max_power;
    const struct pt_libusb_interface *interfaces;
    const unsigned char *extra;
    int extra_length;
};

struct pt_libusb_transfer {
    void *device_handle;
    uint8_t flags;
    unsigned char endpoint;
    unsigned char type;
    unsigned int timeout;
    int status;
    int length;
    int actual_length;
    pt_transfer_callback callback;
    void *user_data;
    unsigned char *buffer;
    int iso_packet_count;
};

_Static_assert(sizeof(struct pt_libusb_endpoint_descriptor) == 20,
               "libusb endpoint ABI changed");
_Static_assert(sizeof(struct pt_libusb_interface_descriptor) == 24,
               "libusb interface ABI changed");
_Static_assert(sizeof(struct pt_libusb_config_descriptor) == 24,
               "libusb config ABI changed");
_Static_assert(sizeof(struct pt_libusb_transfer) == 40,
               "libusb transfer ABI changed");

