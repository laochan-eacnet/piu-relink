# lxrt — ELF binder and glibc 2.31 HLE for relinked i386 Linux code

`lxrt.dll` is a native i686 Windows DLL (clang `--target=i686-pc-windows-msvc`, static UCRT,
`-mstackrealign -mstack-alignment=16`). Every module produced by `relink/relink.py` imports it.
It loads nothing itself: Windows maps all PE images; lxrt binds them with ELF semantics.

## Startup sequence

```mermaid
sequenceDiagram
    participant W as Windows loader
    participant L as lxrt.dll
    participant D as guest DLL stubs
    participant E as piu.exe entry stub
    W->>L: DllMain(PROCESS_ATTACH): canary, phoenixtools.ini, [debug] log/trace, crash handlers
    W->>D: DllMain → lx_register(&desc) (dependency order)
    W->>E: entry → lx_start(&exe_desc)
    E->>L: register EXE, process init (paths, fds, environ, stdio, threads, time, providers)
    L->>L: scope = EXE, preload, BFS(DT_NEEDED); relocate DLLs; relocate EXE; COPY
    L->>D: DT_INIT/INIT_ARRAY (deps first, preload last)
    L->>E: Linux initial stack (argc/argv/envp/auxv), x87 CW 0x037F, jmp e_entry
```

* **Lookup order**: global scope (EXE, preload modules, then DT_NEEDED breadth-first), then
  native tables: lxrt's own groups (`LX_SYMTAB(...)` in every `lxrt/*.c`), then providers
  `lxgfx.dll` (X11/GLX/VidMode/GL/GLU), `lxsnd.dll` (ALSA) and `lxusb.dll` (libusb-1.0/0.1
  over WinUSB) loaded from lxrt's directory.
  GNU/SysV hash, symbol versions (hidden versions only for exact requests), weak undefined → 0,
  unresolved functions → trap stubs that abort with `call to unimplemented function 'X'`.
* **Relocations**: `R_386_32/PC32/GLOB_DAT/JMP_SLOT/COPY/TLS_DTPMOD32/TLS_DTPOFF32/IRELATIVE`;
  `RELATIVE` is pre-applied by the relinker and emitted as PE base relocations.
  Initial-exec/local-exec TLS (`%gs:` other than the canary) is rejected at relink time — guest
  *executables* must not use TLS (piu does not); shared objects use general-dynamic TLS via
  `___tls_get_addr`.
* **COPY of native data** (`stdout`, `optind`, `tzname`, ...): the EXE copy is the live object;
  native code reads/writes such globals through `lx_data_addr(&var)`.
* **Stack protector**: each module's `%gs:0x14` accesses read a per-module slot in `.lxdata`
  initialized to one random per-process value.

## Running

All configuration lives in **`phoenixtools.ini` beside the EXE**, shared with phoenixtools and
the bridges; no environment variable configures lxrt, lxgfx, lxsnd or lxusb. Diagnostics are `[debug]`
keys (all off by default; booleans `true/false/yes/no/on/off/1/0`; relative paths resolve against
the EXE directory). `python tools/run.py -s debug.trace=true ...` sets keys for a single run.

| `[debug]` key | Effect |
|---|---|
| `trace` | log registration, scope, unresolved symbols, init order, `open()` calls, first-chance faults in foreign code |
| `trace_missing` | log each distinct failed path lookup (`stat`/`lstat`/`open`/`opendir`) once, with a timestamp — cheap enough during play; the first tool for "resource missing" bugs |
| `log = <file>` | append all lxrt log lines to a file (stderr is still written) |
| `heapcheck` | exact-size guest heap blocks with 16-byte guards; overflows are logged with allocation/free callers |
| `snapshot_dir = <dir>`, `snapshot_every = N` | dump every Nth frame as BMP (lxgfx) |
| `no_present` | render without presenting (`glFinish` instead of `SwapBuffers`); keeps headless/locked-session runs at full speed (Windows throttles presentation of a locked session to ~3 fps) |
| `audio_capture = <wav>`, `audio_stats` | tee PCM written via `snd_pcm_writei`; 5-second level statistics (lxsnd) |

