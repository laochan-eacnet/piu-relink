# phoenixtools

`phoenixtools` is a GPLv2-only, preloadable i386 compatibility and safety layer
for studying one specific plaintext Phoenix executable in an isolated offline
environment. It is designed for Box32/Box64 on an aarch64 WSL2 host, but its
build and unit tests use an i686 cross toolchain and QEMU user mode.

## Important disclaimer

This project is provided **only for study and research**. It contains no Pump It
Up game executable, extracted resources, song catalog, media, protected key
material, SenseLock identity, or other Andamiro data. It does not provide or
circumvent protected media or online entitlements. The project is independent
and is **not affiliated with, endorsed by, or supported by Andamiro**.

Only use software and hardware that you are authorized to study.

## Supported executable

Fixed-address patches target exactly the i386 ET_EXEC image with Build ID:

```text
3bda4c418b536bc77a6aa79db790cff14b9656c1
```

Every patch validates the original instruction bytes before writing. If a
supported target contains unexpected bytes, initialization terminates instead
of applying a partial patch set.

## Safety model

The hook keeps the runtime offline and limits host exposure:

- extracted plaintext resources are opened read-only through a case-insensitive
  resolver;
- optional overlay data is checked before the plaintext root;
- writes to `/SETTINGS` and `/var/ucs` are redirected into a configured runtime
  sandbox;
- writable raw `open()` access to `/dev/sda` is always denied; read-only `open()`
  gets the configured virtual-disk file (created empty if missing), and
  `HDIO_GET_IDENTITY` on that descriptor reports a fixed virtual identity
  (`PHOENIXTOOLS VIRTUAL DISK`, serial `PTVD0001`) for the HDD line of the
  system-information screens;
- `fopen()` access to `/dev/sda` is denied unless redirected to the configured
  virtual-disk file;
- original HTTP(S) destinations are blocked, except for explicit isolated mock
  endpoints configured by the user;
- update execution and unnecessary raw-port access are disabled;
- the offline lock response contains only a non-secret region selection and
  exposes no protected SenseLock identity or opcode payload;
- missing remote/DLC resources remain unavailable.

The game executable and extracted resources are not modified by this project.

## LXIO modes

`phoenixtools.so` interposes both libusb-1.0 and libusb-0.1 APIs.

### `virtual_lxio = true`

Only two in-process synthetic devices are exposed:

- `0d2f:1010`, retained as a neutral legacy companion;
- `0d2f:1020`, the exclusive synthetic input/output transport.

No real USB device is enumerated or opened through the interposed APIs. P1,
P2, Service, Test, and Coin1 keyboard bindings are read from
`phoenixtools.ini`.

### `virtual_lxio = false`

Every interposed `libusb_*` and `usb_*` operation is forwarded with
`dlsym(RTLD_NEXT, ...)` to the real 32-bit libusb implementation. This is an
explicit opt-in for authorized real LXIO hardware. Disk, network, lock-payload,
and raw-port safety restrictions remain active.

The launcher does not select the mode or parse configuration.

## Card-reader modes

`[input] virtual_card_reader = false` is the default and is independent of
`virtual_lxio`. Disabled mode leaves the game's original serial-reader consumer
and scan keys untouched; it does not read card files. On Windows, map the game's
Linux serial reader devices to Windows COM ports through `[serial]` in the shared
configuration, as described below.

To opt into keyboard-driven card insertion:

```ini
[input]
virtual_card_reader = true
card1 = F3
card2 = F4
```

Provide your own `card1.txt` (P1) and `card2.txt` (P2), beside the **loaded INI**,
not in the working directory or `runtime_dir`. The INI pathname's directory is
anchored at startup, including when the INI is a symlink. Forward/backslash
directory separators and absolute Windows drive paths are supported. These
files are external user inputs: the hook does not generate UIDs, create files,
or create server accounts.

Press `F3` to insert P1's card or `F4` for P2. Each new physical key press reads
that player's file afresh, so edits take effect without restarting the game.
Holding the key (including repeated Windows key-down events) does not rescan;
release it before scanning again. P1 and P2 have independent held/pending state.
Another explicit scan replaces that player's pending UID.

Each readable regular file must contain **2–34 ASCII hexadecimal digits** in
complete byte pairs (1–17 UID bytes). Upper/lowercase, leading zeroes, and
surrounding ASCII whitespace, including CRLF, are accepted; byte order is
preserved. Internal whitespace and files larger than 256 bytes are rejected.
Missing, unreadable, or invalid files are diagnosed only when the insertion key
is pressed, clear that player's pending scan, and do not crash the game. Fix
the file, release the key, and press again.

There is no automatic startup scan or environment-variable injector. Emulated
mode bypasses consumption of real reader events, including when no emulated
scan is pending, but does not disable native device enumeration/polling. Native
player-state and online-session gates still apply: a key press supplies a card
event, not a login or entitlement. See `docs/networking.md` in the parent project
for the private server's existing-card/account requirements.

