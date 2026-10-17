/* Native provider semantics: one Display/primary screen/game window; guest
 * structs use Linux i386 layouts, while GL/GLU thunks convert cdecl to stdcall.
 * phoenixtools.ini [display]: windowed (default), borderless, or real fullscreen;
 * width/height select output size (0 = automatic), not the game's render size.
 * A game-sized RGBA8/depth-stencil FBO letterboxes scaled output when supported.
 * GDI background/erase and native buffers are black before the first game frame.
 * WM_CLOSE queues ClientMessage plus UnmapNotify: piu exits only on the latter.
 * Input uses US-PC keysyms with X keycode = evdev + 8 and tracked keymap state.
 * [debug] snapshot_dir enables pre-swap BMP diagnostics (period default 60);
 * capture preserves read/pack state. Swap count/FPS is reported every 300 swaps.
 * This DLL has no lxrt dependency. Standalone rendering/input tests are recorded
 * in docs/opengl-x86.md; full relinked-game behavior is not claimed by them. */
#include "gfx_internal.h"
#include <GL/gl.h>
#include <GL/glu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../../lxrt/lxabi.h"
#include "../common/lxini.h"
#include <limits.h>

#define ROOT_WINDOW 1u
#define EVENT_CAPACITY 1024u
#define KEY_PRESS_MASK 1u
#define KEY_RELEASE_MASK 2u
#define EXPOSURE_MASK (1u << 15)
#define STRUCTURE_MASK (1u << 17)

static LXDisplay display;
static LXScreen screen;
static LXVisual root_visual;
static LXEvent events[EVENT_CAPACITY];
static unsigned event_head, event_count;
static int initialized, keyboard_grab;
enum DisplayMode { DISPLAY_WINDOWED, DISPLAY_BORDERLESS, DISPLAY_FULLSCREEN };
static struct {
    enum DisplayMode mode;
    int width, height, changed, suspended, applying;
    MONITORINFOEXA monitor;
    DEVMODEA desktop, target;
} output;
static uint32_t next_serial;
static struct {
    HWND hwnd;
    HDC dc;
    uint32_t mask;
    int override_redirect, closing;
    int game_width, game_height;
    LXSizeHints hints;
} window;
static const char class_name[] = "lxgfx.X11.WGL";

static int config_dimension(const char *key)
{
    char value[64], *end;
    if (!lx_ini_get("display", key, value, sizeof(value))) return 0;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (!*value || *end || errno || n < 0 || n > INT_MAX) {
        fprintf(stderr, "lxgfx: invalid [display] %s '%s'; using automatic\n", key, value);
        return 0;
    }
    return (int)n;
}
static void display_config(void)
{
    char mode[64];
    output.mode = DISPLAY_WINDOWED;
    if (lx_ini_get("display", "mode", mode, sizeof(mode))) {
        if (!_stricmp(mode, "borderless")) output.mode = DISPLAY_BORDERLESS;
        else if (!_stricmp(mode, "fullscreen")) output.mode = DISPLAY_FULLSCREEN;
        else if (_stricmp(mode, "windowed"))
            fprintf(stderr, "lxgfx: invalid [display] mode '%s'; using windowed\n", mode);
    }
    output.width = config_dimension("width"); output.height = config_dimension("height");
    char ignored[64];
    if (output.mode == DISPLAY_BORDERLESS &&
        (lx_ini_get("display", "width", ignored, sizeof(ignored)) || lx_ini_get("display", "height", ignored, sizeof(ignored))))
        fputs("lxgfx: [display] width/height ignored in borderless mode\n", stderr);
}
static void display_restore(void)
{
    if (!output.changed) return;
    LONG result = ChangeDisplaySettingsExA(output.monitor.szDevice, &output.desktop, NULL, 0, NULL);
    if (result != DISP_CHANGE_SUCCESSFUL)
        fprintf(stderr, "lxgfx: cannot restore desktop mode on %s (result=%ld)\n", output.monitor.szDevice, result);
    else output.changed = 0;
}
static void display_cover(HWND hwnd)
{
    MONITORINFOEXA monitor = {0}; monitor.cbSize = sizeof(monitor);
    HMONITOR h = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoA(h, (MONITORINFO *)&monitor)) return;
    RECT r = monitor.rcMonitor;
    SetWindowPos(hwnd, output.mode == DISPLAY_FULLSCREEN ? HWND_TOPMOST : HWND_NOTOPMOST,
                 r.left, r.top, r.right-r.left, r.bottom-r.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
}
static void display_apply(HWND hwnd)
{
    if (output.mode != DISPLAY_FULLSCREEN || output.applying) return;
    output.applying = 1;
    if (output.target.dmPelsWidth != output.desktop.dmPelsWidth ||
        output.target.dmPelsHeight != output.desktop.dmPelsHeight) {
        LONG result = ChangeDisplaySettingsExA(output.monitor.szDevice, &output.target, NULL, CDS_FULLSCREEN, NULL);
        if (result == DISP_CHANGE_SUCCESSFUL) {
            output.changed = 1;
            fprintf(stderr, "lxgfx: fullscreen mode %lux%lu on %s\n",
                    output.target.dmPelsWidth, output.target.dmPelsHeight, output.monitor.szDevice);
        } else {
            fprintf(stderr, "lxgfx: fullscreen mode %lux%lu failed on %s (result=%ld); using borderless\n",
                    output.target.dmPelsWidth, output.target.dmPelsHeight, output.monitor.szDevice, result);
            display_restore();
            output.mode = DISPLAY_BORDERLESS;
        }
    }
    output.suspended = 0;
    display_cover(hwnd);
    if (wglGetCurrentDC() == window.dc) gfx_render_black();
    output.applying = 0;
}
static void display_shutdown(void) { display_restore(); }