Faults in guest modules, lxrt or providers are reported with a register dump and a heuristic
backtrace (guest addresses as ELF vaddrs) and terminate the process with status 139.
`tools/stacks.py <pid|piu.exe>` prints all thread stacks of a running (hung) process;
`tools/run.py` runs a program with a timeout and full 32-bit exit code; `tools/inject.py` posts
scripted keyboard input to the game window.

lxrt's sections of `phoenixtools.ini` map guest paths and serial devices (other sections belong to
phoenixtools and the bridges and are ignored here):

```ini
[mounts]
/piu/data = ../../data      ; relative to the EXE directory, or absolute
[immutable]
/piu/data = yes             ; read-only tree: cache stat results and directory listings forever
[process]
cwd = /piu/data
[serial]
/dev/ttyUSB0 = COM3         ; cabinet light board (game opens it at 9600 8N1)
/dev/ttyACM0 = COM4         ; P1 RFID card reader (115200 8N1; needs [input] virtual_card_reader = false)
/dev/ttyACM1 = COM5         ; P2 RFID card reader
```

`[serial]` maps a guest tty path to a Windows COM port (`COMn` or a full `\\.\` device name);
unmapped paths keep the filesystem behaviour (`ENOENT`). A mapped `open` takes the port
exclusively (busy → `EBUSY`, missing → `ENOENT`, each failure logged once). termios is per fd:
`tcsetattr` applies speed (all Linux `B*` codes up to `B4000000`), `CSIZE`, `CSTOPB`, parity
(`PARENB`/`PARODD`/`CMSPAR`, `INPCK`), `CRTSCTS` and `IXON`/`IXOFF` to the port; `B0` drops DTR/RTS.
Reads follow raw-mode `VMIN`/`VTIME` and `O_NONBLOCK` (`EAGAIN`); `tcflush`, `tcdrain`,
`TCSADRAIN`/`TCSAFLUSH`, `FIONREAD`, `isatty` and `dup` work. Not emulated: the line discipline
(canonical editing, echo, signals, CR/NL translation — bytes pass raw while those flags are
stored and reported), modem-control ioctls, and select/poll readiness (reported ready like other
non-socket files). The game itself only uses raw mode with `VMIN = VTIME = 0`.

`[immutable]` mounts get a process-lifetime metadata cache (`metacache.c`): stat/lstat results
(including `ENOENT`) and directory listings keyed by case-folded host path, and handle-less
`opendir` for cached directories. phoenixtools' resolver walks the whole 35 GB data tree for every
missing basename (`find_basename`); without the cache that walk takes seconds per call on
Windows and freezes song selection.

When the EXE has no console or redirected std handles (GUI subsystem, started from Explorer),
stdout and stderr are appended to `<exe name>.log` beside the EXE.

Built-in mounts: `/tmp` → `%TEMP%`, `/` → `<exe dir>/rootfs`. Drive paths (`D:/...`, as returned
by `dladdr` for relinked modules) pass through unchanged. Special files: `/dev/null`,
`/dev/zero`, `/dev/urandom`, `/dev/random`, `/proc/self/maps` (synthesized from the module
registry), `/proc/cpuinfo` (model name = the Windows `ProcessorNameString`), `/proc/meminfo`,
`/proc/net/route` and `/sys/class/net/<if>/{operstate,carrier,address,mtu}` (same adapter list
and names as `getifaddrs`), `/sys/class/dmi/id/{board,sys,bios}_*` (Windows BIOS registry
values, e.g. `BaseBoardManufacturer`/`BaseBoardProduct`).

## Guest heap

16-byte aligned blocks from a private Win32 heap (guest corruption cannot damage the CRT heap).
Usable size follows glibc i386 chunk rounding (`max(16, (n + 4 + 15) & ~15) - 4`), so small
overruns that glibc tolerates stay harmless. Everything returned to the guest (strdup, getcwd(NULL),
getline, scandir, ...) comes from this heap.

## Semantics and known differences by area

| Area | Notes |
|---|---|
| Files | POSIX share modes; `O_APPEND` seeks before every write; `stat` uses `GetFileInformationByName` (no handle open) where available; uid/gid 1000, synthetic modes (0755 dirs, 0644 files, readonly → no write bits); symlink creation needs Windows privilege (else `EPERM`); `realpath` is lexical; `mkdir` of any existing path → `EEXIST`. |
| Directories, glob | `readdir` snapshots, includes `.`/`..`; glob matching is case-sensitive while lookups are case-insensitive; GNU alternate-directory callbacks unsupported. |
| mmap/mprotect | anonymous + file mappings (shared/private/COW, unaligned offsets); `mprotect` back to executable flushes the instruction cache (phoenixtools patches piu's `.text`); `MAP_FIXED` limited by occupied 64 KiB regions/PE images. |
| stdio/printf/scanf | own FILE (opaque), own format engine and scanner (positional args, 12-byte x87 `long double`, 4-byte `wchar_t`, `%m`); output byte-identical to glibc for the test tables; `popen` → `ENOSYS`. |
| Threads/sync | pthread objects live inside the guest's glibc-sized objects (zero initializers work); legacy `pthread_cond_*@GLIBC_2.0`; futex `syscall(240)` WAIT/WAKE on `WaitOnAddress`; full stack commit for threads (Linux code doesn't probe stacks); thread entry sets x87 CW 0x037F; no robust/PI mutexes, no cancellation; `pthread_kill` with non-zero signals → `ENOSYS`. |
| Time | 32-bit `time_t`; local time from the Windows time zone (not `TZ`); high-resolution waitable timers for sleeps. |
| Locale/ctype/wchar/iconv | "C"/"POSIX" and `*.UTF-8` (as C.UTF-8); glibc-layout `__locale_struct` and ctype tables (byte-identical to glibc); iconv: UTF-8/16/32, UCS-2/4, Latin-1, ASCII, CP949/EUC-KR, Shift_JIS, GBK, CP1252 with `//TRANSLIT`/`//IGNORE`. |
| Math | double/float forwarded to UCRT (last-ulp differences vs glibc for some transcendental functions; `remquo` quotient bits differ); `long double` functions in x87 asm. Prism caveat: its x87 emulation keeps only double precision (1 + 2⁻⁶⁰ with PC=extended rounds to 1; `fld %st(0)` after `fldt` rounds both copies), so guest `long double` *arithmetic* is double-precision; the 12-byte ABI (args, returns, `%Lf`, `strtold`) is exact. `tests/locale/math.c` skips its 80-bit arithmetic checks when the host lacks them. |
| Network | Winsock with Linux constants/structs; sockets are natively non-blocking with emulated blocking; `getifaddrs` (glibc order: IPv4 entries, then IPv6) yields `lo` + `eth0` = the up adapter with the lowest-metric IPv4 default gateway (VPN/TUN adapters without a gateway become `eth1…`); `socketpair` → `ENOSYS`; socket `dup` → `EOPNOTSUPP`. |
| Process | `fork`/`exec*`/`system` never run host programs (mixer helpers are patched out by phoenixtools; busybox `reboot` `execl` sites return `-1/ENOENT`); `mount/umount` no-ops for `/mnt/0`, `/mnt/1`, `/mnt/hd`. |
| Signals | handlers recorded; `raise` invokes them; timers deliver on a native thread; no fault-to-signal translation. |
| setjmp | glibc jmp_buf layout, pointers not mangled. |

## Building and testing

```sh
C:/msys64/usr/bin/bash.exe lxrt/build.sh [outdir]           # out/win/lxrt.dll
C:/msys64/usr/bin/bash.exe tools/lxtest.sh tests/binder/libtlsm.so.c tests/binder/fopenhook.preload.so.c tests/binder/binder.c
C:/msys64/usr/bin/bash.exe tools/lxtest.sh tests/cxx/libthrow.so.cc tests/cxx/cxx.cc
C:/msys64/usr/bin/bash.exe tests/run-all.sh       # everything below
C:/msys64/usr/bin/bash.exe tests/{stdio,fs,thread,locale}/run.sh   # compare against qemu-i386 + glibc 2.31
C:/msys64/usr/bin/bash.exe tests/serial/run.sh COM1 COM2  # needs two linked COM ports (null modem / virtual pair)
```

Test programs are i386 ELF built against `sysroot/` (Ubuntu focal glibc 2.31, `tools/mksysroot.sh`
in WSL); reference output comes from `qemu-i386 -L sysroot -E LD_LIBRARY_PATH=sysroot/lib/i386-linux-gnu`.
