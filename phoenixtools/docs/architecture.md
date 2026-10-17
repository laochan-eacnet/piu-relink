# Architecture

`phoenixtools.so` is one i386 shared object. The launcher only loads it; policy
selection occurs after the hook reads `phoenixtools.ini`.

## Module ownership

- `core/`: logging and shared real-function pointers.
- `config/`: defaults, INI parsing, value validation, and relative path
  resolution.
- `phoenix/`: supported fixed addresses, byte-validated patch primitives, and
  the constructor patch manifest.
- `resources/`: case-insensitive path traversal, song media rules, grouped VFS
  selection, Phoenix VFS ABI hooks, and test exports.
- `media/`: command/media gate hooks that bridge Phoenix to resource and input
  modules.
- `gameplay/`: offline catalog filtering limited to locally backed records.
- `input/`: configured X11 binding resolution, Phoenix keyboard fallback, and
  synthetic LXIO report ownership.
- `compat/`: libusb ABI definitions, virtual `1010/1020` implementations,
  hardware forwarding, and optional legacy dependency fallbacks.
- `safety/`: filesystem sandboxing, raw-disk policy, offline endpoint policy,
  non-secret lock response, raw-port denial, and frame pacing.

## Initialization order

1. Resolve real libc/libcurl functions used by interposers.
2. Load defaults and `phoenixtools.ini`.
3. Verify that the supported Phoenix executable mapping is present.
4. Initialize input report state.
5. Validate and apply the complete fixed-address patch set.
6. Mark the hook ready only after every required patch succeeds.

Loading the library into a unit-test process performs steps 1 and 2, then exits
early because the Phoenix executable mapping is absent.

## Resource contract

The resolver normalizes guest separators, rejects traversal and control
characters, and performs case-insensitive component lookup beneath configured
roots. Overlay lookup precedes plaintext lookup. Resolved files must be regular
files; resolved group roots must be directories.

Song media paths preserve these distinctions:

- full video: `D/<ID>/V/.V2`;
- preview video: `D/<ID>/P/V/.V2`;
- full audio: `D/<ID>/A/.A`;
- preview audio: `D/<ID>/P/A/.A`;
- preview graphics: `D/<ID>/P/.G`.

Encoded aliases may fall back to their base ID. Missing data is never fabricated.

## USB contract

`pt_virtual_usb_mode()` is the single mode selector.

In virtual mode, every exported libusb API operates only on static synthetic
objects owned by `compat/` and calls the LXIO bridge for transfers. In hardware
mode, every exported operation resolves the same symbol from `RTLD_NEXT` and
forwards the original arguments. A missing real symbol returns the relevant
libusb failure value rather than silently enabling emulation.

The legacy `1010` input response is intentionally neutral. Mapped synthetic
input is emitted only through `1020`, preventing duplicate panel events through
two Phoenix decoder layouts.

## Source policy

Project headers use `#pragma once`. C/Python source should remain below 200
lines where practical and may never exceed 350 lines. `tools/source_audit.py`
enforces the hard limit, header policy, and build-output layout.
