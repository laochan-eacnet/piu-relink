/* Native WinUSB backend. Guest ABI and libusb ownership live in usb.c.
 * Registry records and descriptor snapshots deliberately outlive disconnection: guest
 * libusb_device objects can retain their backend pointer after a later enumeration. */
#include "usb_backend.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define IFACE_COUNT 256

static const GUID usb_device_guid = {
    0xa5dcbf10, 0x6530, 0x11d2, {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}
};

typedef struct usb_function {
    struct usb_function *next;
    WCHAR *instance, *path;
    int first_iface; /* MI_xx, or -1 for a non-composite device */
    int present;
} usb_function;

typedef struct open_function {
    struct open_function *next;
    usb_function *source;
    HANDLE file;
    WINUSB_INTERFACE_HANDLE primary;
} open_function;

typedef struct usb_interface {
    WINUSB_INTERFACE_HANDLE winusb;
    open_function *function;
    UCHAR alt;
    int claimed;
} usb_interface;

typedef struct pending_io {
    struct pending_io *next;
    OVERLAPPED *ov;
    open_function *function;
} pending_io;

struct lxusb_bdev {
    struct lxusb_bdev *next;
    WCHAR *instance; /* physical USB parent instance for MI_xx functions */
    usb_function *functions;
    lxusb_bhandle *opens;
    uint8_t bus, address;
    int present, descriptors_ready;
    uint8_t descriptor[18];
    uint8_t **configs;
};

struct lxusb_bhandle {
    lxusb_bhandle *next;
    lxusb_bdev *dev;
    HANDLE iocp;
    ULONG_PTR key;
    open_function *functions, *control_function;
    usb_interface interfaces[IFACE_COUNT];
    usb_interface *endpoints[IFACE_COUNT];
    pending_io *pending, *spare;
};

typedef struct diagnosed_device {
    struct diagnosed_device *next;
    WCHAR *instance;
} diagnosed_device;

/* One lock keeps registry/path replacement, lazy function opens, endpoint routes and
 * OVERLAPPED ownership atomic. Never wait for I/O completion while holding it. */
static SRWLOCK backend_lock = SRWLOCK_INIT;
static lxusb_bdev *devices;
static diagnosed_device *diagnosed;

static unsigned get16(const uint8_t *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}

static int hex_digit(WCHAR c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

static const WCHAR *find_token(const WCHAR *s, const WCHAR *token)
{
    size_t n = wcslen(token);
    for (; *s; ++s)
        if (_wcsnicmp(s, token, n) == 0) return s + n;
    return NULL;
}

static int parse_hex(const WCHAR **text, unsigned count, unsigned *value)
{
    unsigned v = 0;
    for (unsigned i = 0; i < count; ++i) {
        int d = hex_digit(**text);
        if (d < 0) return 0;
        v = (v << 4) | (unsigned)d;
        ++*text;
    }
    *value = v;
    return 1;
}

static int usb_id(const WCHAR *id, unsigned *vid, unsigned *pid, int *iface)
{
    const WCHAR *v = find_token(id, L"VID_"), *p = find_token(id, L"PID_");
    const WCHAR *m = find_token(id, L"&MI_");
    unsigned mi;
    if (!v || !p || !parse_hex(&v, 4, vid) || !parse_hex(&p, 4, pid)) return 0;
    *iface = m && parse_hex(&m, 2, &mi) ? (int)mi : -1;
    return 1;
}

static int parse_guid(const WCHAR *s, GUID *guid)
{
    unsigned v;
    int braces = *s == L'{';
    if (braces) ++s;
    if (!parse_hex(&s, 8, &v) || *s++ != L'-') return 0;
    guid->Data1 = v;
    if (!parse_hex(&s, 4, &v) || *s++ != L'-') return 0;
    guid->Data2 = (USHORT)v;
    if (!parse_hex(&s, 4, &v) || *s++ != L'-') return 0;
    guid->Data3 = (USHORT)v;
    for (unsigned i = 0; i < 8; ++i) {
        if (i == 2 && *s++ != L'-') return 0;
        if (!parse_hex(&s, 2, &v)) return 0;
        guid->Data4[i] = (UCHAR)v;
    }
    if (braces && *s++ != L'}') return 0;
    return *s == 0;
}

static WCHAR *path_for_guid(const GUID *guid, DEVINST instance)
{
    HDEVINFO set = SetupDiGetClassDevsW(guid, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return NULL;
    WCHAR *path = NULL;
    DWORD error = ERROR_NOT_FOUND;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA data = {0};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, guid, i, &data)) break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &data, NULL, 0, &needed, NULL);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = malloc(needed);
        if (!detail) { error = ERROR_NOT_ENOUGH_MEMORY; break; }
        SP_DEVINFO_DATA devinfo = {0};
        detail->cbSize = sizeof(*detail);
        devinfo.cbSize = sizeof(devinfo);
        if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, needed, NULL, &devinfo)
            && devinfo.DevInst == instance) {
            path = _wcsdup(detail->DevicePath);
            error = path ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
            free(detail);
            break;
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    SetLastError(error);
    return path;
}

