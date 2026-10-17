# Phoenix networking: protocol and private server

This investigation targets the unmodified i386 `piu`, Build ID
`3bda4c418b536bc77a6aa79db790cff14b9656c1`. Addresses are original ELF virtual
addresses, not relocated Windows DLL addresses. Static evidence comes from
`imports.tsv`, printable strings mapped through ELF PT_LOAD segments, and
`i686-linux-gnu-objdump -d --no-show-raw-insn piu` in WSL.

**Current transport:** an explicitly configured private LAN or loopback
`api_base` opts the client into the private-server trailer format: the 64-byte
trailer is not passed through SenseLock. Framing, RC4, magic and SHA-1 validation
remain intact in both directions. Empty configuration still blocks networking.
Section 8 documents the LAN service and customization page; section 9 covers
runtime card insertion and the Windows serial-hardware boundary.

Sections 1–6 preserve the original investigation and recorder-era results;
section 7 records the initial registration/login/score cutover. Their earlier
feature limits are superseded by the expanded service in section 8.

## 1. Endpoints and transport

All discovered literal URLs use plain HTTP, not HTTPS. The imported easy API
has six functions: init, setopt, perform, getinfo, cleanup, strerror. There are
four `curl_easy_perform` call sites; **no POSTFIELDS/POST option is set by these
callers**. Hooks nevertheless log a bounded POST-body summary if one is set.

| Method / transport | Original host:port and path | Request fields | Response / consumer | Flow and call-site vaddrs |
|---|---|---|---|---|
| GET, curl HTTP | `www.piugame.com:80/piu.module/piu.get_ip_to_country.php` | None at this call site | JSON object `country_data` with **string** `country_no` and `ip`; consumer requires both keys and calls `strtol` on country_no's cJSON valuestring. Local response is `{"country_data":{"country_no":"0","ip":"127.0.0.1"}}`. | Startup: URL at `0x08074b0a`, perform `0x08074b6d`, body callback `0x080824c3`, parse `0x08074014`, object/key lookup `0x08074024/36/48`, numeric conversion `0x08074067`. Sets `0x096999a0`, not the online handshake flag. |
| GET, curl HTTP | `www2.piugame.com:80/ip/` | None | Response appended to a string; `curl_easy_getinfo(CURLINFO_RESPONSE_CODE)` called, successful transfer returns true. Local service returns `127.0.0.1`. Exact external text formatting is not independently captured. | IP/connectivity helper `0x0807d364`; URL `0x0807d392`, callback `0x080a1b98`, perform `0x0807d3dd`, getinfo `0x0807d429`. |
| GET, curl HTTP | `xx-cdn.piugame.com:80/2023/update/<dynamic filename>` | Dynamic suffix, no decoded POST fields | Download bytes/file; updater filenames include `piu_phoenix_2120.bin` and `key_phoenix_2120.bin`. No update manifest response is fabricated. | Updater origin string `0x081cfef0`, referenced `0x080962f7`; generic string download perform `0x080820cd`, file download perform `0x0808239b`. Updater entry `0x08095ff0` remains disabled. |
| GET, curl HTTP | `xx-cdn.piugame.com:80/2023/ad/<dynamic filename>` | Dynamic suffix not fully decoded | Advertisement download bytes; no mock media supplied | Origin `0x081cfe8c`; uses generic download helpers above. |
| GET, curl HTTP | `update-kr.piugame.com:4989/dummy/test500mb.db` | None visible at literal URL reference | Speed-test bytes; local service returns 404 rather than claiming a 500 MB transfer | Network diagnostic: URL reference `0x080ad44c`, string `0x081d1820`; generic curl download helpers. |
| DNS + application TCP | `primedaemon.piugame.com:50400` | Framed binary messages below | Protected binary replies; not HTTP | Resolve helper `0x08066d64`, call `0x0807428d`, IPv4 buffer `0x0979e760`; socket `0x08087dbb`, connect `0x08087e99`, send `0x080838d2`, recv `0x08088a75`. |
| DNS | `tenporouter.loc` | DNS name | IPv4 string at `0x0a1adcc0`, included in machine request | Resolve `0x080742a9`; fallback string at `0x081c764c`. |

The 11-entry regional port table at `0x081c9560` and two-entry retry table at
`0x081c5780` contain only decimal 50400. Port global is `0x0979e7a0`, initialized
at `0x080742cd`; connection uses `inet_addr` and `sockaddr_in`.
`fcntl(O_NONBLOCK)`, writable `select` (`0x08087f3b`) and `SO_ERROR` implement
connection completion, followed by restoration of blocking mode. Readable
`select` is at `0x08088868`.

There is one application `socket` call, AF_INET/SOCK_STREAM, and no identified
UDP/datagram game path. `send`/`recv` carry binary packets independently of curl.
Interface enumeration `getifaddrs` (`0x080edbcf`) selects `eth0`; it does not
establish that an online registration succeeded.

## 2. Framing, encryption and authentication

### Ordinary packets

For application payload length N (other than four):

```text
wire[0:4]     = LE32(N + 0x4c)          # total length
plain        = LE32(sequence) || LE32(command) || payload
key          = 32 independent low bytes from rand()
digest       = digest-selector-0x1a(plain), 20 bytes
trailer      = magic[12] || key[32] || digest[20]    # 64 bytes
wire[4:-64]  = RC4(key, plain)
wire[-64:]   = SenseLock-protect64(session, trailer)
magic        = 37 53 ae 51 b7 3d 86 58 27 eb 86 fa
```

The send builder is `0x080836c2`: EAX=transport subobject, EDX=command,
ECX=payload, stack argument=payload size. It checks the payload-plus-header
against `0x281f`, increments sequence at transport+0x14, and handles partial
`send` calls. RC4 is `0x0805d88a` (fresh 256-byte KSA/PRGA each packet; fixed
32-byte key). `srand(time(NULL))` is at `0x080838f7–0x08083915`.

**The RC4 key does not derive from the dongle.** It is generated by `rand` and
then carried inside the protected trailer. This is not evidence of a secure
CSPRNG, a TLS session, a public-key signature, or an HMAC.

Digest selector `0x1a`, dispatched by `0x0815cca0`, produces 20 bytes and uses
the SHA-family initial words `67452301 efcdab89 98badcfe 10325476 c3d2e1f0`
(`0x08160760`) and round constant `0x5a827999`. **[INFERENCE] SHA-1**; the complete
message schedule was not independently audited to distinguish SHA-1 from SHA-0.
The dynamically exported MD5 names are not evidence that packet signing is MD5.

### Exactly 16-byte packets

When **payload size equals four**, not when any particular command is selected,
the send path emits only `LE32(16), LE32(sequence), LE32(command), LE32(value)`.
There is no RC4, digest, or trailer. Heartbeat `0x03000000`, value `0x0000ffff`,
is an actual use (`0x08097f04–0x08097f1d`). The heartbeat handler is
`0x08085b59–0x08085c49`. A 16-byte packet cannot substitute for a larger
registration/login response: those handlers read substantially larger bodies.

### Both directions require the protected operation

Send: `0x08083817–0x0808382a` calls
`0x081216d0(0x097bf8e0, trailer, 64)` under mutex `0x097bf8f4`.
It wraps backend `0x0813fb30(session, 7, in, length, out, &length)`.

Receive: `0x08088a16` accumulates up to `0x2820` bytes and handles fragmented
and coalesced frames. At `0x08088b65–0x08088ba3` it copies the last 64 bytes and
calls **`0x08122020(0x097bf8e0, trailer, 64)`**, also under the mutex.
It compares all 12 magic bytes (`0x08088bb4–0x08088bde`), obtains the RC4 key from
the transformed trailer (`0x08088bf2`), recalculates the digest (`0x08088c4a`) and
compares 20 bytes (`0x08088c62`). Only then does it dispatch the command
(`0x08088c8d–0x08088cb8`). Invalid magic/digest returns without dispatch.

