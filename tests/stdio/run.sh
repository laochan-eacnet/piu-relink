#!/usr/bin/env bash
# Native glibc-HLE conformance against the same executable under glibc 2.31.
# Run from Windows: C:/msys64/usr/bin/bash.exe tests/stdio/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/env.sh
out=out/t-stdio
if [ "${1:-}" = --no-build ]; then
    shift
else
    C:/msys64/usr/bin/bash.exe lxrt/build.sh "$out"
fi
if [ "$#" = 0 ]; then set -- printf scanf file flush; fi
root="$(cygpath -m "$PWD")"
linuxroot="$(wsl.exe -e wslpath -a "$root" | tr -d '\r')"
for name; do
    # Audited callers use the opaque symbol ABI, not glibc's FILE-field inlines.
    C:/msys64/usr/bin/bash.exe tools/lxtest.sh -o "$out" -n "tests/stdio/$name.c" -- -fno-builtin -D__NO_INLINE__
    "$root/$out/$name.exe" > "$out/$name.native.txt" 2>&1
    wsl.exe -e bash -lc "cd '$linuxroot' && qemu-i386 -L sysroot -E LD_LIBRARY_PATH=sysroot/lib/i386-linux-gnu '$out/$name' 2>&1" > "$out/$name.glibc.txt" 2>&1
    diff -u "$out/$name.glibc.txt" "$out/$name.native.txt"
    printf '%s: identical\n' "$name"
done
for mode in overflow-sprintf overflow-snprintf; do
    status=0
    "$root/$out/printf.exe" "$mode" > "$out/$mode.native.txt" 2>&1 || status=$?
    [ "$status" = 134 ] || { printf '%s: native status %s, expected 134\n' "$mode" "$status"; exit 1; }
    status=0
    wsl.exe -e bash -lc "ulimit -c 0; cd '$linuxroot' && qemu-i386 -L sysroot -E LD_LIBRARY_PATH=sysroot/lib/i386-linux-gnu '$out/printf' '$mode' 2>&1" > "$out/$mode.glibc.txt" 2>&1 || status=$?
    [ "$status" = 134 ] || { printf '%s: glibc status %s, expected 134\n' "$mode" "$status"; exit 1; }
    printf '%s: both terminate with status 134\n' "$mode"
done
