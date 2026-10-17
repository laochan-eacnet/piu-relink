# piu X11 / GLX / XF86VidMode usage and the native bridge

This investigation is for the supplied ELF32 i386 `piu`, Build ID
`3bda4c418b536bc77a6aa79db790cff14b9656c1`. Addresses below are original ELF
virtual addresses and stay unchanged in the relinked executable. Evidence is
`i686-linux-gnu-objdump -d` in WSL, including all 402070 lines of the full output,
and the copied `phoenixtools` source. The host's ARM64 `objdump` cannot decode this
ELF; use the i686-prefixed program.

## Imported entry points and all direct PLT call sites

There are **12 Xlib, 3 XF86VidMode, and 4 GLX** imports in this binary, rather than
an assumed larger generic Xlib surface. The native table additionally includes
three keyboard-conversion/query symbols for phoenixtools and the dynamically
requested swap-interval function. piu itself imports 54 GL/GLU functions, but the
provider now covers the complete public native GL 1.1/GLU surface so guest
libraries such as libftgl are not restricted to that piu-only subset.

| Symbol | PLT | Call instructions |
|---|---|---|
| XOpenDisplay | 0804f770 | 08067f45 |
| glXChooseVisual | 08050620 | 08067f72 |
| XCreateColormap | 0804fd20 | 08067fb9 |
| XGrabKeyboard | 0804f3d0 | 0806804f |
| XCreateWindow | 0804f590 | 080680f7 |
| XWarpPointer | 08050130 | 08068146 |
| XSetNormalHints | 0804f7b0 | 08068182 |
| XSetStandardProperties | 080502e0 | 080681c1 |
| glXCreateContext | 080509c0 | 080681e3 |
| XFree | 08050540 | 080681f0 |
| XMapRaised | 080509e0 | 08068208 |
| glXMakeCurrent | 0804f280 | 08068229 |
| XF86VidModeGetAllModeLines | 08050920 | 0806859b |
| XF86VidModeSwitchToMode | 0804f2f0 | 080685de |
| XF86VidModeSetViewPort | 0804f2d0 | 08068600 |
| XPending | 08050b60 | 08058681 |
| XNextEvent | 080505c0 | 0805869a |
| XLookupString | 0804fc40 | 080586d9, 08058775 |
| glXSwapBuffers | 080501e0 | listed below |

All 15 swap call instructions are:

```text
08066167 0806eafe 0806ec5a 08071fb5 08072403 08074fa7
080e2872 080e2915 080e29b9 080e2a5c 080e3577 080e361b
080e36be 080e3762 08102e6c
```

Useful focused investigation commands (from the Windows repository root):

```sh
wsl -e i686-linux-gnu-objdump -d -j .plt /mnt/d/tmp/piurelink/piu
wsl -e i686-linux-gnu-objdump -d --start-address=0x8067f30 --stop-address=0x8068620 /mnt/d/tmp/piurelink/piu
wsl -e i686-linux-gnu-objdump -d --start-address=0x8058624 --stop-address=0x80587ec /mnt/d/tmp/piurelink/piu
wsl -e i686-linux-gnu-objdump -s --start-address=0x81c5760 --stop-address=0x81c5780 /mnt/d/tmp/piurelink/piu
wsl -e i686-linux-gnu-objdump -s --start-address=0x81c6400 --stop-address=0x81c6420 /mnt/d/tmp/piurelink/piu
```

## Display macro reads and layout

The saved Display pointer is at `0x097bf92c`, the created window XID at
`0x097bf930`, and the context at `0x098043a0`. Examination of all 30 references to
the saved Display global finds the inline macro reads in the window initializer;
other references pass the pointer to the functions above.

The binary reads these public i386 Xlib fields directly:

| Object / field | Offset | Evidence / use |
|---|---:|---|
| `_XPrivDisplay.default_screen` | 0x84 / 132 | `08067f4f`, `08068033`; DefaultScreen |
| `_XPrivDisplay.screens` | 0x8c / 140 | `08067f8c`, `08067fd6`, `08068610`; screen-array base |
| `Screen` stride | 0x50 / 80 | index multiplication at `08067f82..08067f89` and `08068039..08068040` |
| `Screen.root` | 0x08 / 8 | `08067f92`, `08068045`; RootWindow |
| `Screen.black_pixel` | 0x38 / 56 | `08067f99`; background/border pixels |
| `Screen.height` | 0x10 / 16 | `08067fe8`; DisplayHeight compared with 719 |