Therefore merely ignoring authentication on **requests** in a local server is
insufficient: the game also transforms and verifies **replies**. The vendor
function's return is not explicitly tested at these call sites, but magic and
digest validation still occur; this investigation does not exploit failure
behavior to supply unprotected ordinary responses.

Session initialization is at `0x080e5d5e`, through
`0x08128b80(4, 0x08219ed8, 8, &session)`. The same session is used by the lock
response function `0x0811acb0` and disk-ID operation `0x0811bfd0`. The existing
hook replaces only the lock response with a non-secret region value. It supplies
neither an authorized hardware session nor the protected trailer transform.
No secret extraction, brute force, transform emulation or acceptance patch was
attempted. Actual protected bytes/context derivation are outside this local
server's implementation boundary.

## 3. Machine, card, profile and other commands

The client initiates registration; there is **no initial server greeting**.
After connect, `0x0809786d` calls builder `0x0807ea98`, sending command
`0x00132003`, payload `0x394`, wire length `0x3e0`. Payload is:

| Offset | Bytes | Meaning/source |
|---|---:|---|
| 0 | `0x310` | Machine/configuration block `0x09699994` |
| `0x310` | `0x74` | Block `0x0962b920` |
| `0x384` | 16 | Router/IP text from `0x0a1adcc0`, forced NUL |

Registration reply `0x132004` dispatches to `0x080865d9`. It consumes at least
`0x5c` bytes: u32 machine value at +0, low-byte state at +4, descriptive string
72 bytes at +8, identifier string 12 bytes at +0x50. At `0x080867bf` it sets
handshake-completed global `0x0979e7d4=1`. State byte `0x09699c98==1` suppresses
several network paths; its meaning is not established, so it is **not** chosen
as an invented success code. An alternate system-check request/reply pair is
`0x132001/0x132002`; successful response requires its first two u32s both zero,
then consumes identifier[12], machine value and another u32 (28 bytes total).

The title-screen top-left identifier is **server-assigned, not a dongle ID**.
Renderer `0x0809a8c4` formats `PIU25-%s (%s)` using string `0x097c0150`.
It is filled by the successful SYSTEMCHECK reply `0x132002` (copy at
`0x08086520`, ten characters) or registration reply `0x132004` (copy at
`0x0808677f`, eleven characters). Both are ordinary protected replies, so the
local service does not invent this identifier before their authenticated path
is reachable.

### Complete located outbound command table

All rows are calls to `0x080836c2`; sizes are application payload bytes, not
wire lengths. Unnamed rows are not falsely labeled rankings/scores/unlocks.

| Command | Payload bytes | Send call | Established role |
|---|---:|---|---|
| `0x132001` | `0x318` | `0x080855f2` | Alternate machine/system check |
| `0x132003` | `0x394` | `0x0807eb4c` | Machine registration |
| `0x132005` | `0x3c` | `0x080758b1` | Card lookup/login request |
| `0x13200a` | `0x6c` | `0x0808586d` | Played-song result upload (score/grade/judgements), below |
| `0x13200c` | `0x0c` | `0x080857b4` | Unknown |
| `0x13200d` | `0x14` | `0x0808571c` | Unknown |
| `0x13200e` | `0x2c` | `0x0808567e` | Machine/profile identifiers + loaded profile prefix[36]; not settings save |
| `0x13200f` | `0x28` | `0x080850c6` | Unknown |
| `0x132011` | `0x24` | `0x08085242` | Unknown |
| `0x132015` | `0x210` | `0x08084e5b` | Four words + formatted text[512]; not proven a score packet |
| `0x132016` | `0x14` | `0x08070e7b` | Player u32, profile ID u32, then 12 version bytes from `0x09699a01` |
| `0x132019` | `0x40` | `0x08084b66` | Player/session and selected-song metadata; not played result |
| `0x13201c` | `0x10` | `0x08084a7a` | Unknown |
| `0x13201d` | `0x10` | `0x080847e3` | 16-byte block from `0x096fc3c0` |
| `0x132020` | `0x10` | `0x08084730` | Unknown |
| `0x132021` | 4 | `0x08084682` | Unknown; plaintext size exception |
| `0x132022` | `0x34` | `0x08084423` | Unknown |
| `0x132024` | `0x10` | `0x08084175` | Same 16-byte block as `0x13201d` |
| `0x132026` | `0x10` | `0x08083c39` | Unknown |
| `0x132027` | `0x10` | `0x08083b86` | Unknown |
| `0x132028` | `0x10` | `0x08083ed7` | Unknown |
| `0x13202b` | 4 | `0x080641e2` | Unknown; plaintext size exception |
| `0x13202c` | 4 | `0x08083ad9` | Unknown; plaintext size exception |
| `0x13202e` | variable | `0x08083a2e` | Screenshot/image chunks: count,total-size,index,bytes; calls `0x080bc5de/0x080bcbc5` |
| `0x13202f` | 8 | `0x080858f2` | Unknown |
| `0x132033` | 8 | `0x080859d2` | Unknown |
| `0x132036` | 4 | `0x0808595f` | Unknown; plaintext size exception |
| `0x13203b` | `0x30` | `0x0807eda2` | Unknown |
| `0x03000000` | 4 | `0x08097f1d` | Heartbeat, value `0xffff` |

### Card request and profile response

Builder `0x080757ec` takes EAX=session, EDX=player, ECX=hex-card-text. Its
60-byte `0x132005` payload is six u32s (player, machine value from `0x09699994`,
region byte `0x096999a4`, country value `0x096999a0`, zero, one) followed by
char[36] card text, final byte forced NUL. After successful request submission,
the reader consumer advances player state from 1 to 2 at `0x0807e82b`.

Reply `0x132006` dispatches `0x080867fb` → `0x080654f6`. It consumes
`0x180` bytes:

| Body offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | Player 0/1 |
| 4, 8 | 4 each | Status/profile-associated values; stored separately and at record+0x68/+0x64 |
| `0x0c` | 4 | Resource-path-present flag |
| `0x10` | 256 | Resource path (not nickname/settings); may initiate download worker `0x08081936` |
| `0x110` | 112 | Profile record copied to `R = 0x096fc7f4 + player*0x3c090` |

Identified record fields: R+0x24 nickname (displayed uppercase, maximum 12
characters), R+0x34 numeric discriminator (formatted ` #%04d`), R+0x3c
profile-associated identifier (the local service uses the user ID), R+0x40
title byte, and R+0x42 avatar byte. The original investigation had these two
cosmetic fields reversed; direct Hex-Rays analysis of `0x0809d7b8` and its
`0x08064870` call proves the corrected order. R+0x41 is propagated in matching
metadata but has no established customization meaning. The identifier does
not overlap either cosmetic field.

Short reply `0x132007`, handler `0x0808761b`, reads player/status; status 1
advances the ordinary profile-load completion path. Paged reply `0x132009`,
parser `0x080653a6`, carries four u32s (player, zero-required status, starting
record index, count), followed by a fixed 6144-byte area of 256 records, each
24 bytes. Further analysis identifies these as native per-chart best records:
song u32 at 0, selector/mode/flags/grade/category bytes at 4–8, score u32 at 12,
play count u32 at 16, and an opaque u32 at 20.

`0x13200e` builder `0x0808560a` sends u32 machine ID, u32 profile-associated
ID, then R[0:36]. Callers include `0x08089148`, `0x080af437`, `0x080c2cf0`.
It does **not** send the nickname, avatar or full settings record. A dedicated
nickname/avatar/settings save request has not been identified.

