# Native-port investigations

Static investigation of the supplied i386 ELF images. Addresses below are original ELF virtual addresses; DLL addresses are before load bias. A patch is considered installed only after phoenixtools validates its original bytes and completes initialization. “Not patched” is not a claim that a path is exercised at startup.

## 6.1 System, process, device, and loader call sites

### Evidence and completeness

Commands executed from WSL with `/mnt/d/tmp/piurelink` as working directory:

```sh
i686-linux-gnu-objdump -d piu
i686-linux-gnu-readelf --dyn-syms --wide piu
i686-linux-gnu-readelf -r --wide piu
i686-linux-gnu-objdump -d lib/libstdc++.so.6
```

The host ARM64 `objdump -d piu` cannot decode i386 (`can't disassemble for architecture UNKNOWN`); all address evidence uses `i686-linux-gnu-objdump`. The full disassembly was parsed for every direct `call`/tail `jmp` to each named PLT entry. All 20 imported target GOT slots occur only in their respective PLT dispatch; none has another direct absolute GOT consumer. There are **88 call sites**, **zero tail jumps** for the requested names. `syscall` is **not imported or called by piu**, and no `int $0x80`/`syscall` instruction or non-canary `%gs:` access exists in its disassembly. The syscall dependency actually belongs to libstdc++ (below).

Arguments were decoded from preceding `mov ...,(%esp)` stores or pushes. Referenced strings were read using the PT_LOAD virtual-address/file-offset mapping, not by guessing printable text. Relevant `.rodata` ranges can be reproduced with:

```sh
i686-linux-gnu-objdump -s --start-address=0x081c1ea3 --stop-address=0x081c1eb0 piu
i686-linux-gnu-objdump -s --start-address=0x081cf6d2 --stop-address=0x081cf800 piu
i686-linux-gnu-objdump -s --start-address=0x08216720 --stop-address=0x08216770 piu
i686-linux-gnu-objdump -s --start-address=0x08217848 --stop-address=0x08217851 piu
```

Hook evidence: `phoenixtools/src/phoenix/addresses.h:6–27`; `patch.c:108–132`; `safety/system_hooks.c:9–64`; `safety/filesystem.c:43–46,84–144`; `safety/network.c:11–85`. The command hook (`media/media_hooks.c:88–100`) resumes the original function at `0x08060f9b`; it does **not** suppress arbitrary system commands. Curl hooks only block curl HTTP, not `socket`/`connect`.

### Complete non-ioctl call table

