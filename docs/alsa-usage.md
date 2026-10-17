# piu ALSA usage and native WASAPI provider

## Evidence and scope

Investigated the original ELF32 i386 `piu`, Build ID
`3bda4c418b536bc77a6aa79db790cff14b9656c1`, using WSL's
`i686-linux-gnu-objdump -d --no-show-raw-insn`. The host `objdump` could read the
ELF header but could not disassemble i386, so the cross-binutils executable is
required. All addresses below are original ELF virtual addresses, not PE RVAs.
Searching `imports.tsv` for `snd_` / `ALSA_` finds **22 imports**. No mixer,
capture, mmap, PCM status, explicit start, drain, drop, or recover function is
imported. `snd_strerror` and the parameter `*_sizeof` functions are not imports
of this binary either; the bridge additionally supplies them for normal ALSA
allocation/error-reporting patterns and tests.

Disassembly commands used included:

```sh
wsl -e bash -lc 'i686-linux-gnu-objdump -d --no-show-raw-insn /mnt/d/tmp/piurelink/piu'
wsl -e bash -lc 'i686-linux-gnu-objdump -d --no-show-raw-insn --start-address=0x80b0000 --stop-address=0x8110000 /mnt/d/tmp/piurelink/piu'
wsl -e bash -lc 'i686-linux-gnu-objdump -d --no-show-raw-insn --start-address=0x8110000 --stop-address=0x8170000 /mnt/d/tmp/piurelink/piu'
wsl -e bash -lc 'i686-linux-gnu-objdump -d --no-show-raw-insn --start-address=0x8170000 /mnt/d/tmp/piurelink/piu'
wsl -e bash -lc 'i686-linux-gnu-objdump -s --start-address=0x81c7380 --stop-address=0x81c73d0 /mnt/d/tmp/piurelink/piu'
wsl -e bash -lc 'i686-linux-gnu-objdump -s --start-address=0x81d7810 --stop-address=0x81d7830 /mnt/d/tmp/piurelink/piu'
```

The rodata at `0x081c73ab` is **`hw:0,0`**, followed by NUL. There are two
`snd_pcm_open` callers, and both pass that same address.

## Initialization and negotiated parameters

`_start` passes `main = 0x08058624` to `__libc_start_main` at `0x08058b3d`.
Main calls `0x08071e22` at `0x08058666`; that routine performs the audio setup
below on the initial/main thread. The PCM pointer is stored at `0x0a18db48`.
The game does not check the individual setup call return values.

| Order | Call site | Operation / argument |
|---|---|---|
| 1 | `0x0807274e` | `snd_pcm_hw_params_malloc(&hw)` |
| 2 | `0x08072772` | `snd_pcm_open(&pcm, "hw:0,0", 0, 0)`; playback, blocking |
| 3 | `0x0807278b` | `snd_pcm_hw_params_any(pcm, hw)` |
| 4 | `0x080727ac` | `snd_pcm_hw_params_set_access(pcm, hw, 3)` = `SND_PCM_ACCESS_RW_INTERLEAVED` |
| 5 | `0x080727cd` | `snd_pcm_hw_params_set_format(pcm, hw, 2)` = `SND_PCM_FORMAT_S16_LE` |
| 6 | `0x080727f5` | `snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, dir)`; rate initially **48000** (`0xbb80`) |
| 7 | `0x08072815` | `snd_pcm_hw_params_set_channels(pcm, hw, 2)` = stereo |
| 8 | `0x0807283e` | `snd_pcm_hw_params_set_periods(pcm, hw, 4, 0)` |
| 9 | `0x0807285f` | `snd_pcm_hw_params_set_buffer_size(pcm, hw, 2048)` frames |
| 10 | `0x08072877` | `snd_pcm_hw_params(pcm, hw)` |
| 11 | `0x08072886` | `snd_pcm_hw_params_free(hw)` |
| 12 | `0x0807289f` | `snd_pcm_sw_params_malloc(&sw)` |
| 13 | `0x080728b8` | `snd_pcm_sw_params_current(pcm, sw)` |
| 14 | `0x080728d9` | `snd_pcm_sw_params_set_avail_min(pcm, sw, 256)` frames |
| 15 | `0x080728f2` | `snd_pcm_sw_params(pcm, sw)` |
| 16 | `0x08072901` | `snd_pcm_sw_params_free(sw)` |