static WCHAR *device_path(HDEVINFO set, SP_DEVINFO_DATA *info)
{
    HKEY key = SetupDiOpenDevRegKey(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV,
                                  KEY_QUERY_VALUE);
    if (key != INVALID_HANDLE_VALUE) {
        static const WCHAR *names[] = {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"};
        for (unsigned n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
            DWORD bytes = 0, type = 0;
            if (RegQueryValueExW(key, names[n], NULL, &type, NULL, &bytes) != ERROR_SUCCESS
                || (type != REG_SZ && type != REG_MULTI_SZ) || !bytes) continue;
            if (bytes > MAXDWORD - 2 * sizeof(WCHAR)) continue;
            WCHAR *text = calloc(1, (size_t)bytes + 2 * sizeof(WCHAR));
            if (!text) {
                RegCloseKey(key);
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return NULL;
            }
            if (RegQueryValueExW(key, names[n], NULL, &type, (BYTE *)text, &bytes)
                == ERROR_SUCCESS && (type == REG_SZ || type == REG_MULTI_SZ)) {
                for (const WCHAR *s = text; *s; s += wcslen(s) + 1) {
                    GUID guid;
                    if (!parse_guid(s, &guid)) continue;
                    WCHAR *path = path_for_guid(&guid, info->DevInst);
                    DWORD error = GetLastError();
                    if (path || error == ERROR_NOT_ENOUGH_MEMORY) {
                        free(text);
                        RegCloseKey(key);
                        SetLastError(error);
                        return path;
                    }
                }
            }
            free(text);
        }
        RegCloseKey(key);
    }
    return path_for_guid(&usb_device_guid, info->DevInst);
}

static void diagnose_driver(const WCHAR *instance, unsigned vid, unsigned pid,
                            const WCHAR *service)
{
    if (vid != 0x0d2f && vid != 0x0547) return;
    for (diagnosed_device *d = diagnosed; d; d = d->next)
        if (_wcsicmp(d->instance, instance) == 0) return;
    diagnosed_device *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->instance = _wcsdup(instance);
    if (!d->instance) { free(d); return; }
    d->next = diagnosed;
    diagnosed = d;
    fprintf(stderr, "lxusb: %04x:%04x uses driver %ls; bind WinUSB (e.g. Zadig) for hardware LXIO\n",
            vid, pid, *service ? service : L"(none)");
}

static int pair_used(unsigned bus, unsigned address)
{
    for (lxusb_bdev *d = devices; d; d = d->next)
        if (d->bus == bus && d->address == address) return 1;
    return 0;
}

static int assign_address(lxusb_bdev *dev, HDEVINFO set, SP_DEVINFO_DATA *info)
{
    WCHAR location[512] = {0};
    DWORD address = 0, bytes = 0, type = 0;
    unsigned bus = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_LOCATION_INFORMATION, &type,
                                        (BYTE *)location, sizeof(location), &bytes)
        && type == REG_SZ) {
        location[sizeof(location) / sizeof(location[0]) - 1] = 0;
        const WCHAR *hub = find_token(location, L"Hub_#");
        if (hub) bus = (unsigned)wcstoul(hub, NULL, 10);
    }
    if (!SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_ADDRESS, &type,
                                         (BYTE *)&address, sizeof(address), &bytes)
        || type != REG_DWORD) address = 0;
    unsigned hash = 2166136261u;
    for (const WCHAR *s = dev->instance; *s; ++s) {
        WCHAR c = *s >= L'a' && *s <= L'z' ? *s - (L'a' - L'A') : *s;
        hash = (hash ^ (unsigned)c) * 16777619u;
    }
    if (!bus || bus > 255) bus = 1 + (hash >> 8) % 255;
    if (!address || address > 255) address = 1 + hash % 255;
    /* Reserve pairs even for absent records: reconnects retain their old identity.
     * Linear probing also disambiguates equal hub numbers on different controllers. */
    for (unsigned tries = 0; tries < 255 * 255; ++tries) {
        if (!pair_used(bus, address)) {
            dev->bus = (uint8_t)bus;
            dev->address = (uint8_t)address;
            return 0;
        }
        if (++address > 255) { address = 1; if (++bus > 255) bus = 1; }
    }
    return LXUSB_ERROR_OVERFLOW;
}