| Function | Call site | Decoded arguments / evidence | Existing neutralization |
|---|---|---|---|
| `fork` | `0x0805d73f` | fork(); no arguments. | Both enclosing mixer helpers are bypassed: entry jumps at 0x0805d6b2 / 0x08065826 to hook_mixer_helper (xor eax,eax; ret). |
| `fork` | `0x080658b3` | fork(); no arguments. | Both enclosing mixer helpers are bypassed: entry jumps at 0x0805d6b2 / 0x08065826 to hook_mixer_helper (xor eax,eax; ret). |
| `execl` | `0x08095ef9` | execl("/bin/busybox", "/bin/busybox", "reboot", NULL); path 0x081cf7b8, command 0x081cf7b1. | NOT neutralized by updater entry 0x08095ff0 or update-state jump 0x08066033→0x0806600a; these sites are outside both replaced blocks. |
| `execl` | `0x0809efec` | execl("/bin/busybox", "/bin/busybox", "reboot", NULL); path 0x081cf7b8, command 0x081cf7b1. | NOT neutralized by updater entry 0x08095ff0 or update-state jump 0x08066033→0x0806600a; these sites are outside both replaced blocks. |
| `execl` | `0x080a3a2d` | execl("/bin/busybox", "/bin/busybox", "reboot", NULL); path 0x081cf7b8, command 0x081cf7b1. | NOT neutralized by updater entry 0x08095ff0 or update-state jump 0x08066033→0x0806600a; these sites are outside both replaced blocks. |
| `execv` | `0x0805d799` | execv("/usr/bin/amixer", {"/usr/bin/amixer", "set", "Master", percent, "unmute", NULL}); percent=sprintf("%d%%", adjusted config volume). Strings: 0x0821672c, 0x08216728, 0x081c1ea8, 0x081c1ea3, 0x08216720. | Same whole-function mixer-entry patch; not executed after successful patching. |
| `execv` | `0x0806590d` | execv("/usr/bin/amixer", {"/usr/bin/amixer", "set", "Master", percent, "unmute", NULL}); percent=sprintf("%d%%", adjusted config volume). Strings: 0x0821672c, 0x08216728, 0x081c1ea8, 0x081c1ea3, 0x08216720. | Same whole-function mixer-entry patch; not executed after successful patching. |
| `waitpid` | `0x0805d763` | waitpid(fork_result, &status, 0); status pre-zeroed. | Same whole-function mixer-entry patch; not executed after successful patching. |
| `waitpid` | `0x080658d7` | waitpid(fork_result, &status, 0); status pre-zeroed. | Same whole-function mixer-entry patch; not executed after successful patching. |
| `mount` | `0x08092f1f` | mount("/dev/sd?1", "/mnt/0", "vfat", 0x498, NULL); runtime letter replaces ? at 0x08092eb0. /mnt/n at 0x08217848 has its last char replaced by 0 at 0x08092ef1. Source 0x08218664; type 0x081cf6d2. | No direct patch/hook. |
| `mount` | `0x080932b1` | mount("/dev/sda1", "/mnt/hd", NULL, 0x420, NULL); source sprintf("%s1", "/dev/sda") at 0x0809325f–0x08093289, target 0x081cf6e5. | No direct patch/hook; raw-system-disk open hook does not interpose mount. |
| `mount` | `0x080936e0` | mount("/dev/sda1", "/mnt/hd", NULL, 0x4b1, NULL); source sprintf("%s1", "/dev/sda") at 0x0809325f–0x08093289, target 0x081cf6e5. | No direct patch/hook; raw-system-disk open hook does not interpose mount. |
| `mount` | `0x08094090` | mount("/dev/sda1", "/mnt/hd", NULL, 0x4b1, NULL); source sprintf("%s1", "/dev/sda") at 0x0809325f–0x08093289, target 0x081cf6e5. | No direct patch/hook; raw-system-disk open hook does not interpose mount. |
| `umount` | `0x08066583` | umount("/mnt/0"); pointer 0x08217848, last character set to ASCII 0 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x080667b6` | umount("/mnt/1"); pointer 0x08217848, last character set to ASCII 1 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x08066b3f` | umount("/mnt/1"); pointer 0x08217848, last character set to ASCII 1 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x08066b99` | umount("/mnt/0"); pointer 0x08217848, last character set to ASCII 0 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x08074c71` | umount("/mnt/1"); pointer 0x08217848, last character set to ASCII 1 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x08074c9e` | umount("/mnt/0"); pointer 0x08217848, last character set to ASCII 0 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x08092bc1` | umount("/mnt/1"); pointer 0x08217848, last character set to ASCII 1 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x0809377e` | umount("/mnt/0"); pointer 0x08217848, last character set to ASCII 0 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x080939cd` | umount("/mnt/0"); pointer 0x08217848, last character set to ASCII 0 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x080af39e` | umount("/mnt/1"); pointer 0x08217848, last character set to ASCII 1 in preceding strlen/store sequence. | No direct patch/hook. |
| `umount` | `0x080af3f8` | umount("/mnt/0"); pointer 0x08217848, last character set to ASCII 0 in preceding strlen/store sequence. | No direct patch/hook. |
| `sigaction` | `0x0807431f` | sigaction(4, &action, &old); handler=0x080e6b19, flags=4 (SA_SIGINFO), handler set at 0x080742ec and flags at 0x080742f7. | No hook. Signals SIGILL/SIGFPE/SIGSEGV/SIGBUS install a diagnostic handler that prints siginfo and _exit(0) at 0x080e6b61/0x080e6b6d. |
| `sigaction` | `0x08074341` | sigaction(8, &action, &old); handler=0x080e6b19, flags=4 (SA_SIGINFO), handler set at 0x080742ec and flags at 0x080742f7. | No hook. Signals SIGILL/SIGFPE/SIGSEGV/SIGBUS install a diagnostic handler that prints siginfo and _exit(0) at 0x080e6b61/0x080e6b6d. |
| `sigaction` | `0x08074363` | sigaction(11, &action, &old); handler=0x080e6b19, flags=4 (SA_SIGINFO), handler set at 0x080742ec and flags at 0x080742f7. | No hook. Signals SIGILL/SIGFPE/SIGSEGV/SIGBUS install a diagnostic handler that prints siginfo and _exit(0) at 0x080e6b61/0x080e6b6d. |
| `sigaction` | `0x08074385` | sigaction(7, &action, &old); handler=0x080e6b19, flags=4 (SA_SIGINFO), handler set at 0x080742ec and flags at 0x080742f7. | No hook. Signals SIGILL/SIGFPE/SIGSEGV/SIGBUS install a diagnostic handler that prints siginfo and _exit(0) at 0x080e6b61/0x080e6b6d. |
| `tcgetattr` | `0x0810c9d6` | tcgetattr(serial_fd, saved_termios=0x0a1bee60); fd from runtime path open(...,0x902). | No direct patch/hook. |
| `tcsetattr` | `0x081057a3` | tcsetattr(serial_fd, 0 /* TCSANOW */, saved_termios=0x0a1bee60). | No direct patch/hook. |
| `tcsetattr` | `0x08109ebb` | tcsetattr(serial_fd, 0 /* TCSANOW */, saved_termios=0x0a1bee60). | No direct patch/hook. |
| `tcsetattr` | `0x0810a05a` | tcsetattr(serial_fd, 0 /* TCSANOW */, saved_termios=0x0a1bee60). | No direct patch/hook. |
| `tcsetattr` | `0x0810a14e` | tcsetattr(serial_fd, 0 /* TCSANOW */, saved_termios=0x0a1bee60). | No direct patch/hook. |
| `tcsetattr` | `0x0810a218` | tcsetattr(serial_fd, 0 /* TCSANOW */, saved_termios=0x0a1bee60). | No direct patch/hook. |
| `tcsetattr` | `0x0810b1d0` | tcsetattr(serial_fd, 0 /* TCSANOW */, saved_termios=0x0a1bee60). | No direct patch/hook. |
| `tcsetattr` | `0x0810c940` | tcsetattr(serial_fd, 0 /* TCSANOW */, saved_termios=0x0a1bee60). | No direct patch/hook. |
| `tcsetattr` | `0x0810ca4a` | tcsetattr(serial_fd, 0 /* TCSANOW */, &new_termios); new iflag=4, oflag=0, lflag=0; cflag=0x8bd for 9600 baud, else 0x18b2; input baud selector compare at 0x0810c9eb. | No direct patch/hook. |
| `tcflush` | `0x0810ca2f` | tcflush(serial_fd, 0 /* TCIFLUSH */) before applying new termios. | No direct patch/hook. |
| `mmap` | `0x080eb58a` | mmap(runtime_hint, runtime_length, 3 /* READ\|WRITE */, 1 /* SHARED */, video_fd at object+0x18, runtime_offset); V4L2 buffers after VIDIOC_QUERYBUF. | No direct patch/hook; optional capture device, not FFmpeg playback. |
| `mmap` | `0x081a48d1` | mmap(NULL, file_size, 1 /* READ */, 2 /* PRIVATE */, font_fd, 0); pushes at 0x081a48c7–0x081a48cf. | No direct patch/hook; live statically linked FreeType file loader in piu. |
| `dlopen` | `0x0806823d` | dlopen(NULL, 0x100 /* RTLD_GLOBAL, no RTLD_NOW/LAZY */); no named shared library. | Not patched. Accept this glibc-compatible NULL/self-scope special case rather than rejecting missing LAZY/NOW. |
| `dlsym` | `0x0806824d` | dlsym(self_handle, "glXSwapIntervalSGI" at 0x081c6401). | DIRECTLY replaced at this address by PHOENIX_SWAP_INTERVAL_CALL_VA: xor eax,eax; nop;nop;nop. Despite its name, the patch removes dlsym, so subsequent null check at 0x08068252 skips indirect glXSwapIntervalSGI(1) at 0x0806825d. |
| `getifaddrs` | `0x080edbcf` | getifaddrs(&list); consumer checks AF_INET (2), name "eth0" at 0x081d614b, ifa_addr and ifa_netmask offsets 0x0c/0x10. | No direct patch/hook; curl hooks do not cover interface enumeration. Caller dereferences ifa_addr without null check. |
| `socket` | `0x08087dbb` | socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0); socket saved at object+0x74. | No direct patch/hook; curl blocking is not general Winsock blocking. |
| `connect` | `0x08087e99` | connect(fd, &sockaddr_in, 16); family=2, IPv4=inet_addr(runtime server string at 0x0979e760), port=low 16 bits of global 0x0979e7a0 (load 0x08087c87), network-order rotation at 0x08087e25. | No direct patch/hook. Nonblocking EINPROGRESS expected (errno=115 checked at 0x08087eaf). |
| `sysinfo` | `0x080ee0b5` | sysinfo(&info); totalram @+0x10 read at 0x080ee0c4, mem_unit @+0x34 read at 0x080ee0ba then used as divisor (mem_unit<<10). | No direct patch/hook. Returning success without initializing fields can divide by zero. |
| `select` | `0x08087f3b` | select(socket_fd+1, NULL, &writefds, NULL, &{1,0}); nonblocking connect completion, then getsockopt(SOL_SOCKET=1, SO_ERROR=4). | No direct patch/hook. |
| `select` | `0x08088868` | select(socket_fd+1, &readfds, NULL, NULL, &{1,0}); 1024-bit Linux fd_set. | No direct patch/hook. |
| `select` | `0x080e6292` | select(video_fd+1, &readfds, NULL, NULL, &{0,0}); zero-time poll of video capture fd. | No direct patch/hook. |
| `fcntl` | `0x08087e54` | socket F_GETFL (cmd=3), third slot 0. | No direct patch/hook. |
| `fcntl` | `0x08087e76` | socket F_SETFL (cmd=4), value=previous_flags\|0x800 (O_NONBLOCK). | No direct patch/hook. |
| `fcntl` | `0x08087fab` | socket F_GETFL (cmd=3), third slot 0. | No direct patch/hook. |
| `fcntl` | `0x08087fcd` | socket F_SETFL (cmd=4), value=previous_flags&~0x800. | No direct patch/hook. |
| `fcntl` | `0x0810c9c3` | serial F_SETFL (cmd=4), value=0. | No direct patch/hook. |
| `fcntl` | `0x081a488e` | font fd F_SETFD (cmd=2), value=1 (FD_CLOEXEC). | No direct patch/hook. |

The mutable `/mnt/n` string has length six: stores replace `n`, producing `/mnt/0` or `/mnt/1` (not `/mnt/n0`). `mount` flags: `0x498=MS_NOATIME|MS_DIRSYNC|MS_SYNCHRONOUS|MS_NOEXEC`; `0x420=MS_NOATIME|MS_REMOUNT`; `0x4b1=MS_NOATIME|MS_DIRSYNC|MS_REMOUNT|MS_SYNCHRONOUS|MS_RDONLY`. The latter three calls remount `/mnt/hd`; they are **not bind mounts**. The connect port global is initialized at `0x080742cd` from the 11-entry `.rodata` table `0x081c9560`, indexed by config byte `0x096999a4`: `50400, 50400, 50400, 50400, 50400, 50400, 50400, 50400, 50400, 50400, 50400` (host-order decimal). The alternate retry assignment at `0x08065cad` selects from two entries at `0x081c5780`: `50400, 50400`, indexed by a counter low bit. The server IPv4 string remains runtime configuration at `0x0979e760`; no single constant remote address is asserted. Both mixer functions construct the same `amixer set Master N% unmute` command, using config bytes `0x0962b93c` / `0x0962b93d`; neither invokes a shell.

### Complete ioctl call table

All requests are immediate stores to `0x4(%esp)` in the 20 instructions preceding each call. Request-name evidence: `sysroot/usr/include/linux/videodev2.h:2402–2445`, `usbdevice_fs.h:187`, `hiddev.h:157–176`. Argument pointers are Linux i386 layouts, never Windows device-control layouts.

| Call site | Request | Meaning | Existing neutralization |
|---|---|---|---|
| `0x08074e75` | `0xc0105500` | USBDEVFS_CONTROL (16-byte usbdevfs_ctrltransfer) | No direct patch/hook. |
| `0x08096589` | `0x0000030d` | HDIO_GET_IDENTITY (512-byte hd_driveid) | Inside updater 0x08095ff0: entry-patched to return 0; also /dev/sda open is blocked. |
| `0x080968b8` | `0x0000030d` | HDIO_GET_IDENTITY (512-byte hd_driveid) | Disk-ID routine entry 0x08096883 replaced by hook_disk_id_entry; also /dev/sda open blocked. |
| `0x0809f0c2` | `0x40045613` | VIDIOC_STREAMOFF (int type) | No direct patch/hook. |
| `0x080aec48` | `0xc008561c` | VIDIOC_S_CTRL (v4l2_control) | No direct patch/hook. |
| `0x080c050d` | `0xc008561c` | VIDIOC_S_CTRL (v4l2_control) | No direct patch/hook. |
| `0x080c4f0e` | `0x40045613` | VIDIOC_STREAMOFF (int type) | No direct patch/hook. |
| `0x080e63af` | `0xc0445611` | VIDIOC_DQBUF (v4l2_buffer) | No direct patch/hook. |
| `0x080e63dd` | `0xc044560f` | VIDIOC_QBUF (v4l2_buffer) | No direct patch/hook. |
| `0x080eb2e1` | `0x80685600` | VIDIOC_QUERYCAP (v4l2_capability) | No direct patch/hook. |
| `0x080eb31f` | `0xc008561c` | VIDIOC_S_CTRL (v4l2_control) | No direct patch/hook. |
| `0x080eb37d` | `0xc02c563a` | VIDIOC_CROPCAP (v4l2_cropcap) | No direct patch/hook. |
| `0x080eb3d5` | `0x4014563c` | VIDIOC_S_CROP (v4l2_crop) | No direct patch/hook. |
| `0x080eb44c` | `0xc0cc5605` | VIDIOC_S_FMT (v4l2_format) | No direct patch/hook. |
| `0x080eb4be` | `0xc0145608` | VIDIOC_REQBUFS (v4l2_requestbuffers) | No direct patch/hook. |
| `0x080eb53b` | `0xc0445609` | VIDIOC_QUERYBUF (v4l2_buffer) | No direct patch/hook. |
| `0x080eb613` | `0xc044560f` | VIDIOC_QBUF (v4l2_buffer) | No direct patch/hook. |
| `0x080eb64e` | `0x40045612` | VIDIOC_STREAMON (int type) | No direct patch/hook. |
| `0x080eb686` | `0xc0cc5604` | VIDIOC_G_FMT (v4l2_format) | No direct patch/hook. |
| `0x080ec579` | `0xc0105500` | USBDEVFS_CONTROL (16-byte usbdevfs_ctrltransfer) | No direct patch/hook. |
| `0x080ec5f8` | `0xc0105500` | USBDEVFS_CONTROL (16-byte usbdevfs_ctrltransfer) | No direct patch/hook. |
| `0x080ee13e` | `0x0000030d` | HDIO_GET_IDENTITY (512-byte hd_driveid) | /dev/sda raw open at 0x080ee11f is blocked by filesystem.c:129–132; ioctl skipped on fd=-1. |
| `0x08156afa` | `0x00000049` | legacy lock-driver request 0x49 | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x08156df0` | `0x00000049` | legacy lock-driver request 0x49 | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x08157039` | `0x00000048` | legacy lock-driver request 0x48 | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x081575a6` | `0x801c4803` | HIDIOCGDEVINFO | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x0815bd34` | `0x801c4803` | HIDIOCGDEVINFO | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x0815c0de` | `0x00000046` | legacy lock-driver request 0x46 | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x0815c178` | `0x00000045` | legacy lock-driver request 0x45 | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x0815c49f` | `0x501c4814` | HIDIOCSUSAGES (multi-usage write) | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x0815c4fa` | `0x400c4808` | HIDIOCSREPORT (hiddev_report_info) | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x0815c6af` | `0x400c4807` | HIDIOCGREPORT (hiddev_report_info) | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |
| `0x0815c6e8` | `0xd01c4813` | HIDIOCGUSAGES (multi-usage read) | Not individually patched. Lock-response entry 0x0811acb0 is replaced by hook_lock_response; [INFERENCE] ordinary response handling should not require this transport, but every transport entry is not proven unreachable. |