## Repository layout

```text
src/       Hook implementation, organized by responsibility
tests/     i386 unit and integration tests
tools/     Read-only catalog/audit utilities and source policy audit
bin/       All generated compiler/linker output (ignored by Git)
docs/      Architecture documentation
```

The repository root contains only build metadata, documentation, licensing, and
`phoenixtools.ini`. Launch scripts and runtime state intentionally live outside
this repository.

All project headers use `#pragma once`. `make audit` rejects source files over
350 lines and headers that reintroduce traditional include guards; source files
should preferably remain below 200 lines.

## Build requirements

Typical package/tool requirements are:

- `i686-linux-gnu-gcc` cross compiler;
- Python 3;
- `qemu-i386` for tests;
- a usable i386 glibc loader/sysroot at `/usr/i686-linux-gnu`.

Build everything:

```sh
make
```

Primary output:

```text
bin/phoenixtools.so
```

The offline curl dependency shim, test executables, objects, and dependency
files are also generated only below `bin/`. FFmpeg is never interposed: Phoenix
uses the real i386 media libraries so VP9 and gameplay video remain available.
USB emulation is not built as a separate compatibility DSO.

Run static layout checks and tests:

```sh
make audit
make test
```

The test suite covers case-insensitive resources, preview/full media
separation, P1/P2/cabinet report bytes, synthetic `1010/1020` enumeration,
asynchronous transfers, `RTLD_NEXT` forwarding in hardware mode, private IPv4
boundaries, API/CDN routing, game TCP redirection and legacy HTTP destinations.
Card/input fixtures exercise real X11 hooks, fresh two-player scans, held-key
filtering, malformed/missing-file recovery, disabled-mode pass-through, and
native consumer continuation/patch-mismatch behavior. Network policy fixtures
record connections without contacting a LAN server.

## Configuration

The game, hook and native runtime use one `phoenixtools.ini` beside `piu.exe`;
there are no environment-variable configuration overrides. The hook searches
beside its module, then one parent directory above it. `tools/mkgame.sh` appends
native runtime sections when composing the deployed file from this hook template.

Hook-owned settings are grouped as follows:

- `[offline]`: region, lock-screen diagnostic hold, logging;
- `[gameplay]`: offline guest catalog/chart policy and native Full Mode default;
- `[catalog]`: optional Custom channel slot replacement and inclusive song-ID range;
- `[input]`: independent LXIO/card-reader modes and all key bindings;
- `[paths]`: plaintext root, optional overlay, runtime sandbox, virtual disk;
- `[server]`: optional isolated private-server endpoints and raw game TCP port.

The same file also contains sections owned by `lxrt` and the native graphics/audio
bridges: `[mounts]`, `[immutable]`, `[process]`, `[display]`, `[audio]`, `[serial]`
and `[debug]`. The hook silently ignores these sections; it still diagnoses
unknown hook keys. Runtime diagnostic switches and output paths belong in
`[debug]` and are off by default.

`[serial]` maps Linux device paths to Windows COM ports. For example,
`/dev/ttyUSB0 = COM3` selects the cabinet light board (9600 baud), while
`/dev/ttyACM0 = COM4` and `/dev/ttyACM1 = COM5` select the P1/P2 card readers
(115200 baud). Real card readers are consumed only when
`[input] virtual_card_reader = false`; leave mappings unset for devices you do not
use. The deployed template includes these mappings as commented examples only.

`[server] api_base` and `cdn_base` are empty by default, blocking HTTP, DNS
and raw TCP. Explicit **plain HTTP** bases may use `localhost`, `127.0.0.1`,
or a numeric RFC1918 address: `10.0.0.0/8`, `172.16.0.0/12`, or
`192.168.0.0/16`. For a separate LAN machine, for example:

```ini
[server]
api_base = http://192.168.1.42:8080
cdn_base = http://192.168.1.42:8080
game_port = 15040
```

Public addresses, arbitrary DNS names, shorthand/zero-padded IPv4, userinfo,
HTTPS, invalid ports, query/fragment base suffixes and control characters are
rejected. The client never resolves or contacts the original public servers.
Original request paths and queries are appended to the configured base;
an optional base path prefix needs matching server routes. Only requests to
`xx-cdn.piugame.com/2023/` (including an explicit port) select `cdn_base`;
other requests select `api_base`. An empty/invalid matching base blocks that
request instead of falling back to the other endpoint. The updater stays disabled.
HTTP connects only to the configured API/CDN address and port, not arbitrary
other hosts or ports. Legacy curl also rejects public hosts, TLS, redirects
and proxies when used without the hook.

With a valid `api_base`, isolated DNS answers use its IPv4 address, and game
TCP port 50400 is redirected to that same address on `[server] game_port`
(default `15040`, valid range `1..65535`). Match this setting to the server's
`--game-port`; an invalid value is diagnosed at load time and leaves the default.
The HTTP port in `api_base` does not change the game port. With CDN alone,
DNS uses the CDN address for isolation, but game TCP and trailer opt-in stay
disabled.

