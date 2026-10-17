/* libusb guest ABI over the replaceable WinUSB backend. */
#include "usb_abi.h"
#include "../../lxrt/lxabi.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDLE_MAGIC 0x4853554cu
#define TRANSFER_MAGIC 0x5453554cu
#define WAKE_KEY ((ULONG_PTR)1)
struct libusb_context { int refs; };
struct libusb_device {
    lxusb_bdev *backend;
    uint8_t bus, address;
    int legacy_created, legacy_present, legacy_reported;
    usb_device legacy;
    struct libusb_device *next;
};
struct libusb_device_handle {
    unsigned magic;
    lxusb_bhandle *backend;
    libusb_device *device;
    int last_iface;
    unsigned refs;
    int closing;
    struct libusb_device_handle *next;
};
typedef struct pending {
    unsigned magic;
    OVERLAPPED ov;
    struct pending *next, *prev;
    libusb_device_handle *handle;
    ULONGLONG deadline;
    ULONG requested;
    int inflight, cancelling, timed_out;
} pending;
typedef struct bus_node {
    usb_bus guest;
    struct bus_node *next;
    unsigned number;
    int present, reported;
} bus_node;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION lock;
static HANDLE iocp;
static struct libusb_context context;
static libusb_device *devices;
static bus_node *busses;
static usb_bus *bus_head;
static pending *inflight;
static libusb_device_handle *handles;

static BOOL CALLBACK initialize(PINIT_ONCE init, PVOID arg, PVOID *out)
{
    (void)init; (void)arg; (void)out;
    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!iocp) return FALSE;
    InitializeCriticalSection(&lock);
    return TRUE;
}
static int ensure_state(void)
{
    return InitOnceExecuteOnce(&once, initialize, NULL, NULL) ? 0 : LXUSB_ERROR_NO_MEM;
}
static void *guest_lookup(const char *name)
{
    HMODULE rt = GetModuleHandleW(L"lxrt.dll");
    lx_guest_symbol_fn lookup = rt ? (lx_guest_symbol_fn)GetProcAddress(rt, "lx_guest_symbol") : NULL;
    return lookup ? lookup(name) : NULL;
}
/* Intentionally replaceable by the native fake-backend test. */
void *(*lxusb_guest_lookup)(const char *) = guest_lookup;
static int valid_context(libusb_context *ctx) { return !ctx || ctx == &context; }
/* The registry avoids reading a freed handle when a completion callback tries
 * to resubmit after close has dropped the final I/O reference. Lock required. */