static int backend_enumerate(lxusb_device_info *out, int cap)
{
    if (cap < 0 || (cap && !out)) return LXUSB_ERROR_INVALID_PARAM;
    AcquireSRWLockExclusive(&backend_lock);
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        int error = lxusb_error_from_win32(GetLastError());
        ReleaseSRWLockExclusive(&backend_lock);
        return error;
    }
    for (lxusb_bdev *d = devices; d; d = d->next) {
        d->present = 0;
        for (usb_function *f = d->functions; f; f = f->next) f->present = 0;
    }
    int error = 0;
    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA info = {0};
        info.cbSize = sizeof(info);
        if (!SetupDiEnumDeviceInfo(set, i, &info)) {
            DWORD e = GetLastError();
            if (e != ERROR_NO_MORE_ITEMS) error = lxusb_error_from_win32(e);
            break;
        }
        WCHAR instance[MAX_DEVICE_ID_LEN], physical[MAX_DEVICE_ID_LEN];
        if (CM_Get_Device_IDW(info.DevInst, instance, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
            continue;
        unsigned vid, pid;
        int iface;
        if (!usb_id(instance, &vid, &pid, &iface)) continue;
        WCHAR service[256] = {0};
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_SERVICE, &type,
                                             (BYTE *)service, sizeof(service), NULL)
            || type != REG_SZ) service[0] = 0;
        service[sizeof(service) / sizeof(service[0]) - 1] = 0;
        if (_wcsicmp(service, L"WinUSB") != 0) {
            diagnose_driver(instance, vid, pid, service);
            continue;
        }
        WCHAR *path = device_path(set, &info);
        if (!path) {
            if (GetLastError() == ERROR_NOT_ENOUGH_MEMORY) { error = LXUSB_ERROR_NO_MEM; break; }
            continue;
        }
        wcscpy(physical, instance);
        SP_DEVINFO_DATA physical_info = info;
        if (iface >= 0) {
            DEVINST parent;
            WCHAR parent_id[MAX_DEVICE_ID_LEN];
            unsigned parent_vid, parent_pid;
            int parent_iface;
            if (CM_Get_Parent(&parent, info.DevInst, 0) == CR_SUCCESS
                && CM_Get_Device_IDW(parent, parent_id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS
                && usb_id(parent_id, &parent_vid, &parent_pid, &parent_iface)
                && parent_vid == vid && parent_pid == pid && parent_iface < 0) {
                wcscpy(physical, parent_id);
                SP_DEVINFO_DATA parent_info = {0};
                parent_info.cbSize = sizeof(parent_info);
                if (SetupDiOpenDeviceInfoW(set, parent_id, NULL, 0, &parent_info))
                    physical_info = parent_info;
            }
        }
        lxusb_bdev **slot = &devices;
        while (*slot && _wcsicmp((*slot)->instance, physical) != 0) slot = &(*slot)->next;
        lxusb_bdev *dev = *slot;
        if (!dev) {
            dev = calloc(1, sizeof(*dev));
            if (dev) dev->instance = _wcsdup(physical);
            if (!dev || !dev->instance) {
                free(dev); free(path); error = LXUSB_ERROR_NO_MEM; break;
            }
            error = assign_address(dev, set, &physical_info);
            if (error) { free(dev->instance); free(dev); free(path); break; }
            *slot = dev;
        }
        usb_function **fslot = &dev->functions;
        while (*fslot && _wcsicmp((*fslot)->instance, instance) != 0) fslot = &(*fslot)->next;
        usb_function *function = *fslot;
        if (!function) {
            function = calloc(1, sizeof(*function));
            if (function) function->instance = _wcsdup(instance);
            if (!function || !function->instance) {
                free(function); free(path); error = LXUSB_ERROR_NO_MEM; break;
            }
            function->first_iface = iface;
            *fslot = function;
        }
        free(function->path);
        function->path = path;
        function->present = dev->present = 1;
    }
    SetupDiDestroyDeviceInfoList(set);
    int count = 0;
    if (!error) {
        for (lxusb_bdev *d = devices; d; d = d->next) {
            if (!d->present) continue;
            if (count < cap) {
                out[count].dev = d;
                out[count].bus = d->bus;
                out[count].address = d->address;
            }
            ++count;
        }
    }
    ReleaseSRWLockExclusive(&backend_lock);
    return error ? error : count;
}

