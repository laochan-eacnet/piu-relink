# Native x86 OpenGL on the Adreno host

Measured 2026-10-16 with `bridges/gfx/wgltest.c`, compiled as PE32/i386 with the
repository's `WIN_CC` flags. The program creates a Win32 window, selects an RGBA,
double-buffered pixel format, creates a legacy WGL context, prints capabilities,
and clears/swaps eight frames. It runs directly on Windows under Prism; WSL is
not involved in rendering.

## Driver versus compatibility pack

This machine has `Microsoft.D3DMappingLayers` **1.2608.3.0** installed. The actual
x86 ICD loaded by the probe is:

```text
C:\Program Files\WindowsApps\Microsoft.D3DMappingLayers_1.2608.3.0_arm64__8wekyb3d8bbwe\x86\OpenGLOn12.dll
```

Thus the measured implementation is the **OpenCL, OpenGL & Vulkan Compatibility
Pack's Mesa OpenGL-on-D3D12 implementation**, accelerated by the Qualcomm Adreno
D3D12 driver, not a Qualcomm native x86 OpenGL ICD and not Microsoft's old GDI
Generic software OpenGL 1.1 implementation. The probe identifies the loaded ICD,
not just the GPU name. No experiment disabling/uninstalling the compatibility
pack was performed, so native-ICD-only behavior is not claimed.

Observed output (function addresses vary with ASLR):

```text
GL_VENDOR=Microsoft Corporation
GL_RENDERER=D3D12 (Qualcomm(R) Adreno(TM) X1-85 GPU)
GL_VERSION=4.6 (Compatibility Profile) Mesa 26.3.0-devel (git-e3fc7749c6)
ARB_multitexture=1 ARB_texture_compression=1 S3TC=1
wglGetProcAddress: glActiveTexture=6F980D80 glClientActiveTexture=6F982770 glCompressedTexImage2D=6F983920
OpenGL ICD=C:\Program Files\WindowsApps\Microsoft.D3DMappingLayers_1.2608.3.0_arm64__8wekyb3d8bbwe\x86\OpenGLOn12.dll
```

All three queried functions resolve successfully after making the context current.
The compatibility profile supports piu's immediate-mode/fixed-function rendering.
The native bridge also actually uploads and samples a DXT1/S3TC texture in the
acceptance test, rather than relying on extension strings alone.

## Build and native acceptance commands

Run from the repository root using Windows MSYS bash. On this workstation a bare
`bash` command resolves to Windows' WSL launcher and cannot build against the
Windows SDK using `C:/...` paths; use the explicit MSYS executable:

```sh
C:/msys64/usr/bin/bash.exe bridges/gfx/build.sh
D:/tmp/piurelink/out/wgltest.exe
D:/tmp/piurelink/out/test_gfx.exe 12
D:/tmp/piurelink/out/test_gfx.exe 8 --map-first
D:/tmp/piurelink/out/test_display.exe
D:/tmp/piurelink/out/test_glu.exe
```

The build sources `. tools/env.sh` and generates cdecl-to-stdcall wrappers for
every public GL 1.1 function and every glu32 export, cross-checked against the
native DLL export tables rather than a game-specific import list. It preserves
the three WGL-resolved extension functions. Callback-bearing GLU objects use
custom handles and stdcall-to-cdecl callback adapters; see the coverage and
callback regression notes below. It builds `out/win/lxgfx.dll`,
`out/test_gfx.exe`, `out/test_display.exe`, `out/test_glu.exe`, and `out/wgltest.exe`.
`test_gfx.exe` locates `win/lxgfx.dll` relative to its own executable, not the
current working directory. `test_display.exe` copies itself and the DLL into a
temporary directory and writes its own `phoenixtools.ini`. The provider has no lxrt dependency.

Captured initial test output (before expanding the original 77-symbol provider
to the full public GL/GLU surface):