static int valid_handle(libusb_device_handle *h)
{
    libusb_device_handle *live;
    for (live = handles; live; live = live->next)
        if (live == h) return live->magic == HANDLE_MAGIC && !live->closing;
    return 0;
}
static void unref_handle_locked(libusb_device_handle *h)
{
    libusb_device_handle **link;
    if (--h->refs) return;
    for (link = &handles; *link != h; link = &(*link)->next) {}
    *link = h->next;
    lxusb_backend_impl.close(h->backend);
    free(h);
}
static int acquire_handle(libusb_device_handle *h)
{
    int r = ensure_state();
    if (r) return r;
    EnterCriticalSection(&lock);
    if (valid_handle(h)) ++h->refs;
    else r = LXUSB_ERROR_NO_DEVICE;
    LeaveCriticalSection(&lock);
    return r;
}
static void release_handle(libusb_device_handle *h)
{
    EnterCriticalSection(&lock);
    unref_handle_locked(h);
    LeaveCriticalSection(&lock);
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static libusb_transfer *public_transfer(pending *p) { return (libusb_transfer *)(p + 1); }
static pending *private_transfer(libusb_transfer *t) { return ((pending *)t) - 1; }

int libusb_init(libusb_context **ctx)
{
    int r = ensure_state();
    if (r) return r;
    EnterCriticalSection(&lock);
    ++context.refs;
    if (ctx) *ctx = &context;
    LeaveCriticalSection(&lock);
    return 0;
}
void libusb_exit(libusb_context *ctx)
{
    if (!valid_context(ctx) || ensure_state()) return;
    EnterCriticalSection(&lock);
    if (context.refs) --context.refs;
    LeaveCriticalSection(&lock);
}
/* Enumeration and persistent-object lookup are cold paths, serialized by lock. */
static int snapshot(lxusb_device_info **out)
{
    lxusb_device_info *list = NULL;
    int n, cap = 0;
    for (;;) {
        n = lxusb_backend_impl.enumerate(list, cap);
        if (n < 0) { free(list); return n; }
        if (n <= cap) { *out = list; return n; }
        free(list);
        cap = n;
        list = malloc((size_t)cap * sizeof(*list));
        if (!list) return LXUSB_ERROR_NO_MEM;
    }
}
static libusb_device *device_for(const lxusb_device_info *info)
{
    libusb_device *d;
    for (d = devices; d; d = d->next) if (d->backend == info->dev) break;
    if (!d) {
        d = calloc(1, sizeof(*d));
        if (!d) return NULL;
        d->backend = info->dev;
        d->next = devices;
        devices = d;
    }
    d->bus = info->bus;
    d->address = info->address;
    return d;
}
int libusb_get_device_list(libusb_context *ctx, libusb_device ***out)
{
    lxusb_device_info *info = NULL;
    libusb_device **list;
    int n, i, r;
    if (!out || !valid_context(ctx)) return LXUSB_ERROR_INVALID_PARAM;
    *out = NULL;
    if ((r = ensure_state())) return r;
    EnterCriticalSection(&lock);
    n = snapshot(&info);
    if (n < 0) { LeaveCriticalSection(&lock); return n; }
    list = calloc((size_t)n + 1, sizeof(*list));
    if (!list) { free(info); LeaveCriticalSection(&lock); return LXUSB_ERROR_NO_MEM; }
    for (i = 0; i < n; ++i) {
        list[i] = device_for(&info[i]);
        if (!list[i]) break;
    }
    free(info);
    LeaveCriticalSection(&lock);
    if (i != n) { free(list); return LXUSB_ERROR_NO_MEM; }
    *out = list;
    return n;
}
void libusb_free_device_list(libusb_device **list, int unref) { (void)unref; free(list); }
int libusb_get_device_descriptor(libusb_device *d, libusb_device_descriptor *desc)
{
    if (!d || !desc) return LXUSB_ERROR_INVALID_PARAM;
    return lxusb_backend_impl.device_descriptor(d->backend, (uint8_t *)desc);
}

/* Two passes: validate/count first, then materialize one owned descriptor tree.
 * Raw bytes at its tail own every extra pointer, including HID descriptors. */
static int parse_config(const uint8_t *raw, int size, libusb_config_descriptor **out)
{
    unsigned counts[256] = {0}, used[256] = {0};
    int map[256], groups = 0, alts = 0, eps = 0, expected = 0, seen = 0;
    int pos, n, type, current = -1, i;
    size_t bytes;
    libusb_config_descriptor *cfg;
    libusb_interface_descriptor *alt, *cur_alt = NULL;
    libusb_endpoint_descriptor *ep, *cur_ep = NULL;
    uint8_t *copy;
    *out = NULL;
    if (!raw || size < 9 || raw[0] < 9 || raw[0] > size || raw[1] != 2) return LXUSB_ERROR_IO;
    n = le16(raw + 2);
    if (n < raw[0] || n > size) return LXUSB_ERROR_IO;
    size = n;
    for (i = 0; i < 256; ++i) map[i] = -1;
    for (pos = raw[0]; pos < size; pos += n) {
        if (size - pos < 2 || (n = raw[pos]) < 2 || n > size - pos) return LXUSB_ERROR_IO;
        type = raw[pos + 1];
        if (type == 4) {
            if (n < 9 || (current >= 0 && seen != expected)) return LXUSB_ERROR_IO;
            current = raw[pos + 2];
            if (map[current] < 0) map[current] = groups++;
            ++counts[current]; ++alts;
            expected = raw[pos + 4]; seen = 0;
        } else if (type == 5) {
            if (n < 7 || current < 0 || ++seen > expected) return LXUSB_ERROR_IO;
            ++eps;
        } else if (type == 1 || type == 2) return LXUSB_ERROR_IO;
    }
    if ((current >= 0 && seen != expected) || groups != raw[4]) return LXUSB_ERROR_IO;
    bytes = sizeof(*cfg) + (size_t)groups * sizeof(libusb_interface) +
            (size_t)alts * sizeof(*alt) + (size_t)eps * sizeof(*ep) + (size_t)size;
    cfg = calloc(1, bytes);
    if (!cfg) return LXUSB_ERROR_NO_MEM;
    memcpy(cfg, raw, 9);
    cfg->interface = (libusb_interface *)(cfg + 1);
    alt = (libusb_interface_descriptor *)(cfg->interface + groups);
    ep = (libusb_endpoint_descriptor *)(alt + alts);
    copy = (uint8_t *)(ep + eps);
    memcpy(copy, raw, (size_t)size);
    for (i = 0; i < 256; ++i) if (map[i] >= 0) {
        libusb_interface *iface = &cfg->interface[map[i]];
        iface->altsetting = alt;
        iface->num_altsetting = (int)counts[i];
        alt += counts[i];
    }
    for (pos = raw[0]; pos < size; pos += n) {
        n = raw[pos]; type = raw[pos + 1];
        if (type == 4) {
            unsigned number = raw[pos + 2], j;
            libusb_interface *iface = &cfg->interface[map[number]];
            cur_alt = &iface->altsetting[used[number]++];
            for (j = 0; j + 1 < used[number]; ++j) {
                if (iface->altsetting[j].bAlternateSetting == raw[pos + 3]) {
                    free(cfg); return LXUSB_ERROR_IO;
                }
            }
            memcpy(cur_alt, raw + pos, 9);
            cur_alt->endpoint = ep;
            cur_ep = NULL;
        } else if (type == 5) {
            cur_ep = ep++;
            memcpy(cur_ep, raw + pos, (size_t)(n < 9 ? n : 9));
        } else {
            unsigned char **extra;
            int *length;
            if (cur_ep) { extra = &cur_ep->extra; length = &cur_ep->extra_length; }
            else if (cur_alt) { extra = &cur_alt->extra; length = &cur_alt->extra_length; }
            else { extra = &cfg->extra; length = &cfg->extra_length; }
            if (!*extra) *extra = copy + pos;
            *length += n;
        }
    }
    *out = cfg;
    return 0;
}
int libusb_get_config_descriptor(libusb_device *d, uint8_t index, libusb_config_descriptor **out)
{
    const uint8_t *raw = NULL;
    int n;
    if (!out) return LXUSB_ERROR_INVALID_PARAM;
    *out = NULL;
    if (!d) return LXUSB_ERROR_INVALID_PARAM;
    n = lxusb_backend_impl.config_descriptor(d->backend, index, &raw);
    return n < 0 ? n : parse_config(raw, n, out);
}
int libusb_get_active_config_descriptor(libusb_device *d, libusb_config_descriptor **out)
{ return libusb_get_config_descriptor(d, 0, out); }
void libusb_free_config_descriptor(libusb_config_descriptor *cfg) { free(cfg); }
uint8_t libusb_get_bus_number(libusb_device *d) { return d ? d->bus : 0; }
uint8_t libusb_get_device_address(libusb_device *d) { return d ? d->address : 0; }
int libusb_open(libusb_device *d, libusb_device_handle **out)
{
    libusb_device_handle *h;
    int r;
    if (!out) return LXUSB_ERROR_INVALID_PARAM;
    *out = NULL;
    if (!d) return LXUSB_ERROR_NO_DEVICE;
    if ((r = ensure_state())) return r;
    h = calloc(1, sizeof(*h));
    if (!h) return LXUSB_ERROR_NO_MEM;
    h->device = d;
    h->last_iface = -1;
    r = lxusb_backend_impl.open(d->backend, iocp, (ULONG_PTR)h, &h->backend);
    if (r) { free(h); return r; }
    h->magic = HANDLE_MAGIC;
    h->refs = 1;
    EnterCriticalSection(&lock);
    h->next = handles;
    handles = h;
    LeaveCriticalSection(&lock);
    *out = h;
    return 0;
}
void libusb_close(libusb_device_handle *h)
{
    pending *p;
    if (!h || ensure_state()) return;
    EnterCriticalSection(&lock);
    if (valid_handle(h)) {
        h->magic = 0;
        h->closing = 1;
        for (p = inflight; p; p = p->next) if (p->handle == h && !p->cancelling) {
            p->cancelling = 1;
            lxusb_backend_impl.cancel(h->backend, &p->ov);
        }
        /* Outstanding operations own references until result() is consumed.
         * Never wait here: a close caller need not be an event-pump thread. */
        unref_handle_locked(h);
    }
    LeaveCriticalSection(&lock);
}
int libusb_claim_interface(libusb_device_handle *h, int iface)
{
    int r = acquire_handle(h);
    if (r) return r;
    r = lxusb_backend_impl.claim(h->backend, iface);
    release_handle(h);
    return r;
}
int libusb_release_interface(libusb_device_handle *h, int iface)
{
    int r = acquire_handle(h);
    if (r) return r;
    r = lxusb_backend_impl.release(h->backend, iface);
    release_handle(h);
    return r;
}
int libusb_kernel_driver_active(libusb_device_handle *h, int iface)
{
    int r = acquire_handle(h);
    (void)iface;
    if (!r) release_handle(h);
    return r;
}
int libusb_detach_kernel_driver(libusb_device_handle *h, int iface)
{
    int r = acquire_handle(h);
    (void)iface;
    if (r) return r;
    release_handle(h);
    return LXUSB_ERROR_NOT_FOUND;
}

static int synchronous(libusb_device_handle *h, unsigned char ep, const uint8_t *setup,
                       uint8_t *data, ULONG length, ULONG *transferred, unsigned int timeout)
{
    OVERLAPPED ov = {0};
    HANDLE event;
    DWORD wait, error;
    int r, timed_out = 0;
    *transferred = 0;
    if (length && !data) return LXUSB_ERROR_INVALID_PARAM;
    if ((r = acquire_handle(h))) return r;
    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) { release_handle(h); return LXUSB_ERROR_NO_MEM; }
    ov.hEvent = (HANDLE)((ULONG_PTR)event | 1);
    r = lxusb_backend_impl.submit(h->backend, ep, setup, data, length, &ov);
    if (!r) {
        wait = WaitForSingleObject(event, timeout ? timeout : INFINITE);
        if (wait != WAIT_OBJECT_0) {
            timed_out = wait == WAIT_TIMEOUT;
            lxusb_backend_impl.cancel(h->backend, &ov);
            WaitForSingleObject(event, INFINITE);
        }
        lxusb_backend_impl.result(h->backend, &ov, transferred, &error);
        /* A completion that wins the race against our timeout cancel keeps its own result. */
        r = timed_out && error == ERROR_OPERATION_ABORTED ? LXUSB_ERROR_TIMEOUT : lxusb_error_from_win32(error);
        if (wait == WAIT_FAILED && !r) r = LXUSB_ERROR_IO;
    }
    CloseHandle(event);
    release_handle(h);
    return r;
}
int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t request,
                            uint16_t value, uint16_t index, unsigned char *data,
                            uint16_t length, unsigned int timeout)
{
    uint8_t setup[8] = { type, request, 0, 0, 0, 0, 0, 0 };
    ULONG transferred;
    int r;
    put16(setup + 2, value); put16(setup + 4, index); put16(setup + 6, length);
    r = synchronous(h, 0, setup, data, length, &transferred, timeout);
    return r ? r : (int)transferred;
}
int libusb_interrupt_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data,
                              int length, int *transferred, unsigned int timeout)
{
    ULONG count = 0;
    int r;
    if (transferred) *transferred = 0;
    if (length < 0 || !ep) return LXUSB_ERROR_INVALID_PARAM;
    r = synchronous(h, ep, NULL, data, (ULONG)length, &count, timeout);
    if (transferred) *transferred = (int)count;
    return r;
}
int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data,
                         int length, int *transferred, unsigned int timeout)
{ return libusb_interrupt_transfer(h, ep, data, length, transferred, timeout); }