Thus the requested ALSA period is **512 frames** (2048 / 4), or approximately
10.667 ms at 48 kHz. Mixer writes and `avail_min` are half a period, **256
frames** / 1024 bytes / approximately 5.333 ms. The buffer is approximately
42.667 ms. No period-size-near, buffer-size-near, rate-exact, software start
threshold, or software stop threshold setter is imported.

The rate local is at `esp+0x140`. `dir` is a non-NULL pointer in `%ebp`, previously
set to `esp+0x240`; that stack storage is reused during startup, and the nearby
code does not explicitly initialize a fresh direction integer. No branch uses
the returned direction. The returned **rate** is compared, unsigned, against
**50000** (`0xc350`) at `0x0807288b`. If it is greater, the game closes the PCM
at `0x08074873`, changes the request to **47999** (`0xbb7f`), and repeats the
same hardware setup at `0x0807488d..0x080749c5`, then rejoins software setup at
`0x0807289c`. It does not loop/recheck the second negotiated rate. This is the
only observed `snd_pcm_close` call site. The mixer/timing logic still assumes
48 frames per millisecond; returning the requested 48000 exactly avoids this
fallback and preserves its clock assumptions.

## Playback, waits, and error recovery

The startup routine initializes a mutex at `0x0a18db2c`, then creates a pthread
at `0x08072975` with entry **`0x080e6c19`**, NULL argument, and requested
`SCHED_RR` policy 2 / priority 50. There is no evidence of an ALSA async callback.
The actual writes are exclusively in this mixer pthread:

1. `snd_pcm_prepare(pcm)` at `0x080e6c28` before entering the loop.
2. Check shutdown byte `0x0a18db44` at `0x080e6c2d`.
3. `snd_pcm_avail_update` at `0x080e6c43`; any negative result goes to prepare.
4. `snd_pcm_avail_update` again at `0x080e6c59`.
5. If availability is at least 256, mix/clamp signed 16-bit stereo samples into
   `0x0a1b6420`, then call **`snd_pcm_writei(pcm, buffer, 256)`** at
   `0x080e6db4`. A negative return goes to `snd_pcm_prepare` at `0x080e6dca` and
   retries the availability loop. Positive short writes are not handled:
   the next iteration generates a new 256-frame block.
6. If availability is below 256, call **`snd_pcm_wait(pcm, 1000)`** at
   `0x080e7ba1`. It checks specifically for **`-EPIPE = -32`**. That case
   subtracts 2048 from an audio-position counter (`0x0a1bee38`), with wrap
   bookkeeping against `0x8219864`, then rejoins the outer loop. The outer
   availability check subsequently prepares the stream if it is still in XRUN.
   Other wait returns, including timeout, also rejoin the outer loop.

There is **no `snd_pcm_recover` call** in piu. Its recovery is direct
`snd_pcm_prepare` on any negative availability/write result. Therefore the
bridge returns Linux negative errno numbers, not HRESULTs or Windows errno.
Its blocking `writei` completes the requested block unless an actual error
occurs; nonblocking mode, though not requested by piu, returns a short count or
`-EAGAIN` when full.

## Delay, availability, and concurrent threads

Availability is also called immediately before both latency queries, but those
callers ignore its return value:

- `0x080ec04c` → `snd_pcm_avail_update`; `0x080ec062` → `snd_pcm_delay` with an
  i386 signed-long output at `esp+0x14`. The containing position function starts
  at `0x080ebf6a`; gameplay callers include `0x080c1d09`, `0x080c5204`, and
  `0x080e1486`. It subtracts delay from a software frames-written position,
  divides by **48.0** (double at `0x081d7820`), and uses the result for
  millisecond playback-position correction.