static void event_push(LXEvent *event)
{
    if (event_count == EVENT_CAPACITY) {
        fputs("lxgfx: X event queue overflow (guest is not consuming events)\n", stderr);
        abort();
    }
    event->xany.serial = ++next_serial;
    event->xany.display = &display;
    events[(event_head + event_count) % EVENT_CAPACITY] = *event;
    display.qlen = (int)++event_count;
}
static void key_event(HWND hwnd, unsigned ev, int down)
{
    if (!ev || ev >= 248) return;
    uint32_t state = gfx_modifiers();
    gfx_set_key(ev, down);
    if (!(window.mask & (down ? KEY_PRESS_MASK : KEY_RELEASE_MASK))) return;
    LXEvent e = {0};
    e.xkey.type = down ? 2 : 3;
    e.xkey.window = (LXWindow)(uintptr_t)hwnd;
    e.xkey.root = ROOT_WINDOW;
    e.xkey.time = GetTickCount();
    POINT p = {0}; GetCursorPos(&p);
    e.xkey.x_root = p.x; e.xkey.y_root = p.y;
    ScreenToClient(hwnd, &p); e.xkey.x = p.x; e.xkey.y = p.y;
    e.xkey.state = state; e.xkey.keycode = ev + 8; e.xkey.same_screen = 1;
    event_push(&e);
}
static void close_events(HWND hwnd)
{
    if (window.closing) return;
    window.closing = 1;
    LXEvent e = {0};
    e.xclient.type = 33; e.xclient.window = (LXWindow)(uintptr_t)hwnd;
    e.xclient.message_type = 0x100; /* bridge-local WM_PROTOCOLS atom */
    e.xclient.format = 32; e.xclient.data.l[0] = 0x101; /* WM_DELETE_WINDOW */
    e.xclient.send_event = 1;
    event_push(&e);
    /* piu handles UnmapNotify (18), not WM_DELETE_WINDOW (33). Keep its DC alive
       through the final frame, while giving it the event that exits its loop. */
    e = (LXEvent){0}; e.xunmap.type = 18;
    e.xunmap.event = e.xunmap.window = (LXWindow)(uintptr_t)hwnd;
    event_push(&e);
}
static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (!GetWindowLongPtrA(hwnd, GWLP_USERDATA)) {
        if (message == WM_NCCREATE) {
            CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
            if (cs->lpCreateParams) SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        }
        if (!GetWindowLongPtrA(hwnd, GWLP_USERDATA)) return DefWindowProcA(hwnd, message, wp, lp);
    }
    switch (message) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: key_event(hwnd, gfx_evdev(wp, lp), 1); return 0;
    case WM_KEYUP: case WM_SYSKEYUP: key_event(hwnd, gfx_evdev(wp, lp), 0); return 0;
    case WM_KILLFOCUS:
        for (unsigned ev = 1; ev < 248; ++ev) if (gfx_key_down(ev)) key_event(hwnd, ev, 0);
        return 0;
    case WM_ACTIVATEAPP:
        if (output.mode == DISPLAY_FULLSCREEN && !output.applying) {
            if (!wp) {
                output.applying = 1; output.suspended = 1;
                ShowWindow(hwnd, SW_MINIMIZE); display_restore();
                output.applying = 0;
            } else if (output.suspended) {
                output.suspended = 0;
                ShowWindow(hwnd, SW_RESTORE); display_apply(hwnd);
            }
        }
        break;
    case WM_DISPLAYCHANGE:
        if (!output.applying && !output.suspended && output.mode != DISPLAY_WINDOWED) display_cover(hwnd);
        if (wglGetCurrentDC() == window.dc) gfx_render_black();
        break;
    case WM_DESTROY:
        display_restore();
        if (wglGetCurrentDC() == window.dc) gfx_render_destroy();
        if (window.dc) { ReleaseDC(hwnd, window.dc); window.dc = NULL; }
        window.hwnd = NULL;
        return 0;
    case WM_SIZE: case WM_MOVE: {
        RECT client; GetClientRect(hwnd, &client);
        gfx_render_size(window.game_width, window.game_height, client.right, client.bottom);
        if (window.mask & STRUCTURE_MASK) {
            POINT p = {0}; ClientToScreen(hwnd, &p);
            LXEvent e = {0}; e.xconfigure.type = 22;
            e.xconfigure.event = e.xconfigure.window = (LXWindow)(uintptr_t)hwnd;
            e.xconfigure.x = p.x; e.xconfigure.y = p.y;
            e.xconfigure.width = window.game_width; e.xconfigure.height = window.game_height;
            e.xconfigure.override_redirect = window.override_redirect;
            event_push(&e);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint; BeginPaint(hwnd, &paint); EndPaint(hwnd, &paint);
        if (window.mask & EXPOSURE_MASK) {
            LXEvent e = {0}; e.xexpose.type = 12; e.xexpose.window = (LXWindow)(uintptr_t)hwnd;
            e.xexpose.x = e.xexpose.y = 0;
            e.xexpose.width = window.game_width; e.xexpose.height = window.game_height;
            event_push(&e);
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT client; GetClientRect(hwnd, &client);
        FillRect((HDC)wp, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_CLOSE: close_events(hwnd); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m = (MINMAXINFO *)lp;
        DWORD style = (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE);
        if (window.hints.flags & (1 << 4)) {
            RECT r = {0, 0, window.hints.min_width, window.hints.min_height};
            AdjustWindowRectEx(&r, style, FALSE, 0);
            m->ptMinTrackSize.x = r.right-r.left; m->ptMinTrackSize.y = r.bottom-r.top;
        }
        if (window.hints.flags & (1 << 5)) {
            RECT r = {0, 0, window.hints.max_width, window.hints.max_height};
            AdjustWindowRectEx(&r, style, FALSE, 0);
            m->ptMaxTrackSize.x = r.right-r.left; m->ptMaxTrackSize.y = r.bottom-r.top;
        }
        return 0;
    }
    case WM_SETCURSOR:
        if ((output.mode != DISPLAY_WINDOWED || window.override_redirect) && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    }
    return DefWindowProcA(hwnd, message, wp, lp);
}
void gfx_pump(void)
{
    MSG msg;
    /* Reserve enough space for WM_KILLFOCUS to release every held key. */
    while (event_count < EVENT_CAPACITY - 256 && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) close_events(window.hwnd);
        else { TranslateMessage(&msg); DispatchMessageA(&msg); }
    }
}
LXDisplay *lx_XOpenDisplay(const char *name)
{
    (void)name;
    if (initialized) return &display;
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
    display_config();
    atexit(display_shutdown);
    WNDCLASSA wc = {0}; wc.style = CS_OWNDC; wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL); wc.hCursor = LoadCursorA(NULL, IDC_ARROW); wc.lpszClassName = class_name;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    display.fd = -1; display.proto_major_version = 11; display.vendor = "lxgfx Win32/WGL";
    display.byte_order = 0; display.bitmap_unit = display.bitmap_pad = 32;
    display.max_request_size = 65535; display.display_name = ":0";
    display.nscreens = 1; display.screens = &screen; display.min_keycode = 8; display.max_keycode = 255;
    screen.display = &display; screen.root = ROOT_WINDOW;
    screen.width = GetSystemMetrics(SM_CXSCREEN); screen.height = GetSystemMetrics(SM_CYSCREEN);
    HDC desktop = GetDC(NULL);
    screen.mwidth = GetDeviceCaps(desktop, HORZSIZE); screen.mheight = GetDeviceCaps(desktop, VERTSIZE);
    ReleaseDC(NULL, desktop);
    screen.root_depth = 24; screen.root_visual = &root_visual; screen.cmap = 2;
    screen.white_pixel = 0xffffff; screen.max_maps = screen.min_maps = 1;
    root_visual.class_id = 4; root_visual.red_mask = 0xff0000; root_visual.green_mask = 0xff00;
    root_visual.blue_mask = 0xff; root_visual.bits_per_rgb = 8; root_visual.map_entries = 256;
    initialized = 1;
    return &display;
}
static uint32_t color_mask(unsigned bits, unsigned shift)
{
    return bits >= 32 ? UINT32_MAX : ((1u << bits) - 1) << shift;
}
LXVisualInfo *lx_glXChooseVisual(LXDisplay *d, int number, const int *attrs)
{
    if (d != &display || number != 0) return NULL;
    PIXELFORMATDESCRIPTOR p = {0}; p.nSize = sizeof(p); p.nVersion = 1;
    p.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    p.iPixelType = PFD_TYPE_RGBA; p.cColorBits = 24; p.cDepthBits = 24;
    if (attrs) for (const int *a = attrs; *a; ++a) {
        switch (*a) {
        case 1: case 4: break; /* GLX_USE_GL, GLX_RGBA */
        case 5: p.dwFlags |= PFD_DOUBLEBUFFER; break;
        case 6: p.dwFlags |= PFD_STEREO; break;
        case 2: p.cColorBits = (BYTE)*++a; break;
        case 3: if (*++a) return NULL; break; /* only layer zero */
        case 7: p.cAuxBuffers = (BYTE)*++a; break;
        case 8: p.cRedBits = (BYTE)*++a; break; case 9: p.cGreenBits = (BYTE)*++a; break;
        case 10: p.cBlueBits = (BYTE)*++a; break; case 11: p.cAlphaBits = (BYTE)*++a; break;
        case 12: p.cDepthBits = *++a ? (BYTE)(*a < 24 ? 24 : *a) : 0; break;
        case 13: p.cStencilBits = *++a ? (BYTE)(*a < 8 ? 8 : *a) : 0; break;
        case 14: p.cAccumRedBits = (BYTE)*++a; break; case 15: p.cAccumGreenBits = (BYTE)*++a; break;
        case 16: p.cAccumBlueBits = (BYTE)*++a; break; case 17: p.cAccumAlphaBits = (BYTE)*++a; break;
        default: return NULL;
        }
    }
    HWND probe = CreateWindowA(class_name, "", WS_POPUP, 0, 0, 8, 8, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!probe) return NULL;
    HDC dc = GetDC(probe);
    int format = ChoosePixelFormat(dc, &p);
    PIXELFORMATDESCRIPTOR actual = {0};
    int described = format && DescribePixelFormat(dc, format, sizeof(actual), &actual);
    ReleaseDC(probe, dc); DestroyWindow(probe);
    if (!described || actual.iPixelType != PFD_TYPE_RGBA || (actual.dwFlags & p.dwFlags) != p.dwFlags) return NULL;
    LXVisualInfo *v = (LXVisualInfo *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    root_visual.visualid = (uint32_t)format;
    root_visual.red_mask = color_mask(actual.cRedBits, actual.cRedShift);
    root_visual.green_mask = color_mask(actual.cGreenBits, actual.cGreenShift);
    root_visual.blue_mask = color_mask(actual.cBlueBits, actual.cBlueShift);
    screen.root_depth = actual.cRedBits + actual.cGreenBits + actual.cBlueBits;
    v->visual = &root_visual; v->visualid = (uint32_t)format; v->screen = number; v->depth = screen.root_depth;
    v->class_id = 4; v->red_mask = root_visual.red_mask; v->green_mask = root_visual.green_mask;
    v->blue_mask = root_visual.blue_mask; v->colormap_size = 256; v->bits_per_rgb = actual.cRedBits;
    return v;
}
uint32_t lx_XCreateColormap(LXDisplay *d, LXWindow w, LXVisual *v, int alloc)
{
    (void)w; (void)alloc;
    return d == &display && v ? 2u : 0u; /* TrueColor needs no mutable palette. */
}
LXWindow lx_XCreateWindow(LXDisplay *d, LXWindow parent, int x, int y, unsigned width, unsigned height,
                         unsigned border, int depth, unsigned class_id, LXVisual *visual,
                         uint32_t mask, const LXSetWindowAttributes *attrs)
{
    (void)border; (void)depth;
    if (d != &display || parent != ROOT_WINDOW || class_id != 1 || !visual || window.hwnd) return 0;
    window.mask = attrs && (mask & (1 << 11)) ? (uint32_t)attrs->event_mask : 0;
    window.override_redirect = attrs && (mask & (1 << 9)) && attrs->override_redirect;
    window.game_width = (int)width; window.game_height = (int)height;
    output.monitor.cbSize = sizeof(output.monitor);
    RECT requested = {x, y, x + (LONG)width, y + (LONG)height};
    HMONITOR monitor = MonitorFromRect(&requested, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoA(monitor, (MONITORINFO *)&output.monitor)) return 0;
    output.desktop.dmSize = sizeof(output.desktop);
    if (!EnumDisplaySettingsA(output.monitor.szDevice, ENUM_CURRENT_SETTINGS, &output.desktop)) return 0;
    output.target = output.desktop;
    output.target.dmPelsWidth = output.width ? (DWORD)output.width : output.desktop.dmPelsWidth;
    output.target.dmPelsHeight = output.height ? (DWORD)output.height : output.desktop.dmPelsHeight;
    output.target.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    DWORD style = output.mode == DISPLAY_WINDOWED ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    RECT r = {0, 0, output.width ? output.width : (LONG)width, output.height ? output.height : (LONG)height};
    if (output.mode != DISPLAY_WINDOWED) {
        r = output.monitor.rcMonitor; x = r.left; y = r.top;
    } else AdjustWindowRectEx(&r, style, FALSE, 0);
    window.hwnd = CreateWindowExA(0, class_name, "piu", style, x, y, r.right-r.left, r.bottom-r.top,
                                  NULL, NULL, GetModuleHandleA(NULL), &window);
    if (!window.hwnd) return 0;
    window.dc = GetDC(window.hwnd);
    PIXELFORMATDESCRIPTOR p;
    if (!DescribePixelFormat(window.dc, (int)visual->visualid, sizeof(p), &p) ||
        !SetPixelFormat(window.dc, (int)visual->visualid, &p)) {
        DestroyWindow(window.hwnd); memset(&window, 0, sizeof(window)); return 0;
    }
    if (output.mode == DISPLAY_FULLSCREEN) display_apply(window.hwnd);
    else if (output.mode == DISPLAY_BORDERLESS) display_cover(window.hwnd);
    RECT client; GetClientRect(window.hwnd, &client);
    gfx_render_size(window.game_width, window.game_height, client.right, client.bottom);
    fprintf(stderr, "lxgfx: display %s game=%dx%d output=%ldx%ld\n",
            output.mode == DISPLAY_WINDOWED ? "windowed" : output.mode == DISPLAY_BORDERLESS ? "borderless" : "fullscreen",
            window.game_width, window.game_height, client.right, client.bottom);
    return (LXWindow)(uintptr_t)window.hwnd;
}
int lx_XMapRaised(LXDisplay *d, LXWindow w)
{
    if (d != &display || (HWND)(uintptr_t)w != window.hwnd) return 0;
    ShowWindow(window.hwnd, SW_SHOW); BringWindowToTop(window.hwnd);
    if (keyboard_grab) { SetForegroundWindow(window.hwnd); SetFocus(window.hwnd); }
    UpdateWindow(window.hwnd);
    return 1;
}
int lx_XSetNormalHints(LXDisplay *d, LXWindow w, const LXSizeHints *h)
{
    if (d != &display || (HWND)(uintptr_t)w != window.hwnd || !h) return 0;
    window.hints.flags = h->flags;
    if (h->flags & (1 << 4)) {
        window.hints.min_width = h->min_width; window.hints.min_height = h->min_height;
    }
    if (h->flags & (1 << 5)) {
        window.hints.max_width = h->max_width; window.hints.max_height = h->max_height;
    }
    if (output.mode != DISPLAY_WINDOWED) return 1;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (!(h->flags & (1 | 4))) flags |= SWP_NOMOVE;
    if (!(h->flags & (2 | 8))) flags |= SWP_NOSIZE;
    RECT r = {0};
    if (!(flags & SWP_NOSIZE)) {
        r.right = output.width ? output.width : h->width;
        r.bottom = output.height ? output.height : h->height;
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrA(window.hwnd, GWL_STYLE), FALSE, 0);
    }
    int x = flags & SWP_NOMOVE ? 0 : h->x;
    int y = flags & SWP_NOMOVE ? 0 : h->y;
    return SetWindowPos(window.hwnd, NULL, x, y, r.right-r.left, r.bottom-r.top, flags) != 0;
}
int lx_XSetStandardProperties(LXDisplay *d, LXWindow w, const char *name, const char *icon_name,
                             uint32_t icon, char **argv, int argc, const LXSizeHints *hints)
{
    (void)icon_name; (void)icon; (void)argv; (void)argc;
    if (d != &display || (HWND)(uintptr_t)w != window.hwnd) return 0;
    if (name) SetWindowTextA(window.hwnd, name);
    if (hints) return lx_XSetNormalHints(d, w, hints);
    return 1;
}
int lx_XFree(void *p) { free(p); return 1; }
int lx_XPending(LXDisplay *d) { if (d != &display) return 0; gfx_pump(); return (int)event_count; }
int lx_XNextEvent(LXDisplay *d, LXEvent *e)
{
    if (d != &display || !e) return -1;
    while (!event_count) { gfx_pump(); if (!event_count) WaitMessage(); }
    *e = events[event_head]; event_head = (event_head + 1) % EVENT_CAPACITY;
    display.qlen = (int)--event_count; display.last_request_read = e->xany.serial;
    return 0;
}
int lx_XGrabKeyboard(LXDisplay *d, LXWindow w, int owner, int pointer_mode, int keyboard_mode, uint32_t time)
{
    (void)owner; (void)pointer_mode; (void)keyboard_mode; (void)time;
    if (d != &display || (w != ROOT_WINDOW && (HWND)(uintptr_t)w != window.hwnd)) return 3; /* GrabNotViewable */
    keyboard_grab = 1;
    if (window.hwnd) { SetForegroundWindow(window.hwnd); SetFocus(window.hwnd); }
    return 0; /* GrabSuccess */
}
int lx_XWarpPointer(LXDisplay *d, LXWindow src, LXWindow dest, int sx, int sy, unsigned sw, unsigned sh, int dx, int dy)
{
    if (d != &display) return 0;
    POINT cursor; GetCursorPos(&cursor);
    if (src) {
        POINT relative = cursor;
        if (src != ROOT_WINDOW) ScreenToClient((HWND)(uintptr_t)src, &relative);
        if (relative.x < sx || relative.y < sy || (sw && relative.x >= sx + (int)sw) || (sh && relative.y >= sy + (int)sh)) return 1;
    }
    POINT p = {dx, dy};
    if (!dest) { p.x += cursor.x; p.y += cursor.y; }
    else if (dest != ROOT_WINDOW) ClientToScreen((HWND)(uintptr_t)dest, &p);
    return SetCursorPos(p.x, p.y) != 0;
}
int lx_XF86VidModeGetAllModeLines(LXDisplay *d, int number, int *count, LXVidModeInfo ***modes)
{
    if (d != &display || number != 0 || !count || !modes) return 0;
    struct ModeBlock { LXVidModeInfo *list[1]; LXVidModeInfo mode; };
    struct ModeBlock *b = (struct ModeBlock *)calloc(1, sizeof(*b));
    if (!b) { *count = 0; *modes = NULL; return 0; }
    b->list[0] = &b->mode;
    b->mode.hdisplay = b->mode.hsyncstart = b->mode.hsyncend = b->mode.htotal = (uint16_t)screen.width;
    b->mode.vdisplay = b->mode.vsyncstart = b->mode.vsyncend = b->mode.vtotal = (uint16_t)screen.height;
    DEVMODEA dm = {0}; dm.dmSize = sizeof(dm);
    unsigned hz = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
    b->mode.dotclock = (uint32_t)((uint64_t)screen.width * screen.height * hz / 1000);
    *count = 1; *modes = b->list;
    return 1;
}
int lx_XF86VidModeSwitchToMode(LXDisplay *d, int number, const LXVidModeInfo *m)
{
    /* Guest VidMode requests are advisory; phoenixtools.ini owns the native mode. */
    return d == &display && number == 0 && m != NULL;
}
int lx_XF86VidModeSetViewPort(LXDisplay *d, int number, int x, int y)
{
    return d == &display && number == 0 && x == 0 && y == 0;
}
void *lx_glXCreateContext(LXDisplay *d, const LXVisualInfo *v, void *share, int direct)
{
    (void)direct;
    if (d != &display || !v || !window.dc || (int)v->visualid != GetPixelFormat(window.dc)) return NULL;
    HGLRC rc = wglCreateContext(window.dc);
    if (rc && share && !wglShareLists((HGLRC)share, rc)) { wglDeleteContext(rc); return NULL; }
    return rc;
}
int lx_glXMakeCurrent(LXDisplay *d, LXWindow w, void *rc)
{
    if (!rc && !w) return wglMakeCurrent(NULL, NULL) != 0;
    if (d != &display || (HWND)(uintptr_t)w != window.hwnd) return 0;
    if (!wglMakeCurrent(window.dc, (HGLRC)rc)) return 0;
    gfx_load_extensions();
    gfx_render_current();
    return 1;
}
/* Snapshot storage is allocated only when enabled, then reused across frames.
 * OpenGL returns bottom-up RGBA; BMP BI_RGB stores bottom-up BGRA. */