static usb_function *first_function(lxusb_bdev *dev)
{
    usb_function *first = NULL;
    for (usb_function *f = dev->functions; f; f = f->next)
        if (f->present && (!first || f->first_iface < first->first_iface)) first = f;
    return first;
}

static int open_native(usb_function *function, HANDLE *file, WINUSB_INTERFACE_HANDLE *winusb)
{
    if (!function || !function->present) return LXUSB_ERROR_NO_DEVICE;
    *file = CreateFileW(function->path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, NULL);
    if (*file == INVALID_HANDLE_VALUE) return lxusb_error_from_win32(GetLastError());
    if (!WinUsb_Initialize(*file, winusb)) {
        int error = lxusb_error_from_win32(GetLastError());
        CloseHandle(*file);
        *file = INVALID_HANDLE_VALUE;
        return error;
    }
    return 0;
}

static int validate_config(const uint8_t *raw, unsigned size)
{
    if (size < 9 || raw[0] < 9 || raw[1] != 2 || get16(raw + 2) != size) return 0;
    for (unsigned pos = 0; pos < size;) {
        if (size - pos < 2 || raw[pos] < 2 || raw[pos] > size - pos) return 0;
        if ((raw[pos + 1] == 4 && raw[pos] < 9)
            || (raw[pos + 1] == 5 && raw[pos] < 7)) return 0;
        pos += raw[pos];
    }
    return 1;
}

/* Called with the backend lock. A live process-owned open avoids a second exclusive
 * device open; temporary opens are used only before the first libusb_open. */
static int cache_descriptors(lxusb_bdev *dev)
{
    if (dev->descriptors_ready) return 0;
    WINUSB_INTERFACE_HANDLE winusb = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    if (dev->opens) winusb = dev->opens->control_function->primary;
    int error = 0;
    if (!winusb) {
        error = open_native(first_function(dev), &file, &winusb);
        if (error) return error;
    }
    uint8_t descriptor[18];
    ULONG transferred = 0;
    uint8_t **configs = NULL;
    unsigned loaded = 0;
    if (!WinUsb_GetDescriptor(winusb, 1, 0, 0, descriptor, sizeof(descriptor), &transferred)) {
        error = lxusb_error_from_win32(GetLastError());
        goto done;
    }
    if (transferred != sizeof(descriptor) || descriptor[0] != 18 || descriptor[1] != 1) {
        error = LXUSB_ERROR_IO;
        goto done;
    }
    if (descriptor[17]) {
        configs = calloc(descriptor[17], sizeof(*configs));
        if (!configs) { error = LXUSB_ERROR_NO_MEM; goto done; }
    }
    for (unsigned i = 0; i < descriptor[17]; ++i) {
        uint8_t header[9];
        if (!WinUsb_GetDescriptor(winusb, 2, (UCHAR)i, 0, header, sizeof(header), &transferred)) {
            error = lxusb_error_from_win32(GetLastError());
            goto done;
        }
        if (transferred != sizeof(header) || header[0] < 9 || header[1] != 2
            || get16(header + 2) < 9) { error = LXUSB_ERROR_IO; goto done; }
        unsigned size = get16(header + 2);
        configs[i] = malloc(size);
        if (!configs[i]) { error = LXUSB_ERROR_NO_MEM; goto done; }
        ++loaded;
        if (!WinUsb_GetDescriptor(winusb, 2, (UCHAR)i, 0, configs[i], size, &transferred)) {
            error = lxusb_error_from_win32(GetLastError());
            goto done;
        }
        if (transferred != size || !validate_config(configs[i], size)) {
            error = LXUSB_ERROR_IO;
            goto done;
        }
    }
    memcpy(dev->descriptor, descriptor, sizeof(descriptor));
    dev->configs = configs;
    dev->descriptors_ready = 1;
    configs = NULL;
done:
    if (configs) {
        for (unsigned i = 0; i < loaded; ++i) free(configs[i]);
        free(configs);
    }
    if (file != INVALID_HANDLE_VALUE) { WinUsb_Free(winusb); CloseHandle(file); }
    return error;
}