### Played-song submission and acknowledgement

**`0x13200a` is the actual result submission**, builder `0x080857dc`, current
record call `0x080d9e59`, queued-record retry call `0x080d9dd4`. The completion
method `0x080d9028` builds a 96-byte record, retains it in the result-history
vector at player+0x204a4/+0x204a8/+0x204ac, and appends three u32s to send 108
bytes. The registered `PLAY_RESULT` renderer `0x08082de6` reads the same
96-byte records and displays score+0x0c (`0x080833a4–0x080833e4`).

| Payload offset | Bytes | Meaning/source |
|---|---:|---|
| 0 | 4 | Selected song-table first word [INFERENCE: song ID] |
| 4 | 1 | Chart selector, selected chart+9; enumeration unknown |
| 5 | 1 | `(chart[0x0a] & 3) << 6`, values 1–3 |
| 6 | 1 | Gameplay flags 0x80/0x20; names unknown |
| 7 | 1 | Grade code from `0x080d3354` |
| 8 | 1 | Clear/judgement-quality category [INFERENCE]; 0xff can mean invalid/failure |
| `0x0c` | 4 | Calculated score |
| `0x18` | 4 | Machine value from `0x09699994` |
| `0x1c` | 4 | Profile-associated ID, player-state+0x50 |
| `0x20` | 2 | Country value, low 16 bits of `0x096999a0` |
| `0x22` | 1 | Set to 1 when buffered after failed/unavailable submission |
| `0x24`–`0x34` | 5 x 4 | Counters at player+0x20230/+0x20234/+0x20238/+0x2023c/+0x20240; [INFERENCE] Perfect, Great, Good, Bad, Miss |
| `0x38` | 4 | Maximum combo [INFERENCE], player+0x2027c |
| `0x3c` | 4 | Float-valued statistic, ordinarily zero; meaning unknown |
| `0x40`, `0x44` | 4 each | Player+0x203cc/+0x203d0, option bitfields [INFERENCE] |
| `0x48`, `0x4a` | 2 each | Low words of player+0x203d4/+0x203e8 |
| `0x4c` | 4 | Sign-extended word from player+0x203dc |
| `0x50` | 4 | Float from player+0x203e4 |
| `0x54` | 12 | Version/identifier text from `0x09699a01`, eleven copied characters then zero |
| `0x60` | 4 | Player index |
| `0x64`, `0x68` | 4 each | Completion-context values, defaults 10 and 11; names unknown |

From `0x080d9a6f–0x080d9b27`, with N=player+0x20268 and the first four
judgement counters P,G,O,B and combo C:

```text
score = trunc(995000 * (P + 0.6*G + 0.2*O + 0.1*B)/N + 5000*C/N)
```

N=0 or nonzero global modifier `0x096fc374` produces zero. Grade codes 0–15
are best-to-worst, thresholds 995000, 990000, 985000, 980000, 975000, 970000,
960000, 950000, 925000, 900000, 825000, 750000, 650000, 550000, 450000, else.
A gameplay flag can add 0x10; invalid/zero-total conditions yield 0xff.
Letter labels have not been independently mapped.

Reply `0x13200b` handler `0x080869f7–0x08086ae5` resets timing/counters and
consumes **no result payload fields**. This does not prove the actual response
has an empty body. Buffered entries (stride 108) are removed on successful
transport-send return, not after receipt of this acknowledgement.

### Unlocks, rankings and non-score transfers

`0x132017` handler `0x0808753c` consumes player/event target at +0, status at
+8 (must equal 1) and title ID at +0x10 (at most 255), then creates
`CGameEventShowAvatar` through `0x080ee685`. Despite that internal class name,
the consumer indexes the 213-entry title table at `0x081c57a0`, not avatar
graphics. This is a title acquisition/display notification.

`0x132012` writes a 1024-byte update-provisioning block to
`/SETTINGS/DOWNLOAD/key_phoenix_2120.bin` with an Adler-32. It is not a profile
save or ranking table, and its protected contents were not analyzed.
`0x13202e` uploads image bytes obtained from `glReadPixels` (320x240 RGB),
processed into `0x2000`-byte chunks; its three words are chunk length, total
image length and zero-based chunk index. `0x132019` is selected-song/session
metadata, with no played judgement counters.

No HTTP login, profile save/load, score, ranking or unlock endpoint was found.
Rankings/general song-unlock semantics and a dedicated full-profile/settings
save operation remain unknown. There is no trustworthy database row to quote
for an authenticated profile or result until registration/login and the
protected reply path are available.

## 4. Card hardware path and injection

This path is **serial RFID**, not the synthetic LXIO panel report:

* `0x0807444f/0x08074464` open two reader objects `0x097bbd20` and
  `0x097bc278` (stride `0x558`). Enumerator `0x0810ca78` probes
  `/dev/ttyACM0` through `/dev/ttyACM9`, requesting 115200 baud.
* Opener `0x0810c966` uses `open(path,0x902)`, fcntl, termios and tcflush.
  A separate `/dev/ttyUSB0` path/object `0x097bc7e0` is opened at 9600 baud
  (`0x0807443b–0x0807444a`); it is not proven to be a second card protocol.
* Poller `0x0810d38b`, outgoing processor `0x0810ac52` and parser
  `0x0810a23a` service ACM readers. Card frame is
  `AA 03 01 xx L ID[L] CHECKSUM FF`, with XOR of bytes 1 through `L+4`.
  Byte 3's meaning remains unknown. IDs are raw bytes; no fixed UID length
  is established.
* Parser enqueues `id[128], uint32 length` (`0x84` bytes). Consumer
  `0x0807e614`, called `0x08099a99/0x0809afe5`, checks player state 1,
  maps readers through `0x097c0148[player]`, dequeues, and converts bytes to
  lowercase hex without reversing them. Player state stride is `0x3c090`,
  base `0x096fc7e0`. It calls the normal card request builder at `0x0807e812`.
* LXIO's synthetic 0d2f:1010/1020 report supplies panels/service/test/coin,
  not a card UID. The direct debugfs USB-control paths are not established
  as RFID input. The traced `/mnt/0` mass-storage discovery leads to
  `piu_phoenix_2120.bin` updater media, not a user-profile file format.

The current keyboard reader uses explicit P1/P2 insertion keys and
`card1.txt`/`card2.txt`; see section 9. UIDs remain limited to 2–34 hex
characters because the native 36-byte field holds at most 17 complete pairs
plus NUL. The older one-shot startup environment injector is no longer used.

In emulation mode, `network_card.c` validates the exact 11 original instruction
bytes at `0x0807e671` before installing its detour. When the original consumer
reaches player state 1, a pending UID supplies the local `0x84`-byte result at
original ESP+0x4c and resumes at `0x0807e6f8`, after the reader mutex unlock.
Without a pending UID, emulation advances to the next player at `0x0807e64f`
rather than consuming a real-reader event. With emulation disabled, no detour
is installed. Native hex conversion, request serialization and login validation
remain intact; screen/network gates are not bypassed.

## 5. Original local recorder README (before private-server cutover)

Python 3, standard library only (`http.server`, `socketserver`, `sqlite3`). From
the repository root:

```console
python server/piuserver.py --db server/piu.db --port 8080
```

It binds HTTP to `127.0.0.1:8080` and binary recording/heartbeat to
`127.0.0.1:15040`. Original game port 50400 can fall in a Windows-reserved port
range (binding it returned WinError 10013 in this investigation); local port
15040 avoids that collision. `--game-port N` changes the listener; set the same
`[server] game_port = N` in the game's `phoenixtools.ini`. `--game-port 0` disables the
TCP listener. `--country N` changes the country-number response string.
Unknown HTTP paths return 404, not fabricated success. The game updater remains
suppressed and no real host is contacted.