libusb_transfer *libusb_alloc_transfer(int iso)
{
    pending *p;
    libusb_transfer *t;
    size_t bytes;
    if (iso < 0 || (size_t)iso > (SIZE_MAX - sizeof(*p) - sizeof(*t)) / sizeof(libusb_iso_packet_descriptor))
        return NULL;
    bytes = sizeof(*p) + sizeof(*t) + (size_t)iso * sizeof(libusb_iso_packet_descriptor);
    p = calloc(1, bytes);
    if (!p) return NULL;
    p->magic = TRANSFER_MAGIC;
    t = public_transfer(p);
    t->num_iso_packets = iso;
    return t;
}
void libusb_free_transfer(libusb_transfer *t)
{
    pending *p;
    void (*guest_free)(void *);
    if (!t) return;
    p = private_transfer(t);
    if (p->magic != TRANSFER_MAGIC || p->inflight) return;
    if ((t->flags & LIBUSB_TRANSFER_FREE_BUFFER) && t->buffer) {
        guest_free = lxusb_guest_lookup ? (void (*)(void *))lxusb_guest_lookup("free") : NULL;
        if (guest_free) guest_free(t->buffer);
    }
    p->magic = 0;
    free(p);
}
int libusb_submit_transfer(libusb_transfer *t)
{
    pending *p;
    const uint8_t *setup = NULL;
    uint8_t *buffer;
    ULONG length;
    unsigned char ep;
    int r;
    if (!t) return LXUSB_ERROR_INVALID_PARAM;
    p = private_transfer(t);
    if (p->magic != TRANSFER_MAGIC) return LXUSB_ERROR_INVALID_PARAM;
    if ((r = ensure_state())) return r;
    EnterCriticalSection(&lock);
    if (p->inflight) { r = LXUSB_ERROR_BUSY; goto done; }
    if (!valid_handle(t->dev_handle)) { r = LXUSB_ERROR_NO_DEVICE; goto done; }
    if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS || t->type >= LIBUSB_TRANSFER_TYPE_BULK_STREAM ||
        (t->flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET)) { r = LXUSB_ERROR_NOT_SUPPORTED; goto done; }
    if (t->length < 0 || (t->length && !t->buffer)) { r = LXUSB_ERROR_INVALID_PARAM; goto done; }
    buffer = t->buffer; length = (ULONG)t->length; ep = t->endpoint;
    if (t->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        if (length < 8) { r = LXUSB_ERROR_INVALID_PARAM; goto done; }
        setup = buffer; buffer += 8; length = le16(setup + 6); ep = 0;
        if (length > (ULONG)t->length - 8) { r = LXUSB_ERROR_INVALID_PARAM; goto done; }
    } else if (!ep) { r = LXUSB_ERROR_INVALID_PARAM; goto done; }
    memset(&p->ov, 0, sizeof(p->ov));
    p->handle = t->dev_handle;
    p->requested = length;
    p->timed_out = p->cancelling = 0;
    p->deadline = t->timeout ? GetTickCount64() + t->timeout : 0;
    r = lxusb_backend_impl.submit(p->handle->backend, ep, setup, buffer, length, &p->ov);
    if (r) goto done;
    ++p->handle->refs;
    p->inflight = 1;
    p->prev = NULL; p->next = inflight;
    if (inflight) inflight->prev = p;
    inflight = p;
    PostQueuedCompletionStatus(iocp, 0, WAKE_KEY, NULL);
 done:
    LeaveCriticalSection(&lock);
    return r;
}
int libusb_cancel_transfer(libusb_transfer *t)
{
    pending *p;
    int r = LXUSB_ERROR_NOT_FOUND;
    if (!t || ensure_state()) return r;
    p = private_transfer(t);
    if (p->magic != TRANSFER_MAGIC) return r;
    EnterCriticalSection(&lock);
    if (p->inflight && !p->cancelling) {
        p->cancelling = 1;
        lxusb_backend_impl.cancel(p->handle->backend, &p->ov);
        r = 0;
    }
    LeaveCriticalSection(&lock);
    return r;
}
static DWORD next_wait(ULONGLONG cap)
{
    ULONGLONG now = GetTickCount64(), next = cap;
    pending *p;
    EnterCriticalSection(&lock);
    for (p = inflight; p; p = p->next) if (!p->cancelling && p->deadline) {
        if (p->deadline <= now) {
            p->timed_out = p->cancelling = 1;
            lxusb_backend_impl.cancel(p->handle->backend, &p->ov);
        } else if (p->deadline < next) next = p->deadline;
    }
    LeaveCriticalSection(&lock);
    return now >= next ? 0 : (DWORD)(next - now);
}
static void complete_transfer(OVERLAPPED *ov)
{
    pending *p = (pending *)((char *)ov - offsetof(pending, ov));
    libusb_transfer *t = public_transfer(p);
    ULONG bytes;
    DWORD error;
    unsigned flags;
    void (*callback)(libusb_transfer *);
    EnterCriticalSection(&lock);
    lxusb_backend_impl.result(p->handle->backend, ov, &bytes, &error);
    if (p->prev) p->prev->next = p->next; else inflight = p->next;
    if (p->next) p->next->prev = p->prev;
    p->inflight = 0;
    t->actual_length = (int)bytes;
    if (!error) t->status = (t->flags & LIBUSB_TRANSFER_SHORT_NOT_OK) && bytes < p->requested ?
                            LIBUSB_TRANSFER_ERROR : LIBUSB_TRANSFER_COMPLETED;
    else if (error == ERROR_OPERATION_ABORTED) t->status = p->timed_out ? LIBUSB_TRANSFER_TIMED_OUT : LIBUSB_TRANSFER_CANCELLED;
    else if (error == ERROR_SEM_TIMEOUT) t->status = LIBUSB_TRANSFER_TIMED_OUT;
    else if (error == ERROR_GEN_FAILURE) t->status = LIBUSB_TRANSFER_STALL;
    else if (error == ERROR_MORE_DATA) t->status = LIBUSB_TRANSFER_OVERFLOW;
    else if (lxusb_error_from_win32(error) == LXUSB_ERROR_NO_DEVICE) t->status = LIBUSB_TRANSFER_NO_DEVICE;
    else t->status = LIBUSB_TRANSFER_ERROR;
    flags = t->flags;
    callback = t->callback;
    unref_handle_locked(p->handle);
    p->handle = NULL;
    LeaveCriticalSection(&lock);
    if (callback) callback(t);
    if (flags & LIBUSB_TRANSFER_FREE_TRANSFER) libusb_free_transfer(t);
}
int libusb_handle_events(libusb_context *ctx)
{
    ULONGLONG cap;
    DWORD bytes, timeout;
    ULONG_PTR key;
    OVERLAPPED *ov;
    int r, completed = 0;
    if (!valid_context(ctx)) return LXUSB_ERROR_INVALID_PARAM;
    if ((r = ensure_state())) return r;
    cap = GetTickCount64() + 60000;
    for (;;) {
        timeout = completed ? 0 : next_wait(cap);
        ov = NULL;
        if (!GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, timeout) && !ov) {
            if (GetLastError() != WAIT_TIMEOUT) { r = LXUSB_ERROR_IO; break; }
            if (completed || GetTickCount64() >= cap) { r = 0; break; }
            continue;
        }
        if (ov) { complete_transfer(ov); completed = 1; }
        /* Wake packets never finish a blocking call: they only refresh deadlines. */
    }
    return r;
}

