# Running piu natively on Windows

The unmodified i386 Linux `piu` (Build ID `3bda4c41…`), its guest libraries and `phoenixtools.so`
are relinked into PE32 images and run as an ordinary 32-bit Windows process (Prism on ARM64,
WoW64 on x64). Design: `PLAN.md`; runtime details: `docs/lxrt.md`; bridges:
`docs/x11-usage.md`, `docs/opengl-x86.md`, `docs/alsa-usage.md`; FFmpeg: `docs/media-audit.md`.

## Components

| Path | Output | Role |
|---|---|---|
| `relink/relink.py` | `*.exe`, `*.so.N.dll` | ELF32 → PE32: segments → sections, `%gs:0x14` canary rewrite, RELATIVE → `.reloc`, module descriptor + entry/DllMain stubs |
| `lxrt/` | `lxrt.dll` | ELF binder (scope, versions, COPY, TLS, `RTLD_NEXT`, `dladdr`), glibc 2.31 HLE |
| `bridges/gfx/` | `lxgfx.dll` | X11 + XF86VidMode + GLX → Win32/WGL, all GL 1.1/GLU entry points (stdcall wrappers) |
| `bridges/snd/` | `lxsnd.dll` | ALSA PCM → WASAPI shared mode |
| `ffmpeg/build.sh` | `out/linux/ffmpeg/*.so.*` | minimal FFmpeg 4.2.11 (Matroska/WebM, VP9, Opus, swscale) as i386 ELF, relinked like the other guest libraries |
| `tools/mksysroot.sh` | `sysroot/` | Ubuntu focal i386 glibc 2.31 / GCC 10 sysroot (WSL) |
| `tools/mkgame.sh` | `out/game/` | relink everything into a development game directory |
| `tools/package.sh` | `out/dist/piu/` | Explorer-launchable package (GUI subsystem, `data` junction) |

## Build

All Windows-side scripts run under MSYS2 bash (`C:/msys64/usr/bin/bash.exe`; plain `bash` is WSL).

```sh
wsl -e bash /mnt/d/tmp/piurelink/tools/mksysroot.sh        # once: i386 sysroot
wsl -e bash /mnt/d/tmp/piurelink/ffmpeg/build.sh           # once: minimal FFmpeg
C:/msys64/usr/bin/bash.exe lxrt/build.sh                   # out/win/lxrt.dll
C:/msys64/usr/bin/bash.exe bridges/gfx/build.sh            # out/win/lxgfx.dll
C:/msys64/usr/bin/bash.exe bridges/snd/build.sh            # out/win/lxsnd.dll
C:/msys64/usr/bin/bash.exe tools/mkgame.sh                 # out/game (console subsystem)
C:/msys64/usr/bin/bash.exe tools/package.sh [dest] [data]  # out/dist/piu (GUI subsystem)
```

## Publish or update a package

After rebuilding any changed runtime DLLs, guest libraries or phoenixtools hook, run the same
package command again:

```sh
C:/msys64/usr/bin/bash.exe tools/package.sh [dest] [data-source]
```

The default destination is `out/dist/piu`. Publication relinks the GUI `piu.exe` and guest
DLLs (including `phoenixtools.so.dll` and `libcurl.so.4.dll`), refreshes
`lxrt.dll`/`lxgfx.dll`/`lxsnd.dll`, and adds these server entry points:

* `start-server.cmd`: double-click to run the LAN server, or pass its usual command-line
  arguments, for example `start-server.cmd --bind 192.168.1.42 --port 8080`.
  It uses installed Python 3 (`py -3`, or `python` when `py` is unavailable); no third-party
  Python packages are required.
* `server/piuserver.py`, `server/gameplay.py`, `server/playerdata.py`,
  `server/webui.py`, `server/matching.py`: the server's executable code. Tests, Python
  caches and repository databases are not included.

The launcher uses the package directory as its working directory. Without arguments, the
server listens on all interfaces at HTTP `8080` and game TCP `15040`, with its SQLite database
at `server/piu.db` and optional assets under `server/assets`. Use it only on a trusted private
LAN; launching it does not change Windows Firewall rules or enable the game's server routes.
Configure those routes in the packaged `phoenixtools.ini`; see `docs/networking.md`.

Republishing is an update, not a reset:

* Existing `phoenixtools.ini` settings, including blank values, custom mounts,
  display/audio choices, bindings and server URLs, are retained. When updating an older package
  with separate runtime configuration, its native settings fill missing values in
  `phoenixtools.ini`; existing values in the unified INI win. New template defaults and their
  comments then fill any remaining gaps. The legacy separate file is removed after a successful
  merge. Continue editing only `phoenixtools.ini` beside `piu.exe`.
  `plain_root = /piu/data` is the initial package default, not an override of an existing value.
* `runtime/`, existing databases, optional server assets/content and other user files are left
  in place. The existing `data` junction/directory is reused; `data-source` creates a junction
  only when `data` is absent, and does not retarget an existing one or copy the game data.
* Absent `card1.txt` and `card2.txt` are created as empty, unconfigured input files beside the
  INI. Existing card files are not overwritten or touched, and no card UID is generated.

`tools/mkgame.sh` still regenerates configuration for development directories by default.
The package command selects its `PRESERVE_CONFIG=1` mode for the additive merge above.

## Run

* Package: double-click `out/dist/piu/piu.exe`. Logs go to `piu.log` beside it; the writable
  sandbox (`/SETTINGS`, `/var/ucs`, virtual disk) is `runtime/` per `phoenixtools.ini`.
* Development: `python tools/run.py -t 120 -o out/run.log -C out/game out/game/piu.exe`.
  Add repeatable `-s` overrides before the executable, for example `-s debug.trace=true`,
  `-s debug.snapshot_dir=<dir>` and `-s debug.audio_stats=true`; use `-i <input script>`
  for scripted input.