Set the **run directory's** `phoenixtools.ini`:

```ini
[offline]
verbose = true
[server]
api_base = http://127.0.0.1:8080
cdn_base =
```

Only `http://127.0.0.1[:port][/prefix]` and
`http://localhost[:port][/prefix]` bases are accepted. Original request paths
are appended; a prefix therefore needs matching server paths. `cdn_base`
separately routes original `/2023/` CDN downloads. An empty/invalid base
becomes `offline://blocked`. Original HTTPS destinations can be redirected to
local **plain HTTP**, but never contacted directly. The transport itself also
rejects HTTPS, non-loopback hosts, proxies, redirects and transfer-encoding.
It implements the easy options actually used by the game, write/progress
callbacks, response-code/size/time info, partial socket I/O and timeouts.
It is not a general replacement for libcurl.

Default empty config blocks game HTTP, `gethostbyname`, `connect` and `sendto`.
Opt-in DNS is resolved in-process to 127.0.0.1, without a real DNS query.
Original TCP destination :50400 is rewritten to the local game port; all other
non-loopback connects are denied. This covers the game's identified imported
network surface, not an OS-wide sandbox for arbitrary future libraries.
`tools/mkgame.sh` now relinks the actual built
`phoenixtools/bin/lib/network/libcurl.so.4`, not the stale top-level stub.

### Persistence and request logging

Schema is created on startup:

* `users`: unique card_id, nickname, avatar_settings BLOB, created, last_login.
* `profiles`: user FK, version, exact save_data BLOB, parsed_fields JSON text,
  timestamp; each save can be retained as a version.
* `scores`: user FK, song_id, chart, score, grade, judgements JSON, timestamp.
* `machines`: machine ID, info BLOB, registered, last_seen.
* `requests`: every handled HTTP request, exact body, path, status, timestamp.
* `tcp_packets`: exact received socket chunks and actual heartbeat replies,
  per-connection identifiers and timestamps. Chunks need not equal frames.

User/profile/score/machine tables stay empty unless a real decoded flow can
populate them. They are **not** filled by synthetic HTTP test calls or by
unauthenticated gameplay guesses. Each HTTP request prints method/path/body
summary; the TCP parser logs plaintext commands and opaque protected frame
lengths, accepting fragmentation/coalescing and echoing actual plaintext
heartbeat frames only.

## 6. Original verification evidence and limitations (before cutover)

The initial native run exposed missing `__recv_chk@GLIBC_2.4`; this was fixed in
lxrt with the real five-argument ABI and an overflow check delegating to the
existing `__chk_fail` fatal path. It is not a blanket success shim. The regular
loopback suite now explicitly exercises fortified receive and EBADF behavior.
Runtime/file/test paths and final observed output are recorded below.

### Native default and local HTTP runs

`out/net/default-block.log` (30-second native run, empty bases):

```text
[phoenixtools:network] URL http://www.piugame.com/piu.module/piu.get_ip_to_country.php -> offline://blocked
[phoenixtools:network] HTTP complete curl=1 status=0
[phoenixtools:network] DNS blocked primedaemon.piugame.com
[phoenixtools:network] DNS blocked tenporouter.loc
```

Historical `out/net/startup-fixed.log` capture (70 seconds, `LXRT_TRACE=1`,
verbose=true, local API and the since-removed one-shot injector):

```text
[phoenixtools:network] armed one-shot P1 card UID 01020304 (requires normal online reader state)
[phoenixtools:network] URL http://www.piugame.com/piu.module/piu.get_ip_to_country.php -> http://127.0.0.1:8080/piu.module/piu.get_ip_to_country.php
[phoenixtools:network] HTTP complete curl=0 status=200
[phoenixtools:network] DNS isolated primedaemon.piugame.com -> 127.0.0.1
[phoenixtools:network] DNS isolated tenporouter.loc -> 127.0.0.1
```

The same trace records `/dev/ttyUSB0` and `/dev/ttyACM0`–`9` probes.
This first local run could not reach TCP: `/sys/class/net/eth0/operstate` was
missing. Static checker `0x080edda9–0x080edde8` requires the interface's actual
`up` state; returning country JSON alone does not satisfy it. Native adapter
selection/sysfs support was subsequently added by the runtime integrator,
derived from the actual default-gateway adapter rather than an unconditional
online flag. The integrated run below reaches TCP.

Python-client **self-tests**, separate from game traffic, exercised both HTTP
responses and two heartbeat frames split/coalesced across sends:

```text
HTTP self-test 200 /piu.module/piu.get_ip_to_country.php?probe=stdlib {"country_data":{"country_no":"0","ip":"127.0.0.1"}}
HTTP self-test 200 /ip/?probe=stdlib 127.0.0.1
TCP connection=1 plaintext seq=42 command=0x03000000 value=0x0000ffff
TCP connection=1 heartbeat reply
```

They establish server framing/HTTP behavior, **not** a successful game login or
game-origin heartbeat. Test query parameters distinguish these HTTP rows.

### Integrated card/gameplay run: what actually reaches the server

The integrated runtime exposes `/sys/class/net/eth0/{operstate,carrier,address,mtu}`
and `/proc/net/route` using the actual adapter inventory, selecting the IPv4
default-gateway adapter as eth0. On this machine that is WLAN, 192.168.5.11
with gateway 192.168.5.1, rather than the unrelated 6.6.5.3/32 tunnel.

Historical commands exercised before runtime-key scanning; the environment
injector shown below is not supported by the current build:

```console
wsl -e bash -lc 'cd /mnt/d/tmp/piurelink/phoenixtools && make'
C:/msys64/usr/bin/bash.exe tools/mkgame.sh out/game-net
python -u server/piuserver.py --db out/net/piu.db --port 8080
python tools/run.py -t 320 -o out/net/gameplay.log -C out/game-net -e LXGFX_NO_PRESENT=1 -e LXRT_TRACE=1 -e PHOENIXTOOLS_CARD_ID=01020304 -e LXGFX_SNAPSHOT=D:/tmp/piurelink/out/net/snap -e LXGFX_SNAPSHOT_EVERY=1800 -i out/m7/s3.txt out/game-net/piu.exe
```

The run copy's INI had verbose=true and api_base=http://127.0.0.1:8080.
`tools/run.py` ended with `timeout after 320s (killed)`, not a game crash.
The injector log contains both coins, the start/selection taps and all three
repeat loops. Selected exact game log lines:

```text
[phoenixtools:network] HTTP complete curl=0 status=200
[phoenixtools:network] game TCP isolated -> 127.0.0.1:15040 fd=3
[phoenixtools:network] TCP send bytes=992 head=e0 03 00 00 ...
[phoenixtools:network] injected P1 reader event (4 UID bytes)
[phoenixtools:network] TCP send bytes=136 head=88 00 00 00 ...
```

Here the shown `...` marks omitted bytes from the logged 64-byte prefix.
The 992-byte machine request and 136-byte card request match the statically
identified 0x394/0x3c payloads plus 76-byte framing. Card injection is thus
**exercised**, not merely armed.

`out/net/server.log` preserves the actual server transcript. It records:

```text
TCP connection=5 protected frame bytes=992 ...; not decrypting/forging dongle trailer
TCP connection=7 protected frame bytes=136 ...; not decrypting/forging dongle trailer
TCP connection=11 plaintext seq=2 command=0x03000000 value=0x0000ffff
TCP connection=11 heartbeat reply
TCP connection=15 protected frame bytes=184 ...; not decrypting/forging dongle trailer
```

The matching game connections make the latter heartbeat **game-origin**, unlike
the earlier Python self-test. Two actual game heartbeat frames were received
and answered. No claim is made that a heartbeat establishes registration.
An 184-byte frame is the expected size of a 108-byte result upload; its exact
encrypted command is not decoded solely from the length.