```text
provider symbols=77
Display: screen=0 root=1 desktop=2944x1840 black=0
VidMode: count=1 mode=2944x1840 private=0
GL_VENDOR=Microsoft Corporation
GL_RENDERER=D3D12 (Qualcomm(R) Adreno(TM) X1-85 GPU)
GL_VERSION=4.6 (Compatibility Profile) Mesa 26.3.0-devel (git-e3fc7749c6)
binding Q=24 binding E=26 binding S=39 binding Z=52 binding C=54 binding U=30 binding O=32 binding K=45 binding M=58 binding period=60 binding F1=67 binding F2=68 binding 5=14
GLU cdecl/double forwarding=PASS
frame=0 texture=RGBA pixel=64,160,224,255
frame=1 texture=DXT1 pixel=255,0,0,255
key type=2 code=24 keysym=0x71 state=0x10 text_len=1 text0=0x71
keymap frame=2 Q=1 Control_R=0 Up=0
key type=3 code=24 keysym=0x71 state=0x10 text_len=1 text0=0x71
keymap frame=3 Q=0 Control_R=0 Up=0
key type=2 code=105 keysym=0xffe4 state=0x10 text_len=0 text0=0x00
key type=2 code=111 keysym=0xff52 state=0x14 text_len=0 text0=0x00
keymap frame=4 Q=0 Control_R=1 Up=1
key type=3 code=111 keysym=0xff52 state=0x14 text_len=0 text0=0x00
key type=3 code=105 keysym=0xffe4 state=0x14 text_len=0 text0=0x00
keymap frame=5 Q=0 Control_R=0 Up=0
PASS frames=12 key_events=6 ConfigureNotify=2 Expose=1 ClientMessage=1 UnmapNotify=1
```

The historical `--map-first` and VidMode-driven borderless runs printed the
same renderer and pixel values and ended with:

```text
PASS frames=8 key_events=6 ConfigureNotify=2 Expose=1 ClientMessage=1 UnmapNotify=1
```

The test loads every called guest-facing function from `lx_provider_symbols`,
including GL and GLU, rather than linking those cdecl functions directly. Only
its independent `glGetError` checks and WGL cleanup call the native stdcall APIs.
It draws alternating uncompressed RGBA and compressed DXT1 textured quads,
checks GLU double-argument projection/view transforms, reads non-black rendered
pixels, swaps frames, checks XEvent write-boundary canaries, and translates
posted Win32 Q/right-Control/Up key messages into X events and keymap bits.
These keyboard checks are automated message-injection checks, not a claim of a
manual physical-keyboard test or a complete piu/phoenixtools integration run.

## PE and Linux ABI inspection

```sh
C:/msys64/clangarm64/bin/llvm-readobj.exe --file-headers --coff-imports out/win/lxgfx.dll
wsl -e bash -lc 'i686-linux-gnu-gcc -std=gnu11 -Wall -Wextra -Werror -static -idirafter /usr/include /mnt/d/tmp/piurelink/bridges/gfx/probe_x11_abi.c -o /mnt/d/tmp/piurelink/out/probe_x11_abi && qemu-i386 /mnt/d/tmp/piurelink/out/probe_x11_abi'
```

`llvm-readobj` reported `COFF-i386`, `IMAGE_FILE_MACHINE_I386`, PE32 magic `0x10B`,
and only USER32, GDI32, OPENGL32, GLU32, and KERNEL32 DLL imports (static CRT).
The Linux probe uses the real Xorg Xlib/Xutil/XF86VidMode headers; in this WSL
installation `libxxf86vm-dev` was installed for the mode-info declaration.

```text
PASS Linux i386 ABI: Display=176 Screen=80 Visual=32 XVisualInfo=40 XSetWindowAttributes=60 XSizeHints=72 VidMode=36 XEvent=96
Display.default_screen=132 screens=140 Screen.root=8 height=16 black_pixel=56 XKeyEvent.keycode=52
```

All native builds also compile the fixed size/offset assertions in
`bridges/gfx/x11_abi.h`. See [x11-usage.md](x11-usage.md) for binary call sites,
field offsets, ownership, and event/input behavior.

## Complete GL/GLU coverage and callback ABI regression

The provider was expanded after the relinked game reached libftgl font loading
and reported an unresolved `glPushClientAttrib` (integration-owner observation).
The original piu-only import list was insufficient for its guest libraries.
The generator now cross-checks the native x86 exports and emits:

- **336** public OpenGL 1.1 entry points.
- **52** glu32 entry points, including the legacy polygon APIs.
- The original **3** WGL-resolved extensions.
- **382 generated wrapper definitions plus 9 custom GLU object/callback APIs**,
  totaling **391 GL/GLU entries** and **414 provider symbols** overall.

No glu32 export is omitted. Windows-only wgl/Glmf exports and the undocumented
diagnostic `glDebugEntry` are not Linux GL/GLU APIs and are excluded from this
public-API coverage. The generator errors on missing public signatures rather
than creating guessed stack-cleanup wrappers.