The three direct `USBDEVFS_CONTROL` paths use the fd saved at `0x0a18db0c` (initial assignment `0x080735e6`), opened at `0x080735e1` from the formatted path `/sys/kernel/debug/usb/%s/%s` (`0x081c73f8`). They do **not** go through the `libusb_*` / `usb_*` hooks. The lock transport also probes `/dev/usb/hid/hiddev`, `/dev/usb/hiddev`, `/dev/hiddev` (strings `0x081d8c92`, `0x081d8caf`, `0x081d8cc4`); the small requests `0x45/46/48/49` have no matching standard Linux ioctl name established here. Their exact numeric values, rather than invented meanings, are the ABI requirement.

### Required lxrt semantics

| API | Native-port contract derived from these paths |
|---|---|
| `syscall` | No piu syscall numbers. Libstdc++ requires i386 `__NR_futex=240` with `FUTEX_WAIT=0` and `FUTEX_WAKE=1`: compare 32-bit expected value atomically, return `-1/EAGAIN` on mismatch, wait/wake the keyed address, honor relative i386 timespec timeouts with `-1/ETIMEDOUT`, return actual wake count. A constant-success or ENOSYS implementation is not correct for C++ static initialization/futures. Other numbers may fail `ENOSYS`; do not forward Linux syscall numbers to Windows. |
| `fork`, `execv`, `waitpid` | Keep bindable exports, but after successful mixer entry patches no child process is required. Defensive failures are `fork=-1/ENOSYS`, `execv=-1/ENOENT` or `EPERM`, `waitpid=-1/ECHILD`; never return child `0` from an unimplemented fork because child path aborts after failed execv. No host `amixer` launch. |
| `execl` | Unpatched paths request **system reboot**, not a game restart/update helper. Do not invoke host busybox or reboot Windows. Return `-1` with Linux `EPERM`; do not pretend a successful exec returns. These sites remain reachable in principle, so updater suppression alone is not a blanket process sandbox. |
| `mount`, `umount` | Offline desktop needs no real Linux mount namespace. Reject unprovided devices/privileged remounts with Linux `-1/ENODEV` or `EPERM`; an unmounted target can fail `-1/EINVAL`. Never mount or remount a host disk. If removable-media support is added, it needs a real guest path/mount mapping for `/mnt/0`, `/mnt/1`, `/mnt/hd`, preserving readonly state; no success-only shim. |
| `sigaction` | Copy/store i386 glibc `struct sigaction` (140 bytes: handler @0, 128-byte mask @4, flags @132, restorer @136), fill requested old action, preserve `SA_SIGINFO=4`. These four handlers are crash diagnostics, not normal control flow; record-only installation suffices without synthesizing routine signals. If translating native faults, invoke `(signum, siginfo*, ucontext*)` with Linux layouts; never swallow a native access violation as success. |
| `ioctl` | Keep fd/error semantics and request decoding. With virtual LXIO + patched lock/disk-ID helpers, no successful raw USB, IDE, hiddev, or V4L2 operation is required for ordinary offline play. Unsupported device opens must fail, and ioctl must return `-1/ENOTTY` (`EBADF` for invalid fd), not success with unwritten output. Optional camera capture would require the complete V4L2 buffer/mmap/select protocol shown above, not FFmpeg playback APIs. Direct debugfs USB control is not neutralized by libusb interposition. |
| `tcgetattr/tcsetattr/tcflush` | Unpatched optional serial-device API. Unsupported tty fds fail `-1/ENOTTY` or invalid fd `EBADF`; do not initialize a fake serial device successfully. If a mapped COM device is enabled, marshal Linux termios, TCSANOW=0, TCIFLUSH=0, cflag/baud; save/restore the `0x0a1bee60` state, clear queued input for tcflush. |
| `mmap/munmap` | Live font files require file-backed `PROT_READ|MAP_PRIVATE`, offset zero, arbitrary length; return `(void*)-1` and Linux errno on failure. Preserve data until munmap. V4L2 mapping is shared/read-write on an optional device. Native mapping granularity differs: guest offsets need page semantics; do not confuse mmap with anonymous calloc or leak CRT fd/handle identities. |
| `dlopen/dlsym` | `dlopen(NULL,0x100)` returns a valid handle for the global ELF scope even without LAZY/NOW. Original dlsym asks `glXSwapIntervalSGI` but is NOPed at `0x0806824d` by phoenixtools. Independently, preload hooks require real RTLD_NEXT resolution after the caller module, native-provider lookup, and `dladdr` for their INI path; no recursive return of the interposed fopen/open/mkdir/X11 hook. |
| `getifaddrs/freeifaddrs` | Construct/free an i386 linked list with ifa_next @0, name @4, flags @8, addr @12, netmask @16, union @20, data @24. Game looks for AF_INET `eth0`; present a selected native IPv4 adapter under that guest name, with nonnull addr/netmask and Linux sockaddr_in. Empty successful list is a valid offline state; failure must not leave the caller's output pointer uninitialized. |
| `socket/connect` | Winsock-backed Linux descriptor abstraction: AF_INET=2, SOCK_STREAM=1; 16-byte sockaddr_in with network-order port/IP; translate WSAEWOULDBLOCK to Linux EINPROGRESS=115 on nonblocking connect and preserve SO_ERROR completion behavior. Curl offline hooks do not replace these APIs. If network policy denies it, fail explicitly with Linux errno. |
| `sysinfo` | Fill 64-byte i386 Linux sysinfo, especially totalram @16 and **nonzero** mem_unit @52 (use 1 for byte-valued RAM, clamp to uint32 as needed), not Windows MEMORYSTATUSEX layout. Caller computes totalram/(mem_unit<<10), so uninitialized/zero output crashes. |
| `select` | Translate Linux 1024-bit bitset fd_set, not Winsock's count+array structure. Support write readiness for connect and read readiness; honor {1,0} seconds and zero-time poll, update fdsets and timeout using Linux conventions, return ready count or `-1` with translated errno. |
| `fcntl` | Implement Linux F_GETFL=3 / F_SETFL=4 with O_NONBLOCK=0x800 via socket nonblocking state; preserve unrelated flags. Implement F_SETFD=2 with FD_CLOEXEC=1 for real font fds. Serial F_SETFL(0) clears nonblocking. Same symbol must accept both GLIBC_2.0 (piu) and GLIBC_2.28 (freetype). |

Libstdc++ syscall call addresses (relative ELF): `0x0008c2dc` WAIT on guard word (expected `0x100`/`0x10100`), `0x0008c349` and `0x0008c3b5` WAKE with `INT_MAX`, `0x000b779f` WAIT with relative timeout, `0x000b77e1` WAIT without timeout, `0x000b7853` WAKE with `INT_MAX`. Evidence: pushes `0xf0`, opcode arguments `0/1`, and timeout/error branches around these addresses in the full disassembly. No private-flag futex opcode was found in these six calls.

## 6.2 Guest-library closure and native import surface

The inventory in this document is the complete **non-FFmpeg** relink closure (piu plus nine guest DLLs). FFmpeg 4.2's rebuilt `libavcodec.so.58`, `libavformat.so.58`, `libavutil.so.56`, `libswscale.so.5`, and `libswresample.so.3` closure/import/TLS audit is reported separately by the FFmpeg build; its rebuilt ELF outputs were not available during this investigation. The FFmpeg build's media audit found 29 VP9+Opus WebMs, requiring the native guest Opus decoder and swresample in that separate closure. Original bundled FFmpeg libraries are not substituted into this inventory.

### Acquisition, exact inputs, and commands

`libmad.so.0` was missing from `lib/`. The extracted game rootfs supplied it; no download or replacement ABI was needed:

```sh
find ~/piuxx -name 'libmad.so*' -print
cp /home/geeki/piuxx/extracted/phoneix-2024-upi/u/slib/libmad.so.0.2.1 lib/libmad.so.0
sha256sum lib/libmad.so.0 /home/geeki/piuxx/extracted/phoneix-2024-upi/u/slib/libmad.so.0.2.1
i686-linux-gnu-readelf -h -d lib/libmad.so.0
```

Both SHA-256 results were `7a5b8c8304c64c7b11096bce6b0717aad89e85d0eee15f7521b6662a930ca9d8`. Header: ELF32, little-endian, Intel 80386, ET_DYN; DT_SONAME=`libmad.so.0`; DT_NEEDED=`libc.so.6`. This is the game's extracted `u/slib/libmad.so.0.2.1`, copied byte-for-byte, not the host ARM64 library. No package version beyond the on-disk upstream filename is inferred.

For each input in the inventory below the following commands were executed (the same five readelf views, plus complete disassembly, parsed in memory):

```sh
i686-linux-gnu-readelf -d MODULE
i686-linux-gnu-readelf --dyn-syms --wide MODULE
i686-linux-gnu-readelf -r --wide MODULE
i686-linux-gnu-readelf -l --wide MODULE
i686-linux-gnu-readelf -V --wide MODULE
i686-linux-gnu-objdump -d MODULE
```

The `.gnu.version_r` version-index-to-provider mapping, not merely the prefix `GLIBC`, determines the native surface. In particular `__divdi3/__udivdi3/__umoddi3@GLIBC_2.0` belong to **relinked libgcc_s**, not libc HLE. The C++/FTGL/FreeType/libpng/zlib/mad/curl definitions also remain guest-provided. Weak unversioned pthread references still need the native pthread implementation when present; optional transaction/profiling references may remain null. Static metadata investigation only: these commands are not a runtime launch/build/test of the game.

`imports.tsv` keeps the original five columns with empty version for unversioned symbols. Existing labels `libstdc++` and `libgcc_s` are retained and mean `lib/libstdc++.so.6` / `lib/libgcc_s.so.1`; new modules use SONAME labels. Every prior row is preserved. Missing piu C++ imports and libstdc++ unwinder imports are restored rather than treating the original TSV as complete. The full undefined-symbol inventory is also listed below per guest DLL.

### Exact relink inputs