Snapshots (original BMPs in `out/net/snap`, reduced PNGs beside the log) show:

* `frame-1800.png`: title/card loading UI, explicitly **OFFLINE**.
* `frame-3600.png`: stage 1 gameplay with active arrows/judgements.
* `frame-7200.png`: **PLAY RESULT**, Queencard, score **42,078**, grade **F**,
  default guest name PUMPITUP.

This demonstrates real guest gameplay/result rendering and an emitted
result-sized packet, **not** authenticated score persistence. Card login cannot
complete because the server cannot construct the protected registration/profile
replies. The server deliberately sends no forged ordinary packet.

### SQLite rows, queried with Python's sqlite3

Database: `out/net/piu.db`. Selected actual query results:

```sql
SELECT id,method,path,status,length(body) FROM requests ORDER BY id;
```

```text
(1, 'GET', '/piu.module/piu.get_ip_to_country.php', 200, 0)
(2, 'GET', '/piu.module/piu.get_ip_to_country.php', 200, 0)
(3, 'GET', '/piu.module/piu.get_ip_to_country.php?probe=stdlib', 200, 0)
(4, 'GET', '/ip/?probe=stdlib', 200, 0)
(5, 'GET', '/piu.module/piu.get_ip_to_country.php', 200, 0)
```

The final integrated run is HTTP row 5. Selected rows of
`SELECT id,connection,direction,length(data) FROM tcp_packets ORDER BY id`:

```text
(6, 5, 'receive', 992)
(8, 7, 'receive', 136)
(13, 11, 'receive', 16)
(14, 11, 'send', 16)
(16, 15, 'receive', 184)
(21, 19, 'receive', 16)
(22, 19, 'send', 16)
(24, 23, 'receive', 184)
```

Counts from the required application tables:

```text
('users', 0)
('profiles', 0)
('scores', 0)
('machines', 0)
```

These zeroes are the honest limit of this run, not successful account/score
rows. The missing prerequisite is an authorized SenseLock protected-trailer
operation for ordinary **server replies** (and the matching request decoding),
not an absent HTTP route or a card-reader guess. No secret extraction,
brute force, dongle emulation, plaintext ordinary reply, or verification patch
was used. A separate full-profile/settings save command and ranking/unlock
schemas also remain semantically unmapped.

### Regression checks exercised

`wsl -e bash -lc 'cd /mnt/d/tmp/piurelink/phoenixtools && make test'`
passed after integration, including resolver/media, virtual LXIO, real-USB
forwarding, the new network-isolation test and the integrator's disk-identity
test:

```text
source audit passed
network isolation: PASS
disk-identity model="PHOENIXTOOLS VIRTUAL DISK" serial="PTVD0001"
```

The source audit emits only its preferred-length warning for the 247-line
local curl implementation (hard limit 350).

`C:/msys64/usr/bin/bash.exe tests/run-all.sh` exercised the complete native
suite and returned 1:

```text
PASS hello
PASS binder
PASS cxx
PASS drivepath
PASS errors
PASS loopback
PASS algorithms
PASS process
PASS signals
PASS sysinfo
PASS termios
PASS suite-stdio
PASS suite-fs
PASS suite-thread
FAIL suite-locale (out/test/suite-locale.out)
```

The locale log reports remquo checks at math.c:574–576:
`result == (double/float/long double)-1 && quotient == 4`.
The log also records 13 host-x87 precision skips. The network/fortified-recv
checks pass, but **a passing full run-all result is not claimed**; this separate
math/host precision failure was handed to the runtime integrator.

## 7. Initial private-server cutover (registration/login/scores)

### Client transport

When `[server] api_base` is an accepted local HTTP origin, `patch.c` replaces
only these original five-byte network call sites with five NOPs:

| Original VA | Expected bytes | Removed call |
|---|---|---|
| `0x0808382a` | `e8 a1 de 09 00` | Outgoing trailer transform `0x081216d0` |
| `0x08088ba3` | `e8 78 94 09 00` | Incoming trailer transform `0x08122020` |

Both return values are unused and both callees use plain `ret`; removing the
calls does not change stack cleanup. The shared SenseLock functions, region
response and disk identity operations are not changed. Patch bytes are checked
before mutation; a mismatch aborts startup through the existing patch failure
path rather than continuing partially patched.

The ordinary packet layout from section 2 is unchanged except that the final
64-byte trailer is sent directly as `magic || key || SHA1(plain)`. The client
still checks frame bounds, magic and digest before dispatch; 16-byte packets
are unchanged. A decoded game-origin registration captured in
`out/net/bypass-before.db` independently confirmed `hashlib.sha1(plain)` matches
the game's 20-byte digest. That capture also showed the failed hardware call
already left the outgoing trailer clear on this machine; the cutover makes
both directions explicitly independent of hardware instead of relying on that
failure behavior.

**Security boundary:** this is a private protocol, not vendor authentication.
The RC4 key travels in the clear trailer, so neither RC4 nor SHA-1 authenticates
a server or provides confidentiality against an observer. The expanded LAN
policy accepts only explicitly configured loopback/RFC1918 HTTP origins and
keeps empty/public/malformed destinations blocked. Use it on a trusted LAN,
not the public Internet; anyone with access can submit local game traffic.
An empty/invalid `api_base`, including CDN-only configuration, does not enable
the transport patches.

### Build and run

```console
wsl -e bash -lc 'cd /mnt/d/tmp/piurelink/phoenixtools && make'
C:/msys64/usr/bin/bash.exe tools/mkgame.sh out/game-private
python -u server/piuserver.py --db server/piu.db --port 8080 --game-port 15040
```

In `out/game-private/phoenixtools.ini`:

```ini
[server]
api_base = http://127.0.0.1:8080
cdn_base =
```

Run `out/game-private/piu.exe` from its directory. Current card insertion uses
the P1/P2 keys and card files in section 9, not a startup environment variable.
`[server] game_port` (default 15040) sets the client's game TCP port and must
match the service's `--game-port`. No official endpoint or server-shutdown
claim is needed for this local compatibility change.

### Mapped local service behavior

| Request | Local response / persistence |
|---|---|
| `0x132001` system check | `0x132002`, zero status words, local machine value 1 and identifier `LOCAL00001` |
| `0x132003` registration | `0x132004`, 92-byte body, machine value 1, enabled state 0, description and local identifier; raw machine info persisted |
| `0x132005` card lookup | Validates player 0/1, registered machine and hex card; creates/reloads SQLite user and 112-byte local profile, replies `0x132006` then `0x132007` load-completion status 1 |
| `0x13200a` played result | Validates the mapped 108-byte body, resolves its transmitted profile ID, persists score and raw result fields, then replies `0x13200b` |
| `0x03000000` heartbeat | Echoes the original 16-byte packet |

This initial implementation represented one standalone local cabinet.
Profile nicknames default to `LOCAL` plus the last seven card hex
characters. Mapped profile display fields are initialized; unmapped bytes and
the two opaque profile-associated response words remain zero. Existing saved
112-byte local profiles reload by card. A buffered result is associated with
its transmitted profile ID, not the card most recently scanned. Guest/unknown
profile results are recorded as raw TCP traffic but do not create an invented
user or score acknowledgement.

At this stage, settings-save messages, history pages, rankings and general
unlocks remained unmapped. Section 8 records the subsequent exhaustive command
mapping and expanded local implementation. Received/sent wire chunks and
HTTP requests remain recorded in SQLite. Malformed packet magic, digest,
length or known-command payloads close the session without dispatching them.
SQLite connections close deterministically after each transaction, including
on errors.

### Cutover checks

