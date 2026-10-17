#!/usr/bin/env bash
set -euo pipefail
# From the repository root: C:/msys64/usr/bin/bash.exe bridges/snd/build.sh
. tools/env.sh
mkdir -p out/win
"${WIN_CC[@]}" -std=c11 -shared bridges/snd/snd.c -o out/win/lxsnd.dll \
  "${WIN_LDFLAGS[@]}" -lole32 -lavrt
"${WIN_CC[@]}" -std=c11 bridges/snd/test_snd.c -o out/win/test_snd.exe \
  "${WIN_LDFLAGS[@]}"