GLU objects are opaque guest handles containing their native object pointer and
per-object guest callbacks. All object-taking wrappers enter/restore a
thread-local active handle. Native stdcall trampolines call the guest cdecl
functions for all twelve tessellation callback signatures, including polygon
data and four-argument COMBINE/five-argument COMBINE_DATA. Quadric and NURBS
GLU_ERROR callbacks use the same convention adaptation. Native GLU 1.2 does not
export the later GLU 1.3 NURBS tessellation callback APIs. The opaque handles
must be used through the provider rather than passed directly to native GLU;
same-object thread safety and prohibition on exceptions/longjmp through native
frames remain unchanged.

Commands run after the expansion:

```sh
C:/msys64/usr/bin/bash.exe bridges/gfx/build.sh
D:/tmp/piurelink/out/test_glu.exe
D:/tmp/piurelink/out/test_gfx.exe 12
```

The build completed with `-Wall -Wextra -Werror` and printed:

```text
Generated 382 cdecl wrappers and 9 custom entries (336 GL, 52 GLU, 3 WGL-resolved extensions)
```

The native callback test performs real self-intersecting-polygon tessellation
without a GL context. It verifies every regular/DATA callback, COMBINE-generated
vertices and weights, polygon-data identity, two-object isolation, replacement,
null unregister/fallback, a nested tessellation, and deterministic two-thread
isolation. All registered guest callbacks are explicitly cdecl. Output:

```text
PASS tess regular begin=195 vertex=1170 end=195 edge=195 error=197 combine=195
PASS tess DATA begin=196 vertex=1170 end=196 edge=196 error=196 combine=196
PASS polygons=396 ESP_checks=396 nested=1 threaded=256 substituted_vertices=6/6 quadric_errors=3 nurbs_errors=3(last=100900) unregister=PASS
```

The graphics regression test printed `provider symbols=414`, the same Adreno
D3D12 renderer as above, and:

```text
GLU cdecl/double forwarding=PASS
GL1.1 attrib/list forwarding=PASS
frame=0 texture=RGBA pixel=64,160,224,255
frame=1 texture=DXT1 pixel=255,0,0,255
PASS frames=12 key_events=6 ConfigureNotify=2 Expose=1 ClientMessage=1 UnmapNotify=1
```

The added attrib/list checks call the newly available glPushClientAttrib,
glPopClientAttrib, glPushAttrib, glPopAttrib, display-list creation/use/deletion,
glIsList, and glNormal3d through the provider table. Full-game integration is
left to the integration owner; these checks do not rerun the failing game.

## Display configuration

The bridge reads **`phoenixtools.ini` beside the executable**, sharing the game
and runtime configuration file. Restart the process after changing it:

```ini
[display]
mode = windowed
width = 1920
height = 1080
```

| Key | Default | Behavior |
|---|---|---|
| `mode` | `windowed` | `windowed`, `borderless`, or `fullscreen` (case-insensitive) |
| `width`, `height` | `0` (automatic) | Nonnegative decimal output dimensions; each missing/zero dimension is automatic |

Invalid mode/dimension values are named on stderr with an `lxgfx:` prefix and
use that key's default. Windowed mode uses a decorated, resizable window; its
client size is the configured output size, or the game's requested size for
automatic dimensions. Borderless mode uses an undecorated popup covering the
window's monitor at its existing desktop resolution; width/height are ignored
and their presence is logged. Fullscreen uses a topmost popup on that monitor.
Configured non-desktop dimensions request a real mode change using
`ChangeDisplaySettingsEx(CDS_FULLSCREEN)`; automatic dimensions use the desktop
size and do not change modes. A failed request logs its result and falls back
to borderless. On application deactivation, fullscreen minimizes and restores
the desktop mode; activation restores the window and reapplies the requested
mode. Window destruction and normal process shutdown restore the desktop mode.
Windows also owns restoration when a process is forcibly terminated.

Guest XF86VidMode switch requests return success without overriding this
configuration. Monitor selection starts from the game's requested window
position. The bridge opts into per-monitor-v2 DPI awareness so physical output
pixel sizes remain correct after display-mode changes. No refresh-rate or
monitor-selection key is provided.

