/* Native self-check: real Windows IOCP/events, scripted USB devices, no driver. */
#include "usb_abi.h"
#include "../../lxrt/lxabi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__declspec(dllexport) const lx_sym *lx_provider_symbols(unsigned *count);

struct lxusb_bdev { int index, present; const uint8_t *config; int config_size; uint8_t descriptor[18]; };
struct lxusb_bhandle { lxusb_bdev *device; HANDLE port; ULONG_PTR key; int claimed, closed; };
typedef struct fake_op {
    int state;
    lxusb_bhandle *handle;
    OVERLAPPED *ov;
    uint8_t endpoint, *buffer;
    ULONG length;
} fake_op;
static CRITICAL_SECTION fake_lock;
static lxusb_bdev fake_devices[2];
static fake_op operations[32];
static lxusb_bhandle fake_handles[64];
static int fake_handle_count;
static LONG backend_closes;
static LONG hold_control, guest_frees, callback_count;
static DWORD next_error;
static int next_bytes = -1;
static unsigned char last_write[64];
static ULONG last_write_length;
static int last_alt_iface = -1, last_alt = -1, resets;
static int pending_notifications, event_notifications, port_notifications;
static usb_bus *guest_busses;
static int failures;
static uint8_t hid_config[] = {
    9,2,41,0,1,1,0,0x80,50,
    9,4,0,0,2,3,0,0,0,
    9,0x21,0x11,1,0,1,0x22,32,0,
    7,5,0x81,3,16,0,1,
    7,5,0x02,3,16,0,1
};
static uint8_t vendor_config[] = { 9,2,18,0,1,1,0,0x80,50, 9,4,2,0,0,0xff,0,0,0 };
#define CHECK(test, label) do { if (test) printf("PASS %s\n", label); \
    else { printf("FAIL %s (line %d)\n", label, __LINE__); ++failures; } } while (0)

