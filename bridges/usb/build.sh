#!/usr/bin/env bash
set -euo pipefail
# From the repository root: C:/msys64/usr/bin/bash.exe bridges/usb/build.sh
. tools/env.sh
mkdir -p out/win
"${WIN_CC[@]}" -std=c11 -Wall -Wextra -Werror -shared bridges/usb/usb.c bridges/usb/winusb.c \
  -o out/win/lxusb.dll "${WIN_LDFLAGS[@]}" -lwinusb -lsetupapi -lcfgmgr32 -ladvapi32
"${WIN_CC[@]}" -std=c11 -Wall -Wextra -Werror bridges/usb/usb.c bridges/usb/test_usb.c \
  -o out/win/test_usb.exe "${WIN_LDFLAGS[@]}"