The game's render size remains **1280x720 (HD) or 640x480 (SD)** regardless of
output size. If the client size differs, the bridge renders into a game-sized
**RGBA8 color + DEPTH24_STENCIL8** framebuffer and linearly blits it into the
largest aspect-preserving rectangle in the window, with black letterbox or
pillarbox bars. Window resizes update that rectangle and switch between the
offscreen and direct paths as necessary. Equal game/output sizes use the
original direct path without an FBO. This requires OpenGL 3.0 or
`GL_ARB_framebuffer_object` and its WGL entry points; if unavailable or FBO
creation fails, a diagnostic is printed and rendering remains direct (there is
then **no output scaling**, so larger/smaller windows can expose background or
crop the game).

The initial viewport/scissor and X ConfigureNotify/Expose sizes remain
game-sized. Visual queries (including double buffering and color/depth/stencil
bits) continue to describe the window's selected visual, not the internal FBO.
Guest `glReadBuffer`/`glDrawBuffer` front/back selections address the game color
attachment when scaling, including readback at game coordinates. Swapping
preserves framebuffer bindings, read/draw selections, scissor, color write
mask, clear color, and guest attribute-stack semantics. Snapshots contain the
**game image at game resolution**, without the output bars. There is one game
window and one active rendering context; guest-created/bound FBOs and independent
front/back image history in the emulated default FBO are not supported. Input
and focus handling are unchanged, with the cursor hidden in borderless/fullscreen.

The window is **black from first show, before OpenGL starts drawing**: its
Win32 class has a `BLACK_BRUSH` background, and `WM_ERASEBKGND` fills the client
black. The first successful `glXMakeCurrent`, new framebuffer setup, and display
mode changes prime the native window buffers black and present once, preserving
guest GL state. These priming presents happen even with `[debug] no_present=true`,
but do not count as game swaps, snapshots, or FPS samples. Subsequent scaled
presentation clears the whole native back buffer black before blitting content,
so both popup backgrounds and letterbox/pillarbox areas stay black.

### Display regression (2026-10-17)

`test_display.exe` uses isolated copied executables/configs with
`[debug] no_present=On`. Its inherited working directory differs from the EXE
directory, exercising executable-relative configuration and snapshot paths. It checks native styles/client/monitor rectangles,
advisory VidMode behavior, game-coordinate readback, scaled native-window back
buffer pixels, black bars, visual/viewport queries, resize transitions,
swap state/attribute preservation, game-sized BMPs, invalid configuration,
and fullscreen deactivate/reactivate/destroy restoration. Its native-window
readbacks explicitly bind framebuffer zero, rather than accidentally sampling
the game's FBO.
Before make-current it uses `PrintWindow` into a white-prefilled bitmap and
checks black at the client corners/center; after make-current it independently
reads the native front buffer as black. These checks cover windowed, borderless,
fullscreen, the real mode-change case, and invalid/failing-mode fallbacks.

The `-Wall -Wextra -Werror` build generated **374 cdecl wrappers and 17 custom
entries** (336 GL, 52 GLU, 3 WGL-resolved extensions; symbol coverage unchanged).
Representative actual output:

```text
PASS windowed boot PrintWindow=black native-front=black
PASS borderless boot PrintWindow=black native-front=black
PASS fullscreen boot PrintWindow=black native-front=black
PASS fullscreen-change boot PrintWindow=black native-front=black
PASS windowed output=960x540 path=FBO game-readback=green content=red bars=black state=restored
PASS windowed output=640x480 path=FBO game-readback=green content=red bars=black state=restored
PASS windowed output=320x240 path=direct game-readback=green content=red bars=black state=restored
PASS windowed output=600x300 path=FBO game-readback=green content=red bars=black state=restored
PASS borderless output=2944x1840 path=FBO game-readback=green content=red bars=black state=restored
PASS fullscreen output=2944x1840 path=FBO game-readback=green content=red bars=black state=restored
PASS fullscreen-change output=640x480 path=FBO game-readback=green content=red bars=black state=restored
PASS fullscreen-change deactivate/restore/reactivate desktop=2944x1840 active=640x480
lxgfx: fullscreen mode 1x1 failed on \\.\DISPLAY1 (result=-2); using borderless
lxgfx: invalid [display] mode 'not-a-mode'; using windowed
lxgfx: invalid [display] width '-12'; using automatic
lxgfx: invalid [display] height 'bad-size'; using automatic
PASS display configuration, scaling, resize, readback, state, invalid values, fullscreen lifecycle
```

The 640x480 mode change succeeded on this host, was restored to 2944x1840 on
deactivation, reapplied on activation, and restored on destruction. These were
non-interactive API/message-injection checks, not a physical monitor/Alt-Tab
inspection. Multi-monitor operation and the unavailable-FBO fallback were not
exercised. Presentation can be throttled to approximately 3 fps in a locked
Windows session; use `[debug] no_present=true` for diagnostic rendering throughput.