- `0x080eccea` → `snd_pcm_avail_update`; `0x080ecd00` → `snd_pcm_delay` with
  output at `esp+0x70`. It computes
  `*(0x0a1adb60) = *(0x0a1adb68) - delay` for decoder/playback synchronization.
  This code belongs to the long-running I/O/audio-update pthread entry
  **`0x080ec15f`**, created at `0x080743e6`, with requested `SCHED_RR` / priority
  99. It is separate from the PCM-writing mixer pthread.

Both latency paths are inside the game's audio mutex, but that does not make
all PCM operations single-threaded: the mixer availability/writes/waits run
separately. The native provider therefore serializes shared state and keeps all
WASAPI COM interfaces on its own native MTA worker thread. Guest handles are
opaque pointers; neither Win32 nor COM structs are exposed to the guest.

## Complete imported symbol table

Every row below is provided by `lxsnd.dll!lx_provider_symbols`. Names have the
`ALSA_0.9` version except `snd_pcm_hw_params_set_rate_near`, whose import is
`ALSA_0.9.0rc4`.

| Symbol | PLT address | Direct caller(s) |
|---|---|---|
| `snd_pcm_writei` | `0x0804f230` | `0x080e6db4` |
| `snd_pcm_avail_update` | `0x0804f270` | `0x080e6c43`, `0x080e6c59`, `0x080ec04c`, `0x080eccea` |
| `snd_pcm_close` | `0x0804f3e0` | `0x08074873` |
| `snd_pcm_hw_params_set_buffer_size` | `0x0804f450` | `0x0807285f`, `0x0807499d` |
| `snd_pcm_sw_params` | `0x0804f560` | `0x080728f2` |
| `snd_pcm_hw_params_set_channels` | `0x0804f640` | `0x08072815`, `0x08074954` |
| `snd_pcm_sw_params_malloc` | `0x0804f810` | `0x0807289f` |
| `snd_pcm_hw_params_free` | `0x0804f8b0` | `0x08072886`, `0x080749c5` |
| `snd_pcm_hw_params_any` | `0x0804fb80` | `0x0807278b`, `0x080748ca` |
| `snd_pcm_wait` | `0x0804fbe0` | `0x080e7ba1` |
| `snd_pcm_hw_params` | `0x0804fcf0` | `0x08072877`, `0x080749b6` |
| `snd_pcm_hw_params_malloc` | `0x0804fe40` | `0x0807274e`, `0x0807488d` |
| `snd_pcm_delay` | `0x08050030` | `0x080ec062`, `0x080ecd00` |
| `snd_pcm_sw_params_current` | `0x080500d0` | `0x080728b8` |
| `snd_pcm_hw_params_set_rate_near` | `0x08050140` | `0x080727f5`, `0x08074933` |
| `snd_pcm_open` | `0x080502d0` | `0x08072772`, `0x080748b1` |
| `snd_pcm_hw_params_set_format` | `0x08050390` | `0x080727cd`, `0x0807490c` |
| `snd_pcm_hw_params_set_periods` | `0x080504c0` | `0x0807283e`, `0x0807497d` |
| `snd_pcm_sw_params_free` | `0x080508e0` | `0x08072901` |
| `snd_pcm_sw_params_set_avail_min` | `0x08050910` | `0x080728d9` |
| `snd_pcm_hw_params_set_access` | `0x08050b00` | `0x080727ac`, `0x080748eb` |
| `snd_pcm_prepare` | `0x08050b50` | `0x080e6c28`, `0x080e6dca` |

Five additional table entries are `snd_pcm_hw_params_sizeof`,
`snd_pcm_sw_params_sizeof`, `snd_pcm_recover`, `snd_pcm_drain`, and
`snd_strerror`, making **27 provider symbols**. Only the provider-table function
is a PE export. There is no dependency on `lxrt.dll`.