static int legacy_error(int r)
{
    switch (r) {
    case LXUSB_SUCCESS: return 0;
    case LXUSB_ERROR_TIMEOUT: return -110;
    case LXUSB_ERROR_NO_DEVICE: return -19;
    case LXUSB_ERROR_PIPE: return -32;
    case LXUSB_ERROR_ACCESS: return -13;
    case LXUSB_ERROR_BUSY: return -16;
    case LXUSB_ERROR_INVALID_PARAM: return -22;
    case LXUSB_ERROR_NO_MEM: return -12;
    case LXUSB_ERROR_NOT_FOUND: return -2;
    default: return -5;
    }
}
static void publish_busses(void)
{
    static LONG warned;
    usb_bus **target = lxusb_guest_lookup ? lxusb_guest_lookup("usb_busses") : NULL;
    if (target) *target = bus_head;
    else if (!InterlockedExchange(&warned, 1))
        fputs("lxusb: guest usb_busses symbol unavailable; legacy USB enumeration cannot be published\n", stderr);
}
static bus_node *bus_for(unsigned number)
{
    bus_node *b;
    for (b = busses; b; b = b->next) if (b->number == number) return b;
    b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->number = number;
    snprintf(b->guest.dirname, sizeof(b->guest.dirname), "%03u", number);
    b->guest.location = number;
    b->next = busses; busses = b;
    return b;
}
static int create_legacy(libusb_device *d)
{
    usb_device *g = &d->legacy;
    int i, r;
    r = libusb_get_device_descriptor(d, &g->descriptor);
    if (r) return r;
    if (g->descriptor.bNumConfigurations) {
        g->config = calloc(g->descriptor.bNumConfigurations, sizeof(*g->config));
        if (!g->config) return LXUSB_ERROR_NO_MEM;
        for (i = 0; i < g->descriptor.bNumConfigurations; ++i) {
            libusb_config_descriptor *cfg;
            r = libusb_get_config_descriptor(d, (uint8_t)i, &cfg);
            if (r) {
                /* Trees are persistent on success; on failure discard the partial set. */
                int j;
                for (j = 0; j < i; ++j) free((char *)g->config[j].interface - sizeof(*cfg));
                free(g->config); g->config = NULL;
                return r;
            }
            g->config[i] = *cfg;
        }
    }
    snprintf(g->filename, sizeof(g->filename), "%03u", d->address);
    g->dev = d; g->devnum = d->address;
    d->legacy_created = 1;
    return 0;
}
static int legacy_enumerate(int scan_devices)
{
    lxusb_device_info *info = NULL;
    libusb_device *d;
    bus_node *b;
    usb_bus *tail = NULL;
    int n, i, changes = 0, r;
    if ((r = ensure_state())) return legacy_error(r);
    EnterCriticalSection(&lock);
    n = snapshot(&info);
    if (n < 0) { LeaveCriticalSection(&lock); return legacy_error(n); }
    for (b = busses; b; b = b->next) b->present = 0;
    /* Presence and each API's last reported state are independent, so a bus
     * refresh cannot swallow a device-removal notification (or vice versa). */
    for (d = devices; d; d = d->next) d->legacy_present = 0;
    for (i = 0; i < n; ++i) {
        b = bus_for(info[i].bus);
        d = device_for(&info[i]);
        if (!b || !d) { r = LXUSB_ERROR_NO_MEM; goto failed; }
        b->present = 1;
        if (scan_devices && !d->legacy_created && (r = create_legacy(d))) goto failed;
        if (d->legacy_created) d->legacy_present = 1;
    }
    bus_head = NULL;
    for (b = busses; b; b = b->next) {
        if (!scan_devices && b->present != b->reported) {
            ++changes;
            b->reported = b->present;
        }
        b->guest.next = b->guest.prev = NULL; b->guest.devices = NULL;
        if (!b->present) continue;
        b->guest.prev = tail;
        if (tail) tail->next = &b->guest; else bus_head = &b->guest;
        tail = &b->guest;
    }
    for (d = devices; d; d = d->next) {
        usb_device *g = &d->legacy;
        if (scan_devices && d->legacy_present != d->legacy_reported) {
            ++changes;
            d->legacy_reported = d->legacy_present;
        }
        g->next = g->prev = NULL;
        if (!d->legacy_present) continue;
        b = bus_for(d->bus);
        g->bus = &b->guest;
        g->next = b->guest.devices;
        if (g->next) g->next->prev = g;
        b->guest.devices = g;
    }
    publish_busses();
    free(info);
    LeaveCriticalSection(&lock);
    return changes;
 failed:
    free(info);
    LeaveCriticalSection(&lock);
    return legacy_error(r);
}
void usb_init(void) { (void)ensure_state(); }
int usb_find_busses(void) { return legacy_enumerate(0); }
int usb_find_devices(void) { return legacy_enumerate(1); }
libusb_device_handle *usb_open(usb_device *d)
{
    libusb_device_handle *h = NULL;
    if (d) (void)libusb_open(d->dev, &h);
    return h;
}
int usb_close(libusb_device_handle *h) { libusb_close(h); return 0; }
int usb_set_configuration(libusb_device_handle *h, int value)
{
    libusb_config_descriptor *cfg;
    int r = acquire_handle(h);
    if (r) return legacy_error(r);
    r = libusb_get_config_descriptor(h->device, 0, &cfg);
    if (!r) {
        r = cfg->bConfigurationValue == value ? 0 : LXUSB_ERROR_INVALID_PARAM;
        free(cfg);
    }
    release_handle(h);
    return legacy_error(r);
}
int usb_claim_interface(libusb_device_handle *h, int iface)
{
    int r = acquire_handle(h);
    if (r) return legacy_error(r);
    r = lxusb_backend_impl.claim(h->backend, iface);
    EnterCriticalSection(&lock);
    if (!r) h->last_iface = iface;
    unref_handle_locked(h);
    LeaveCriticalSection(&lock);
    return legacy_error(r);
}
int usb_set_altinterface(libusb_device_handle *h, int alt)
{
    int iface, r = acquire_handle(h);
    if (r) return legacy_error(r);
    EnterCriticalSection(&lock);
    iface = h->last_iface;
    LeaveCriticalSection(&lock);
    r = iface < 0 ? LXUSB_ERROR_INVALID_PARAM : lxusb_backend_impl.set_alt(h->backend, iface, alt);
    release_handle(h);
    return legacy_error(r);
}
int usb_reset(libusb_device_handle *h)
{
    int r = acquire_handle(h);
    if (r) return legacy_error(r);
    r = lxusb_backend_impl.reset(h->backend);
    release_handle(h);
    return legacy_error(r);
}
int usb_control_msg(libusb_device_handle *h, int type, int request, int value, int index,
                    char *bytes, int size, int timeout)
{
    int r;
    if (size < 0 || size > 65535 || timeout < 0) return -22;
    r = libusb_control_transfer(h, (uint8_t)type, (uint8_t)request, (uint16_t)value,
                               (uint16_t)index, (unsigned char *)bytes, (uint16_t)size, (unsigned int)timeout);
    return r < 0 ? legacy_error(r) : r;
}