static int fake_enumerate(lxusb_device_info *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < 2; ++i) if (fake_devices[i].present) {
        if (n < cap) { out[n].dev = &fake_devices[i]; out[n].bus = 3; out[n].address = (uint8_t)(7 + i); }
        ++n;
    }
    return n;
}
static int fake_descriptor(lxusb_bdev *d, uint8_t out[18]) { memcpy(out, d->descriptor, 18); return 0; }
static int fake_config(lxusb_bdev *d, uint8_t index, const uint8_t **out)
{
    if (index) return LXUSB_ERROR_NOT_FOUND;
    *out = d->config; return d->config_size;
}
static int fake_open(lxusb_bdev *d, HANDLE port, ULONG_PTR key, lxusb_bhandle **out)
{
    lxusb_bhandle *h;
    if (!d->present) return LXUSB_ERROR_NO_DEVICE;
    if (fake_handle_count == 64) return LXUSB_ERROR_NO_MEM;
    h = &fake_handles[fake_handle_count++];
    h->device = d; h->port = port; h->key = key; h->claimed = -1; *out = h;
    return 0;
}
static int fake_alive(lxusb_bhandle *h)
{
    if (h && !h->closed) return 1;
    puts("FAIL backend operation after close"); ++failures;
    return 0;
}
static void fake_close(lxusb_bhandle *h)
{
    int i;
    EnterCriticalSection(&fake_lock);
    if (!fake_alive(h)) { LeaveCriticalSection(&fake_lock); return; }
    for (i = 0; i < 32; ++i) if (operations[i].state && operations[i].handle == h) {
        puts("FAIL close with outstanding I/O"); ++failures;
    }
    h->closed = 1;
    InterlockedIncrement(&backend_closes);
    LeaveCriticalSection(&fake_lock);
}
static int fake_claim(lxusb_bhandle *h, int iface)
{ if (!fake_alive(h)) return LXUSB_ERROR_NO_DEVICE; h->claimed = iface; return 0; }
static int fake_release(lxusb_bhandle *h, int iface)
{ (void)iface; if (!fake_alive(h)) return LXUSB_ERROR_NO_DEVICE; h->claimed = -1; return 0; }
static int fake_alt(lxusb_bhandle *h, int iface, int alt)
{ if (!fake_alive(h)) return LXUSB_ERROR_NO_DEVICE; last_alt_iface = iface; last_alt = alt; return 0; }
static int fake_reset(lxusb_bhandle *h)
{ if (!fake_alive(h)) return LXUSB_ERROR_NO_DEVICE; ++resets; return 0; }
static void notify(fake_op *op, ULONG bytes, DWORD error)
{
    op->ov->Internal = error; op->ov->InternalHigh = bytes;
    op->state = 2;
    ++pending_notifications;
    if ((ULONG_PTR)op->ov->hEvent & 1) {
        ++event_notifications;
        SetEvent((HANDLE)((ULONG_PTR)op->ov->hEvent & ~(ULONG_PTR)1));
    } else {
        ++port_notifications;
        PostQueuedCompletionStatus(op->handle->port, bytes, op->handle->key, op->ov);
    }
}
static int fake_submit(lxusb_bhandle *h, unsigned char ep, const uint8_t setup[8],
                       uint8_t *buffer, ULONG length, OVERLAPPED *ov)
{
    fake_op *op = NULL;
    ULONG count = length;
    DWORD error;
    int i;
    EnterCriticalSection(&fake_lock);
    if (!fake_alive(h)) { LeaveCriticalSection(&fake_lock); return LXUSB_ERROR_NO_DEVICE; }
    if (!h->device->present) { LeaveCriticalSection(&fake_lock); return LXUSB_ERROR_NO_DEVICE; }
    for (i = 0; i < 32; ++i) if (!operations[i].state) { op = &operations[i]; break; }
    if (!op) { LeaveCriticalSection(&fake_lock); return LXUSB_ERROR_NO_MEM; }
    op->state = 1; op->handle = h; op->ov = ov; op->endpoint = ep;
    op->buffer = buffer; op->length = length;
    if ((ep & 0x80) || (!ep && InterlockedCompareExchange(&hold_control, 0, 0))) {
        LeaveCriticalSection(&fake_lock); return 0;
    }
    if (!ep && setup[0] == 0x80 && setup[1] == 6) {
        static const uint8_t language[] = {4,3,9,4};
        static const uint8_t product[] = {10,3,'L',0,'X',0,'I',0,'O',0};
        const uint8_t *s = setup[2] ? product : language;
        count = length < s[0] ? length : s[0];
        memcpy(buffer, s, count);
    } else if (!ep && (setup[0] & 0x80)) {
        for (i = 0; i < (int)length; ++i) buffer[i] = (uint8_t)(0xa0 + i);
    } else {
        last_write_length = length < sizeof(last_write) ? length : sizeof(last_write);
        if (last_write_length) memcpy(last_write, buffer, last_write_length);
    }
    error = next_error; next_error = 0;
    if (next_bytes >= 0) { count = (ULONG)next_bytes; next_bytes = -1; }
    notify(op, count, error);
    LeaveCriticalSection(&fake_lock);
    return 0;
}
static void fake_cancel(lxusb_bhandle *h, OVERLAPPED *ov)
{
    int i;
    EnterCriticalSection(&fake_lock);
    if (!fake_alive(h)) { LeaveCriticalSection(&fake_lock); return; }
    for (i = 0; i < 32; ++i) if (operations[i].state == 1 && operations[i].handle == h && operations[i].ov == ov) {
        notify(&operations[i], 0, ERROR_OPERATION_ABORTED); break;
    }
    LeaveCriticalSection(&fake_lock);
}
static void fake_result(lxusb_bhandle *h, OVERLAPPED *ov, ULONG *bytes, DWORD *error)
{
    int i;
    EnterCriticalSection(&fake_lock);
    if (!fake_alive(h)) {
        *bytes = 0; *error = ERROR_INVALID_HANDLE;
        LeaveCriticalSection(&fake_lock); return;
    }
    for (i = 0; i < 32; ++i) if (operations[i].state == 2 && operations[i].handle == h && operations[i].ov == ov) {
        operations[i].state = 0; --pending_notifications; break;
    }
    if (i == 32) { puts("FAIL result without notification"); ++failures; }
    *bytes = (ULONG)ov->InternalHigh; *error = (DWORD)ov->Internal;
    LeaveCriticalSection(&fake_lock);
}
const lxusb_backend lxusb_backend_impl = {
    fake_enumerate, fake_descriptor, fake_config, fake_open, fake_close,
    fake_claim, fake_release, fake_alt, fake_reset, fake_submit, fake_cancel, fake_result
};
static void guest_free(void *p) { InterlockedIncrement(&guest_frees); free(p); }
static void *lookup(const char *name)
{
    if (!strcmp(name, "usb_busses")) return &guest_busses;
    if (!strcmp(name, "free")) return (void *)guest_free;
    return NULL;
}
static int feed(ULONG bytes, DWORD error)
{
    ULONGLONG end = GetTickCount64() + 1000;
    do {
        int i;
        EnterCriticalSection(&fake_lock);
        for (i = 0; i < 32; ++i) if (operations[i].state == 1 && operations[i].endpoint == 0x81) {
            ULONG j;
            for (j = 0; j < bytes && j < operations[i].length; ++j) operations[i].buffer[j] = (uint8_t)(j + 1);
            notify(&operations[i], bytes, error);
            LeaveCriticalSection(&fake_lock);
            return 1;
        }
        LeaveCriticalSection(&fake_lock);
        Sleep(1);
    } while (GetTickCount64() < end);
    return 0;
}
static void count_callback(libusb_transfer *t) { (void)t; InterlockedIncrement(&callback_count); }
static void free_callback(libusb_transfer *t) { InterlockedIncrement(&callback_count); libusb_free_transfer(t); }
static void signal_callback(libusb_transfer *t) { SetEvent((HANDLE)t->user_data); }

