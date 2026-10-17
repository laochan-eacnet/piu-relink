#!/usr/bin/env bash
# Run from MSYS2: C:/msys64/usr/bin/bash.exe tests/thread/run.sh [outdir]
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${1:-out/t-HleThread}"
"$BASH" lxrt/build.sh "$out"
"$BASH" tools/lxtest.sh -o "$out" -n tests/thread/thread.c
"$BASH" tools/lxtest.sh -o "$out" -n tests/thread/time.c
"$BASH" tools/lxtest.sh -o "$out" -n tests/thread/threadlocal.so.cc tests/thread/tls.cc
root_win="$(cygpath -m "$PWD")"
root_wsl="$(wsl.exe wslpath -u "$root_win" | tr -d '\r')"
for program in thread time tls; do
  wsl.exe -e sh -c "cd '$root_wsl' && LD_LIBRARY_PATH='$out:sysroot/lib/i386-linux-gnu' qemu-i386 -L sysroot '$out/$program'" > "$out/$program.qemu.txt"
done
# Acceptance loop: every native program must pass and match the glibc reference
# twenty consecutive times, including the 10 ms sleep's +/-2 ms assertion.
for run in $(seq 1 20); do
  for program in thread time tls; do
    "$root_win/$out/$program.exe" > "$out/$program.native.txt"
    diff --strip-trailing-cr -u "$out/$program.qemu.txt" "$out/$program.native.txt"
  done
done
printf 'thread time tls: 20 consecutive native runs match qemu-i386\n'