static int backend_device_descriptor(lxusb_bdev *dev, uint8_t out[18])
{
    if (!dev || !out) return LXUSB_ERROR_INVALID_PARAM;
    AcquireSRWLockExclusive(&backend_lock);
    int error = cache_descriptors(dev);
    if (!error) memcpy(out, dev->descriptor, 18);
    ReleaseSRWLockExclusive(&backend_lock);
    return error;
}

static int backend_config_descriptor(lxusb_bdev *dev, uint8_t index, const uint8_t **raw)
{
    if (!dev || !raw) return LXUSB_ERROR_INVALID_PARAM;
    *raw = NULL;
    AcquireSRWLockExclusive(&backend_lock);
    int result = cache_descriptors(dev);
    if (!result) {
        if (index >= dev->descriptor[17]) result = LXUSB_ERROR_NOT_FOUND;
        else { *raw = dev->configs[index]; result = (int)get16(*raw + 2); }
    }
    ReleaseSRWLockExclusive(&backend_lock);
    return result;
}

static void close_function(lxusb_bhandle *h, open_function *function)
{
    for (unsigned i = 0; i < IFACE_COUNT; ++i) {
        usb_interface *iface = &h->interfaces[i];
        if (iface->function != function) continue;
        if (iface->winusb != function->primary) WinUsb_Free(iface->winusb);
        memset(iface, 0, sizeof(*iface));
    }
    WinUsb_Free(function->primary);
    CloseHandle(function->file);
    free(function);
}

static int register_interface(lxusb_bhandle *h, open_function *function,
                              WINUSB_INTERFACE_HANDLE winusb)
{
    USB_INTERFACE_DESCRIPTOR descriptor;
    UCHAR alt;
    if (!WinUsb_QueryInterfaceSettings(winusb, 0, &descriptor)
        || !WinUsb_GetCurrentAlternateSetting(winusb, &alt))
        return lxusb_error_from_win32(GetLastError());
    /* WinUSB defaults control requests to 5000 ms, unlike data pipes. Only usb.c's
     * cancellation timer may impose a timeout (legacy calls allow 10000 ms). */
    ULONG timeout = 0;
    if (!WinUsb_SetPipePolicy(winusb, 0, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout))
        return lxusb_error_from_win32(GetLastError());
    usb_interface *iface = &h->interfaces[descriptor.bInterfaceNumber];
    if (iface->winusb) return LXUSB_ERROR_IO;
    iface->winusb = winusb;
    iface->function = function;
    iface->alt = alt;
    return 0;
}

static int open_function_for_handle(lxusb_bhandle *h, usb_function *source)
{
    for (open_function *f = h->functions; f; f = f->next)
        if (f->source == source) return 0;
    open_function *function = calloc(1, sizeof(*function));
    if (!function) return LXUSB_ERROR_NO_MEM;
    function->source = source;
    int error = open_native(source, &function->file, &function->primary);
    if (error) { free(function); return error; }
    /* Never enable FILE_SKIP_COMPLETION_PORT_ON_SUCCESS: immediate completions must
     * still post once. A low-bit-tagged hEvent suppresses the packet in Windows. */
    if (!CreateIoCompletionPort(function->file, h->iocp, h->key, 0)) {
        error = lxusb_error_from_win32(GetLastError());
        close_function(h, function);
        return error;
    }
    error = register_interface(h, function, function->primary);
    if (error) { close_function(h, function); return error; }
    for (unsigned i = 0; i < IFACE_COUNT - 1; ++i) {
        WINUSB_INTERFACE_HANDLE associated;
        if (!WinUsb_GetAssociatedInterface(function->primary, (UCHAR)i, &associated)) {
            DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_ITEMS) break;
            error = lxusb_error_from_win32(e);
            close_function(h, function);
            return error;
        }
        error = register_interface(h, function, associated);
        if (error) {
            WinUsb_Free(associated);
            close_function(h, function);
            return error;
        }
    }
    function->next = h->functions;
    h->functions = function;
    if (!h->control_function) h->control_function = function;
    return 0;
}