| Module | ELF input path | SHA-256 |
|---|---|---|
| `piu` | `piu` | `850cccbf706f2b7e5b30d4a677316ed8a1601993e5a96d353dc6a88f5e23c96f` |
| `phoenixtools.so` | `phoenixtools/bin/phoenixtools.so` | `2bf58a40b8320d329c73e253583278b380955825883e5d21022fd345127e807b` |
| `libcurl.so.4` | `phoenixtools/bin/libcurl.so.4` | `c77fb1f7bcdec51c8994c4107edb353ea5ca915d85c442168129d0581778fd8a` |
| `libstdc++.so.6` | `lib/libstdc++.so.6` | `0dd6a959b95b989f542a0f2a7fa96fed4526b73669f522560a8b291eeec1114c` |
| `libgcc_s.so.1` | `lib/libgcc_s.so.1` | `fab8c4046b93255bbdc4b61cdcab1d36316b1edfa42310388a0c0310bb030c80` |
| `libftgl.so.2` | `lib/libftgl.so.2` | `4015ea2f37346816fde40e66a6a5852fdeeb30277013b20624c66f5bfadc8910` |
| `libfreetype.so.6` | `lib/libfreetype.so.6` | `33b625a3dfe98d12fb1100c6a207b5f5632cac9f84e126db1a72bc311d608977` |
| `libpng16.so.16` | `lib/libpng16.so.16` | `89d5b8c4236768a4f5cd1643c69e582e3a985aa8a5f224f03552b4a0f344b30a` |
| `libz.so.1` | `lib/libz.so.1` | `51bd894b3a6a637e6fd7aa783a9a5eb454e7ea7d68b84a53a17921960d1de29b` |
| `libmad.so.0` | `lib/libmad.so.0` | `7a5b8c8304c64c7b11096bce6b0717aad89e85d0eee15f7521b6662a930ca9d8` |

### Complete non-FFmpeg DT_NEEDED closure (native boundaries stop recursion)

`piu` directly requires the following 20 SONAMEs; phoenixtools is an explicit preload in addition, not a piu DT_NEEDED entry. “Native” here means lxrt/bridge/provider, not an ELF DLL to relink. libusb-0.1 and libusb-1.0 exports are supplied by phoenixtools in virtual mode. The real bundled curl and original large-closure FFmpeg are **not** relink inputs.

- piu DT_NEEDED: `libpthread.so.0`, `libgcc_s.so.1`, `libc.so.6`, `libcurl.so.4`, `libusb-0.1.so.4`, `libmad.so.0`, `libftgl.so.2`, `libGL.so.1`, `libGLU.so.1`, `libasound.so.2`, `libavcodec.so.58`, `libavutil.so.56`, `libswscale.so.5`, `libavformat.so.58`, `libX11.so.6`, `libdl.so.2`, `libusb-1.0.so.0`, `libXxf86vm.so.1`, `libstdc++.so.6`, `libm.so.6`.

| Relinked DLL | Complete DT_NEEDED | Other closure decision |
|---|---|---|
| `phoenixtools.so` | `libc.so.6` (native) | All dependencies accounted for in this table or native boundary. |
| `libcurl.so.4` | (none) | Use phoenixtools stub, not a real curl dependency tree. |
| `libstdc++.so.6` | `libm.so.6` (native), `libc.so.6` (native), `ld-linux.so.2` (native), `libgcc_s.so.1` (guest) | All dependencies accounted for in this table or native boundary. |
| `libgcc_s.so.1` | `libc.so.6` (native) | All dependencies accounted for in this table or native boundary. |
| `libftgl.so.2` | `libfreetype.so.6` (guest), `libGL.so.1` (native), `libGLU.so.1` (native), `libstdc++.so.6` (guest), `libm.so.6` (native), `libc.so.6` (native), `libgcc_s.so.1` (guest) | All dependencies accounted for in this table or native boundary. |
| `libfreetype.so.6` | `libpng16.so.16` (guest), `libz.so.1` (guest), `libc.so.6` (native) | All dependencies accounted for in this table or native boundary. |
| `libpng16.so.16` | `libz.so.1` (guest), `libm.so.6` (native), `libc.so.6` (native) | All dependencies accounted for in this table or native boundary. |
| `libz.so.1` | `libc.so.6` (native) | All dependencies accounted for in this table or native boundary. |
| `libmad.so.0` | `libc.so.6` (native) | All dependencies accounted for in this table or native boundary. |

Native system boundaries: `libc.so.6`, `libm.so.6`, `libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `ld-linux.so.2`, `libGL.so.1`, `libGLU.so.1`, `libX11.so.6`, `libXxf86vm.so.1`, `libasound.so.2`, `libusb-0.1.so.4`, `libusb-1.0.so.0`. No independent bundled `libbrotli`, `libbz2`, `libharfbuzz`, SSL, or HTTP dependency is introduced by the actual freetype/curl inputs above.

### TLS, segment mapping, and relocations

Executable disassembly was scanned for every `%gs:` access, not just a substring count of `0x14`. First PT_LOAD offset comes from `readelf -l --wide`; relocation counts come from `readelf -r --wide`. All counts are dynamic relocations. `PT_TLS` is separate from stack-canary accesses.

| Module | `%gs:0x14` count | Other `%gs:` | TLS relocation types | First PT_LOAD file offset / mapped header | Dynamic relocation counts |
|---|---:|---|---|---|---|
| `piu` | 390 | none | none | `0x000000` / yes | COPY ×14, GLOB_DAT ×1, JUMP_SLOT ×439 |
| `phoenixtools.so` | 0 | none | none | `0x000000` / yes | GLOB_DAT ×33, JUMP_SLOT ×98, RELATIVE ×58 |
| `libcurl.so.4` | 0 | none | none | `0x000000` / yes | GLOB_DAT ×4, RELATIVE ×3 |
| `libstdc++.so.6` | 2257 | none | R_386_TLS_DTPMOD32 ×3, R_386_TLS_DTPOFF32 ×2 | `0x000000` / yes | 32 ×2696, GLOB_DAT ×547, JUMP_SLOT ×1026, RELATIVE ×890, TLS_DTPMOD32 ×3, TLS_DTPOFF32 ×2 |
| `libgcc_s.so.1` | 0 | none | none | `0x000000` / yes | 32 ×1, GLOB_DAT ×6, JUMP_SLOT ×44, RELATIVE ×3 |
| `libftgl.so.2` | 122 | none | none | `0x000000` / yes | 32 ×771, GLOB_DAT ×94, JUMP_SLOT ×312, RELATIVE ×7 |
| `libfreetype.so.6` | 774 | none | none | `0x000000` / yes | GLOB_DAT ×5, JUMP_SLOT ×58, RELATIVE ×1327 |
| `libpng16.so.16` | 212 | none | none | `0x000000` / yes | GLOB_DAT ×6, JUMP_SLOT ×38, RELATIVE ×6 |
| `libz.so.1` | 26 | none | none | `0x000000` / yes | GLOB_DAT ×4, JUMP_SLOT ×18, RELATIVE ×28 |
| `libmad.so.0` | 39 | none | none | `0x000000` / yes | GLOB_DAT ×7, JUMP_SLOT ×16, RELATIVE ×65 |

Unsupported/out-of-list relocation types: **none** in every listed input.

Only libstdc++ has PT_TLS among the non-FFmpeg inputs: ELF `PT_TLS` vaddr `0x001d5a0c`, FileSiz=0, MemSiz=`0x10`, alignment=4. Its five TLS relocation locations are `0x001da704` DTPMOD32 (local module), `0x001da844` DTPMOD32 / `0x001da848` DTPOFF32 for `_ZSt15__once_callable` (offset 12), and `0x001dab8c` DTPMOD32 / `0x001dab90` DTPOFF32 for `_ZSt11__once_call` (offset 8). These require general-dynamic DTV support through regparm(1) `___tls_get_addr@GLIBC_2.3`; they do not justify retaining Linux `%gs` segment TLS. Every audited input maps ELF header file offset 0 and therefore keeps valid in-image ELF/phdr/dynamic metadata for the binder and unwinder.

### Combined de-duplicated native symbol list (PLAN §3.2 groups)

289 distinct `(symbol, version)` pairs, including unversioned weak pthread/lifetime requests; 21 pairs are **NEW** relative to the original 534-row TSV. De-duplication preserves distinct versions of the same name. **existing** means that exact name/version was already in the old TSV, not that lxrt currently implements it. Provider names come from GNU version needs; unversioned weak pthread/lifetime entries are explicitly classified by API. Symbol kind/binding remain available in imports.tsv.

#### loader, lifetime, and diagnostics

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `___tls_get_addr` | `GLIBC_2.3` | existing | `libstdc++.so.6` | `ld-linux.so.2` |
| `__assert_fail` | `GLIBC_2.0` | existing | `libftgl.so.2`, `libstdc++.so.6` | `libc.so.6` |
| `__cxa_atexit` | `GLIBC_2.1.3` | existing | `libftgl.so.2`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `__cxa_finalize` | `` | **NEW** | `libcurl.so.4` | `libc.so.6` (weak) |
| `__cxa_finalize` | `GLIBC_2.1.3` | existing | `libfreetype.so.6`, `libftgl.so.2`, `libgcc_s.so.1`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so` | `libc.so.6` (weak) |
| `__cxa_thread_atexit_impl` | `GLIBC_2.18` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__libc_start_main` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `__stack_chk_fail` | `GLIBC_2.4` | existing | `libfreetype.so.6`, `libftgl.so.2`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `piu` | `libc.so.6` |
| `dl_iterate_phdr` | `GLIBC_2.2.4` | existing | `libgcc_s.so.1` | `libc.so.6` |
| `dladdr` | `GLIBC_2.34` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `dlopen` | `GLIBC_2.1` | existing | `piu` | `libdl.so.2` |
| `dlsym` | `GLIBC_2.0` | existing | `piu` | `libdl.so.2` |
| `dlsym` | `GLIBC_2.34` | **NEW** | `phoenixtools.so` | `libc.so.6` |

#### string, memory, allocation, and character conversion

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `__ctype_b_loc` | `GLIBC_2.3` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `__ctype_tolower_loc` | `GLIBC_2.3` | existing | `phoenixtools.so`, `piu` | `libc.so.6` |
| `__fdelt_chk` | `GLIBC_2.15` | existing | `piu` | `libc.so.6` |
| `__memcpy_chk` | `GLIBC_2.3.4` | existing | `libfreetype.so.6`, `libpng16.so.16`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `__memset_chk` | `GLIBC_2.3.4` | existing | `piu` | `libc.so.6` |
| `__strcat_chk` | `GLIBC_2.3.4` | existing | `piu` | `libc.so.6` |
| `__strcpy_chk` | `GLIBC_2.3.4` | existing | `phoenixtools.so`, `piu` | `libc.so.6` |
| `aligned_alloc` | `GLIBC_2.16` | existing | `libstdc++.so.6` | `libc.so.6` |
| `btowc` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `calloc` | `GLIBC_2.0` | existing | `libgcc_s.so.1`, `libmad.so.0`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `free` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libftgl.so.2`, `libgcc_s.so.1`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `isalnum` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `isalpha` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `isspace` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `iswspace` | `GLIBC_2.0` | **NEW** | `libftgl.so.2` | `libc.so.6` |
| `malloc` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libftgl.so.2`, `libgcc_s.so.1`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `mbrtowc` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `mbsnrtowcs` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `mbsrtowcs` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `memchr` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libstdc++.so.6`, `libz.so.1`, `piu` | `libc.so.6` |
| `memcmp` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libpng16.so.16`, `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `memcpy` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libftgl.so.2`, `libgcc_s.so.1`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `memmove` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libftgl.so.2`, `libmad.so.0`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `memset` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libftgl.so.2`, `libgcc_s.so.1`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `qsort` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `piu` | `libc.so.6` |
| `rand` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `realloc` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libgcc_s.so.1`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `srand` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `strcasecmp` | `GLIBC_2.0` | existing | `phoenixtools.so`, `piu` | `libc.so.6` |
| `strcat` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `piu` | `libc.so.6` |
| `strchr` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `strcmp` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libftgl.so.2`, `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `strcpy` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `piu` | `libc.so.6` |
| `strdup` | `GLIBC_2.0` | existing | `libftgl.so.2`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `strerror` | `GLIBC_2.0` | existing | `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `strlen` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libgcc_s.so.1`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `strncasecmp` | `GLIBC_2.0` | existing | `phoenixtools.so`, `piu` | `libc.so.6` |
| `strncmp` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libftgl.so.2`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `strncpy` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `strndup` | `GLIBC_2.0` | **NEW** | `libftgl.so.2` | `libc.so.6` |
| `strnlen` | `GLIBC_2.0` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `strrchr` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `strstr` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `strtod` | `GLIBC_2.0` | existing | `libpng16.so.16`, `piu` | `libc.so.6` |
| `strtok` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `strtok_r` | `GLIBC_2.0` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `strtol` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `piu` | `libc.so.6` |
| `strtoul` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `tolower` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `toupper` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `wcrtomb` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `wcscmp` | `GLIBC_2.0` | existing | `libftgl.so.2`, `libstdc++.so.6` | `libc.so.6` |
| `wcsdup` | `GLIBC_2.0` | existing | `libftgl.so.2`, `piu` | `libc.so.6` |
| `wcslen` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `wcsncmp` | `GLIBC_2.0` | **NEW** | `libftgl.so.2` | `libc.so.6` |
| `wcsnrtombs` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `wctob` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `wmemchr` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `wmemcmp` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `wmemcpy` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `wmemmove` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `wmemset` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |

#### stdio and formatted I/O

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `__fprintf_chk` | `GLIBC_2.3.4` | existing | `libftgl.so.2`, `libpng16.so.16`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `__fread_chk` | `GLIBC_2.7` | existing | `piu` | `libc.so.6` |
| `__isoc99_sscanf` | `GLIBC_2.7` | existing | `phoenixtools.so`, `piu` | `libc.so.6` |
| `__printf_chk` | `GLIBC_2.3.4` | existing | `piu` | `libc.so.6` |
| `__snprintf_chk` | `GLIBC_2.3.4` | existing | `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `__sprintf_chk` | `GLIBC_2.3.4` | existing | `libfreetype.so.6`, `libmad.so.0`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `__vsnprintf_chk` | `GLIBC_2.3.4` | existing | `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `__vsprintf_chk` | `GLIBC_2.3.4` | existing | `piu` | `libc.so.6` |
| `fclose` | `GLIBC_2.1` | existing | `libpng16.so.16`, `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `fdopen` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `ferror` | `GLIBC_2.0` | existing | `libpng16.so.16`, `piu` | `libc.so.6` |
| `fflush` | `GLIBC_2.0` | existing | `libpng16.so.16`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `fgets` | `GLIBC_2.0` | existing | `phoenixtools.so`, `piu` | `libc.so.6` |
| `fileno` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `fopen` | `GLIBC_2.1` | existing | `libpng16.so.16`, `piu` | `libc.so.6` |
| `fopen64` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `fputc` | `GLIBC_2.0` | existing | `libpng16.so.16`, `libstdc++.so.6` | `libc.so.6` |
| `fputs` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `fread` | `GLIBC_2.0` | existing | `libpng16.so.16`, `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `fscanf` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `fseek` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `fseeko` | `GLIBC_2.1` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `fseeko64` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `ftell` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `ftello64` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `fwrite` | `GLIBC_2.0` | existing | `libftgl.so.2`, `libpng16.so.16`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `getc` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `getwc` | `GLIBC_2.2` | existing | `libstdc++.so.6` | `libc.so.6` |
| `putc` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `putwc` | `GLIBC_2.2` | existing | `libstdc++.so.6` | `libc.so.6` |
| `setvbuf` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `snprintf` | `GLIBC_2.0` | existing | `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `sprintf` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `sscanf` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `stderr` | `GLIBC_2.0` | existing | `libftgl.so.2`, `libpng16.so.16`, `libstdc++.so.6` | `libc.so.6` |
| `stdin` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `stdout` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `ungetc` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `ungetwc` | `GLIBC_2.2` | existing | `libstdc++.so.6` | `libc.so.6` |
| `vsnprintf` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `vsprintf` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |

#### locale ABI and locale-specific conversion

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `__ctype_get_mb_cur_max` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__duplocale` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__freelocale` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__iswctype_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__newlocale` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__nl_langinfo_l` | `GLIBC_2.2` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__strcoll_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__strftime_l` | `GLIBC_2.3` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__strtod_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__strtof_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__strxfrm_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__towlower_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__towupper_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__uselocale` | `GLIBC_2.3` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__wcscoll_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__wcsftime_l` | `GLIBC_2.3` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__wcsxfrm_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__wctype_l` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `setlocale` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `strtold_l` | `GLIBC_2.3` | existing | `libstdc++.so.6` | `libc.so.6` |

#### files, directories, and mappings

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `__fxstat` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `piu` | `libc.so.6` |
| `__fxstat64` | `GLIBC_2.2` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__lxstat64` | `GLIBC_2.2` | existing | `libstdc++.so.6` | `libc.so.6` |
| `__xstat` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `__xstat64` | `GLIBC_2.2` | existing | `libstdc++.so.6` | `libc.so.6` |
| `chdir` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `close` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libmad.so.0`, `libstdc++.so.6`, `libz.so.1`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `closedir` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `fchmod` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `fchmodat` | `GLIBC_2.4` | existing | `libstdc++.so.6` | `libc.so.6` |
| `fcntl` | `GLIBC_2.0` | existing | `libmad.so.0`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `fcntl` | `GLIBC_2.28` | **NEW** | `libfreetype.so.6` | `libc.so.6` |
| `fsync` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `futimes` | `GLIBC_2.3` | existing | `piu` | `libc.so.6` |
| `getcwd` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `glob` | `GLIBC_2.27` | existing | `piu` | `libc.so.6` |
| `globfree` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `ioctl` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `link` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `lseek` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `lseek64` | `GLIBC_2.1` | existing | `libstdc++.so.6`, `libz.so.1` | `libc.so.6` |
| `lseek64` | `GLIBC_2.2` | existing | `piu` | `libpthread.so.0` |
| `lstat` | `GLIBC_2.33` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `mkdir` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `mmap` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `piu` | `libc.so.6` |
| `mprotect` | `GLIBC_2.0` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `munmap` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `piu` | `libc.so.6` |
| `open` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libstdc++.so.6`, `libz.so.1`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `open64` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `opendir` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `pipe` | `GLIBC_2.0` | **NEW** | `libmad.so.0` | `libc.so.6` |
| `read` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libmad.so.0`, `libstdc++.so.6`, `libz.so.1`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `readdir64` | `GLIBC_2.2` | existing | `libstdc++.so.6`, `phoenixtools.so` | `libc.so.6` |
| `readlink` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `realpath` | `GLIBC_2.3` | existing | `libstdc++.so.6` | `libc.so.6` |
| `remove` | `GLIBC_2.0` | existing | `libpng16.so.16`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `rename` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `sendfile64` | `GLIBC_2.3` | existing | `libstdc++.so.6` | `libc.so.6` |
| `stat` | `GLIBC_2.33` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `statvfs64` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `symlink` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `sync` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `truncate64` | `GLIBC_2.1` | existing | `libstdc++.so.6` | `libc.so.6` |
| `utimensat` | `GLIBC_2.6` | existing | `libstdc++.so.6` | `libc.so.6` |
| `write` | `GLIBC_2.0` | existing | `libmad.so.0`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `writev` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |

#### threads and synchronization

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `__pthread_key_create` | `` | existing | `libgcc_s.so.1`, `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `__pthread_key_create` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` (weak) |
| `pthread_attr_destroy` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_attr_init` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |
| `pthread_attr_setdetachstate` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_attr_setschedparam` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_attr_setschedpolicy` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_attr_setstacksize` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |
| `pthread_barrier_destroy` | `GLIBC_2.2` | existing | `piu` | `libpthread.so.0` |
| `pthread_barrier_init` | `GLIBC_2.2` | existing | `piu` | `libpthread.so.0` |
| `pthread_barrier_wait` | `GLIBC_2.2` | existing | `piu` | `libpthread.so.0` |
| `pthread_cond_broadcast` | `GLIBC_2.3.2` | existing | `libstdc++.so.6`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `pthread_cond_destroy` | `GLIBC_2.3.2` | existing | `libstdc++.so.6`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `pthread_cond_init` | `GLIBC_2.3.2` | existing | `piu` | `libpthread.so.0` |
| `pthread_cond_signal` | `GLIBC_2.3.2` | existing | `libstdc++.so.6`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `pthread_cond_timedwait` | `GLIBC_2.3.2` | existing | `piu` | `libpthread.so.0` |
| `pthread_cond_wait` | `GLIBC_2.3.2` | existing | `libstdc++.so.6`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `pthread_create` | `` | existing | `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_create` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |
| `pthread_detach` | `` | existing | `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_exit` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_getspecific` | `` | existing | `libgcc_s.so.1`, `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_join` | `` | existing | `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_join` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_key_create` | `` | existing | `libgcc_s.so.1`, `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_key_delete` | `` | existing | `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_kill` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_mutex_destroy` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_mutex_init` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_mutex_lock` | `GLIBC_2.0` | existing | `libgcc_s.so.1`, `libstdc++.so.6`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `pthread_mutex_trylock` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_mutex_unlock` | `GLIBC_2.0` | existing | `libgcc_s.so.1`, `libstdc++.so.6`, `piu` | `libc.so.6`, `libpthread.so.0` |
| `pthread_mutexattr_destroy` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_mutexattr_init` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `pthread_mutexattr_settype` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |
| `pthread_once` | `` | existing | `libgcc_s.so.1`, `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_rwlock_rdlock` | `` | existing | `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_rwlock_unlock` | `` | existing | `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_rwlock_wrlock` | `` | existing | `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `pthread_setspecific` | `` | existing | `libgcc_s.so.1`, `libstdc++.so.6` | `libpthread.so.0` (weak) |
| `sem_destroy` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |
| `sem_init` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |
| `sem_post` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |
| `sem_trywait` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |
| `sem_wait` | `GLIBC_2.1` | existing | `piu` | `libpthread.so.0` |

