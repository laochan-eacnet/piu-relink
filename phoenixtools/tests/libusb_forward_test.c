#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int fixture_anchor(void);

static void *required(const char *name)
{
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol) {
        fprintf(stderr, "missing %s: %s\n", name, dlerror());
        exit(2);
    }
    return symbol;
}

int main(void)
{
    void *context = NULL;
    void **list = NULL;
    int (*libusb_init_fn)(void **);
    long (*get_list_fn)(void *, void ***);
    int (*find_busses_fn)(void);
    int init_result;
    long list_result;
    int busses_result;

    if (fixture_anchor() != 99)
        return 1;
    libusb_init_fn = required("libusb_init");
    get_list_fn = required("libusb_get_device_list");
    find_busses_fn = required("usb_find_busses");
    init_result = libusb_init_fn(&context);
    list_result = get_list_fn(context, &list);
    busses_result = find_busses_fn();
    if (init_result != 42 || context != (void *)0x1234 ||
        list_result != 43 || !list || busses_result != 24) {
        fprintf(stderr,
                "forward mismatch init=%d context=%p list=%ld/%p busses=%d\n",
                init_result, context, list_result, (void *)list, busses_result);
        return 5;
    }
    puts("hardware-mode=RTLD_NEXT libusb-1.0+0.1 forwarded");
    return 0;
}