### Real-game rendering (2026-10-17)

The staged game was run for 90 seconds per configuration, with snapshots every
120 game swaps and presentation disabled. To repeat with the current configuration
interface (the `[display]` section of `out/game/phoenixtools.ini` selects the mode):

```sh
python tools/run.py -t 90 -o out/gfx-windowed-final.log -C out/game \
  -s debug.no_present=true \
  -s debug.snapshot_dir=D:/tmp/piurelink/out/gfx-game-windowed-final \
  -s debug.snapshot_every=120 out/game/piu.exe
```

Windowed 1920x1080 used a 1280x720 FBO and produced non-black, changing,
1280x720 snapshots, including visibly rendered attract video and credit text.
Actual diagnostic lines from `out/gfx-windowed-final.log`:

```text
lxgfx: display windowed game=1280x720 output=1920x1080
lxgfx: scaling game framebuffer 1280x720 to output 1920x1080 (RGBA8 + DEPTH24_STENCIL8)
lxgfx: swaps=600 fps=59.48
lxgfx: swaps=1200 fps=59.59
lxgfx: snapshot frame 2160 -> D:/tmp/piurelink/out/gfx-game-windowed-final/frame-2160.bmp (mean rgb 50.61,125.55,224.28)
lxgfx: swaps=2700 fps=59.36
```

This was not a constant-60-fps claim: startup averaged 20.11 fps, and some later
300-frame intervals dropped to 29.19–29.35 fps; other steady intervals were
approximately 59 fps. The runner intentionally killed the still-running game
after 90 seconds; this was not a normal-exit test.

Borderless used the 2944x1840 desktop output with the same 1280x720 game FBO,
including aspect-preserving output bars. Attract video and credit text were
visible in the 1280x720 snapshots; captured video frames changed over time.
Actual lines from `out/gfx-borderless.log`:

```text
lxgfx: display borderless game=1280x720 output=2944x1840
lxgfx: scaling game framebuffer 1280x720 to output 2944x1840 (RGBA8 + DEPTH24_STENCIL8)
lxgfx: swaps=1200 fps=59.40
lxgfx: snapshot frame 2160 -> D:/tmp/piurelink/out/gfx-game-borderless/frame-2160.bmp (mean rgb 49.60,120.48,215.18)
lxgfx: swaps=2700 fps=59.84
lxgfx: swaps=4500 fps=59.36
```

Borderless startup averaged 19.38 fps, steady intervals were mostly 58–60 fps,
and the lowest later 300-frame interval was 49.13 fps. The timeout termination
was intentional, as in the windowed run.

Fullscreen successfully changed the actual mode to **1920x1080**; no
locked-session fallback was needed. The game again used a 1280x720 FBO, and
its game-sized snapshots visibly contained attract video and credit text.
Actual lines from `out/gfx-fullscreen.log`:

```text
lxgfx: fullscreen mode 1920x1080 on \\.\DISPLAY1
lxgfx: display fullscreen game=1280x720 output=1920x1080
lxgfx: scaling game framebuffer 1280x720 to output 1920x1080 (RGBA8 + DEPTH24_STENCIL8)
lxgfx: swaps=900 fps=59.64
lxgfx: snapshot frame 2160 -> D:/tmp/piurelink/out/gfx-game-fullscreen/frame-2160.bmp (mean rgb 53.33,132.07,224.90)
lxgfx: swaps=3000 fps=59.85
lxgfx: swaps=3900 fps=59.91
```

Fullscreen startup averaged 20.94 fps; steady intervals were mostly 59–60 fps,
with later intervals as low as 35.05 fps. After the intentional 90-second
timeout/kill, an independent `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)`
query confirmed the desktop was back at **2944x1840**. This verifies native
mode changes and offscreen rendering, not smooth on-screen presentation or
physical keyboard/Alt-Tab behavior in an unlocked desktop session.

## Headless frame snapshots and swap diagnostics

Set these keys in **`phoenixtools.ini` beside the executable** before the
provider's first swap. All diagnostics are off by default:

```ini
[debug]
snapshot_dir =
snapshot_every = 60
no_present = false
```