## WASAPI implementation and ABI

`bridges/snd/snd.c` uses event-driven **WASAPI shared or exclusive mode**, not
waveOut. Shared is the default and the automatic fallback if exclusive opening
fails. Native Prism tests successfully opened and streamed the exact game format.

- `hw:0,0` maps to the default Windows rendering endpoint (`eConsole`).
  `default` and `plughw:0,0` are also accepted; other names return `-ENODEV`.
  The implemented guest format is interleaved S16_LE stereo; unsupported
  format/access/channel requests return `-EINVAL`, not fake success.
- In shared mode the WASAPI source format remains S16 stereo at the
  guest-requested rate. `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
  SRC_DEFAULT_QUALITY` delegates sample format/channel conversion and resampling
  to the Windows shared audio engine. The measured endpoint mix is **48 kHz,
  32-bit IEEE float** (extensible tag 65534, subtype 3); both 48 kHz and
  **44.1 kHz** guest source tests succeeded.
- A fixed-size ring is allocated when hardware params are committed, not on
  writes. It separates guest callers from the COM-owning worker. The worker
  waits on configuration/data events and the WASAPI render event. Guest writers
  and `snd_pcm_wait` sleep on a condition variable signaled by that worker;
  there is no buffer-space busy loop or `Sleep`-based playback polling.
- Total ring-plus-WASAPI pending frames are limited to the requested ALSA buffer
  capacity. Shared mode measures WASAPI padding; exclusive mode measures device
  progress with `IAudioClock` and tracks real samples in full-period packets,
  excluding silence used to complete a final packet. QPC interpolates between
  worker snapshots. All counts remain in the **guest/source sample rate**,
  including during shared-mode 44.1 → 48 kHz conversion. Downstream device
  latency beyond the WASAPI client queue is not separately measured.
- Software defaults include start threshold 1 and stop threshold equal to the
  requested buffer, and the game sets `avail_min=256`. ALSA's requested period
  count is recorded; the physical wake quantum is controlled by the Windows
  audio engine (shared) or the endpoint's device period (exclusive).
- Actual starvation enters XRUN and returns `-32` until prepare/reset. Prepare
  stops/resets WASAPI and clears both queues. Recover handles `-EPIPE` and
  `-ESTRPIPE` through prepare, and `-EINTR` as a retryable interruption. Static
  error strings never expose UCRT-owned allocations.
- Parameters are stored **inline** in the guest-owned buffers. No parameter
  object contains a heap pointer. The mirrored i386 kernel ALSA layouts are
  604-byte hardware params and 104-byte software params, packed to 4 bytes;
  frame counts are 32-bit. `*_sizeof` + alloca + memset works, independently of
  malloc/free allocation. Params are opaque to piu; this is the used subset,
  not a replacement for ALSA's complete constraint/plugin engine.

`bridges/snd/snd_abi.h` asserts sizes and offsets. In addition,
`bridges/snd/check_alsa_abi.c` was compiled for **i386** against the installed WSL
`/usr/include/alsa` and `/usr/include/sound` headers. It checks actual ALSA enum
values and scalar widths in one mode, and kernel structure sizes/offsets in a
second mode; both modes also check Linux errno constants.

## Exclusive mode

Set the following in **`phoenixtools.ini` beside the executable** (the same file
used for game settings and runtime mounts). No guest code change is required:

```ini
[audio]
mode = exclusive
```

`mode = shared` is the default, including a missing file/key. Values are
case-insensitive. An unknown value produces one `lxsnd:` diagnostic naming it
and selects shared mode. The selection is read for each PCM open; an already
configured PCM does not change mode until reopened.

Exclusive mode opens the default Windows render endpoint with
`AUDCLNT_SHAREMODE_EXCLUSIVE | AUDCLNT_STREAMFLAGS_EVENTCALLBACK` semantics
(the mode and stream flags are separate WASAPI arguments). It negotiates stereo
at the **unchanged guest-requested rate**, using `IsFormatSupported(EXCLUSIVE)`
in this order:

1. The guest's S16_LE format as `WAVEFORMATEXTENSIBLE`, front-left/front-right.
2. 32-bit IEEE float in an extensible container.
3. 24 valid integer bits, left-aligned in a 32-bit extensible container.
4. A plain `WAVE_FORMAT_PCM` 16-bit container for drivers that reject the
   equivalent extensible S16 description.

The bridge converts S16 samples itself for float32 and 24-in-32; it does **not
resample in exclusive mode**. If the endpoint does not accept any of these
containers at the guest rate, exclusive negotiation fails rather than changing
the game's audio clock. The single fallback diagnostic includes the HRESULT,
operation, and reason (for example, device busy, unsupported format/rate, or
exclusive mode disabled); playback then automatically opens shared mode.
Activation, initialization, aligned-buffer retry, and render/clock-service
failures all take this same fallback path. A successful open emits exactly one
chosen-mode line giving format, rate, ALSA/device buffer sizes and period.

The device's `GetDevicePeriod` minimum period is used (default period if no
minimum is reported). Event-driven exclusive initialization uses **equal buffer
duration and periodicity**. `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` is handled by
querying the aligned frame count on the failed client, releasing it, activating
a new client, and retrying with that aligned duration for both arguments.
WASAPI's two exclusive event buffers and the endpoint period determine native
latency; the guest still sees its requested **2048-frame / 42.667 ms** total
capacity at 48 kHz. Exclusive is not a promise to remove that guest buffering.
Availability, delay, blocking writes, `-EPIPE` starvation, prepare and recover
retain the shared-mode guest contract, with counts always in guest frames.
The COM worker and the guest writer retain MMCSS **Pro Audio** scheduling.

`snd_pcm_close` stops and releases the exclusive stream on its COM-owning worker.
Windows releases the endpoint when the process terminates as well, including a
forced launcher timeout; the provider deliberately avoids loader-lock shutdown
callbacks that wait for an already-terminated worker. Device invalidation after
opening surfaces as `-ENODEV`, just as in shared mode; it is not hidden by a
mid-stream mode switch.

`out/win/test_snd.exe --modes` copies the test executable and provider into a
temporary directory and writes a separate `phoenixtools.ini` there. It runs the exact
game initialization sequence with a one-second 48 kHz tone in both modes,
first with game-like 256-frame writes and then a single blocking 48000-frame
write. Each child enables `[debug] audio_stats=YeS`, holds the drained stream
long enough for a statistics interval, and reports the actual negotiated
mode/format/period and any fallback reason. The suite requires successful
playback/drain, a statistics line with zero underruns, and a fallback reason
whenever requested exclusive mode actually selected shared. It also checks the
one-second WAVE capture through a UTF-8 relative path containing `#`, with a
working directory different from the EXE directory. Its temporary files are removed.

## Audio diagnostics

Set diagnostics in the same executable-local `phoenixtools.ini`:

```ini
[debug]
audio_capture =
audio_stats = false
```

| `[debug]` key | Default | Behavior |
|---|---|---|
| `audio_capture` | empty (off) | UTF-8 host WAVE path; relative paths resolve against the EXE/INI directory, not the working directory or guest mounts. The parent directory must exist. |
| `audio_stats` | `false` | Report interval frames, underruns, peak, and RMS on stderr every five seconds. |

Booleans accept `true`/`false`, `yes`/`no`, `on`/`off`, and `1`/`0`,
case-insensitively. Both diagnostics are sampled on each successful hardware
parameter commit, not at DLL load or PCM open. Reconfiguration reopens/truncates
the capture. Runtime environment variables are not consulted.

Capture tees every `snd_pcm_writei` submission before format conversion,
including a rejected or short-write tail. The file contains stereo S16 at the
requested guest rate, with WAVE header checkpoints every second and on close;
long captures switch to RF64 without moving sample data. One PCM owns a capture
path, so use distinct paths across processes. Statistics normalize both S16
channels by 32768 and are not cleared by prepare. Captures and statistics remain
disabled unless explicitly enabled.