static struct {
    int initialized;
    wchar_t *path;
    size_t prefix_length;
    unsigned every, frames;
    unsigned char *pixels;
    size_t capacity;
} snapshot;
static void snapshot_init(void)
{
    snapshot.initialized = 1;
    char period[64];
    wchar_t dir[LX_INI_PATH_CAPACITY];
    snapshot.every = 60;
    if (lx_ini_get("debug", "snapshot_every", period, sizeof(period)) && *period) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(period, &end, 10);
        if (period[0] != '-' && !*end && !errno && value) snapshot.every = value;
    }
    if (!lx_ini_get_path("debug", "snapshot_dir", dir, sizeof(dir) / sizeof(dir[0]))) return;
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "lxgfx: cannot create snapshot directory %ls (Win32=%lu)\n", dir, GetLastError());
        return;
    }
    size_t length = wcslen(dir);
    snapshot.path = (wchar_t *)malloc((length + 64) * sizeof(*snapshot.path));
    if (!snapshot.path) {
        fputs("lxgfx: cannot allocate snapshot path\n", stderr);
        return;
    }
    wmemcpy(snapshot.path, dir, length);
    if (length && dir[length - 1] != L'/' && dir[length - 1] != L'\\') snapshot.path[length++] = L'/';
    snapshot.path[length] = L'\0';
    snapshot.prefix_length = length;
}
static void snapshot_frame(unsigned long long frame)
{
    RECT client;
    if (wglGetCurrentDC() != window.dc || !GetClientRect(window.hwnd, &client)) {
        fprintf(stderr, "lxgfx: snapshot frame %llu requires the window's current WGL context\n", frame);
        return;
    }
    LONG width = window.game_width, height = window.game_height;
    if (width <= 0 || height <= 0) return;
    uint64_t bytes = (uint64_t)width * (uint64_t)height * 4;
    _Static_assert(sizeof(BITMAPFILEHEADER) == 14 && sizeof(BITMAPINFOHEADER) == 40, "BMP disk headers");
    const DWORD header_bytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    if (bytes > UINT32_MAX - header_bytes) {
        fprintf(stderr, "lxgfx: snapshot frame %llu exceeds BMP file size limit\n", frame);
        return;
    }
    size_t image_bytes = (size_t)bytes;
    if (image_bytes > snapshot.capacity) {
        unsigned char *pixels = (unsigned char *)realloc(snapshot.pixels, image_bytes);
        if (!pixels) {
            fprintf(stderr, "lxgfx: cannot allocate %zu snapshot bytes\n", image_bytes);
            return;
        }
        snapshot.pixels = pixels;
        snapshot.capacity = image_bytes;
    }
    /* Row length and skip offsets matter even with RGBA/byte pixels: leaving
       guest pack settings active could otherwise write outside this buffer. */
    GLint read_buffer, pack_alignment, pack_row_length, pack_skip_rows, pack_skip_pixels;
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
    glReadBuffer(gfx_render_scaled() ? 0x8ce0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, snapshot.pixels);
    glReadBuffer((GLenum)read_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);

    uint64_t red = 0, green = 0, blue = 0;
    for (size_t offset = 0; offset < image_bytes; offset += 4) {
        unsigned char r = snapshot.pixels[offset], b = snapshot.pixels[offset + 2];
        red += r; green += snapshot.pixels[offset + 1]; blue += b;
        snapshot.pixels[offset] = b; snapshot.pixels[offset + 2] = r;
    }
    BITMAPFILEHEADER file_header = {0};
    file_header.bfType = 0x4d42;
    file_header.bfSize = header_bytes + (DWORD)image_bytes;
    file_header.bfOffBits = header_bytes;
    BITMAPINFOHEADER info_header = {0};
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = width; info_header.biHeight = height;
    info_header.biPlanes = 1; info_header.biBitCount = 32; info_header.biCompression = BI_RGB;
    info_header.biSizeImage = (DWORD)image_bytes;
    swprintf(snapshot.path + snapshot.prefix_length, 48, L"frame-%llu.bmp", frame);
    FILE *file = _wfopen(snapshot.path, L"wb");
    if (!file) {
        fprintf(stderr, "lxgfx: cannot write snapshot %ls (errno=%d)\n", snapshot.path, errno);
        return;
    }
    int written = fwrite(&file_header, sizeof(file_header), 1, file) == 1 &&
                  fwrite(&info_header, sizeof(info_header), 1, file) == 1 &&
                  fwrite(snapshot.pixels, image_bytes, 1, file) == 1;
    if (fclose(file)) written = 0;
    if (!written) {
        int error = errno;
        _wremove(snapshot.path);
        fprintf(stderr, "lxgfx: cannot complete snapshot %ls (errno=%d)\n", snapshot.path, error);
        return;
    }
    double pixels = (double)(image_bytes / 4);
    fprintf(stderr, "lxgfx: snapshot frame %llu -> %ls (mean rgb %.2f,%.2f,%.2f)\n",
            frame, snapshot.path, red / pixels, green / pixels, blue / pixels);
}
void lx_glXSwapBuffers(LXDisplay *d, LXWindow w)
{
    if (d != &display || (HWND)(uintptr_t)w != window.hwnd) return;
    static unsigned long long swaps;
    static unsigned fps_frames;
    static LARGE_INTEGER frequency, fps_start;
    if (!snapshot.initialized) {
        snapshot_init();
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&fps_start);
    }
    ++swaps;
    if (snapshot.path && ++snapshot.frames == snapshot.every) {
        snapshot.frames = 0;
        snapshot_frame(swaps);
    }
    if (++fps_frames == 300) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG elapsed = now.QuadPart - fps_start.QuadPart;
        double fps = elapsed > 0 ? (double)fps_frames * frequency.QuadPart / elapsed : 0;
        fprintf(stderr, "lxgfx: swaps=%llu fps=%.2f\n", swaps, fps);
        fps_start = now; fps_frames = 0;
    }
    /* [debug] no_present=true: render but never present (glFinish instead of SwapBuffers). Windows
     * throttles presentation of occluded windows / locked sessions to a few frames per second;
     * this keeps headless diagnostic runs (snapshots read GL_BACK) at full game speed. */
    static int no_present = -1;
    if (no_present < 0)
        no_present = lx_ini_get_bool("debug", "no_present", 0);
    gfx_render_present();
    if (no_present)
        glFinish();
    else
        SwapBuffers(window.dc);
}
PROC gfx_extension(const char *name)
{
    PROC p = wglGetProcAddress(name);
    if ((uintptr_t)p <= 3 || (intptr_t)p == -1) return NULL;
    return p;
}
__declspec(noreturn) void gfx_missing_extension(const char *name)
{
    fprintf(stderr, "lxgfx: required OpenGL entry point %s unavailable in current WGL context\n", name);
    abort();
}
int lx_glXSwapIntervalSGI(int interval)
{
    typedef BOOL (WINAPI *SwapInterval)(int);
    SwapInterval p = (SwapInterval)gfx_extension("wglSwapIntervalEXT");
    if (interval <= 0) return 6; /* GLX_BAD_VALUE */
    return p && p(interval) ? 0 : 5; /* GLX_BAD_CONTEXT on unavailable context/extension */
}

