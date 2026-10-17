# Dongle id and system information in piu

Static analysis of `piu` (Build ID `3bda4c41…`); addresses are original ELF vaddrs.

## Title-screen identifier ("dongle id")

The text under the version at the top left of the title screen is **not read from the
dongle**. It is drawn at `0x0809A8C4` as `"%s-%s (%s)"` with `"PIU25"`, the string at
`0x097C0150` and a market name, only when `0x097C0150` is non-empty (`0x0809A649`).

`0x097C0150` (also the `SERIAL:` line of the boot "Loading System Data..." screen) is written
only by the game-server packet handler `0x08085976`:

| Site | Source |
|---|---|
| `0x08086520` | `strncpy(…, reply, 10)` in the `SYSTEMCHECK` reply path |
| `0x0808677F` | `strncpy(…, reply+0x50, 11)`, together with machine no `0x09699994` and byte `0x09699C98` |

Offline it stays empty, so nothing is drawn. It becomes visible only when the game server's
registration/`SYSTEMCHECK` reply is accepted; see `docs/networking.md` for that protocol and
why the reply is not reachable without the SenseLock dongle.

## Dongle (SenseLock) values

`0x080E5D5E` opens the dongle session (`0x08128B80(4, 0x08219ED8, 8, &ctx 0x097BF8E0)`) and
calls the lock-response function `0x0811ACB0(ctx, 0x0A1ADCA0)`. Bytes 4–11 of the 32-byte
response, big-endian, become the 64-bit lock number `0x0A1ADC80`, copied to `0x09699998`
when non-zero. Uses:

- `"Machine No: %u Lock No: %llu"` (`0x080616EE`, bookkeeping line);
- `LOCK: %llX [%s]` on the boot system-data screen (`0x080972DA`);
- settings file names `/SETTINGS/PIU_PHOENIX_%llu.DAT`, `_ADCFG_%llu`, `_DEVICE_%llu`.

phoenixtools replaces `0x0811ACB0` (`hook_lock_response`) and returns only byte 0 = 4 and
byte 1 = region, so the lock number is 0 (hence `PIU_PHOENIX_0.DAT`). Supplying a non-zero
lock number would rename the settings files.

## SYSTEM INFORMATION screen

Data is collected by `0x080EDBB9…0x080EE340` (also run at boot) and drawn by
`0x080A6A40…` (test menu) and `0x080969A0` (boot screen):

| Line | Source |
|---|---|
| CPU | `/proc/cpuinfo` `model name` → `0x09699A0D` |
| M/B | `/sys/class/dmi/id/board_vendor` + `board_name` (`"%s %s"`), else "Unknown Mainboard" → `0x09699A8D` |
| RAM | `sysinfo()` |
| VGA | `glGetString(GL_RENDERER)` → `0x09699B0D` |
| HDD | `open("/dev/sda", O_RDONLY)` + `ioctl(HDIO_GET_IDENTITY=0x30D)`, model/serial as `"%.40s (%.20s)"` → `0x09699B8D` |
| USB | USB mode probe `0x080E7FB1` → `0x09699C0D` |
| IP / mask | `getifaddrs`, first `AF_INET` entry named `eth0` |
| Gateway | `/proc/net/route` default route |
| MAC, LAN cable | `/sys/class/net/eth0/address`, `operstate`, `carrier` |

### Freeze entering SYSTEM INFORMATION (fixed)

phoenixtools denied every raw `open("/dev/sda")`, so the HDD branch was skipped and
`0x09699B8D` kept 127 bytes of `0xFF` left by the settings image. The text renderer's UTF-8
length loop (`0x0806BA43…0x0806BA68`) advances by −1 for a `0xFE`/`0xFF` lead byte, so the
main thread spun forever at `0x0806BA56`; no frames were rendered after the menu selection.

Fixes:

- phoenixtools: read-only raw `/dev/sda` opens get the virtual disk image, and
  `HDIO_GET_IDENTITY` on it reports `PHOENIXTOOLS VIRTUAL DISK` / `PTVD0001`
  (`src/safety/disk_identity.c`; test `tests/disk_identity_test.c`). Writable raw opens stay
  blocked.
- lxrt: `/proc/net/route`, `/sys/class/net/<if>/*`, `/sys/class/dmi/id/*`, real CPU name, and
  `eth0` = the default-route adapter (a VPN TUN adapter previously became `eth0`)
  (`tests/misc/sysinfo.c`).

Two HDD/dongle binding checks also open `/dev/sda`. Their failure path jumps through an
obfuscated address to `0x080FBCA3: jmp 0x080FBCA3`, an infinite loop:

- `0x08096883` is replaced by phoenixtools (`hook_disk_id_entry`).
- `0x08096560` was not observed to hang a run; if it is reached, it now gets past `open` and
  `ioctl` and then depends on the dongle call `0x0811BFD0` (inferred from the code, not run).

## RAM line

The game prints `totalram / (mem_unit * 1024)`. lxrt reports `mem_unit = 1` with a 32-bit
`totalram`, so hosts with ≥ 4 GiB show 4194303 KB.
