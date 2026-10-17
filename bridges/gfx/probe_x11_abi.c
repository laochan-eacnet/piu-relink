/* Compile on Linux i386 against the real Xorg headers; no X server/library needed. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/xf86vmode.h>
#include <stdio.h>
#include "x11_abi.h"
#define SIZE(lx, x) _Static_assert(sizeof(lx) == sizeof(x), #lx " size matches Xorg")
#define OFF(lx, x, field) _Static_assert(offsetof(lx, field) == offsetof(x, field), #lx "." #field " matches Xorg")
SIZE(LXDisplay, *(_XPrivDisplay)0);
OFF(LXDisplay, __typeof__(*(_XPrivDisplay)0), default_screen);
OFF(LXDisplay, __typeof__(*(_XPrivDisplay)0), screens);
OFF(LXDisplay, __typeof__(*(_XPrivDisplay)0), nscreens);
OFF(LXDisplay, __typeof__(*(_XPrivDisplay)0), qlen);
OFF(LXDisplay, __typeof__(*(_XPrivDisplay)0), min_keycode);
SIZE(LXScreen, Screen); OFF(LXScreen, Screen, root); OFF(LXScreen, Screen, width);
OFF(LXScreen, Screen, height); OFF(LXScreen, Screen, black_pixel);
OFF(LXScreen, Screen, root_visual); OFF(LXScreen, Screen, root_depth);
SIZE(LXVisual, Visual); OFF(LXVisual, Visual, visualid); OFF(LXVisual, Visual, red_mask);
SIZE(LXVisualInfo, XVisualInfo); OFF(LXVisualInfo, XVisualInfo, visual);
OFF(LXVisualInfo, XVisualInfo, screen); OFF(LXVisualInfo, XVisualInfo, depth);
SIZE(LXSetWindowAttributes, XSetWindowAttributes);
OFF(LXSetWindowAttributes, XSetWindowAttributes, background_pixel);
OFF(LXSetWindowAttributes, XSetWindowAttributes, border_pixel);
OFF(LXSetWindowAttributes, XSetWindowAttributes, event_mask);
OFF(LXSetWindowAttributes, XSetWindowAttributes, override_redirect);
OFF(LXSetWindowAttributes, XSetWindowAttributes, colormap);
SIZE(LXSizeHints, XSizeHints); OFF(LXSizeHints, XSizeHints, width); OFF(LXSizeHints, XSizeHints, min_width);
SIZE(LXVidModeInfo, XF86VidModeModeInfo); OFF(LXVidModeInfo, XF86VidModeModeInfo, hdisplay);
OFF(LXVidModeInfo, XF86VidModeModeInfo, vdisplay); OFF(LXVidModeInfo, XF86VidModeModeInfo, flags);
_Static_assert(offsetof(LXVidModeInfo, private_data) == offsetof(XF86VidModeModeInfo, private), "VidMode private pointer");
SIZE(LXAnyEvent, XAnyEvent); SIZE(LXKeyEvent, XKeyEvent); OFF(LXKeyEvent, XKeyEvent, time);
OFF(LXKeyEvent, XKeyEvent, keycode); OFF(LXKeyEvent, XKeyEvent, state);
OFF(LXKeyEvent, XKeyEvent, same_screen); SIZE(LXConfigureEvent, XConfigureEvent);
OFF(LXConfigureEvent, XConfigureEvent, width); OFF(LXConfigureEvent, XConfigureEvent, height);
SIZE(LXExposeEvent, XExposeEvent); SIZE(LXUnmapEvent, XUnmapEvent);
SIZE(LXClientMessageEvent, XClientMessageEvent); OFF(LXClientMessageEvent, XClientMessageEvent, data);
SIZE(LXEvent, XEvent);
int main(void)
{
    printf("PASS Linux i386 ABI: Display=%zu Screen=%zu Visual=%zu XVisualInfo=%zu XSetWindowAttributes=%zu XSizeHints=%zu VidMode=%zu XEvent=%zu\n",
           sizeof(*(_XPrivDisplay)0), sizeof(Screen), sizeof(Visual), sizeof(XVisualInfo), sizeof(XSetWindowAttributes), sizeof(XSizeHints), sizeof(XF86VidModeModeInfo), sizeof(XEvent));
    printf("Display.default_screen=%zu screens=%zu Screen.root=%zu height=%zu black_pixel=%zu XKeyEvent.keycode=%zu\n",
           offsetof(__typeof__(*(_XPrivDisplay)0), default_screen), offsetof(__typeof__(*(_XPrivDisplay)0), screens),
           offsetof(Screen, root), offsetof(Screen, height), offsetof(Screen, black_pixel), offsetof(XKeyEvent, keycode));
    return 0;
}