#### time

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `clock_gettime` | `GLIBC_2.17` | existing | `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `clock_nanosleep` | `GLIBC_2.17` | **NEW** | `phoenixtools.so` | `libc.so.6` |
| `gettimeofday` | `GLIBC_2.0` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `gmtime` | `GLIBC_2.0` | **NEW** | `libpng16.so.16` | `libc.so.6` |
| `localtime` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `mktime` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `nanosleep` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `sleep` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `time` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `usleep` | `GLIBC_2.0` | existing | `phoenixtools.so`, `piu` | `libc.so.6` |

#### network

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `connect` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `freeifaddrs` | `GLIBC_2.3` | existing | `piu` | `libc.so.6` |
| `gethostbyname` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `getifaddrs` | `GLIBC_2.3` | existing | `piu` | `libc.so.6` |
| `getsockopt` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `inet_addr` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `inet_ntoa` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `poll` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `recv` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `select` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `send` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `socket` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |

#### process, system, and nonlocal jumps

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `__longjmp_chk` | `GLIBC_2.11` | existing | `libfreetype.so.6`, `libpng16.so.16`, `piu` | `libc.so.6` |
| `_exit` | `GLIBC_2.0` | existing | `libmad.so.0`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `_setjmp` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libpng16.so.16`, `piu` | `libc.so.6` |
| `abort` | `GLIBC_2.0` | existing | `libgcc_s.so.1`, `libpng16.so.16`, `libstdc++.so.6`, `piu` | `libc.so.6` |
| `execl` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `execv` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `exit` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `fork` | `GLIBC_2.0` | existing | `libmad.so.0`, `piu` | `libc.so.6` |
| `get_nprocs` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `getenv` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libstdc++.so.6`, `phoenixtools.so`, `piu` | `libc.so.6` |
| `ioperm` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `mount` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `sigaction` | `GLIBC_2.0` | existing | `piu` | `libpthread.so.0` |
| `syscall` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `sysconf` | `GLIBC_2.0` | existing | `phoenixtools.so`, `piu` | `libc.so.6` |
| `sysinfo` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `tcflush` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `tcgetattr` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `tcsetattr` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `umount` | `GLIBC_2.0` | existing | `piu` | `libc.so.6` |
| `waitpid` | `GLIBC_2.0` | existing | `libmad.so.0`, `piu` | `libc.so.6`, `libpthread.so.0` |

#### gettext and iconv

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `bind_textdomain_codeset` | `GLIBC_2.2` | existing | `libstdc++.so.6` | `libc.so.6` |
| `bindtextdomain` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `dgettext` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `gettext` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libc.so.6` |
| `iconv` | `GLIBC_2.1` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `iconv_close` | `GLIBC_2.1` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |
| `iconv_open` | `GLIBC_2.1` | existing | `libstdc++.so.6`, `piu` | `libc.so.6` |

#### math (including x87 long double)

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `atan2` | `GLIBC_2.0` | **NEW** | `libftgl.so.2` | `libm.so.6` |
| `atan2f` | `GLIBC_2.0` | existing | `piu` | `libm.so.6` |
| `frexp` | `GLIBC_2.0` | **NEW** | `libpng16.so.16` | `libm.so.6` |
| `frexpl` | `GLIBC_2.0` | existing | `libstdc++.so.6` | `libm.so.6` |
| `modf` | `GLIBC_2.0` | **NEW** | `libpng16.so.16` | `libm.so.6` |
| `pow` | `GLIBC_2.0` | existing | `piu` | `libm.so.6` |
| `pow` | `GLIBC_2.29` | **NEW** | `libpng16.so.16` | `libm.so.6` |
| `sin` | `GLIBC_2.0` | existing | `piu` | `libm.so.6` |
| `sinf` | `GLIBC_2.0` | existing | `piu` | `libm.so.6` |
| `sqrt` | `GLIBC_2.0` | existing | `libftgl.so.2`, `piu` | `libm.so.6` |
| `tanf` | `GLIBC_2.0` | existing | `piu` | `libm.so.6` |

#### errno

| Symbol | Version | Delta | Required by | Provider boundary |
|---|---|---|---|---|
| `__errno_location` | `GLIBC_2.0` | existing | `libfreetype.so.6`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` | `libc.so.6`, `libpthread.so.0` |

#### Optional instrumentation / transaction weak imports

These are not extra required libc APIs and must not become unconditional unresolved-symbol traps. Resolve a real definition if in scope, otherwise bind weak to zero. Transactional C++ clones stay guest-owned. Unversioned weak pthread/lifetime functions are already in the native tables above.

| Weak symbol | Modules |
|---|---|
| `_ITM_RU1` | `libstdc++.so.6` |
| `_ITM_RU4` | `libstdc++.so.6` |
| `_ITM_addUserCommitAction` | `libstdc++.so.6` |
| `_ITM_deregisterTMCloneTable` | `libcurl.so.4`, `libfreetype.so.6`, `libftgl.so.2`, `libgcc_s.so.1`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so` |
| `_ITM_memcpyRnWt` | `libstdc++.so.6` |
| `_ITM_memcpyRtWn` | `libstdc++.so.6` |
| `_ITM_registerTMCloneTable` | `libcurl.so.4`, `libfreetype.so.6`, `libftgl.so.2`, `libgcc_s.so.1`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so` |
| `_ZGTtdlPv` | `libstdc++.so.6` |
| `_ZGTtnaj` | `libstdc++.so.6` |
| `__gmon_start__` | `libcurl.so.4`, `libfreetype.so.6`, `libftgl.so.2`, `libgcc_s.so.1`, `libmad.so.0`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1`, `phoenixtools.so`, `piu` |

#### Dynamic lookups not visible as undefined symbols

Phoenixtools resolves `fopen`, `open`, `mkdir`, `curl_easy_setopt` via RTLD_NEXT (`phoenix/patch.c:89–92`); X11 helpers `XStringToKeysym`, `XKeysymToKeycode` (`input/bindings.c:63–65`), `XNextEvent`, `XQueryKeymap`, `XLookupString` (`input/x11_input.c:84,116,151`), `glXSwapBuffers` (`safety/glx_pacing.c:20`), plus physical-mode USB symbols through `compat/usb_forward.c:11`. These require native table/guest scope participation even when there is no corresponding undefined row. Curl setopt resolves the guest stub behind the preload; X11 and GLX resolve their bridge providers. Physical USB mode is not required for virtual LXIO.

### Every audited guest DLL's undefined dynamic symbols

The following is the complete list from the per-input `--dyn-syms --wide` output. `@VERSION` is the version requirement; no suffix means unversioned; `[WEAK]` means null is permitted if unresolved. The five-column `imports.tsv` additionally records FUNC/OBJECT/NOTYPE and binding. piu's complete 440 entries are in that TSV; the original 85 omitted C++/unwind imports have been added.

#### `phoenixtools.so` — 51 undefined symbols

```text
_ITM_deregisterTMCloneTable [WEAK]
_ITM_registerTMCloneTable [WEAK]
__ctype_b_loc@GLIBC_2.3
__ctype_tolower_loc@GLIBC_2.3
__cxa_finalize@GLIBC_2.1.3 [WEAK]
__errno_location@GLIBC_2.0
__gmon_start__ [WEAK]
__isoc99_sscanf@GLIBC_2.7
__memcpy_chk@GLIBC_2.3.4
__snprintf_chk@GLIBC_2.3.4
__strcpy_chk@GLIBC_2.3.4
__vsnprintf_chk@GLIBC_2.3.4
_exit@GLIBC_2.0
calloc@GLIBC_2.0
clock_gettime@GLIBC_2.17
clock_nanosleep@GLIBC_2.17
closedir@GLIBC_2.0
dladdr@GLIBC_2.34
dlsym@GLIBC_2.34
fclose@GLIBC_2.1
fgets@GLIBC_2.0
fread@GLIBC_2.0
free@GLIBC_2.0
fseeko@GLIBC_2.1
getenv@GLIBC_2.0
lstat@GLIBC_2.33
malloc@GLIBC_2.0
memcmp@GLIBC_2.0
memcpy@GLIBC_2.0
memset@GLIBC_2.0
mprotect@GLIBC_2.0
opendir@GLIBC_2.0
readdir64@GLIBC_2.2
snprintf@GLIBC_2.0
stat@GLIBC_2.33
strcasecmp@GLIBC_2.0
strchr@GLIBC_2.0
strcmp@GLIBC_2.0
strerror@GLIBC_2.0
strlen@GLIBC_2.0
strncasecmp@GLIBC_2.0
strncmp@GLIBC_2.0
strncpy@GLIBC_2.0
strnlen@GLIBC_2.0
strrchr@GLIBC_2.0
strstr@GLIBC_2.0
strtok_r@GLIBC_2.0
strtoul@GLIBC_2.0
sysconf@GLIBC_2.0
usleep@GLIBC_2.0
write@GLIBC_2.0
```

#### `libcurl.so.4` — 4 undefined symbols

```text
_ITM_deregisterTMCloneTable [WEAK]
_ITM_registerTMCloneTable [WEAK]
__cxa_finalize [WEAK]
__gmon_start__ [WEAK]
```

#### `libstdc++.so.6` — 172 undefined symbols