typedef struct loop_state {
    libusb_transfer *transfer;
    HANDLE ready, changed;
    LONG stop, packets, timeouts, cancelled, disconnected, submit_error;
    unsigned char last_packet[16];
} loop_state;
static void piu_callback(libusb_transfer *t)
{
    loop_state *s = t->user_data;
    if (t->status == LIBUSB_TRANSFER_CANCELLED || t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        InterlockedIncrement(t->status == LIBUSB_TRANSFER_CANCELLED ? &s->cancelled : &s->disconnected);
        InterlockedExchange(&s->stop, 1);
        SetEvent(s->changed);
        return;
    }
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        memcpy(s->last_packet, t->buffer, (size_t)t->actual_length);
        InterlockedIncrement(&s->packets);
    } else if (t->status == LIBUSB_TRANSFER_TIMED_OUT) {
        InterlockedIncrement(&s->timeouts);
        t->timeout = 0;
    }
    if (libusb_submit_transfer(t)) InterlockedExchange(&s->submit_error, 1);
    SetEvent(s->changed);
}
static DWORD WINAPI piu_thread(void *arg)
{
    loop_state *s = arg;
    if (libusb_submit_transfer(s->transfer)) InterlockedExchange(&s->submit_error, 1);
    SetEvent(s->ready);
    while (!InterlockedCompareExchange(&s->stop, 0, 0))
        if (libusb_handle_events(NULL) < 0) break;
    if (libusb_cancel_transfer(s->transfer) == 0) libusb_handle_events(NULL);
    return 0;
}
static int await_count(loop_state *s, LONG *value, LONG count)
{
    ULONGLONG end = GetTickCount64() + 1500;
    while (InterlockedCompareExchange(value, 0, 0) < count) {
        if (GetTickCount64() >= end) return 0;
        WaitForSingleObject(s->changed, 20);
    }
    return 1;
}
static void run_piu_loop(libusb_device_handle *h, int disconnect)
{
    loop_state s = {0};
    HANDLE thread;
    ULONGLONG start;
    s.ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    s.changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    s.transfer = libusb_alloc_transfer(0);
    s.transfer->dev_handle = h; s.transfer->endpoint = 0x81;
    s.transfer->type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
    s.transfer->timeout = disconnect ? 0 : 40;
    s.transfer->length = 16; s.transfer->buffer = malloc(16);
    s.transfer->callback = piu_callback; s.transfer->user_data = &s;
    thread = CreateThread(NULL, 0, piu_thread, &s, 0, NULL);
    CHECK(thread && WaitForSingleObject(s.ready, 1000) == WAIT_OBJECT_0, "piu USB thread starts");
    if (disconnect) {
        CHECK(feed(0, ERROR_DEVICE_NOT_CONNECTED), "deliver device removal completion");
        CHECK(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0 && s.disconnected == 1,
              "NO_DEVICE callback stops piu thread");
    } else {
        CHECK(await_count(&s, &s.timeouts, 1), "async deadline cancels and reports TIMED_OUT");
        CHECK(feed(16, 0) && await_count(&s, &s.packets, 1), "timeout callback resubmits and queues input");
        CHECK(feed(8, 0) && await_count(&s, &s.packets, 2) && s.last_packet[7] == 8,
              "callback resubmits same transfer for next input");
        {
            libusb_transfer *other = libusb_alloc_transfer(0);
            unsigned char output[16] = {0x6a}, input[16];
            HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
            int transferred;
            CHECK(libusb_interrupt_transfer(h, 2, output, 16, &transferred, 1000) == 0 &&
                  transferred == 16 && last_write[0] == 0x6a,
                  "synchronous output coexists with blocked async event thread");
            other->dev_handle = h; other->endpoint = 0x81; other->type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
            other->buffer = input; other->length = sizeof(input); other->timeout = 25;
            other->callback = signal_callback; other->user_data = done;
            Sleep(20); /* Let the original no-deadline event wait settle. */
            CHECK(libusb_submit_transfer(other) == 0 && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0 &&
                  other->status == LIBUSB_TRANSFER_TIMED_OUT,
                  "concurrent submit wake recomputes blocked event pump deadline");
            libusb_free_transfer(other);
            CloseHandle(done);
        }
        start = GetTickCount64();
        InterlockedExchange(&s.stop, 1);
        CHECK(libusb_cancel_transfer(s.transfer) == 0, "main thread cancels blocked USB thread");
        CHECK(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0 && GetTickCount64() - start < 1000,
              "piu stop/cancel/join finishes within one second");
        CHECK(s.cancelled == 1 && !s.submit_error, "cancellation callback terminal and callback resubmits succeed");
    }
    CHECK(libusb_cancel_transfer(s.transfer) == LXUSB_ERROR_NOT_FOUND, "cancel of completed transfer is -5");
    free(s.transfer->buffer); libusb_free_transfer(s.transfer);
    CloseHandle(thread); CloseHandle(s.ready); CloseHandle(s.changed);
}

