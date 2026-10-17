#pragma once

void *pt_usb_next_symbol(const char *name);
int pt_virtual_usb_mode(void);

#define PT_USB_RETURN_REAL(type, symbol, fallback, parameters) \
    do { \
        if (!pt_virtual_usb_mode()) { \
            static __typeof__((type)0) real_function; \
            if (!real_function) \
                real_function = (type)pt_usb_next_symbol(symbol); \
            return real_function ? real_function parameters : (fallback); \
        } \
    } while (0)

#define PT_USB_CALL_REAL(type, symbol, parameters) \
    do { \
        if (!pt_virtual_usb_mode()) { \
            static __typeof__((type)0) real_function; \
            if (!real_function) \
                real_function = (type)pt_usb_next_symbol(symbol); \
            if (real_function) \
                real_function parameters; \
            return; \
        } \
    } while (0)