`wsl -e bash -lc 'cd /mnt/d/tmp/piurelink/phoenixtools && make test'`
passed: resolver/media, virtual LXIO, USB forwarding, network isolation and
disk identity. An earlier invocation overriding only `PLAIN_ROOT` failed the
resolver fixture because the INI retained its configured data root; using the
project's matching default fixture paths passed.

`python -m unittest discover -s server -p test_*.py -v` passed both socket
regressions: corrupted/oversized registration never creates a machine, and a
buffered result retains its original card owner after another card logs in.
The latter also exercises split registration, coalesced card/heartbeat packets
and profile reload.

A separate temporary smoke exercised decoding the actual captured game
registration, maximum-size packet boundaries, bad magic/digest/ciphertext,
fragmented/coalesced frames, SQLite profile reuse, immediate database removal
on Windows, and successful result acknowledgement/persistence. This synthetic
socket traffic used a temporary database, not the game evidence database.

The rebuilt `out/game-private` also ran for 25 seconds with the default empty
`api_base`: `out/net/private-default.log` records blocked HTTP, DNS and connect,
with no private-transport opt-in. Normal local-server operation must therefore
be enabled explicitly.

### Actual game registration, login and score persistence

The private service used `out/net/private.db`. This historical command predates
the runtime-key cutover; it records the old run, not the current launch recipe:

```console
python tools/run.py -t 310 -o out/net/private-game.log -C out/game-private -e LXGFX_NO_PRESENT=1 -e PHOENIXTOOLS_CARD_ID=01020304 -e LXGFX_SNAPSHOT=D:/tmp/piurelink/out/net/private-snap -e LXGFX_SNAPSHOT_EVERY=900 -i out/m7/s3.txt out/game-private/piu.exe
```

The runner ended with `timeout after 310s (killed)`, not a game crash. The
normal client sent registration `0x132003`, accepted `0x132004`, scanned card
`01020304`, and received `0x132006/0x132007`. The mode-selection screenshot
`out/net/private-snap/frame-1800.bmp` displays **LOCAL1020304 #0001** with its
profile avatar, rather than the guest name. Heartbeats remained active on the
same TCP connection throughout gameplay.

The actual game then sent two decoded `0x13200a` result packets; the server
persisted them and sent `0x13200b` acknowledgements. SQLite queries after exit
returned:

| Table | Observed game-origin data |
|---|---|
| `machines` | `LOCAL00001`, 916 bytes of machine info |
| `users` | ID 1, card `01020304`, nickname `LOCAL1020304` |
| `profiles` | User 1, version 1, 112-byte profile |
| `scores` | User 1, raw song field 6256 (`0x1870`), chart selector 3 / mode bits 0, score **50,772**, grade byte `0x1f` |
| `scores` | Same user/song/chart, next-stage score **0**, grade byte `0xff` |

The second result followed the game's stage-break path. Both raw payloads are
retained in `scores.judgements` JSON and in `tcp_packets`. Unknown commands
`0x13200c`, `0x13200d`, `0x132016` and `0x13200e` were recorded but unanswered;
they did not prevent this login/gameplay/result flow.

This is a real private-server account and score flow, unlike the original
recorder-only run in section 6. It does not establish unknown settings saves,
history download or rankings. Synthetic socket smoke also exercised the
alternate `0x132001/0x132002` system check and rejected an out-of-range player
without creating a user.

## 8. Expanded LAN service and player customization

The LAN expansion replaces the initial single-cabinet service. It retains the
standard-library-only Python server and SQLite database; there is no account
framework, external service, browser build step or JavaScript dependency.

### LAN setup

```console
wsl -e bash -lc 'cd /mnt/d/tmp/piurelink/phoenixtools && make'
C:/msys64/usr/bin/bash.exe tools/mkgame.sh out/game-lan
python -u server/piuserver.py --db server/piu.db --bind 0.0.0.0 --port 8080 --game-port 15040
```

On each cabinet, set the server computer's actual private IPv4 address in that
cabinet's `phoenixtools.ini`:

```ini
[server]
api_base = http://192.168.5.11:8080
cdn_base = http://192.168.5.11:8080
```

The address shown is the verification machine's LAN address, not a required
constant. Permit inbound TCP 8080 and 15040 on the server's private-network
firewall if another computer connects. API/CDN origins accept strict numeric
RFC1918 IPv4 (`10/8`, `172.16/12`, `192.168/16`), `127.0.0.1`, or `localhost`.
Public addresses, arbitrary hostnames, HTTPS and malformed IPv4 are rejected.
DNS for the original game hosts resolves only to the configured API address;
the original game TCP port 50400 routes to that address on `[server] game_port`
(default 15040). Empty configuration still blocks networking.

The service binds all interfaces by default for this explicitly LAN-only use.
`--bind 127.0.0.1` restricts it to the original same-machine setup. The protocol
is not encrypted/authenticated for Internet deployment; the web editor
intentionally lets trusted LAN participants edit registered players.

### Player settings page

Open `http://192.168.5.11:8080/players` (using the chosen server address).
Scan a card in the game once to register it, select that player, change fields
and save. Finish the current game before editing, then scan the card again to
apply changes. Stale forms are rejected rather than overwriting a newer
profile revision.

The editor exposes the native profile's supported nickname, avatar, title,
scroll speed, auto velocity, noteskin, timing and table-backed modifier groups.
It uses the build's 213 title entries, 45 noteskins and installed avatar assets
000–163. Legacy imported values can be kept unchanged even if they have no
current selectable asset. Opaque profile bytes and player identity survive
edits. The page also shows persisted result/best-record counts and earned
local-event titles.

Native profile layout, corrected by direct IDA analysis:

| R offset | Field |
|---|---|
| `0x24` | Nickname, at most 12 printable ASCII characters |
| `0x34` | Numeric discriminator |
| `0x3c` | Local profile/user ID |
| `0x40` | Title byte; not avatar |
| `0x41` | Opaque matching metadata byte; preserved |
| `0x42` | Avatar byte; not title |
| `0x4c/0x50` | 64-bit modifier bitfield |
| `0x54` | Scroll-speed mask, u16 |
| `0x56` | Noteskin, u16 |
| `0x58` | Auto velocity, u16, native range 300–999 |
| `0x5c` | Timing-adjustment float |

`0x080b495e` applies these options; `0x080bb70b` writes them into R. Timing
commands use native values `-15,-12,-9,-6,-3,0,2,4,6,8,10`, not symmetric
milliseconds. Very-hard judgement is cleared by the client at login and is not
offered as an ineffective persistent setting. Rush, sort order, stage-pass and
premium/full-mode selection are session state rather than invented profile
fields. Native result submissions retain the option snapshot used for play;
only current, non-buffered snapshots update preferences. Historical buffered
scores still persist and receive acknowledgements without undoing newer web
settings.

### Complete mapped command surface

The original send builders contain 29 located outbound command references,
including heartbeat. The native response switch has 28 non-default cases;
all have corresponding local service behavior. One-way telemetry is stored
without inventing an acknowledgement that the client never consumes.