#define S(name) { #name, NULL, (void *)name }
static const lx_sym symbols[] = {
    S(libusb_init), S(libusb_exit), S(libusb_get_device_list), S(libusb_free_device_list),
    S(libusb_get_device_descriptor), S(libusb_get_config_descriptor), S(libusb_get_active_config_descriptor),
    S(libusb_free_config_descriptor), S(libusb_get_bus_number), S(libusb_get_device_address),
    S(libusb_open), S(libusb_close), S(libusb_claim_interface), S(libusb_release_interface),
    S(libusb_kernel_driver_active), S(libusb_detach_kernel_driver), S(libusb_control_transfer),
    S(libusb_interrupt_transfer), S(libusb_bulk_transfer), S(libusb_alloc_transfer), S(libusb_free_transfer),
    S(libusb_submit_transfer), S(libusb_cancel_transfer), S(libusb_handle_events),
    S(usb_init), S(usb_find_busses), S(usb_find_devices), S(usb_open), S(usb_close),
    S(usb_set_configuration), S(usb_claim_interface), S(usb_set_altinterface), S(usb_reset), S(usb_control_msg)
};
__declspec(dllexport) const lx_sym *lx_provider_symbols(unsigned *count)
{
    if (count) *count = sizeof(symbols) / sizeof(symbols[0]);
    return symbols;
}