## Build and exercised verification

Install headers (performed successfully):

```sh
wsl -e bash -lc 'sudo apt-get update -qq && sudo apt-get install -y libasound2-dev'
```

Build from the root in the workstation shell. Sourcing is intentional here:
plain `bash bridges/snd/build.sh` resolves to the WSL shell on this workstation
and cannot see Windows SDK/MSVC paths.

```sh
. bridges/snd/build.sh
```

The script sources `. tools/env.sh`, uses `WIN_CC` / `WIN_LDFLAGS`, and produces
`out/win/lxsnd.dll` and `out/win/test_snd.exe`. The only linked Windows libraries
beyond the native CRT/standard platform defaults are `ole32` and `avrt`.

Exact final build/test command executed, successful exit 0:

```sh
. bridges/snd/build.sh && D:/tmp/piurelink/out/win/test_snd.exe && D:/tmp/piurelink/out/win/test_snd.exe 44100 block && D:/tmp/piurelink/out/win/test_snd.exe 48000 edges
```

`test_snd.c` uses `LoadLibrary` and only provider-table addresses, with cdecl
function-pointer types. It replays all game setup calls in order on the main
thread, writes the 440 Hz / 1-second S16 stereo tone from another thread, and
queries delay concurrently. The game has infinite playback; the test's final
128-frame remainder and `snd_pcm_drain` are finite-test additions. The alloca
checks are supplemental, since piu itself uses params malloc/free.

The complete captured console output is in **`out/win/test_snd.log`**. Selected
lines from the final run:

```text
provider=27 symbols; i686 pointers=4 frame_counts=4/4 hw=604 sw=104
snd_pcm_open(&handle, "hw:0,0", 0, 0) = 0 (Success)
negotiated_rate=48000 direction=0
lxsnd: WASAPI shared S16 stereo 48000 Hz -> mix 48000 Hz/32 bits/tag 65534/subtype 3; buffer 2048/2048 frames
snd_pcm_hw_params(handle, hw) = 0 (Success)
stack params: channels=2 rate=48000 periods=4 buffer=2048 avail_min=256 start=1
writei total=12032 frames return=256 delay=1693 rc=0
writei total=24064 frames return=256 delay=1727 rc=0
writei total=36096 frames return=256 delay=1759 rc=0
writei total=48000 frames return=128 delay=1661 rc=0
snd_pcm_drain(b->handle) = 0 (Success)
tone=48000 frames (1 second), writes=188 waits=97 errors=0 thread_exit=0
```

The 44.1 kHz blocking test deliberately passes a whole second to a **single**
`writei` call, exceeding the 2048-frame capacity and exercising event-driven
blocking and automatic resampling:

```text
lxsnd: WASAPI shared S16 stereo 44100 Hz -> mix 48000 Hz/32 bits/tag 65534/subtype 3; buffer 2048/2048 frames
writei total=44100 frames return=44100 delay=1725 rc=0
tone=44100 frames (1 second), writes=1 waits=0 errors=0 thread_exit=0
```

The optional `edges` run first plays another error-free second, then
**intentionally** starves the re-prepared stream to test Linux XRUN/recovery,
and creates a nonblocking handle with auto-start disabled to test a full buffer:

```text
starved avail = -32 (expected -32)
starved wait = -32 (expected -32)
starved delay = -32 (expected -32)
snd_pcm_recover(handle, -LX_EPIPE, 0) = 0 (Success)
recovered avail = 2048 (expected 2048)
nonblocking partial writei = 2048 (expected 2048)
nonblocking full writei = -11 (expected -11)
full buffer wait timeout = 0 (expected 0)
unsupported float format = -22 (expected -22)
tone=48000 frames (1 second), writes=188 waits=96 errors=0 thread_exit=0
```

ABI commands executed successfully (temporary ELF probe executables were removed
afterward; the source remains for repeatable verification):