| Request | Local behavior / native response |
|---|---|
| `0x132001` | Alternate SYSTEMCHECK; two opaque caller words precede the machine block at +8; reply `002` |
| `0x132003` | Persistent cabinet registration; reply `004`, matching capability `02d`, configured messages/events and previously earned chart entitlements |
| `0x132005` | Card/profile `006`, paged native best records `009`, load completion `007` |
| `0x13200a` | Full 108-byte played history, account/guest ownership, per-chart bests/play counts, preference snapshot and local-event progress; acknowledgement `00b` |
| `0x13200c` | Coin/pricing/input-type telemetry, one-way |
| `0x13200d` | Machine/player/user/pricing/session transition, one-way |
| `0x13200e` | Exactly R[0:36] saved as a profile prefix; all later bytes preserved; one-way |
| `0x13200f` | Update lifecycle telemetry, one-way |
| `0x132011` | Exact 1024-byte operator update-package key asset in `012`, if present |
| `0x132015` | Formatted player/machine diagnostic text, one-way |
| `0x132016` | Player at +0, profile ID at +4, version[12]; end-session earned-title notification `017` and completion `018` |
| `0x132019` | Actual compatible-cabinet search; acknowledgement `01a`, real peer metadata `01b` |
| `0x13201c` | Peer ready barrier |
| `0x13201d` | Load acknowledgement `01e`, shared-start `01f` only when both real peers are ready |
| `0x132020` | Native gameplay-start transition, one-way |
| `0x132021` | Two u16 live values relayed to the opponent as `021`; these are not fabricated participant counts |
| `0x132022` | 36-byte peer-result snapshot; acknowledgement `023` |
| `0x132024` | Actual opponent result returned as `025`, once that opponent submits it |
| `0x132026` | Cancellation acknowledgement `026` and peer cleanup |
| `0x132027` | Leave acknowledgement `027` |
| `0x132028` | Cancel/rematch acknowledgement `029` |
| `0x13202b` | Screenshot request token acknowledgement, one-way |
| `0x13202c` | Match exit; capability `02d` remains enabled |
| `0x13202e` | Screenshot chunk validation/storage/reassembly; actual peer relay as `02e`, preceded by handshake `02a` |
| `0x13202f` | Completed local-event chart entitlements `030`, then completion-cache update `035` |
| `0x132033` | Actual local-event progress `034` |
| `0x132036` | Earned local-event title `037`; reserved word at +0, recipient player at +4, status at +8 and title at +12 |
| `0x13203b` | Download/update outcome telemetry, one-way |
| `0x03000000` | Original sequence and four-byte value echoed |

The remaining pushed responses are advertisement metadata `014`, event
definitions `032` and progress-cache updates `035`. Completed event rewards
are acquired before the native cache is marked complete, and earned chart
entitlements reload during registration. Matching pairs distinct cabinets on
compatible version/song/mode/chart/search settings; no bot opponent or made-up
score is supplied. A completed stage's old opponent is not silently reused for
a new search.

Cabinet identity comes from the assigned machine ID and the registration
machine block's lock-number/peer context. For `003`, machine ID is +0 and lock
u64 is +4; for `001`, those offsets are +8 and +12. Replies use persistent
identifiers `LC00000001`, etc. Cloned private hardware numbers can coexist on
different LAN peers. Identical lock number, identical peer IP and zero assigned
ID cannot distinguish two processes from the information in these requests.

`best_records` stores the exact native 24-byte per-chart record; `scores` and
`result_packets` retain played history. The native best rule accepts grades
0–15 for score/category improvement, but counts all plays; failed/flagged
results still remain in full history. Earlier `scores.judgements["raw"]`
captures migrate idempotently into native bests without changing original score
rows or IDs. `decoded_packets`, `cabinet_activity`, `player_sessions`,
`guest_results`, `screenshots`/`screenshot_chunks`, event progress and title
rewards preserve their respective flows. Matching's live socket state is
intentionally not persisted as a bogus resumable connection.

### Content, diagnostics and exact prerequisites

`--assets server/assets` serves real local files under `/assets/`,
`/2023/ad/` and `/2023/update/`; directory traversal and missing files return
404. Both country lookup and `/ip/` report the HTTP peer's actual address.
`/dummy/test500mb.db` streams 500 MiB in bounded 1 MiB chunks. This is explicit
LAN diagnostic policy, not a claim about an unavailable official download.

`--content path/to/content.json` loads optional LAN event/message definitions:
top-level `events` and `messages` arrays. Event fields are `id`, `sprite`,
`target_plays`, optional `title_id` and `word_04/word_08/word_0c`, plus separate
`charts` eligibility and `rewards` entitlement lists of `[song,mode,selector]`.
At most ten native event channels and 32 charts/rewards per event are
representable. Eligibility mode 255 is the native wildcard; reward modes must
be concrete 0/64/128/192. Messages contain `revision`, `enabled` and `path`,
with at most two native slots.

The event `sprite` is an existing client sprite basename (at most 31 ASCII
characters), **not arbitrary display text**. Event word +4 is kind (2 selects
the song-specific overlay), +8 is that kind's song ID, and +12 is its modifier
permission mask. Configured targets/rewards are explicitly local operator
policy. The vendor's schedules and award rules are not encoded in the reply
consumers, so no fake official defaults are supplied.

The original content tree lacks some common event presentation groups already
requested by the native client (`user_interface/select/main_common_event`,
`event_channel_icon`, `event_channel_desc`). Complete event presentation needs
those real client assets and a suitable event sprite. Server-side progression,
reward acquisition and title notifications are implemented independently.

The optional historical `012` route needs the actual 1024-byte
`key_phoenix_2120.bin` under `--assets`. It is not needed for LAN gameplay.
Further analysis identifies this file as a repeating XOR key for downloaded
update packages (`0x08092355`), not the separate dongle trailer transform.
Without an operator-provided asset, the service records the request and does
not claim successful key delivery. Existing updater patches still disable
automatic game updates; this server does not manufacture an update package,
key or package signature.

No dedicated leaderboard or mileage request exists among the located send
builders or the native response switch. Native per-chart bests and play counts
are implemented; opaque prefix bytes are preserved rather than relabeled as
invented mileage. This covers the build's mapped client protocol, not unknown
business rules or absent operator content from the old official service.

### Expanded-service verification

The client build and `wsl -e bash -lc 'cd /mnt/d/tmp/piurelink/phoenixtools && make test'`
passed, including strict private-origin boundaries, API/CDN routing, legacy
HTTP destinations and default isolation. The existing native locale-suite
limitation in section 6 was not rerun or claimed fixed.

`python -m unittest discover -s server -p test_*.py -v` passed **28 tests**.
They cover profile byte preservation/error boundaries/stale forms, native
best-record page boundaries and old-capture migration, preferences, real
event reward ordering, guest ownership, screenshots, malformed frames,
multi-cabinet matching barriers/results/cancellation/rematch, and a camera
photo uploaded **before** its opponent joins. That last regression initially
timed out: the first implementation discarded unpaired photos. The corrected
service buffers the latest bounded upload through matchmaking, then relays it
with the native 8192-byte padding and clears it at gameplay/session boundaries.

Review regressions also proved and corrected three boundary errors: event
title `037` targets the **second** word's player (P2 was previously misrouted),
ended card sessions do not receive a later player's event reward, and buffered
results cannot revert newer web preferences. A separate real HTTP/TCP smoke
confirmed P2 delivery, excluded the departed account, retained the buffered
score, and kept web auto velocity 700 instead of the old snapshot's 350.

Actual private-IP verification used the server bound to `0.0.0.0`, with both
HTTP and game traffic addressed to **192.168.5.11**, not loopback:

* `out/net/lan-first.log` records country HTTP 200, isolated DNS to the private
  address, TCP to `192.168.5.11:15040`, and normal card/profile/best-page
  replies. `out/net/lan-server.log` records peer `192.168.5.11`, machine
  `LC00000001`, card `01020304`, and `006/009/007` payload sizes 384/6160/8.
* The actual browser opened `/players`, selected registered user 1, saved
  **LANPLAYER**, title **43 / THE MASTER**, avatar **7**, speed **12 / x3**,
  noteskin **4 / Slime**, auto velocity **400**, and timing value **2**.
  Reload retained every value; browser error collection was empty.