No direct DisplayWidth read was found in this initializer or the other saved
Display uses. The bridge nevertheless fills both screen width and height,
physical dimensions, root depth/visual, colormap, white/black pixels, protocol
version, screen count, and keycode limits with valid i386-layout values. The
public `_XPrivDisplay` prefix is 176 bytes; the real Xlib allocation has additional
private data beyond that prefix. The bridge only promises the public prefix.

## Visual, attributes, and initialization order

The GLX attribute list at `0x081c5760` is the eight 32-bit integers
`{1,4,5,12,1,13,1,0}`: USE_GL, RGBA, DOUBLEBUFFER, DEPTH_SIZE=1,
STENCIL_SIZE=1, terminator. `glXChooseVisual` is called with DefaultScreen.
The returned `XVisualInfo` is immediately dereferenced without a null check:

- `visual` at +0 is passed to XCreateColormap and XCreateWindow.
- `screen` at +8 selects the root/black-pixel `Screen`.
- `depth` at +12 is passed to XCreateWindow.
- `XVisualInfo` is 40 bytes; its Visual pointer points at a separate 32-byte Visual.

`XCreateColormap(display, root, visual, AllocNone=0)` returns the attributes'
colormap. The `XSetWindowAttributes` at stack +0x6c has:

| Field | Struct offset | Initialized value |
|---|---:|---|
| background_pixel | 4 | Screen.black_pixel |
| border_pixel | 12 | Screen.black_pixel |
| event_mask | 40 | 0x28003 |
| colormap | 52 | XCreateColormap result |

The XCreateWindow value mask is **0x280a** = CWBackPixel | CWBorderPixel |
CWEventMask | CWColormap. Other attribute members are uninitialized and must not
be consulted. In particular CWOverrideRedirect is **not** set. The selected event
mask is KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask.
XCreateWindow's x/y and border width are zero, class is InputOutput=1; size is
chosen by the game's resolution/HD branches (640x480 or 1280x720).

The actual sequence in `08067f30..0806861b` is:

1. XOpenDisplay(NULL), DefaultScreen, glXChooseVisual, root/black-pixel macros.
2. XCreateColormap; optional VidMode search/switch; XGrabKeyboard on the root.
3. XCreateWindow; XWarpPointer to client (0,0).
4. XSetNormalHints with USPosition=1 and x=y=0; remaining size hints are not set.
5. XSetStandardProperties with the same title/icon-name pointer, no icon pixmap,
   no argv/argc, and the same size-hints pointer.
6. glXCreateContext(display, visual_info, share=NULL, direct=True).
7. **XFree(visual_info)**, then XMapRaised, then glXMakeCurrent.
8. `dlopen(NULL, RTLD_GLOBAL)` and `dlsym(...,"glXSwapIntervalSGI")`; if present,
   call it with interval=1 (`0806823d..0806825d`).
9. Set viewport/projection/fixed-function GL state.

The context-before-map order is important; the native test exercises it as well
as an alternative map-before-context order. `XSetNormalHints` normally resolves
to phoenixtools' override, but the provider implementation also honors the
position/size/min/max fields whose flags are present, without reading the
caller's uninitialized size members.

## VidMode array ownership and behavior

When desktop height exceeds 719 and the game's HD byte permits the branch,
`08068580` calls GetAllModeLines with an `int *count` and
`XF86VidModeModeInfo ***` out parameter. It loads **one pointer per mode** from
the returned pointer array (`080685ba`, scale 4), not an inline struct array.
It compares unsigned short hdisplay **+4** to 640 and vdisplay **+14** to 480,
then passes the selected mode pointer to SwitchToMode and calls
SetViewPort(display, screen, 0, 0).

The i386 mode-info layout is 36 bytes: dotclock +0; hdisplay +4; hskew +12;
vdisplay +14; vtotal +20; two bytes of alignment padding; flags +24; privsize
+28; private pointer +32. No private-data access was found.

**piu does not XFree the returned mode array.** There is only one XFree PLT call
in the binary, at `080681f0`, and that argument is the earlier XVisualInfo pointer
in ESI. The mode pointer-array local at stack +0xa8 is subsequently reused as
XSizeHints. The bridge still returns a correctly freeable single allocation
containing the pointer array and its one mode; the test frees it with XFree.