| `[debug]` key | Default | Behavior |
|---|---|---|
| `snapshot_dir` | empty (off) | Host directory for BMP dumps, not a guest lxrt-mapped path. UTF-8 relative paths resolve against the EXE/INI directory. The final directory is created if absent; its parent must exist. |
| `snapshot_every` | `60` | Positive decimal swap interval; missing, zero, negative, or invalid values use 60. Numbering starts at 1, so the default first file is `frame-60.bmp`. |
| `no_present` | `false` | Render normally but call `glFinish` instead of presenting game swaps, avoiding locked/occluded-session presentation throttling. Startup black-buffer priming still presents. |

Booleans accept `true`/`false`, `yes`/`no`, `on`/`off`, and `1`/`0`,
case-insensitively. The snapshot settings and `no_present` are sampled once,
on the first valid game swap. Runtime environment variables are not consulted.
For game runs, `tools/run.py -s section.key=value` temporarily applies INI values
and restores the original configuration when the run ends.

Before each selected SwapBuffers call, the bridge reads the game's **GL_BACK**
image (the game FBO color attachment when scaling) as RGBA and writes a bottom-up,
32-bit BI_RGB BMP named `frame-<swapcount>.bmp`. Its success line on stderr
includes the mean RGB over the game image, excluding output bars. It restores
GL_READ_BUFFER, GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, and
GL_PACK_SKIP_PIXELS before file I/O. Capture storage is allocated only when
enabled and reused/grown as necessary. The operation is synchronous and adds
readback/file-I/O cost to the render thread; failures are reported without
aborting the game.

Independently of snapshot enablement, every 300 swap calls stderr reports
`lxgfx: swaps=N fps=X`, measured with QueryPerformanceCounter over that interval.

To repeat the original snapshot verification with the current interface, first
build the bridge, then set the following in `out/phoenixtools.ini` (beside the
standalone acceptance executable):

```ini
[debug]
snapshot_dir = gfx-snapshot-every3
snapshot_every = 3
```

```sh
C:/msys64/usr/bin/bash.exe bridges/gfx/build.sh
D:/tmp/piurelink/out/test_gfx.exe 12
```

For the default-interval run, change `snapshot_dir` to `gfx-snapshot-default`,
remove `snapshot_every` (or set it to `60`), then run:

```sh
D:/tmp/piurelink/out/test_gfx.exe 300
```

Clear `snapshot_dir` after testing to disable dumps. The paths above resolve
under `out/`, regardless of the shell's working directory.

The build passed `-Wall -Wextra -Werror`. The interval-3 run wrote exactly
frames 3, 6, 9, and 12, alternating the expected RGBA and DXT1-rendered content:

```text
lxgfx: snapshot frame 3 -> D:/tmp/piurelink/out/gfx-snapshot-every3/frame-3.bmp (mean rgb 40.96,102.40,143.36)
lxgfx: snapshot frame 6 -> D:/tmp/piurelink/out/gfx-snapshot-every3/frame-6.bmp (mean rgb 163.20,0.00,0.00)
lxgfx: snapshot frame 9 -> D:/tmp/piurelink/out/gfx-snapshot-every3/frame-9.bmp (mean rgb 40.96,102.40,143.36)
lxgfx: snapshot frame 12 -> D:/tmp/piurelink/out/gfx-snapshot-every3/frame-12.bmp (mean rgb 163.20,0.00,0.00)
PASS swap read/pack state restoration frames=12
PASS frames=12 key_events=6 ConfigureNotify=2 Expose=1 ClientMessage=1 UnmapNotify=1
```

With the interval unset, the 300-frame run wrote exactly frames 60, 120, 180,
240, and 300. Its final diagnostic and acceptance lines were:

```text
lxgfx: snapshot frame 300 -> D:/tmp/piurelink/out/gfx-snapshot-default/frame-300.bmp (mean rgb 163.20,0.00,0.00)
lxgfx: swaps=300 fps=32.14
PASS swap read/pack state restoration frames=300
PASS frames=300 key_events=6 ConfigureNotify=2 Expose=1 ClientMessage=1 UnmapNotify=1
```

The regression test deliberately sets GL_READ_BUFFER to GL_FRONT, pack alignment
to 8, a nondefault row length, and nonzero row/pixel skips before each swap,
then checks every setting was preserved. Binary BMP inspection independently
confirmed 320x240, 32-bit bottom-up files with a 54-byte header, 307200 pixel
bytes, center RGB 64,160,224 / 255,0,0, and means matching the stderr lines.
These FPS values include the test's Sleep(16) calls and are not game benchmarks.