* Keys (phoenixtools virtual LXIO defaults): P1 `Q E S Z C`, P2 `U O K M .`, Service `F1`,
  Test `F2`, Coin `5`.

## Configuration (`phoenixtools.ini` beside `piu.exe`)

One INI configures both the native runtime/bridges and the guest hook. Native sections are
`[mounts]`, `[immutable]`, `[process]`, `[display]`, `[audio]`, `[serial]` and `[debug]`;
guest sections are `[offline]`, `[gameplay]`, `[catalog]`, `[input]`, `[paths]` and `[server]`.
Edit the copy beside the executable and restart the game after changing it. Runtime settings
come from this file, not environment-variable overrides.

Boolean values accept `true`/`false`, `yes`/`no`, `on`/`off` or `1`/`0`, case-insensitively.
Relative native paths, including debug output paths, resolve beside `piu.exe`, not from the
launcher's working directory. Empty debug paths disable their respective outputs. All debug
features are off by default; `snapshot_every = 60` is only an interval and does not enable
snapshots without a nonempty `snapshot_dir`.

| Section / key | Values | Default | Effect |
|---|---|---|---|
| `[mounts] /piu/data` | Windows directory, absolute or relative to the executable | `data` in packages; relative path to repository `data` in development | Maps the guest game-data path to the extracted read-only data tree; other guest-path keys add mounts |
| `[immutable] /piu/data` | boolean | `true` | Caches stat results and directory listings for this read-only mount for the process lifetime |
| `[process] cwd` | guest directory | `/` | Initial guest working directory |
| `[display] mode` | `windowed`, `borderless`, `fullscreen` | `windowed` | Decorated resizable window; popup covering the current monitor at desktop resolution; real display-mode change |
| `[display] width`, `height` | pixels, `0` = automatic | `0` | Windowed: client size (auto = game size). Fullscreen: display mode (auto = desktop). Ignored in borderless |
| `[audio] mode` | `shared`, `exclusive` | `shared` | WASAPI share mode; exclusive falls back to shared when the device refuses |
| `[serial] <guest device>` | Windows COM port, for example `COM3` | no mappings | Maps an explicitly configured guest serial device to a real COM port; baud is selected by the game |
| `[debug] trace` | boolean | `false` | Verbose runtime tracing: module registration, binding/init order, file opens and guest faults |
| `[debug] trace_missing` | boolean | `false` | Logs each distinct failed path lookup once, with a timestamp |
| `[debug] log` | file path; empty = off | empty | Appends runtime log lines to an additional file; stderr is still written |
| `[debug] heapcheck` | boolean | `false` | Uses exact-size guest heap blocks with guards and reports overflows with allocation/free callers |
| `[debug] snapshot_dir` | directory path; empty = off | empty | Writes pre-swap BMP frame snapshots |
| `[debug] snapshot_every` | positive frame interval | `60` | Captures every Nth frame when `snapshot_dir` is enabled |
| `[debug] no_present` | boolean | `false` | Renders without presenting, for headless/locked-session diagnostics |
| `[debug] audio_capture` | WAV file path; empty = off | empty | Captures PCM submitted by the game |
| `[debug] audio_stats` | boolean | `false` | Logs five-second audio level and underrun statistics |
| `[server] game_port` | TCP port `1..65535` | `15040` | Private game-server TCP destination when `api_base` is enabled; match the server's `--game-port` |

Serial mappings are opt-in. These are commented examples, not enabled defaults:

```ini
[serial]
# /dev/ttyUSB0 = COM3  ; light board, 9600 baud
# /dev/ttyACM0 = COM4  ; P1 RFID reader, 115200 baud
# /dev/ttyACM1 = COM5  ; P2 RFID reader, 115200 baud
```

Use the actual COM ports assigned by Windows. Physical RFID readers require
`[input] virtual_card_reader = false`; virtual-card mode bypasses the real-reader consumer.
The game requests the baud rates shown above; they are not extra mapping fields.

The game always renders at its own size (1280x720); other output sizes are scaled with aspect
ratio kept (black bars). Invalid display/audio values log one `lxgfx:`/`lxsnd:` line and use the
default. `tools/mkgame.sh` regenerates `phoenixtools.ini` by default; `tools/package.sh` preserves
existing settings, migrates legacy separate runtime settings and merges missing defaults as
described above. Details: `docs/opengl-x86.md` (Display configuration),
`docs/alsa-usage.md` (Exclusive mode).

## Verified behaviour (this machine: Snapdragon X Elite, Adreno X1-85, Windows 11 ARM64)

| Check | Result |
|---|---|
| Boot | phoenixtools `ready: hooks installed`, catalog 940 songs / 747 local |
| Rendering | OpenGLOn12 compatibility profile; 60 fps attract, select and gameplay |
| Video | VP9 WebM (attract, previews, gameplay backgrounds) via relinked FFmpeg |
| Audio | 48 kHz S16 via WASAPI, 0 underruns over 220 s of menus + gameplay |
| Input | scripted coin ×2 → start → Normal mode → song → stage 1 completed → stage 2 |
| Startup | ~14 s to the first 60 fps frame (data tree scan) |

## Known limits

* Guest executables must not use TLS (local/initial-exec `%gs` accesses are rejected by the
  relinker); piu has none.
* FFmpeg is built without x86 SIMD (its asm needs text relocations); decoding keeps up at 60 fps.
* Real LXIO hardware (`virtual_lxio = false`) would need a native libusb/WinUSB provider behind
  `RTLD_NEXT`; not implemented.
* Prism's x87 emulation rounds `fld %st(0)` copies to double precision.