Only the send/receive SenseLock trailer calls are replaced with no-ops.
Ordinary packets retain RC4, the 12-byte magic and the 20-byte digest, but
carry the 64-byte trailer untransformed; exactly 16-byte packets are
unchanged. The shared SenseLock functions, frame bounds and reply integrity
checks are not patched. Empty/invalid `api_base` installs no trailer patches.
This is an opt-in LAN protocol variant, not a vendor-server transport or an
online entitlement. See the parent project's `docs/networking.md` for wire
framing and the private service; do not expose it to the internet.

X11 bindings use keysym names accepted by `XStringToKeysym`. The shipped
defaults are:

| Control | P1 | P2 |
|---|---|---|
| Upper-left | `Q` | `U` |
| Upper-right | `E` | `O` |
| Center | `S` | `K` |
| Lower-left | `Z` | `M` |
| Lower-right | `C` | `period` |

Cabinet defaults are Service `F1`, Test `F2`, and Coin1 `5`. Optional card
insertion defaults are P1 `F3` (`card1`) and P2 `F4` (`card2`). Duplicate or
invalid resolved X11 keycodes are rejected for active bindings. When card
emulation is disabled, its bindings are inactive and excluded from validation.

Relative `[paths]` values are resolved from the directory containing the INI.
Keep `plain_root` on read-only extracted data and place `runtime_dir` and
`virtual_disk` on writable local storage.

### Custom channel: NX-NXA slot replacement

The Custom channel is disabled by default. To enable it, set:

```ini
[catalog]
custom_channel = true
custom_id_min = 0x7F00
custom_id_max = 0x7FFF
```

This **replaces the ordinary NX-NXA channel, enum 12**; it does not append a new
slot. Event channels 72-81 are not used because they require independent event
state and assets. Setting `custom_channel = false` leaves the original channel
predicate and label untouched.

The range is inclusive and matches a chart when **either its source or display
song ID** is within the configured endpoints. IDs accept unsigned decimal or
explicit `0x`/`0X` hexadecimal through `0xFFFFFFFF`; leading zeroes in decimal
do not imply octal. Signs, bare hexadecimal, overflow and trailing text are
invalid. Invalid endpoint values are logged and ignored; if the completed range
is reversed, both endpoints revert to `0x7F00` and `0x7FFF`. Those defaults avoid
the editor's reserved song-ID masks; they are IDs, not song-table indices.

The hook changes only chart membership for this channel. It does not rewrite
song series, genre, length or flags, and does not edit the catalog or `data/D`.
Custom songs therefore remain reachable through their other native catalog
paths. Native restrictions, grouping, sorting and selector bookkeeping remain
in effect; paths that bypass ordinary channel filtering, including Basic Mode,
are unchanged.

**Presentation requires user-supplied overlays.** When enabled, the runtime
changes the NX-NXA translation key to `CHANNEL_CUSTOM`. Configure `overlay_root`
and provide a **complete** copy of each used locale's
`<overlay_root>/CONFIG/LANG/<locale>/CHANNEL.TXT`, preserving its existing
translations and adding:

```text
CHANNEL_CUSTOM = "Custom songs"
```

There is no built-in translation-file merge or generated translation overlay.
The hook also does not replace channel artwork or audio by itself. The native
enum-12 presentation reads the NX-NXA resources from these concrete paths:

```text
USER_INTERFACE/SELECT_SONG/FULL_CHANNEL2/<locale>/FC_CHANNEL_LOGO.G
USER_INTERFACE/SELECT_SONG/FULL_CHANNEL3/<locale>/FA3_02.G
USER_INTERFACE/SELECT_SONG/FULL_CHANNEL2/<locale>/CHD_SERIES_02_NX.SPR
USER_INTERFACE/SELECT_SONG/FULL_CHANNEL3/<locale>/FA3_CH_15_NX-NXA.SPR
```

The `.SPR` files select atlas rectangles inside the `.G` files; a polished
replacement must shadow the complete affected atlas/group (not just one entry),
because group shadowing has no per-entry fallback. `.G` files are native
8-byte headers followed by 32-bit RGBA atlas pixels; they are not PNG files.
Supply a corresponding voice replacement separately if desired. Keep these
presentation overlays outside `data/D`; no custom art or voice is bundled.

## Deployment contract

The hook output is `bin/phoenixtools.so` (relinked to `phoenixtools.so.dll` for
Windows). The parent project's `tools/mkgame.sh` deploys it beside `piu.exe` and
composes the single `phoenixtools.ini` from this hook template plus native runtime
sections. Preserve user edits to that deployed file when updating the runtime.

A local launcher outside this repository should only arrange for
`phoenixtools.so` to be loaded into the existing executable. It must not parse
`phoenixtools.ini`, duplicate hook policy, or modify the executable.

## License

Copyright contributors. Licensed under the GNU General Public License,
**version 2 only**. See `LICENSE`.
