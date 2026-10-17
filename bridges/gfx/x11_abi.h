#pragma once
#include <stddef.h>
#include <stdint.h>

/* Xlib's public macro ABI on Linux i386. No Win32 types belong in these objects. */
#pragma pack(push, 4)
typedef uint32_t LXWindow;
typedef struct LXDisplay LXDisplay;
typedef struct LXVisual {
    void *ext_data;
    uint32_t visualid;
    int32_t class_id;
    uint32_t red_mask, green_mask, blue_mask;
    int32_t bits_per_rgb, map_entries;
} LXVisual;
typedef struct LXScreen {
    void *ext_data;
    LXDisplay *display;
    LXWindow root;
    int32_t width, height, mwidth, mheight, ndepths;
    void *depths;
    int32_t root_depth;
    LXVisual *root_visual;
    void *default_gc;
    uint32_t cmap, white_pixel, black_pixel;
    int32_t max_maps, min_maps, backing_store, save_unders, root_input_mask;
} LXScreen;
struct LXDisplay {
    void *ext_data, *private1;
    int32_t fd, private2, proto_major_version, proto_minor_version;
    char *vendor;
    uint32_t private3, private4, private5;
    int32_t private6;
    void *resource_alloc;
    int32_t byte_order, bitmap_unit, bitmap_pad, bitmap_bit_order, nformats;
    void *pixmap_format;
    int32_t private8, release;
    void *private9, *private10;
    int32_t qlen;
    uint32_t last_request_read, request;
    void *private11, *private12, *private13, *private14;
    uint32_t max_request_size;
    void *db, *private15;
    char *display_name;
    int32_t default_screen, nscreens;
    LXScreen *screens;
    uint32_t motion_buffer, private16;
    int32_t min_keycode, max_keycode;
    void *private17, *private18;
    int32_t private19;
    char *xdefaults;
};
typedef struct LXVisualInfo {
    LXVisual *visual;
    uint32_t visualid;
    int32_t screen, depth, class_id;
    uint32_t red_mask, green_mask, blue_mask;
    int32_t colormap_size, bits_per_rgb;
} LXVisualInfo;
typedef struct LXSetWindowAttributes {
    uint32_t background_pixmap, background_pixel, border_pixmap, border_pixel;
    int32_t bit_gravity, win_gravity, backing_store;
    uint32_t backing_planes, backing_pixel;
    int32_t save_under, event_mask, do_not_propagate_mask, override_redirect;
    uint32_t colormap, cursor;
} LXSetWindowAttributes;
typedef struct LXSizeHints {
    int32_t flags, x, y, width, height, min_width, min_height, max_width, max_height;
    int32_t width_inc, height_inc;
    struct { int32_t x, y; } min_aspect, max_aspect;
    int32_t base_width, base_height, win_gravity;
} LXSizeHints;
typedef struct LXVidModeInfo {
    uint32_t dotclock;
    uint16_t hdisplay, hsyncstart, hsyncend, htotal, hskew;
    uint16_t vdisplay, vsyncstart, vsyncend, vtotal;
    uint32_t flags;
    int32_t privsize;
    int32_t *private_data;
} LXVidModeInfo;
#define LX_EVENT_HEADER int32_t type; uint32_t serial; int32_t send_event; LXDisplay *display

typedef struct LXAnyEvent { LX_EVENT_HEADER; LXWindow window; } LXAnyEvent;
typedef struct LXKeyEvent {
    LX_EVENT_HEADER;
    LXWindow window, root, subwindow;
    uint32_t time;
    int32_t x, y, x_root, y_root;
    uint32_t state, keycode;
    int32_t same_screen;
} LXKeyEvent;
typedef struct LXConfigureEvent {
    LX_EVENT_HEADER;
    LXWindow event, window;
    int32_t x, y, width, height, border_width;
    LXWindow above;
    int32_t override_redirect;
} LXConfigureEvent;
typedef struct LXExposeEvent {
    LX_EVENT_HEADER;
    LXWindow window;
    int32_t x, y, width, height, count;
} LXExposeEvent;
typedef struct LXUnmapEvent {
    LX_EVENT_HEADER;
    LXWindow event, window;
    int32_t from_configure;
} LXUnmapEvent;
typedef struct LXClientMessageEvent {
    LX_EVENT_HEADER;
    LXWindow window;
    uint32_t message_type;
    int32_t format;
    union { char b[20]; int16_t s[10]; int32_t l[5]; } data;
} LXClientMessageEvent;
typedef union LXEvent {
    int32_t type;
    LXAnyEvent xany;
    LXKeyEvent xkey;
    LXConfigureEvent xconfigure;
    LXExposeEvent xexpose;
    LXUnmapEvent xunmap;
    LXClientMessageEvent xclient;
    int32_t pad[24];
} LXEvent;
#undef LX_EVENT_HEADER
#pragma pack(pop)

