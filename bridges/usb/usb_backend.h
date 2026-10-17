#pragma once
/* Seam between the libusb API layer (usb.c: libusb-1.0 + libusb-0.1 over guest ABI structs)
 * and the device backend. lxusb.dll links winusb.c (SetupAPI + WinUSB); test_usb.exe links a
 * scripted fake that defines the same `lxusb_backend_impl`. All functions are native MSVC ABI
 * and never called by guest code directly.
 *
 * Errors are negative libusb-1.0 codes (LXUSB_ERROR_*). */
#include <windows.h>
#include <stdint.h>

enum {
    LXUSB_SUCCESS = 0,
    LXUSB_ERROR_IO = -1,
    LXUSB_ERROR_INVALID_PARAM = -2,
    LXUSB_ERROR_ACCESS = -3,
    LXUSB_ERROR_NO_DEVICE = -4,
    LXUSB_ERROR_NOT_FOUND = -5,
    LXUSB_ERROR_BUSY = -6,
    LXUSB_ERROR_TIMEOUT = -7,
    LXUSB_ERROR_OVERFLOW = -8,
    LXUSB_ERROR_PIPE = -9,
    LXUSB_ERROR_INTERRUPTED = -10,
    LXUSB_ERROR_NO_MEM = -11,
    LXUSB_ERROR_NOT_SUPPORTED = -12,
    LXUSB_ERROR_OTHER = -99,
};

/* Win32 error of a failed/completed WinUSB operation -> libusb error. */
static inline int lxusb_error_from_win32(DWORD e)
{
    switch (e) {
    case ERROR_SUCCESS: return LXUSB_SUCCESS;
    case ERROR_SEM_TIMEOUT: case WAIT_TIMEOUT: return LXUSB_ERROR_TIMEOUT;
    case ERROR_GEN_FAILURE: return LXUSB_ERROR_PIPE; /* WinUSB reports a STALL handshake this way */
    case ERROR_DEVICE_NOT_CONNECTED: case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND:
    case ERROR_NO_SUCH_DEVICE: case ERROR_BAD_COMMAND: case ERROR_DEV_NOT_EXIST:
    case ERROR_INVALID_HANDLE:
        return LXUSB_ERROR_NO_DEVICE;
    case ERROR_ACCESS_DENIED: return LXUSB_ERROR_ACCESS;
    case ERROR_BUSY: case ERROR_SHARING_VIOLATION: return LXUSB_ERROR_BUSY;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return LXUSB_ERROR_NO_MEM;
    case ERROR_INVALID_PARAMETER: return LXUSB_ERROR_INVALID_PARAM;
    case ERROR_MORE_DATA: return LXUSB_ERROR_OVERFLOW;
    case ERROR_OPERATION_ABORTED: return LXUSB_ERROR_INTERRUPTED;
    case ERROR_NOT_SUPPORTED: return LXUSB_ERROR_NOT_SUPPORTED;
    default: return LXUSB_ERROR_IO;
    }
}

typedef struct lxusb_bdev lxusb_bdev;       /* physical device; pointer stable for the process lifetime */
typedef struct lxusb_bhandle lxusb_bhandle; /* open device */

typedef struct lxusb_device_info {
    lxusb_bdev *dev;       /* same physical device -> same pointer on every enumeration */
    uint8_t bus, address;  /* stable per device, (bus, address) unique among present devices */
} lxusb_device_info;

typedef struct lxusb_backend {
    /* Present devices usable through this backend, in a stable order. Writes at most `cap`
     * entries; returns the total number present (may exceed cap) or a negative error. */
    int (*enumerate)(lxusb_device_info *out, int cap);

    /* Descriptors are read once per device and cached by the backend, so they remain available
     * while the device is open elsewhere. Device descriptor: exactly 18 raw bytes.
     * Configuration `index`: *raw points at backend-owned, immutable wTotalLength bytes;
     * returns that length or a negative error (NOT_FOUND for an index past bNumConfigurations).
     * The active configuration is always index 0 (WinUSB selects the first configuration). */
    int (*device_descriptor)(lxusb_bdev *dev, uint8_t out[18]);
    int (*config_descriptor)(lxusb_bdev *dev, uint8_t index, const uint8_t **raw);

    /* Open/close. Every overlapped operation on the handle that is not event-flagged (below)
     * posts exactly one completion packet (key, ov) to `iocp`. */
    int (*open)(lxusb_bdev *dev, HANDLE iocp, ULONG_PTR key, lxusb_bhandle **out);
    void (*close)(lxusb_bhandle *h);

    /* Interface management by bInterfaceNumber. claim makes the interface's pipes usable
     * (endpoint -> interface routing follows the current alternate setting). */
    int (*claim)(lxusb_bhandle *h, int iface);
    int (*release)(lxusb_bhandle *h, int iface);
    int (*set_alt)(lxusb_bhandle *h, int iface, int alt);
    /* Best available reset: WinUSB cannot port-reset, so this resets every pipe of every claimed
     * interface (clears halt, flushes data toggles). */
    int (*reset)(lxusb_bhandle *h);

    /* Start overlapped I/O. endpoint == 0: control transfer with 8-byte `setup` (little-endian
     * wire format) and wLength data bytes in `buf` (`len` == wLength); otherwise bulk/interrupt
     * pipe I/O of `len` bytes on the claimed interface owning `endpoint` (direction from
     * bit 7), `setup` NULL.
     * Notification: if ((ULONG_PTR)ov->hEvent & 1) the event (hEvent & ~1) is signaled and NO
     * completion packet is posted; otherwise one packet is posted to the open's iocp. This holds
     * for pending and immediately completed operations alike.
     * Returns 0 once started (a notification will follow), or a negative error when nothing was
     * started (no notification). */
    int (*submit)(lxusb_bhandle *h, unsigned char endpoint, const uint8_t setup[8],
                  uint8_t *buf, ULONG len, OVERLAPPED *ov);
    /* Request cancellation of a started operation; its notification still follows (with
     * ERROR_OPERATION_ABORTED unless it completed first). Safe to race with completion. */
    void (*cancel)(lxusb_bhandle *h, OVERLAPPED *ov);
    /* After notification: bytes transferred and the Win32 completion error (0 on success). */
    void (*result)(lxusb_bhandle *h, OVERLAPPED *ov, ULONG *bytes, DWORD *error);
} lxusb_backend;

extern const lxusb_backend lxusb_backend_impl;
