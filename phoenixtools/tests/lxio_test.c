#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct device_descriptor {
    uint8_t length, type;
    uint16_t usb;
    uint8_t device_class, subclass, protocol, packet;
    uint16_t vendor, product, release;
    uint8_t manufacturer, product_string, serial, configurations;
};

struct transfer {
    void *handle;
    uint8_t flags, endpoint, type, pad;
    unsigned int timeout;
    int status, length, actual_length;
    void (*callback)(void *);
    void *user_data;
    unsigned char *buffer;
    int iso_packets;
};

static int callback_count;

static void completed(void *opaque)
{
    struct transfer *transfer = opaque;
    if (transfer->status == 0 && transfer->actual_length == 16)
        ++callback_count;
}

static void *required(void *module, const char *name)
{
    void *symbol = dlsym(module, name);
    if (!symbol) {
        fprintf(stderr, "missing %s: %s\n", name, dlerror());
        exit(2);
    }
    return symbol;
}

int main(int argc, char **argv)
{
    void *hook;
    void **list = NULL;
    void *handle = NULL;
    struct device_descriptor descriptor;
    struct transfer *transfer;
    unsigned char report[16];
    int transferred = 0;
    long (*get_list)(void *, void ***);
    int (*get_descriptor)(void *, void *);
    int (*open_device)(void *, void **);
    int (*interrupt_transfer)(void *, unsigned char, unsigned char *, int,
                              int *, unsigned int);
    void *(*alloc_transfer)(int);
    int (*submit_transfer)(void *);
    int (*handle_events)(void *);
    void (*free_transfer)(void *);
    void (*set_action)(int, int);
    int (*lxio_read)(unsigned char *, int);
    int (*usb_find_busses_fn)(void);
    void (*usb_init_fn)(void);
    void **usb_busses_pointer;
    unsigned char *legacy_device;

    if (argc != 2) {
        fprintf(stderr, "usage: %s phoenixtools.so\n", argv[0]);
        return 1;
    }
    hook = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!hook) {
        fprintf(stderr, "hook: %s\n", dlerror());
        return 2;
    }
    ((void (*)(void))required(hook,
        "phoenixtools_virtual_lxio_test_enable"))();
    get_list = required(hook, "libusb_get_device_list");
    get_descriptor = required(hook, "libusb_get_device_descriptor");
    open_device = required(hook, "libusb_open");
    interrupt_transfer = required(hook, "libusb_interrupt_transfer");
    alloc_transfer = required(hook, "libusb_alloc_transfer");
    submit_transfer = required(hook, "libusb_submit_transfer");
    handle_events = required(hook, "libusb_handle_events");
    free_transfer = required(hook, "libusb_free_transfer");
    set_action = required(hook, "pt_lxio_set_action");
    lxio_read = required(hook, "phoenixtools_lxio_read");

    memset(&descriptor, 0, sizeof(descriptor));
    if (get_list(NULL, &list) != 1 || !list || !list[0] || list[1] ||
        get_descriptor(list[0], &descriptor) != 0 ||
        descriptor.vendor != 0x0d2f || descriptor.product != 0x1020 ||
        open_device(list[0], &handle) != 0 || !handle)
        return 3;
    if (interrupt_transfer(handle, 0x81, report, sizeof(report), &transferred,
                           1000) != 0 || transferred != 16)
        return 4;
    for (transferred = 0; transferred < 16; ++transferred)
        if (report[transferred] != 0xff)
            return 5;

    set_action(0, 1);
    set_action(5, 1);
    set_action(12, 1);
    lxio_read(report, sizeof(report));
    if ((report[0] & 1u) || (report[3] & 1u) ||
        (report[4] & 1u) || (report[7] & 1u) || (report[8] & 4u))
        return 6;

    transfer = alloc_transfer(0);
    if (!transfer)
        return 7;
    transfer->handle = handle;
    transfer->endpoint = 0x81;
    transfer->type = 3;
    transfer->length = 16;
    transfer->buffer = report;
    transfer->callback = completed;
    if (submit_transfer(transfer) != 0 || handle_events(NULL) != 0 ||
        callback_count != 1)
        return 8;
    free_transfer(transfer);

    usb_init_fn = required(hook, "usb_init");
    usb_find_busses_fn = required(hook, "usb_find_busses");
    usb_busses_pointer = required(hook, "usb_busses");
    usb_init_fn();
    if (usb_find_busses_fn() != 1 || !*usb_busses_pointer)
        return 9;
    legacy_device = *(unsigned char **)((unsigned char *)*usb_busses_pointer +
                                        0x100c);
    if (!legacy_device || *(uint16_t *)(legacy_device + 0x1018) != 0x0d2f ||
        *(uint16_t *)(legacy_device + 0x101a) != 0x1010)
        return 10;

    printf("virtual-lxio=0d2f:1010,0d2f:1020 p1+p2+coin async=complete\n");
    dlclose(hook);
    return 0;
}
