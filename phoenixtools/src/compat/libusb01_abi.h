#pragma once

#include <stddef.h>
#include <stdint.h>

#define PT_USB01_PATH_MAX 4096
#define PT_LXIO_LEGACY_PID 0x1010u

struct pt_usb01_device_descriptor {
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

struct pt_usb01_interface_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_index;
    void *endpoints;
    unsigned char *extra;
    int extra_length;
};

struct pt_usb01_interface {
    struct pt_usb01_interface_descriptor *alternate_settings;
    int alternate_setting_count;
};

struct pt_usb01_config_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length;
    uint8_t interface_count;
    uint8_t configuration_value;
    uint8_t configuration_index;
    uint8_t attributes;
    uint8_t max_power;
    struct pt_usb01_interface *interfaces;
    unsigned char *extra;
    int extra_length;
};

struct pt_usb01_bus;

struct pt_usb01_device {
    struct pt_usb01_device *next;
    struct pt_usb01_device *previous;
    char filename[PT_USB01_PATH_MAX + 1];
    struct pt_usb01_bus *bus;
    struct pt_usb01_device_descriptor descriptor;
    struct pt_usb01_config_descriptor *config;
    void *device;
    uint8_t device_number;
    uint8_t child_count;
    struct pt_usb01_device **children;
};

struct pt_usb01_bus {
    struct pt_usb01_bus *next;
    struct pt_usb01_bus *previous;
    char directory[PT_USB01_PATH_MAX + 1];
    struct pt_usb01_device *devices;
    uint32_t location;
    void *root_device;
};

_Static_assert(offsetof(struct pt_usb01_bus, devices) == 0x100c,
               "libusb-0.1 bus ABI changed");
_Static_assert(offsetof(struct pt_usb01_device, descriptor) == 0x1010,
               "libusb-0.1 device ABI changed");
_Static_assert(offsetof(struct pt_usb01_device, config) == 0x1024,
               "libusb-0.1 config ABI changed");