static int ensure_interface(lxusb_bhandle *h, unsigned number)
{
    if (h->interfaces[number].winusb) return 0;
    /* An IAD function may own several interface numbers. Start with the closest
     * preceding MI_xx, then inspect other WinUSB functions if its association did
     * not include the requested interface. Merely enumerating does not open them. */
    usb_function *candidate = NULL;
    for (usb_function *f = h->dev->functions; f; f = f->next)
        if (f->present && f->first_iface <= (int)number
            && (!candidate || f->first_iface > candidate->first_iface)) candidate = f;
    int error = LXUSB_ERROR_NOT_FOUND;
    if (candidate) {
        error = open_function_for_handle(h, candidate);
        if (h->interfaces[number].winusb) return 0;
    }
    for (usb_function *f = h->dev->functions; f; f = f->next) {
        if (!f->present || f == candidate) continue;
        int e = open_function_for_handle(h, f);
        if (h->interfaces[number].winusb) return 0;
        if (e && (!error || error == LXUSB_ERROR_NOT_FOUND)) error = e;
    }
    return error ? error : LXUSB_ERROR_NOT_FOUND;
}

static void free_open(lxusb_bhandle *h)
{
    while (h->functions) {
        open_function *f = h->functions;
        h->functions = f->next;
        CancelIoEx(f->file, NULL);
        close_function(h, f);
    }
    while (h->pending) {
        pending_io *p = h->pending;
        h->pending = p->next;
        free(p);
    }
    while (h->spare) {
        pending_io *p = h->spare;
        h->spare = p->next;
        free(p);
    }
    free(h);
}

static int backend_open(lxusb_bdev *dev, HANDLE iocp, ULONG_PTR key, lxusb_bhandle **out)
{
    if (!dev || !iocp || !out) return LXUSB_ERROR_INVALID_PARAM;
    *out = NULL;
    AcquireSRWLockExclusive(&backend_lock);
    int error = 0;
    lxusb_bhandle *h = NULL;
    if (!dev->present) { error = LXUSB_ERROR_NO_DEVICE; goto done; }
    h = calloc(1, sizeof(*h));
    if (!h) { error = LXUSB_ERROR_NO_MEM; goto done; }
    h->dev = dev;
    h->iocp = iocp;
    h->key = key;
    error = open_function_for_handle(h, first_function(dev));
    if (error) goto done;
    h->next = dev->opens;
    dev->opens = h;
    error = cache_descriptors(dev);
    if (error) { dev->opens = h->next; goto done; }
    *out = h;
done:
    if (error && h) free_open(h);
    ReleaseSRWLockExclusive(&backend_lock);
    return error;
}

static void backend_close(lxusb_bhandle *h)
{
    if (!h) return;
    AcquireSRWLockExclusive(&backend_lock);
    lxusb_bhandle **slot = &h->dev->opens;
    while (*slot && *slot != h) slot = &(*slot)->next;
    if (*slot) *slot = h->next;
    /* usb.c defers backend close until every started operation's notification has
     * been retired, so no OVERLAPPED or caller buffer remains owned by WinUSB.
     * free_open's defensive CancelIoEx calls are harmless after that retirement. */
    free_open(h);
    ReleaseSRWLockExclusive(&backend_lock);
}

static const uint8_t *active_config(lxusb_bhandle *h)
{
    return h->dev->descriptor[17] ? h->dev->configs[0] : NULL;
}

static int has_alt(lxusb_bhandle *h, unsigned number, unsigned alt)
{
    const uint8_t *raw = active_config(h);
    if (!raw) return 0;
    unsigned size = get16(raw + 2);
    for (unsigned p = 0; p < size; p += raw[p])
        if (raw[p + 1] == 4 && raw[p + 2] == number && raw[p + 3] == alt) return 1;
    return 0;
}