```text
_ITM_RU1 [WEAK]
_ITM_RU4 [WEAK]
_ITM_addUserCommitAction [WEAK]
_ITM_deregisterTMCloneTable [WEAK]
_ITM_memcpyRnWt [WEAK]
_ITM_memcpyRtWn [WEAK]
_ITM_registerTMCloneTable [WEAK]
_Unwind_DeleteException@GCC_3.0
_Unwind_GetDataRelBase@GCC_3.0
_Unwind_GetIPInfo@GCC_4.2.0
_Unwind_GetLanguageSpecificData@GCC_3.0
_Unwind_GetRegionStart@GCC_3.0
_Unwind_GetTextRelBase@GCC_3.0
_Unwind_RaiseException@GCC_3.0
_Unwind_Resume@GCC_3.0
_Unwind_Resume_or_Rethrow@GCC_3.3
_Unwind_SetGR@GCC_3.0
_Unwind_SetIP@GCC_3.0
_ZGTtdlPv [WEAK]
_ZGTtnaj [WEAK]
___tls_get_addr@GLIBC_2.3
__assert_fail@GLIBC_2.0
__ctype_get_mb_cur_max@GLIBC_2.0
__ctzdi2@GCC_3.4
__cxa_atexit@GLIBC_2.1.3
__cxa_finalize@GLIBC_2.1.3 [WEAK]
__cxa_thread_atexit_impl@GLIBC_2.18
__divdi3@GLIBC_2.0
__duplocale@GLIBC_2.1
__errno_location@GLIBC_2.0
__fprintf_chk@GLIBC_2.3.4
__freelocale@GLIBC_2.1
__fxstat64@GLIBC_2.2
__gmon_start__ [WEAK]
__iswctype_l@GLIBC_2.1
__lxstat64@GLIBC_2.2
__moddi3@GLIBC_2.0
__newlocale@GLIBC_2.1
__nl_langinfo_l@GLIBC_2.2
__popcountsi2@GCC_3.4
__pthread_key_create [WEAK]
__sprintf_chk@GLIBC_2.3.4
__stack_chk_fail@GLIBC_2.4
__strcoll_l@GLIBC_2.1
__strftime_l@GLIBC_2.3
__strtod_l@GLIBC_2.1
__strtof_l@GLIBC_2.1
__strxfrm_l@GLIBC_2.1
__towlower_l@GLIBC_2.1
__towupper_l@GLIBC_2.1
__udivdi3@GLIBC_2.0
__umoddi3@GLIBC_2.0
__uselocale@GLIBC_2.3
__wcscoll_l@GLIBC_2.1
__wcsftime_l@GLIBC_2.3
__wcsxfrm_l@GLIBC_2.1
__wctype_l@GLIBC_2.1
__xstat64@GLIBC_2.2
abort@GLIBC_2.0
aligned_alloc@GLIBC_2.16
bind_textdomain_codeset@GLIBC_2.2
bindtextdomain@GLIBC_2.0
btowc@GLIBC_2.0
chdir@GLIBC_2.0
clock_gettime@GLIBC_2.17
close@GLIBC_2.0
closedir@GLIBC_2.0
dgettext@GLIBC_2.0
fchmod@GLIBC_2.0
fchmodat@GLIBC_2.4
fclose@GLIBC_2.1
fdopen@GLIBC_2.1
fflush@GLIBC_2.0
fileno@GLIBC_2.0
fopen64@GLIBC_2.1
fputc@GLIBC_2.0
fputs@GLIBC_2.0
fread@GLIBC_2.0
free@GLIBC_2.0
frexpl@GLIBC_2.0
fseeko64@GLIBC_2.1
ftello64@GLIBC_2.1
fwrite@GLIBC_2.0
get_nprocs@GLIBC_2.0
getc@GLIBC_2.0
getcwd@GLIBC_2.0
getenv@GLIBC_2.0
gettext@GLIBC_2.0
gettimeofday@GLIBC_2.0
getwc@GLIBC_2.2
iconv@GLIBC_2.1
iconv_close@GLIBC_2.1
iconv_open@GLIBC_2.1
ioctl@GLIBC_2.0
isspace@GLIBC_2.0
link@GLIBC_2.0
lseek64@GLIBC_2.1
malloc@GLIBC_2.0
mbrtowc@GLIBC_2.0
mbsnrtowcs@GLIBC_2.0
mbsrtowcs@GLIBC_2.0
memchr@GLIBC_2.0
memcmp@GLIBC_2.0
memcpy@GLIBC_2.0
memmove@GLIBC_2.0
memset@GLIBC_2.0
mkdir@GLIBC_2.0
nanosleep@GLIBC_2.0
open@GLIBC_2.0
open64@GLIBC_2.1
opendir@GLIBC_2.0
poll@GLIBC_2.0
pthread_cond_broadcast@GLIBC_2.3.2 [WEAK]
pthread_cond_destroy@GLIBC_2.3.2 [WEAK]
pthread_cond_signal@GLIBC_2.3.2 [WEAK]
pthread_cond_wait@GLIBC_2.3.2 [WEAK]
pthread_create [WEAK]
pthread_detach [WEAK]
pthread_getspecific [WEAK]
pthread_join [WEAK]
pthread_key_create [WEAK]
pthread_key_delete [WEAK]
pthread_mutex_lock@GLIBC_2.0 [WEAK]
pthread_mutex_unlock@GLIBC_2.0 [WEAK]
pthread_once [WEAK]
pthread_rwlock_rdlock [WEAK]
pthread_rwlock_unlock [WEAK]
pthread_rwlock_wrlock [WEAK]
pthread_setspecific [WEAK]
putc@GLIBC_2.0
putwc@GLIBC_2.2
read@GLIBC_2.0
readdir64@GLIBC_2.2
readlink@GLIBC_2.0
realloc@GLIBC_2.0
realpath@GLIBC_2.3
remove@GLIBC_2.0
rename@GLIBC_2.0
sendfile64@GLIBC_2.3
setlocale@GLIBC_2.0
setvbuf@GLIBC_2.0
sprintf@GLIBC_2.0
statvfs64@GLIBC_2.1
stderr@GLIBC_2.0
stdin@GLIBC_2.0
stdout@GLIBC_2.0
strchr@GLIBC_2.0
strcmp@GLIBC_2.0
strdup@GLIBC_2.0
strerror@GLIBC_2.0
strlen@GLIBC_2.0
strtold_l@GLIBC_2.3
strtoul@GLIBC_2.0
symlink@GLIBC_2.0
syscall@GLIBC_2.0
truncate64@GLIBC_2.1
ungetc@GLIBC_2.0
ungetwc@GLIBC_2.2
utimensat@GLIBC_2.6
vsnprintf@GLIBC_2.0
wcrtomb@GLIBC_2.0
wcscmp@GLIBC_2.0
wcslen@GLIBC_2.0
wcsnrtombs@GLIBC_2.0
wctob@GLIBC_2.0
wmemchr@GLIBC_2.0
wmemcmp@GLIBC_2.0
wmemcpy@GLIBC_2.0
wmemmove@GLIBC_2.0
wmemset@GLIBC_2.0
write@GLIBC_2.0
writev@GLIBC_2.0
```

#### `libgcc_s.so.1` — 20 undefined symbols

```text
_ITM_deregisterTMCloneTable [WEAK]
_ITM_registerTMCloneTable [WEAK]
__cxa_finalize@GLIBC_2.1.3 [WEAK]
__gmon_start__ [WEAK]
__pthread_key_create [WEAK]
abort@GLIBC_2.0
calloc@GLIBC_2.0
dl_iterate_phdr@GLIBC_2.2.4
free@GLIBC_2.0
malloc@GLIBC_2.0
memcpy@GLIBC_2.0
memset@GLIBC_2.0
pthread_getspecific [WEAK]
pthread_key_create [WEAK]
pthread_mutex_lock@GLIBC_2.0 [WEAK]
pthread_mutex_unlock@GLIBC_2.0 [WEAK]
pthread_once [WEAK]
pthread_setspecific [WEAK]
realloc@GLIBC_2.0
strlen@GLIBC_2.0
```

#### `libftgl.so.2` — 115 undefined symbols

```text
FT_Attach_File
FT_Attach_Stream
FT_Done_Face
FT_Done_FreeType
FT_Get_Char_Index
FT_Get_Kerning
FT_Init_FreeType
FT_Load_Glyph
FT_New_Face
FT_New_Memory_Face
FT_Outline_Get_CBox
FT_Render_Glyph
FT_Select_Charmap
FT_Set_Char_Size
FT_Set_Charmap
_ITM_deregisterTMCloneTable [WEAK]
_ITM_registerTMCloneTable [WEAK]
_Unwind_Resume@GCC_3.0
_ZNSt11logic_errorC1EPKc@GLIBCXX_3.4.21
_ZNSt11logic_errorD1Ev@GLIBCXX_3.4
_ZNSt8ios_base4InitC1Ev@GLIBCXX_3.4
_ZNSt8ios_base4InitD1Ev@GLIBCXX_3.4
_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base@GLIBCXX_3.4
_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base@GLIBCXX_3.4
_ZSt20__throw_length_errorPKc@GLIBCXX_3.4
_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_@GLIBCXX_3.4
_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_@GLIBCXX_3.4
_ZSt9terminatev@GLIBCXX_3.4
_ZTISt11logic_error@GLIBCXX_3.4
_ZTVN10__cxxabiv117__class_type_infoE@CXXABI_1.3
_ZTVN10__cxxabiv120__si_class_type_infoE@CXXABI_1.3
_ZdaPv@GLIBCXX_3.4
_ZdlPvj@CXXABI_1.3.9
_Znaj@GLIBCXX_3.4
_Znwj@GLIBCXX_3.4
__assert_fail@GLIBC_2.0
__cxa_allocate_exception@CXXABI_1.3
__cxa_atexit@GLIBC_2.1.3
__cxa_finalize@GLIBC_2.1.3 [WEAK]
__cxa_free_exception@CXXABI_1.3
__cxa_guard_abort@CXXABI_1.3
__cxa_guard_acquire@CXXABI_1.3
__cxa_guard_release@CXXABI_1.3
__cxa_pure_virtual@CXXABI_1.3
__cxa_throw@CXXABI_1.3
__cxa_throw_bad_array_new_length@CXXABI_1.3.8
__dynamic_cast@CXXABI_1.3
__fprintf_chk@GLIBC_2.3.4
__gmon_start__ [WEAK]
__gxx_personality_v0@CXXABI_1.3
__stack_chk_fail@GLIBC_2.4
atan2@GLIBC_2.0
free@GLIBC_2.0
fwrite@GLIBC_2.0
glBegin
glBindTexture
glBitmap
glBlendFunc
glCallList
glDeleteLists
glDeleteTextures
glDisable
glDrawPixels
glEnable
glEnd
glEndList
glGenLists
glGenTextures
glGetFloatv
glGetIntegerv
glGetTexLevelParameteriv
glHint
glNewList
glNormal3d
glNormal3dv
glNormal3f
glPixelStorei
glPixelTransferf
glPolygonMode
glPopAttrib
glPopClientAttrib
glPushAttrib
glPushClientAttrib
glTexCoord2f
glTexEnvi
glTexImage2D
glTexParameteri
glTexSubImage2D
glTranslatef
glVertex2f
glVertex3f
gluDeleteTess
gluNewTess
gluTessBeginContour
gluTessBeginPolygon
gluTessCallback
gluTessEndContour
gluTessEndPolygon
gluTessNormal
gluTessProperty
gluTessVertex
iswspace@GLIBC_2.0
malloc@GLIBC_2.0
memcpy@GLIBC_2.0
memmove@GLIBC_2.0
memset@GLIBC_2.0
sqrt@GLIBC_2.0
stderr@GLIBC_2.0
strcmp@GLIBC_2.0
strdup@GLIBC_2.0
strncmp@GLIBC_2.0
strndup@GLIBC_2.0
wcscmp@GLIBC_2.0
wcsdup@GLIBC_2.0
wcsncmp@GLIBC_2.0
```