* With exactly that unchanged profile, `out/net/lan-media-clock.log` reached
  native mode selection, song selection and gameplay. Its
  `frame-1800.bmp` displays **LANPLAYER #0001**, **THE MASTER** and avatar 007;
  `frame-3300.bmp` shows the Slime noteskin during actual gameplay.
* Earlier edited-profile runs stalled in the native video-ring catch-up path
  while network heartbeats continued. State capture proved the profile bytes
  correct and the decoder active, not a failed login or a simple EOF deadlock.
  The unchanged warm run rendered normally; no warning suppression, video skip
  or fabricated EOF workaround was added. This intermittent native playback
  behavior is separate from server/profile persistence.
* A separate two-client **socket smoke**, not two physical game cabinets,
  uploaded a real 320×240 JPEG before search, paired later, received and
  decoded every padded chunk, and verified the exact **37,189-byte** assembled
  image in SQLite. It used a temporary database, not player evidence data.
* An actual HTTP request with `?probe=lan-smoke` received all **524,288,000**
  advertised diagnostic bytes, in 1.67 seconds using 1 MiB reads. That query
  distinguishes synthetic diagnostic traffic from game-origin HTTP rows.
* Original score IDs 1 and 2, including the 50,772-point result, survived the
  expanded-service migration unchanged. Native best records were reconstructed
  according to the client's grade eligibility and play-count rules.

These checks exercise real client/browser/private-IP surfaces. They do not
claim verification from a second physical cabinet or successful presentation
of missing event assets.

The subsequent 310-second native run, `out/net/lan-custom-final.log`, reached
gameplay, GRADE, another stage and PLAY_RESULT with the customized profile.
It emitted real scores **54,184** and **0**, persisted as score IDs 3 and 4,
and received `00b` acknowledgements. Browser reload then showed four uploaded
results and retained nickname/title/avatar/speed/skin/AV/timing unchanged.

That run also exposed a session-end parser error not detectable by a
self-consistent synthetic fixture: the captured `016` body begins
`player=0, profile=1`, not the reverse. The native connection was rejected at
session end. Corrected the parser and every wire fixture to player/profile
order, with a failing-before/passing-after captured-wire regression. A separate
private-IP socket replay used the **exact** captured registration, card and
20-byte session-end payloads against the final code: it received normal
`017/018` completion, persisted the ended session and exchanged a subsequent
heartbeat on the same connection. The earlier rejected run is not presented
as a successful final session-end exchange.

## 9. Runtime P1/P2 card insertion

Card-reader emulation is independently configured from the USB LXIO panel:

```ini
[input]
virtual_card_reader = true
card1 = F3
card2 = F4
```

The INI flag and key bindings are loaded at startup; restart the game after
changing those settings. Only the two card files are reread on each insertion.

Place `card1.txt` and `card2.txt` in the **directory containing the loaded
`phoenixtools.ini`**, not the current working directory or `runtime_dir`.
Each file contains that player's card UID as 2–34 hexadecimal characters
(1–17 bytes), with optional surrounding whitespace/CRLF. Upper/lowercase and
leading zeroes are preserved as the same raw UID; there is no decimal-number
conversion or byte reversal.
The reader accepts regular text files only and bounds each file to 256 bytes,
including surrounding whitespace.

Press **F3** to insert P1's card and **F4** for P2. Each new press reads that
file again, so editing a UID while the game is running affects the next
insertion. Holding a key does not repeatedly scan. Each player has an
independent pending event; a later explicit scan replaces that player's pending
UID. Missing, empty, malformed or overlong input logs a recoverable error and
does not submit an old/partial card. Fix the file, release and press again.
Normal native screen, player-state and network gates still control login.

There is no automatic startup scan. The previous `PHOENIXTOOLS_CARD_ID`
environment injector has been removed; references in earlier evidence sections
describe the old implementation. The new keys are configurable X11 keysym names
and must be distinct from other active bindings.

With `virtual_card_reader = false` (the default), no card-consumer detour is
installed, no card files are read and these two bindings are not intercepted.
The original reader-consumption path is retained: physical RFID readers are
connected through the COM-port mapping below. This setting is independent of
`virtual_lxio`, so keyboard cards do not require synthetic USB cabinet input.
Enabling emulation replaces card consumption for both players; it does not
disable the game's original serial device enumeration.

### Verification and publication

The integrated `make test` run passed, including runtime card freshness,
two-player input isolation, disabled-mode passthrough, native trampoline
continuation, malformed-file recovery and concurrent access. The six Python
publication-preservation tests also passed.

The updated toolsets were published to `out/dist/piu`, including the LAN
server, `start-server.cmd`, runtime DLLs and the two card files. Publication
preserved all ten existing state-file hashes, every existing INI option value
and the data junction's target. Missing card files are created empty; existing
card contents and server databases are not replaced.

A 70-second smoke run launched the **published** game and server with a
separate INI/card directory. No insertion keys were sent for the first
20 seconds. P1's file was changed while F3 was held; eight repeated key-downs
did not rescan. A fresh F3 press loaded the new UID, and F4 loaded P2's file.
The normal native request builder sent exactly two card requests:
P1 `11223344` and P2 `05060708`. Both profiles persisted, and the captured
title screen displayed `LOCAL1223344 #0001` and `LOCAL5060708 #0002`,
each with **Login completed**. The game was still running when the scenario
terminated it. Evidence: `out/card-smoke/game.log`, `server.log`, `smoke.db`
and `reader-proof.png`. No physical COM device was exercised.


### Physical serial hardware (COM ports)

The game has two distinct serial paths:

* RFID probes `/dev/ttyACM0..9` at 115200 baud and parses the binary card frames
  described in section 4. Each of the two reader objects binds to the first
  probed path that opens and is not already taken; a read error closes the
  reader and the poller re-probes.
* A separate board opens `/dev/ttyUSB0` at 9600 baud once at startup
  (`0x0807443b–0x0807444a`), queues ASCII `P1O%d%d----` and `P1G%d-----` commands
  (`0x0805fe6c`, `0x08060148`), writes them one byte per `write` (`0x0810d6be`)
  and reads up to 16 reply bytes (`0x0810d741`). This is the cabinet light board.

Both open with `open(path, O_RDWR|O_NOCTTY|O_NONBLOCK)`, clear `O_NONBLOCK`, save
the termios, apply raw `IGNPAR` / `CS8|CLOCAL|CREAD` with `VMIN = VTIME = 0`,
`tcflush(TCIFLUSH)` and `tcsetattr(TCSANOW)` (`0x0810c966`). Their pollers expect
`read` to return 0 immediately when nothing is queued; `-1` closes an RFID reader.

lxrt maps these guest paths to Windows COM ports in `phoenixtools.ini`:

```ini
[serial]
/dev/ttyUSB0 = COM3      ; light board
/dev/ttyACM0 = COM4      ; P1 RFID reader (with [input] virtual_card_reader = false)
/dev/ttyACM1 = COM5      ; P2 RFID reader
```

Use the port numbers Device Manager shows. Unmapped paths still fail with
`ENOENT`, so a missing board or reader behaves as before. Backend semantics are
in `docs/lxrt.md` (`[serial]`).

Verification: `tests/serial/run.sh COM1 COM2` on a linked virtual COM pair runs
the game's exact opener sequence on both ports and checks light-board-style
one-byte writes, an RFID-format frame in the other direction, zero-byte
`VMIN = VTIME = 0` reads, `O_NONBLOCK` `EAGAIN`, `VTIME` timeout, blocking `VMIN`,
`FIONREAD`, `tcflush`, 115200 baud, `dup`, invalid-speed rejection, exclusive
open (`EBUSY`) and missing-port `ENOENT`; it passed. No physical light board or
RFID reader was available, so the electrical/protocol behaviour of real
hardware is not verified.