static void refresh_routes(lxusb_bhandle *h)
{
    memset(h->endpoints, 0, sizeof(h->endpoints));
    const uint8_t *raw = active_config(h);
    if (!raw) return;
    usb_interface *current = NULL;
    unsigned size = get16(raw + 2);
    for (unsigned p = 0; p < size; p += raw[p]) {
        if (raw[p + 1] == 4) {
            current = &h->interfaces[raw[p + 2]];
            if (!current->claimed || current->alt != raw[p + 3]) current = NULL;
        } else if (raw[p + 1] == 5 && current) {
            h->endpoints[raw[p + 2]] = current;
        }
    }
}

static int set_pipe_timeouts(lxusb_bhandle *h, unsigned number, unsigned alt)
{
    const uint8_t *raw = active_config(h);
    unsigned size = get16(raw + 2);
    int current = 0;
    ULONG timeout = 0;
    for (unsigned p = 0; p < size; p += raw[p]) {
        if (raw[p + 1] == 4) current = raw[p + 2] == number && raw[p + 3] == alt;
        else if (raw[p + 1] == 5 && current) {
            if (!WinUsb_SetPipePolicy(h->interfaces[number].winusb, raw[p + 2],
                                     PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout))
                return lxusb_error_from_win32(GetLastError());
        }
    }
    return 0;
}

static int backend_claim(lxusb_bhandle *h, int number)
{
    if (!h || number < 0 || number >= IFACE_COUNT) return LXUSB_ERROR_INVALID_PARAM;
    AcquireSRWLockExclusive(&backend_lock);
    int error = ensure_interface(h, (unsigned)number);
    if (!error) {
        usb_interface *iface = &h->interfaces[number];
        if (!has_alt(h, (unsigned)number, iface->alt)) error = LXUSB_ERROR_NOT_FOUND;
        else if (!iface->claimed) {
            error = set_pipe_timeouts(h, (unsigned)number, iface->alt);
            if (!error) { iface->claimed = 1; refresh_routes(h); }
        }
    }
    ReleaseSRWLockExclusive(&backend_lock);
    return error;
}

static int backend_release(lxusb_bhandle *h, int number)
{
    if (!h || number < 0 || number >= IFACE_COUNT) return LXUSB_ERROR_INVALID_PARAM;
    AcquireSRWLockExclusive(&backend_lock);
    int error = h->interfaces[number].claimed ? 0 : LXUSB_ERROR_NOT_FOUND;
    h->interfaces[number].claimed = 0;
    refresh_routes(h);
    ReleaseSRWLockExclusive(&backend_lock);
    return error;
}

static int backend_set_alt(lxusb_bhandle *h, int number, int alt)
{
    if (!h || number < 0 || number >= IFACE_COUNT || alt < 0 || alt > 255)
        return LXUSB_ERROR_INVALID_PARAM;
    AcquireSRWLockExclusive(&backend_lock);
    usb_interface *iface = &h->interfaces[number];
    int error = 0;
    if (!iface->claimed || !has_alt(h, (unsigned)number, (unsigned)alt))
        error = LXUSB_ERROR_NOT_FOUND;
    else if (!WinUsb_SetCurrentAlternateSetting(iface->winusb, (UCHAR)alt))
        error = lxusb_error_from_win32(GetLastError());
    else {
        iface->alt = (UCHAR)alt;
        refresh_routes(h);
        error = set_pipe_timeouts(h, (unsigned)number, (unsigned)alt);
    }
    ReleaseSRWLockExclusive(&backend_lock);
    return error;
}

static int backend_reset(lxusb_bhandle *h)
{
    if (!h) return LXUSB_ERROR_INVALID_PARAM;
    AcquireSRWLockExclusive(&backend_lock);
    int error = 0;
    for (unsigned ep = 1; ep < IFACE_COUNT; ++ep) {
        usb_interface *iface = h->endpoints[ep];
        if (!iface) continue;
        if (!WinUsb_AbortPipe(iface->winusb, (UCHAR)ep) && !error)
            error = lxusb_error_from_win32(GetLastError());
        if (!WinUsb_ResetPipe(iface->winusb, (UCHAR)ep) && !error)
            error = lxusb_error_from_win32(GetLastError());
    }
    ReleaseSRWLockExclusive(&backend_lock);
    return error;
}