#### `libfreetype.so.6` — 63 undefined symbols

```text
_ITM_deregisterTMCloneTable [WEAK]
_ITM_registerTMCloneTable [WEAK]
__cxa_finalize@GLIBC_2.1.3 [WEAK]
__errno_location@GLIBC_2.0
__fxstat@GLIBC_2.0
__gmon_start__ [WEAK]
__longjmp_chk@GLIBC_2.11
__memcpy_chk@GLIBC_2.3.4
__sprintf_chk@GLIBC_2.3.4
__stack_chk_fail@GLIBC_2.4
_setjmp@GLIBC_2.0
close@GLIBC_2.0
fcntl@GLIBC_2.28
free@GLIBC_2.0
getenv@GLIBC_2.0
inflate
inflateEnd
inflateInit2_
inflateReset
malloc@GLIBC_2.0
memchr@GLIBC_2.0
memcmp@GLIBC_2.0
memcpy@GLIBC_2.0
memmove@GLIBC_2.0
memset@GLIBC_2.0
mmap@GLIBC_2.0
munmap@GLIBC_2.0
open@GLIBC_2.0
png_create_info_struct@PNG16_0
png_create_read_struct@PNG16_0
png_destroy_read_struct@PNG16_0
png_error@PNG16_0
png_get_IHDR@PNG16_0
png_get_error_ptr@PNG16_0
png_get_io_ptr@PNG16_0
png_get_valid@PNG16_0
png_read_end@PNG16_0
png_read_image@PNG16_0
png_read_info@PNG16_0
png_read_update_info@PNG16_0
png_set_expand_gray_1_2_4_to_8@PNG16_0
png_set_filler@PNG16_0
png_set_gray_to_rgb@PNG16_0
png_set_interlace_handling@PNG16_0
png_set_longjmp_fn@PNG16_0
png_set_packing@PNG16_0
png_set_palette_to_rgb@PNG16_0
png_set_read_fn@PNG16_0
png_set_read_user_transform_fn@PNG16_0
png_set_strip_16@PNG16_0
png_set_tRNS_to_alpha@PNG16_0
qsort@GLIBC_2.0
read@GLIBC_2.0
realloc@GLIBC_2.0
strcat@GLIBC_2.0
strcmp@GLIBC_2.0
strcpy@GLIBC_2.0
strlen@GLIBC_2.0
strncmp@GLIBC_2.0
strncpy@GLIBC_2.0
strrchr@GLIBC_2.0
strstr@GLIBC_2.0
strtol@GLIBC_2.0
```

#### `libpng16.so.16` — 44 undefined symbols

```text
_ITM_deregisterTMCloneTable [WEAK]
_ITM_registerTMCloneTable [WEAK]
__cxa_finalize@GLIBC_2.1.3 [WEAK]
__errno_location@GLIBC_2.0
__fprintf_chk@GLIBC_2.3.4
__gmon_start__ [WEAK]
__longjmp_chk@GLIBC_2.11
__memcpy_chk@GLIBC_2.3.4
__stack_chk_fail@GLIBC_2.4
_setjmp@GLIBC_2.0
abort@GLIBC_2.0
adler32
crc32
deflate
deflateEnd
deflateInit2_
deflateReset
fclose@GLIBC_2.1
ferror@GLIBC_2.0
fflush@GLIBC_2.0
fopen@GLIBC_2.1
fputc@GLIBC_2.0
fread@GLIBC_2.0
free@GLIBC_2.0
frexp@GLIBC_2.0
fwrite@GLIBC_2.0
gmtime@GLIBC_2.0
inflate
inflateEnd
inflateInit2_
inflateReset
inflateReset2@ZLIB_1.2.3.4
inflateValidate@ZLIB_1.2.9
malloc@GLIBC_2.0
memcmp@GLIBC_2.0
memcpy@GLIBC_2.0
memset@GLIBC_2.0
modf@GLIBC_2.0
pow@GLIBC_2.29
remove@GLIBC_2.0
stderr@GLIBC_2.0
strerror@GLIBC_2.0
strlen@GLIBC_2.0
strtod@GLIBC_2.0
```

#### `libz.so.1` — 21 undefined symbols

```text
_ITM_deregisterTMCloneTable [WEAK]
_ITM_registerTMCloneTable [WEAK]
__cxa_finalize@GLIBC_2.1.3 [WEAK]
__errno_location@GLIBC_2.0
__gmon_start__ [WEAK]
__snprintf_chk@GLIBC_2.3.4
__stack_chk_fail@GLIBC_2.4
__vsnprintf_chk@GLIBC_2.3.4
close@GLIBC_2.0
free@GLIBC_2.0
lseek64@GLIBC_2.1
malloc@GLIBC_2.0
memchr@GLIBC_2.0
memcpy@GLIBC_2.0
memset@GLIBC_2.0
open@GLIBC_2.0
read@GLIBC_2.0
snprintf@GLIBC_2.0
strerror@GLIBC_2.0
strlen@GLIBC_2.0
write@GLIBC_2.0
```

#### `libmad.so.0` — 20 undefined symbols

```text
_ITM_deregisterTMCloneTable [WEAK]
_ITM_registerTMCloneTable [WEAK]
__cxa_finalize@GLIBC_2.1.3 [WEAK]
__errno_location@GLIBC_2.0
__gmon_start__ [WEAK]
__sprintf_chk@GLIBC_2.3.4
__stack_chk_fail@GLIBC_2.4
_exit@GLIBC_2.0
calloc@GLIBC_2.0
close@GLIBC_2.0
fcntl@GLIBC_2.0
fork@GLIBC_2.0
free@GLIBC_2.0
malloc@GLIBC_2.0
memcpy@GLIBC_2.0
memmove@GLIBC_2.0
pipe@GLIBC_2.0
read@GLIBC_2.0
waitpid@GLIBC_2.0
write@GLIBC_2.0
```

## Struct-return and long double ABI

### piu and phoenixtools directly

`i686-linux-gnu-readelf --dyn-syms --wide piu` (440 undefined entries) and the same command on `phoenixtools/bin/phoenixtools.so` (51 entries, 48 versioned libc functions + 3 weak instrumentation entries) contain **no `div`, `ldiv`, `lldiv`, `imaxdiv`, `strtold`, `wcstold`, `frexpl`, or other native function returning a struct or taking/returning long double**. Native imports that work with structures return scalar status or pointers (`localtime`, `getifaddrs`, `sysinfo`, stat APIs); `inet_ntoa` takes a 4-byte `struct in_addr` by value, which is passed as one ordinary stack word and requires no struct-return thunk. Guest C++/FTGL/FFmpeg aggregate APIs stay inside guest ELF code and are not MSVC-native ABI boundaries.

Both complete `objdump -d` outputs contain **zero `fldt` / `fstpt` instructions**. Scanning allocated, non-executable `SHT_PROGBITS` sections for printf conversions `%...L[aAeEfFgG]` finds **none** in either image. (Scanning the entire binary without section filtering produces false strings in the dynamic-symbol bytes.) Phoenixtools source format call sites were searched under `src/`; no long-double declaration or `%Lf` argument path exists. This is static evidence for no direct long-double native crossing, not a claim that externally supplied runtime printf formats can never contain an L modifier.

### The complete guest set still requires x87 support

**Do not remove long-double support from lxrt merely because piu/phoenixtools do not call it directly.** The relinked libstdc++ imports:

- `frexpl@GLIBC_2.0`: calls at ELF offsets `0x0009c249` and `0x000b81a9`. Three 32-bit words are pushed for the guest's 12-byte argument; result is consumed from x87 ST(0).
- `strtold_l@GLIBC_2.3`: call `0x000aef58`; returned ST(0) stored with `fstpt` at `0x000aef66` into the guest result.
- `vsnprintf@GLIBC_2.0`: call `0x000dbbc3` in formatting helper `0x000dbb90`. Its callers at `0x000e0907`, `0x000ebc91`, `0x0010e867`, `0x00139401` pass the `.rodata` string `%.*Lf` at `0x0017cd40` (GOT base `0x001db000`, PIC displacement `-0x5e2c0`) and push three words of long-double data before precision and format.

These claims are reproducible with `i686-linux-gnu-objdump -d lib/libstdc++.so.6` and `i686-linux-gnu-objdump -s --start-address=0x17cd40 --stop-address=0x17cd48 lib/libstdc++.so.6`. An MSVC `long double` (8-byte double) forwarder is ABI-incorrect. Use guest-aware x87 thunks / conversion and a printf argument walker that consumes all 12 bytes before the next variadic argument. Linux i386 aggregate-return thunks (hidden pointer, callee `ret $4`) are not demanded by the audited **native** imports; pointer-output APIs still require exact Linux structure packing as in PLAN §4.