static void test_descriptors(libusb_device *d)
{
    libusb_config_descriptor *cfg = NULL;
    libusb_interface_descriptor *iface;
    const uint8_t *saved = fake_devices[0].config;
    int saved_size = fake_devices[0].config_size;
    static const uint8_t alternate[] = {
        9,2,53,0,2,1,0,0x80,50, 3,0x30,1,
        9,4,5,0,1,3,0,0,0, 7,5,0x81,3,16,0,1, 3,0x31,2,
        9,4,5,1,0,3,0,0,0, 4,0x32,3,4,
        9,4,2,0,0,0xff,0,0,0
    };
    uint8_t malformed[sizeof(hid_config)];
    CHECK(libusb_get_active_config_descriptor(d, &cfg) == 0 && cfg && cfg->bNumInterfaces == 1,
          "realistic LXIO configuration tree");
    iface = &cfg->interface[0].altsetting[0];
    CHECK(iface->bInterfaceClass == 3 && iface->bNumEndpoints == 2 && iface->extra_length == 9 &&
          iface->extra[1] == 0x21 && iface->endpoint[0].bEndpointAddress == 0x81 &&
          iface->endpoint[1].bEndpointAddress == 2 && iface->endpoint[0].wMaxPacketSize == 16,
          "HID extra descriptor and descriptor-derived interrupt IN/OUT");
    libusb_free_config_descriptor(cfg);
    fake_devices[0].config = alternate; fake_devices[0].config_size = sizeof(alternate);
    CHECK(libusb_get_config_descriptor(d, 0, &cfg) == 0 && cfg->extra_length == 3 &&
          cfg->interface[0].num_altsetting == 2 && cfg->interface[1].altsetting[0].bInterfaceNumber == 2 &&
          cfg->interface[0].altsetting[0].endpoint[0].extra_length == 3 &&
          cfg->interface[0].altsetting[1].extra_length == 4,
          "non-contiguous interface numbers, alternate settings and all extra levels");
    libusb_free_config_descriptor(cfg);
    memcpy(malformed, hid_config, sizeof(malformed)); malformed[18] = 0;
    fake_devices[0].config = malformed; fake_devices[0].config_size = sizeof(malformed);
    CHECK(libusb_get_config_descriptor(d, 0, &cfg) == LXUSB_ERROR_IO && !cfg, "zero-length descriptor rejected");
    memcpy(malformed, hid_config, sizeof(malformed)); malformed[13] = 3;
    CHECK(libusb_get_config_descriptor(d, 0, &cfg) == LXUSB_ERROR_IO && !cfg, "endpoint count mismatch rejected");
    memcpy(malformed, hid_config, sizeof(malformed)); malformed[2] = 255;
    CHECK(libusb_get_config_descriptor(d, 0, &cfg) == LXUSB_ERROR_IO && !cfg, "truncated configuration rejected");
    fake_devices[0].config = saved; fake_devices[0].config_size = saved_size;
    CHECK(libusb_get_config_descriptor(d, 1, &cfg) == LXUSB_ERROR_NOT_FOUND && !cfg, "missing config index returns -5");
}
static void test_async(libusb_device_handle *h)
{
    libusb_transfer *t = libusb_alloc_transfer(2);
    static const DWORD errors[] = { ERROR_GEN_FAILURE, ERROR_MORE_DATA, ERROR_SEM_TIMEOUT, ERROR_ACCESS_DENIED };
    static const int statuses[] = { LIBUSB_TRANSFER_STALL, LIBUSB_TRANSFER_OVERFLOW,
                                   LIBUSB_TRANSFER_TIMED_OUT, LIBUSB_TRANSFER_ERROR };
    unsigned char buffer[16] = {0x80,6,1,3,9,4,8,0};
    unsigned i;
    LONG before;
    CHECK(t && t->flags == 0 && t->num_iso_packets == 2 && t->iso_packet_desc[1].length == 0,
          "alloc transfer and iso descriptors zero-initialized");
    t->dev_handle = h; t->buffer = buffer; t->length = 16; t->endpoint = 2;
    t->type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
    CHECK(libusb_submit_transfer(t) == LXUSB_ERROR_NOT_SUPPORTED, "isochronous explicitly unsupported");
    t->type = LIBUSB_TRANSFER_TYPE_BULK_STREAM;
    CHECK(libusb_submit_transfer(t) == LXUSB_ERROR_NOT_SUPPORTED, "bulk streams explicitly unsupported");
    t->type = LIBUSB_TRANSFER_TYPE_INTERRUPT; t->flags = LIBUSB_TRANSFER_ADD_ZERO_PACKET;
    CHECK(libusb_submit_transfer(t) == LXUSB_ERROR_NOT_SUPPORTED, "zero-packet flag explicitly unsupported");
    t->flags = 0; t->endpoint = 0x81;
    CHECK(libusb_submit_transfer(t) == 0 && libusb_submit_transfer(t) == LXUSB_ERROR_BUSY,
          "submit in-flight transfer rejected as BUSY");
    CHECK(libusb_cancel_transfer(t) == 0 && libusb_cancel_transfer(t) == LXUSB_ERROR_NOT_FOUND,
          "first cancel succeeds; repeated cancelling returns -5");
    CHECK(libusb_handle_events(NULL) == 0 && t->status == LIBUSB_TRANSFER_CANCELLED,
          "cancel completion drained without waiting for 60-second cap");
    t->endpoint = 2;
    for (i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        next_error = errors[i];
        CHECK(libusb_submit_transfer(t) == 0 && libusb_handle_events(NULL) == 0 && t->status == statuses[i],
              "Win32 completion error translated to transfer status");
    }
    t->flags = LIBUSB_TRANSFER_SHORT_NOT_OK; next_bytes = 3;
    CHECK(libusb_submit_transfer(t) == 0 && libusb_handle_events(NULL) == 0 &&
          t->status == LIBUSB_TRANSFER_ERROR && t->actual_length == 3, "SHORT_NOT_OK detects short completion");
    t->flags = 0; t->type = LIBUSB_TRANSFER_TYPE_CONTROL;
    CHECK(libusb_submit_transfer(t) == 0 && libusb_handle_events(NULL) == 0 &&
          t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length == 8 && buffer[8] == 10,
          "async control parses setup and excludes setup from actual length");
    t->dev_handle = NULL;
    CHECK(libusb_submit_transfer(t) == LXUSB_ERROR_NO_DEVICE, "async NULL handle is NO_DEVICE");
    libusb_free_transfer(t);
    before = guest_frees;
    t = libusb_alloc_transfer(0); t->buffer = malloc(16); t->length = 16;
    t->dev_handle = h; t->type = LIBUSB_TRANSFER_TYPE_INTERRUPT; t->endpoint = 2;
    t->flags = LIBUSB_TRANSFER_FREE_TRANSFER | LIBUSB_TRANSFER_FREE_BUFFER; t->callback = count_callback;
    CHECK(libusb_submit_transfer(t) == 0 && libusb_handle_events(NULL) == 0 &&
          guest_frees == before + 1 && callback_count == 1, "FREE_TRANSFER finalizes after callback using guest free");
    t = libusb_alloc_transfer(0); t->buffer = malloc(1); t->length = 1;
    t->dev_handle = h; t->type = LIBUSB_TRANSFER_TYPE_BULK; t->endpoint = 2;
    t->flags = LIBUSB_TRANSFER_FREE_BUFFER; t->callback = free_callback;
    CHECK(libusb_submit_transfer(t) == 0 && libusb_handle_events(NULL) == 0 &&
          guest_frees == before + 2 && callback_count == 2, "callback may free its own completed transfer");
    {
        libusb_transfer *queued[2];
        unsigned char output[2] = {1, 2};
        LONG previous_callbacks = callback_count;
        for (i = 0; i < 2; ++i) {
            queued[i] = libusb_alloc_transfer(0);
            queued[i]->dev_handle = h; queued[i]->endpoint = 2; queued[i]->type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
            queued[i]->buffer = &output[i]; queued[i]->length = 1; queued[i]->callback = count_callback;
            CHECK(libusb_submit_transfer(queued[i]) == 0, "queue independently completed transfer");
        }
        CHECK(libusb_handle_events(NULL) == 0 && callback_count == previous_callbacks + 2,
              "one event call drains all already-queued completions");
        for (i = 0; i < 2; ++i) libusb_free_transfer(queued[i]);
    }
    run_piu_loop(h, 0);
    run_piu_loop(h, 1);
}
static void test_legacy(void)
{
    usb_bus *b;
    usb_device *d, *aux = NULL, *saved_aux;
    libusb_device_handle *h;
    unsigned char data[8] = {1,2,3,4,5,6,7,8};
    int count = 0;
    usb_init();
    CHECK(usb_find_busses() == 1 && guest_busses, "usb_find_busses publishes live guest COPY object");
    CHECK(usb_find_devices() == 2, "usb_find_devices reports two new devices");
    for (b = guest_busses; b; b = b->next) {
        CHECK(!strcmp(b->dirname, "003") && *(usb_device **)((char *)b + 0x100c) == b->devices,
              "legacy bus name and devices offset");
        for (d = b->devices; d; d = d->next) {
            ++count;
            if (d->descriptor.idVendor == 0x0d2f && d->descriptor.idProduct == 0x1010) aux = d;
        }
    }
    CHECK(count == 2 && aux && !strcmp(aux->filename, "008") &&
          *(uint32_t *)((char *)aux + 0x1018) == 0x10100d2f &&
          *(usb_config_descriptor **)((char *)aux + 0x1024) == aux->config,
          "piu legacy combined VID/PID selection and config offsets");
    CHECK(aux && aux->config[0].interface[0].altsetting[0].bInterfaceClass == 0xff,
          "legacy vendor-class 1010 configuration parsed");
    saved_aux = aux;
    CHECK(usb_find_busses() == 0 && usb_find_devices() == 0 && saved_aux == aux,
          "unchanged legacy enumeration reports no changes");
    h = usb_open(aux);
    CHECK(h && usb_set_configuration(h, aux->config[0].bConfigurationValue) == 0 &&
          usb_set_configuration(h, 2) == -22, "legacy open permits only active configuration");
    CHECK(usb_claim_interface(h, 2) == 0 && usb_set_altinterface(h, 1) == 0 &&
          last_alt_iface == 2 && last_alt == 1 && usb_reset(h) == 0 && resets == 1,
          "legacy claim, remembered-interface altsetting, reset");
    CHECK(usb_control_msg(h, 0x40, 0xae, 0, 0, (char *)data, 8, 10000) == 8 &&
          last_write_length == 8 && last_write[7] == 8, "legacy vendor 0x40/0xae eight-byte write");
    CHECK(usb_control_msg(h, 0xc0, 0xae, 0, 0, (char *)data, 8, 10000) == 8 && data[0] == 0xa0,
          "legacy vendor 0xc0/0xae eight-byte read");
    InterlockedExchange(&hold_control, 1);
    CHECK(usb_control_msg(h, 0xc0, 0xae, 0, 0, (char *)data, 8, 15) == -110,
          "legacy control timeout translated to Linux -110");
    InterlockedExchange(&hold_control, 0);
    next_error = ERROR_GEN_FAILURE;
    CHECK(usb_control_msg(h, 0xc0, 0xae, 0, 0, (char *)data, 8, 100) == -32,
          "legacy STALL translated to Linux -32");
    CHECK(usb_close(h) == 0, "legacy close returns zero");
    fake_devices[1].present = 0;
    CHECK(usb_find_devices() == 1 && guest_busses->devices && !guest_busses->devices->next,
          "departed legacy device unlinked");
    fake_devices[1].present = 1;
    CHECK(usb_find_devices() == 1, "reappearing device reported once");
    h = usb_open(saved_aux);
    CHECK(h && (guest_busses->devices == saved_aux || guest_busses->devices->next == saved_aux),
          "reappearing device retains original legacy pointer");
    usb_close(h);
    fake_devices[0].present = fake_devices[1].present = 0;
    CHECK(usb_find_busses() == 1 && !guest_busses && usb_find_devices() == 2,
          "bus refresh unlinks departed bus without swallowing device changes");
    fake_devices[0].present = fake_devices[1].present = 1;
    CHECK(usb_find_devices() == 2 && usb_find_busses() == 1 && guest_busses,
          "device refresh restores bus without swallowing bus changes");
}