```sh
wsl -e bash -lc 'i686-linux-gnu-gcc -std=c11 -D_GNU_SOURCE -static /mnt/d/tmp/piurelink/bridges/snd/check_alsa_abi.c -o /mnt/d/tmp/piurelink/out/win/check_alsa_abi && qemu-i386 /mnt/d/tmp/piurelink/out/win/check_alsa_abi && i686-linux-gnu-gcc -std=c11 -D_GNU_SOURCE -DCHECK_KERNEL_LAYOUT -static /mnt/d/tmp/piurelink/bridges/snd/check_alsa_abi.c -o /mnt/d/tmp/piurelink/out/win/check_alsa_layout && qemu-i386 /mnt/d/tmp/piurelink/out/win/check_alsa_layout'
```

Both modes printed:

```text
ALSA i386 ABI: access=3 format=2 playback=0 nonblock=1; frames=4/4 hw=604 sw=104; EPIPE=32 EAGAIN=11
```

### Configurable-mode verification (2026-10-17)

The updated provider and native tests built cleanly (exit 0, no compiler output):

```sh
C:/msys64/usr/bin/bash.exe bridges/snd/build.sh
D:/tmp/piurelink/out/win/test_snd.exe --modes
```

The suite exited 0. Selected output (all four children used a one-second
48 kHz tone, successful open/hardware params/write/drain/close, and one
statistics interval):

```text
lxsnd: WASAPI shared S16 stereo 48000 Hz -> mix 48000 Hz/32 bits/tag 65534/subtype 3; buffer 2048/2048 frames; period 10.000 ms
tone=48000 frames (1 second), writes=188 waits=97 errors=0 thread_exit=0
mode=shared obtained=shared writes=game stats_intervals=1 underruns_zero=yes result=PASS
tone=48000 frames (1 second), writes=1 waits=0 errors=0 thread_exit=0
mode=shared obtained=shared writes=blocking stats_intervals=1 underruns_zero=yes result=PASS
lxsnd: WASAPI exclusive fallback to shared: device busy at Initialize (HRESULT 0x8889000a)
mode=exclusive obtained=shared writes=game stats_intervals=1 underruns_zero=yes result=PASS
mode=exclusive obtained=shared writes=blocking stats_intervals=1 underruns_zero=yes result=PASS
lxsnd: frames=48000 underruns=0 peak=0.244141 rms=0.172621
shared/exclusive suite: PASS
```

The real game then ran for **60 seconds** with `[audio] mode = exclusive`,
audio statistics enabled, and presentation disabled so a locked desktop would
not throttle rendering. The launcher stopped it at the requested timeout.
To repeat using the current INI interface:

```sh
python tools/run.py -t 60 -o out/snd-exclusive-game.log -C out/game \
  -s debug.no_present=true -s debug.audio_stats=true out/game/piu.exe
```

```text
lxsnd: WASAPI exclusive fallback to shared: device busy at Initialize (HRESULT 0x8889000a)
lxsnd: WASAPI shared S16 stereo 48000 Hz -> mix 48000 Hz/32 bits/tag 65534/subtype 3; buffer 2048/2048 frames; period 10.000 ms
lxsnd: frames=241152 underruns=0 peak=0.000000 rms=0.000000
lxsnd: frames=240128 underruns=0 peak=0.999969 rms=0.259250
timeout after 60s (killed)
```

The complete game log contains **10 statistics intervals**, every one reporting
**`underruns=0`**, including nonzero audio after attract playback began. This
exercised configuration selection, native format acceptance, device-busy
fallback, shared playback, and timeout/process-exit cleanup. The endpoint was
busy (`AUDCLNT_E_DEVICE_IN_USE`), so **exclusive ownership/rendering, the aligned
buffer retry, converted exclusive formats, and device-invalidation recovery
were not exercised**. There was no loopback capture or subjective listening
check; a 60-second run configured for exclusive must not be described as a
successful exclusive stream when its open line explicitly reports fallback.