static pending_io **find_pending(lxusb_bhandle *h, OVERLAPPED *ov)
{
    pending_io **slot = &h->pending;
    while (*slot && (*slot)->ov != ov) slot = &(*slot)->next;
    return slot;
}

static void recycle_pending(lxusb_bhandle *h, pending_io **slot)
{
    pending_io *p = *slot;
    *slot = p->next;
    p->next = h->spare;
    h->spare = p;
}

static int backend_submit(lxusb_bhandle *h, unsigned char endpoint, const uint8_t setup[8],
                          uint8_t *buf, ULONG len, OVERLAPPED *ov)
{
    if (!h || !ov || (len && !buf) || (!endpoint && (!setup || get16(setup + 6) != len)))
        return LXUSB_ERROR_INVALID_PARAM;
    AcquireSRWLockExclusive(&backend_lock);
    int error = 0;
    usb_interface *iface = NULL;
    if (*find_pending(h, ov)) { error = LXUSB_ERROR_BUSY; goto done; }
    if (!endpoint) {
        if ((setup[0] & 0x1f) == 1) {
            error = ensure_interface(h, setup[4]);
            if (error) goto done;
            iface = &h->interfaces[setup[4]];
        } else if ((setup[0] & 0x1f) == 2) {
            iface = h->endpoints[setup[4]];
            if (!iface) { error = LXUSB_ERROR_NOT_FOUND; goto done; }
        }
    } else {
        iface = h->endpoints[endpoint];
        if (!iface) { error = LXUSB_ERROR_NOT_FOUND; goto done; }
    }
    open_function *function = iface ? iface->function : h->control_function;
    WINUSB_INTERFACE_HANDLE winusb = iface ? iface->winusb : function->primary;
    pending_io *pending = h->spare;
    if (pending) h->spare = pending->next;
    else pending = malloc(sizeof(*pending));
    if (!pending) { error = LXUSB_ERROR_NO_MEM; goto done; }
    pending->ov = ov;
    pending->function = function;
    pending->next = h->pending;
    h->pending = pending;
    BOOL started;
    if (!endpoint) {
        WINUSB_SETUP_PACKET packet;
        packet.RequestType = setup[0];
        packet.Request = setup[1];
        packet.Value = (USHORT)get16(setup + 2);
        packet.Index = (USHORT)get16(setup + 4);
        packet.Length = (USHORT)get16(setup + 6);
        started = WinUsb_ControlTransfer(winusb, packet, buf, len, NULL, ov);
    } else if (endpoint & 0x80) {
        started = WinUsb_ReadPipe(winusb, endpoint, buf, len, NULL, ov);
    } else {
        started = WinUsb_WritePipe(winusb, endpoint, buf, len, NULL, ov);
    }
    if (!started) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            recycle_pending(h, &h->pending);
            error = lxusb_error_from_win32(e);
        }
    }
done:
    ReleaseSRWLockExclusive(&backend_lock);
    return error;
}

static void backend_cancel(lxusb_bhandle *h, OVERLAPPED *ov)
{
    if (!h || !ov) return;
    AcquireSRWLockExclusive(&backend_lock);
    pending_io *pending = *find_pending(h, ov);
    if (pending) CancelIoEx(pending->function->file, ov);
    ReleaseSRWLockExclusive(&backend_lock);
}

static void backend_result(lxusb_bhandle *h, OVERLAPPED *ov, ULONG *bytes, DWORD *error)
{
    *bytes = 0;
    *error = ERROR_NOT_FOUND;
    AcquireSRWLockExclusive(&backend_lock);
    pending_io **slot = find_pending(h, ov);
    if (*slot) {
        /* GetOverlappedResult requires the WinUsb_Initialize handle, including for
         * operations submitted through one of its associated-interface handles. */
        if (WinUsb_GetOverlappedResult((*slot)->function->primary, ov, bytes, FALSE))
            *error = ERROR_SUCCESS;
        else *error = GetLastError();
        if (*error != ERROR_IO_INCOMPLETE) recycle_pending(h, slot);
    }
    ReleaseSRWLockExclusive(&backend_lock);
}

const lxusb_backend lxusb_backend_impl = {
    backend_enumerate, backend_device_descriptor, backend_config_descriptor,
    backend_open, backend_close, backend_claim, backend_release, backend_set_alt,
    backend_reset, backend_submit, backend_cancel, backend_result
};