typedef struct close_callback_state {
    int callbacks, status, resubmit;
    LONG closes_seen;
} close_callback_state;
static void close_callback(libusb_transfer *t)
{
    close_callback_state *s = t->user_data;
    ++s->callbacks;
    s->status = t->status;
    s->closes_seen = backend_closes;
    /* Even a successful completion racing close must not resurrect its handle. */
    s->resubmit = libusb_submit_transfer(t);
}
typedef struct sync_close_state {
    libusb_device_handle *handle;
    int legacy, result, transferred;
    unsigned char bytes[16];
} sync_close_state;
static DWORD WINAPI sync_close_thread(void *arg)
{
    sync_close_state *s = arg;
    if (s->legacy)
        s->result = usb_control_msg(s->handle, 0xc0, 0xae, 0, 0, (char *)s->bytes, 8, 250);
    else
        s->result = libusb_interrupt_transfer(s->handle, 0x81, s->bytes, 16, &s->transferred, 1000);
    return 0;
}
static int await_sync_pending(void)
{
    ULONGLONG end = GetTickCount64() + 1000;
    do {
        int i;
        EnterCriticalSection(&fake_lock);
        for (i = 0; i < 32; ++i)
            if (operations[i].state == 1 && ((ULONG_PTR)operations[i].ov->hEvent & 1)) break;
        LeaveCriticalSection(&fake_lock);
        if (i != 32) return 1;
        Sleep(1);
    } while (GetTickCount64() < end);
    return 0;
}
static void test_close_lifetimes(libusb_device *device)
{
    int i;
    for (i = 0; i < 2; ++i) {
        libusb_device_handle *h = NULL;
        libusb_transfer *t = libusb_alloc_transfer(0);
        unsigned char bytes[16] = {0};
        close_callback_state s = {0};
        LONG before = backend_closes;
        ULONGLONG started;
        CHECK(libusb_open(device, &h) == 0, "open handle for deferred async close");
        t->dev_handle = h; t->endpoint = i ? 2 : 0x81;
        t->type = LIBUSB_TRANSFER_TYPE_INTERRUPT; t->buffer = bytes; t->length = sizeof(bytes);
        t->callback = close_callback; t->user_data = &s;
        CHECK(libusb_submit_transfer(t) == 0, "start transfer before async close");
        started = GetTickCount64();
        libusb_close(h);
        CHECK(GetTickCount64() - started < 100 && backend_closes == before,
              "async close returns immediately and retains backend until notification retirement");
        CHECK(libusb_control_transfer(h, 0x80, 6, 0x0300, 0, bytes, 4, 10) == LXUSB_ERROR_NO_DEVICE,
              "closing handle rejects new synchronous calls");
        CHECK(libusb_handle_events(NULL) == 0 && s.callbacks == 1 &&
              s.status == (i ? LIBUSB_TRANSFER_COMPLETED : LIBUSB_TRANSFER_CANCELLED),
              "event pump delivers completion after nonblocking close");
        CHECK(backend_closes == before + 1 && s.closes_seen == before + 1 && s.resubmit == LXUSB_ERROR_NO_DEVICE,
              "last async result releases backend and stale-handle callback resubmit is safe");
        libusb_free_transfer(t);
    }
    for (i = 0; i < 2; ++i) {
        sync_close_state s = {0};
        HANDLE thread;
        LONG before = backend_closes;
        ULONGLONG started;
        CHECK(libusb_open(device, &s.handle) == 0, "open handle for deferred synchronous close");
        s.legacy = i;
        InterlockedExchange(&hold_control, i);
        thread = CreateThread(NULL, 0, sync_close_thread, &s, 0, NULL);
        CHECK(thread && await_sync_pending(), "another thread blocks in synchronous transfer");
        started = GetTickCount64();
        if (i) CHECK(usb_close(s.handle) == 0, "legacy close shares deferred destruction");
        else libusb_close(s.handle);
        CHECK(GetTickCount64() - started < 100 && backend_closes == before,
              "close does not block on synchronous waiter or destroy its backend");
        if (!i) CHECK(feed(8, ERROR_SUCCESS), "complete synchronous I/O after owner close");
        CHECK(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0 &&
              (i ? s.result == -110 : s.result == 0 && s.transferred == 8 && s.bytes[7] == 8),
              "synchronous call completes or times out safely after close");
        CHECK(backend_closes == before + 1, "backend closes after synchronous result is consumed");
        InterlockedExchange(&hold_control, 0);
        CloseHandle(thread);
    }
}
int main(void)
{
    libusb_context *ctx = NULL, *ctx2 = NULL;
    libusb_device **list = NULL, **again = NULL;
    libusb_device_descriptor desc;
    libusb_device_handle *h = NULL;
    unsigned char data[64] = {0};
    unsigned i, count;
    const lx_sym *symbols;
    int transferred, selected = 0, before_ports;
    InitializeCriticalSection(&fake_lock);
    lxusb_guest_lookup = lookup;
    for (i = 0; i < 2; ++i) {
        static const uint8_t descriptor[18] = {18,1,0x10,1,0,0,0,8,0x2f,0x0d,0x20,0x10,0,1,1,2,3,1};
        fake_devices[i].index = (int)i; fake_devices[i].present = 1;
        memcpy(fake_devices[i].descriptor, descriptor, 18);
    }
    fake_devices[1].descriptor[10] = 0x10;
    fake_devices[0].config = hid_config; fake_devices[0].config_size = sizeof(hid_config);
    fake_devices[1].config = vendor_config; fake_devices[1].config_size = sizeof(vendor_config);
    symbols = lx_provider_symbols(&count);
    CHECK(count == 34, "provider exports all 33 imports plus bulk transfer");
    for (i = 0; i < count; ++i) if (!symbols[i].addr || symbols[i].version) break;
    CHECK(i == count, "all provider symbols callable and unversioned");
    CHECK(libusb_init(&ctx) == 0 && libusb_init(&ctx2) == 0 && ctx == ctx2, "default context init refcount identity");
    libusb_exit(ctx2);
    CHECK(libusb_get_device_list(ctx, &list) == 2 && list && !list[2], "NULL-terminated device enumeration");
    CHECK(libusb_get_device_list(NULL, &again) == 2 && again[0] == list[0], "device pointer stable across lists");
    libusb_free_device_list(again, 1);
    for (i = 0; list[i]; ++i) {
        libusb_config_descriptor *cfg;
        unsigned j, k;
        libusb_get_device_descriptor(list[i], &desc);
        if (desc.bDeviceClass || desc.idVendor != 0x0d2f || desc.idProduct != 0x1020) continue;
        if (libusb_get_active_config_descriptor(list[i], &cfg)) continue;
        for (j = 0; j < cfg->bNumInterfaces; ++j)
            for (k = 0; k < (unsigned)cfg->interface[j].num_altsetting; ++k)
                if (cfg->interface[j].altsetting[k].bInterfaceClass == 3) selected = 1;
        libusb_free_config_descriptor(cfg);
    }
    CHECK(selected && libusb_get_bus_number(list[0]) == 3 && libusb_get_device_address(list[0]) == 7,
          "piu class-zero VID/PID then HID-interface selection walk");
    test_descriptors(list[0]);
    CHECK(libusb_open(list[0], &h) == 0 && libusb_kernel_driver_active(h, 0) == 0 &&
          libusb_detach_kernel_driver(h, 0) == LXUSB_ERROR_NOT_FOUND && libusb_claim_interface(h, 0) == 0,
          "open, kernel-driver skip and interface claim");
    CHECK(libusb_control_transfer(h, 0x80, 6, 0x0300, 0, data, 64, 1000) == 4 && data[2] == 9 && data[3] == 4,
          "piu language string descriptor control request");
    CHECK(libusb_control_transfer(h, 0x80, 6, 0x0301, 0x0409, data, 64, 1000) == 10 && data[2] == 'L',
          "piu UTF-16 product descriptor control request");
    data[0] = 0x5a;
    CHECK(libusb_interrupt_transfer(h, 2, data, 16, &transferred, 1000) == 0 && transferred == 16 && last_write[0] == 0x5a,
          "synchronous interrupt OUT data/count");
    CHECK(libusb_control_transfer(h, 0x21, 9, 0x0200, 0, data, 16, 1000) == 16,
          "HID SET_REPORT control fallback");
    before_ports = port_notifications;
    CHECK(libusb_interrupt_transfer(h, 0x81, data, 16, &transferred, 15) == LXUSB_ERROR_TIMEOUT && transferred == 0 &&
          port_notifications == before_ports && !pending_notifications,
          "sync timeout -7 consumes event notification without IOCP leak");
    CHECK(event_notifications >= 5, "synchronous transfers use event-only notifications");
    test_async(h);
    CHECK(libusb_release_interface(h, 0) == 0, "release claimed interface");
    libusb_close(h);
    test_close_lifetimes(list[0]);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    test_legacy();
    CHECK(!pending_notifications, "all completion notifications consumed");
    CHECK(backend_closes == fake_handle_count, "every opened fake backend closes exactly once");
    CHECK(!failures, "complete USB provider fake-backend suite");
    return failures ? 1 : 0;
}