#define LX_SIZE(t, n) _Static_assert(sizeof(t) == (n), #t " Linux i386 size")
#define LX_OFF(t, f, n) _Static_assert(offsetof(t, f) == (n), #t "." #f " Linux i386 offset")
LX_SIZE(void *, 4);
LX_SIZE(LXVisual, 32); LX_OFF(LXVisual, visualid, 4); LX_OFF(LXVisual, red_mask, 12);
LX_SIZE(LXScreen, 80); LX_OFF(LXScreen, root, 8); LX_OFF(LXScreen, width, 12);
LX_OFF(LXScreen, height, 16); LX_OFF(LXScreen, root_depth, 36); LX_OFF(LXScreen, root_visual, 40);
LX_OFF(LXScreen, white_pixel, 52); LX_OFF(LXScreen, black_pixel, 56);
LX_SIZE(LXDisplay, 176); LX_OFF(LXDisplay, qlen, 88); LX_OFF(LXDisplay, default_screen, 132);
LX_OFF(LXDisplay, nscreens, 136); LX_OFF(LXDisplay, screens, 140); LX_OFF(LXDisplay, min_keycode, 152);
LX_SIZE(LXVisualInfo, 40); LX_OFF(LXVisualInfo, visual, 0); LX_OFF(LXVisualInfo, visualid, 4);
LX_OFF(LXVisualInfo, screen, 8); LX_OFF(LXVisualInfo, depth, 12); LX_OFF(LXVisualInfo, red_mask, 20);
LX_SIZE(LXSetWindowAttributes, 60); LX_OFF(LXSetWindowAttributes, background_pixel, 4);
LX_OFF(LXSetWindowAttributes, border_pixel, 12); LX_OFF(LXSetWindowAttributes, event_mask, 40);
LX_OFF(LXSetWindowAttributes, override_redirect, 48); LX_OFF(LXSetWindowAttributes, colormap, 52);
LX_SIZE(LXSizeHints, 72); LX_OFF(LXSizeHints, x, 4); LX_OFF(LXSizeHints, width, 12);
LX_OFF(LXSizeHints, min_width, 20); LX_OFF(LXSizeHints, max_width, 28);
LX_SIZE(LXVidModeInfo, 36); LX_OFF(LXVidModeInfo, hdisplay, 4); LX_OFF(LXVidModeInfo, vdisplay, 14);
LX_OFF(LXVidModeInfo, flags, 24); LX_OFF(LXVidModeInfo, privsize, 28); LX_OFF(LXVidModeInfo, private_data, 32);
LX_SIZE(LXAnyEvent, 20); LX_OFF(LXAnyEvent, display, 12); LX_OFF(LXAnyEvent, window, 16);
LX_SIZE(LXKeyEvent, 60); LX_OFF(LXKeyEvent, root, 20); LX_OFF(LXKeyEvent, time, 28);
LX_OFF(LXKeyEvent, x, 32); LX_OFF(LXKeyEvent, x_root, 40); LX_OFF(LXKeyEvent, state, 48);
LX_OFF(LXKeyEvent, keycode, 52); LX_OFF(LXKeyEvent, same_screen, 56);
LX_SIZE(LXConfigureEvent, 52); LX_OFF(LXConfigureEvent, window, 20); LX_OFF(LXConfigureEvent, width, 32);
LX_OFF(LXConfigureEvent, height, 36); LX_SIZE(LXExposeEvent, 40); LX_OFF(LXExposeEvent, width, 28);
LX_SIZE(LXUnmapEvent, 28); LX_OFF(LXUnmapEvent, window, 20);
LX_SIZE(LXClientMessageEvent, 48); LX_OFF(LXClientMessageEvent, message_type, 20);
LX_OFF(LXClientMessageEvent, data, 28); LX_SIZE(LXEvent, 96);
#undef LX_SIZE
#undef LX_OFF

LXDisplay *lx_XOpenDisplay(const char *name);
LXVisualInfo *lx_glXChooseVisual(LXDisplay *d, int screen, const int *attributes);
uint32_t lx_XCreateColormap(LXDisplay *d, LXWindow window, LXVisual *visual, int alloc);
LXWindow lx_XCreateWindow(LXDisplay *d, LXWindow parent, int x, int y, unsigned width, unsigned height,
                         unsigned border_width, int depth, unsigned class_id, LXVisual *visual,
                         uint32_t value_mask, const LXSetWindowAttributes *attributes);
int lx_XMapRaised(LXDisplay *d, LXWindow window);
int lx_XSetNormalHints(LXDisplay *d, LXWindow window, const LXSizeHints *hints);
int lx_XSetStandardProperties(LXDisplay *d, LXWindow window, const char *name, const char *icon_name,
                             uint32_t icon, char **argv, int argc, const LXSizeHints *hints);
int lx_XFree(void *pointer);
int lx_XPending(LXDisplay *d);
int lx_XNextEvent(LXDisplay *d, LXEvent *event);
int lx_XLookupString(const LXKeyEvent *event, char *buffer, int capacity, uint32_t *keysym, void *compose);
uint32_t lx_XStringToKeysym(const char *name);
unsigned lx_XKeysymToKeycode(LXDisplay *d, uint32_t keysym);
int lx_XQueryKeymap(LXDisplay *d, char bitmap[32]);
int lx_XGrabKeyboard(LXDisplay *d, LXWindow window, int owner_events, int pointer_mode, int keyboard_mode, uint32_t time);
int lx_XWarpPointer(LXDisplay *d, LXWindow source, LXWindow destination, int sx, int sy,
                    unsigned sw, unsigned sh, int dx, int dy);
int lx_XF86VidModeGetAllModeLines(LXDisplay *d, int screen, int *count, LXVidModeInfo ***modes);
int lx_XF86VidModeSwitchToMode(LXDisplay *d, int screen, const LXVidModeInfo *mode);
int lx_XF86VidModeSetViewPort(LXDisplay *d, int screen, int x, int y);
void *lx_glXCreateContext(LXDisplay *d, const LXVisualInfo *visual, void *share, int direct);
int lx_glXMakeCurrent(LXDisplay *d, LXWindow window, void *context);
void lx_glXSwapBuffers(LXDisplay *d, LXWindow window);
int lx_glXSwapIntervalSGI(int interval);