#define LX_GFX_IMPLEMENTATION
#include "gl_generated.h"
#define SYMBOL(name) {#name, NULL, (void *)lx_##name},
static const lx_sym symbols[] = {
    SYMBOL(XOpenDisplay) SYMBOL(XCreateColormap) SYMBOL(XCreateWindow) SYMBOL(XMapRaised)
    SYMBOL(XSetStandardProperties) SYMBOL(XSetNormalHints) SYMBOL(XFree) SYMBOL(XPending) SYMBOL(XNextEvent)
    SYMBOL(XLookupString) SYMBOL(XStringToKeysym) SYMBOL(XKeysymToKeycode) SYMBOL(XQueryKeymap)
    SYMBOL(XGrabKeyboard) SYMBOL(XWarpPointer)
    SYMBOL(XF86VidModeGetAllModeLines) SYMBOL(XF86VidModeSwitchToMode) SYMBOL(XF86VidModeSetViewPort)
    SYMBOL(glXChooseVisual) SYMBOL(glXCreateContext) SYMBOL(glXMakeCurrent) SYMBOL(glXSwapBuffers) SYMBOL(glXSwapIntervalSGI)
    LX_GL_SYMBOLS(SYMBOL)
};
#undef SYMBOL
__declspec(dllexport) const lx_sym *lx_provider_symbols(unsigned *count)
{
    *count = sizeof(symbols) / sizeof(symbols[0]);
    return symbols;
}