The provider reports only the startup primary desktop mode (measured 2944x1840),
with no private data. Timing fields are synthesized because only dimensions are
used. Consequently piu's 640x480 mode search finds no match on this desktop; it
continues through its normal resolution branch. `SwitchToMode` now acknowledges
guest requests without changing the configured native mode. `phoenixtools.ini` owns
windowed/borderless/real-fullscreen output and resolution; see
[Display configuration](opengl-x86.md#display-configuration).

## Events and input

The main loop (`08058678..080587a5`) drains XPending/XNextEvent and switches on:

- **2 / KeyPress**: call XLookupString; read `XKeyEvent.keycode` at +52;
  subtract 8 and set the corresponding game key-state byte.
- **3 / KeyRelease**: same conversion and keycode read; clear the state byte.
- **18 / UnmapNotify**: mark the main loop for exit.
- Other types are ignored. In particular there is **no** ConfigureNotify,
  Expose, or ClientMessage data-field read in this loop.

Each event occupies **96 bytes** (`long pad[24]` with i386 long=4), not the
64-bit Xlib event size. XKeyEvent is 60 bytes: display +12; window +16; root +20;
time +28; x/y +32/+36; root x/y +40/+44; state +48; keycode +52; same_screen +56.
The game passes XLookupString(event, NULL, 0, &keysym, NULL) on both press and
release; it only needs the keysym, not text. Press handling normalizes keysyms
0xff08..0xff1f to their low byte. It otherwise indexes physical key state by
keycode minus 8, including comparisons with Linux key indices 14, 83, and 87.

The bridge emits KeyPress/KeyRelease, ConfigureNotify, and Expose in the exact
guest layouts, using Win32 key, size/move, and paint messages. ConfigureNotify
and Expose retain the game's requested dimensions when output is resized/scaled.
WM_CLOSE emits a
ClientMessage carrying bridge-local WM_PROTOCOLS/WM_DELETE_WINDOW atom IDs,
**followed by UnmapNotify** so this binary actually exits. It preserves the
window/DC through the final frame; just sending ClientMessage would be ignored
by piu. XNextEvent blocks if no selected event is pending. The bounded event
queue reserves room for releasing every held key on loss of focus.

XGrabKeyboard's arguments at `0806804f` are `(display, root, False, Async=1,
Async=1, CurrentTime=0)`, before a game window exists. The provider records the
focus request and applies it when the window maps; it does not install a global
keyboard hook. XWarpPointer's arguments at `08068146` are `(display, None,
window, 0,0,0,0, 0,0)`; this becomes client-to-screen conversion and SetCursorPos.

### One consistent keycode scheme

The provider uses **X keycode = Linux evdev code + 8** for all three surfaces:
posted/native key events, XKeysymToKeycode, and XQueryKeymap's 32-byte bitmap.
Ordinary PC Set-1 scancodes map directly to evdev; extended scancodes explicitly
map keypad Enter/right Ctrl/keypad slash/right Alt/navigation/Windows/menu keys.
Pause, PrintScreen, NumLock, and F13..F24 receive their Linux evdev codes.

Keysyms/text use the US PC keyboard mapping (a deliberate game-input mapping,
not an international-layout/IME implementation). Shift, CapsLock, Control, Alt,
NumLock, and Super masks are represented. XLookupString returns a keysym even
when no text buffer is supplied and writes at most the requested Latin-1 text
byte, without forcing a NUL terminator. Unknown/unmapped key names return
NoSymbol/keycode zero. Supported names include ASCII characters/punctuation,
F1..F35 keysyms, keypad/navigation/modifier names, numeric hex keysyms and Uxxxx
Unicode keysym spelling; only physically mapped keys have nonzero keycodes.
XQueryKeymap is driven by the same tracked key state updated by the message
pump; losing focus releases held keys to prevent stuck pad inputs.

Default phoenixtools bindings resolve to:

```text
Q=24 E=26 S=39 Z=52 C=54 U=30 O=32 K=45 M=58 period=60 F1=67 F2=68 5=14
Optional card1/F3=69 card2/F4=70 (only with virtual_card_reader=true)
Control_R=105 Up=111
```

### phoenixtools' additional X11 surface

- `src/input/bindings.c` obtains XStringToKeysym and XKeysymToKeycode with
  **dlsym(RTLD_NEXT)**, caches by Display, checks distinct keycodes in 8..255,
  and resolves the 13 configured pad/service/test/coin keys. The two appended
  card bindings (`card1=F3`, `card2=F4`) participate only when
  `[input] virtual_card_reader=true`; disabled card bindings are not resolved
  or validated, and do not reserve their keycodes.
- `src/input/x11_input.c` interposes XNextEvent, calls the next provider,
  reads event type and keycode at **+52**, and handles enabled card insertion
  keys before the independent virtual-LXIO branch. Card events are hook-owned,
  never Phoenix cabinet action IDs or LXIO report inputs. Press/release events
  are consumed by setting their type to zero. Other configured bindings update
  virtual LXIO when `virtual_lxio=true`; without virtual LXIO they install the
  game's native key-index bindings using keycode minus 8. TITLE binding
  installation excludes card actions.
- Enabled card bindings call `src/safety/network_card.c`: a new press reads
  P1 `card1.txt` or P2 `card2.txt` beside the loaded INI, anchored independently
  of CWD and `runtime_dir`. Each player has a synchronized held latch and
  pending UID; repeated Windows KeyPress events do not rescan until release.
  The bridge already releases held keys on focus loss. Files are read fresh
  per press, never at startup; invalid/missing files clear only that player's
  pending scan and are recoverable by fixing the file and pressing again.
  UID text is 2–34 ASCII hexadecimal digits/even with optional surrounding
  ASCII whitespace/CRLF (regular file, at most 256 bytes).
- XQueryKeymap clears key bits and XLookupString suppresses text/keysym for
  enabled card bindings independently of `virtual_lxio`, as well as the
  virtual-LXIO bindings when that mode is enabled. Disabled card keys pass
  through unchanged. Both hooks call the next provider via RTLD_NEXT where
  appropriate.
- `src/safety/system_hooks.c:66..72` overrides XSetNormalHints with a successful
  return (1). The native implementation remains in the provider for direct
  users and non-preloaded tests.

These functions must be binder-visible Linux symbol-table entries, not only PE
exports, so RTLD_NEXT can reach them after the relinked preload module.

## Native implementation and checked scope

`bridges/gfx/gfx.c`, `input.c`, and `x11_abi.h` implement one Display, one primary
screen, and the single top-level game window. XIDs for real windows are 32-bit
HWND values; the fake root XID is 1 and the TrueColor colormap is 2.
GLX ChooseVisual selects a WGL pixel format, CreateContext creates/shares WGL
contexts, MakeCurrent binds them and resolves the three GL 1.3 procedures, and
SwapBuffers forwards to the native DC. The optional glXSwapIntervalSGI forwards
to wglSwapIntervalEXT, returning a GLX error if unavailable.

`gen_gl.py` generates cdecl wrappers for all 336 public GL 1.1 functions and all
52 glu32 exports, checking declarations against the real DLL export tables.
The Windows-only wgl/Glmf APIs and undocumented glDebugEntry are not Linux
GL/GLU entry points. Calls into opengl32/glu32 retain their native
APIENTRY/stdcall types. glActiveTexture, glClientActiveTexture, and
glCompressedTexImage2D additionally use wglGetProcAddress after the context
becomes current, with their equivalent ARB spellings supported. WGL's invalid
sentinel addresses are rejected. Calling a required unsupported extension
procedure diagnoses the missing entry and aborts rather than silently
pretending to render.

GLU callback registration is not a direct calling-convention thunk:
`glu_callbacks.c` wraps each opaque tessellator, quadric, and NURBS object with
its own guest cdecl callbacks and native object pointer. All generated
object-taking GLU APIs unwrap the handle and bracket the native call with a
thread-local active-object save/restore. Native stdcall trampolines invoke the
guest cdecl callbacks with their exact signature, including all six regular
tessellation callbacks and all six `_DATA` variants. COMBINE receives four
arguments; COMBINE_DATA receives those four plus polygon_data. Native
polygon_data is passed through unchanged. Nested tessellations restore the
outer callback object, and separate threads do not share callback context.
Callback replacement/unregistration remains per object.

Windows' exported GLU 1.2 supports GLU_ERROR callbacks for quadrics and NURBS;
the bridge adapts both. It does not invent the newer GLU 1.3 NURBS callback APIs
that glu32 does not export. GLU objects remain opaque to the guest; they must
be used and deleted through the provider functions, not passed directly to
native glu32 APIs. Concurrent access to the same object still follows native
GLU's rules, and guest exceptions/longjmp must not cross native GLU frames.

`x11_abi.h` statically checks every guest-visible struct's size and the relevant
field offsets. `probe_x11_abi.c` independently compiles the same definitions
against real Linux i386 Xorg headers and checks the layouts match. The native
test loads the provider table, follows the initialization order, renders RGBA
and DXT1 textured quads, exercises GLU double arguments, checks non-black pixels,
event boundaries, key events/bitmap agreement, close events, and fullscreen.
Exact commands and captured successful output are in
[opengl-x86.md](opengl-x86.md). A full relinked piu/phoenixtools run is outside
these standalone graphics tests; no full-game rendering/performance claim is
made here.
