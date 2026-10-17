#!/usr/bin/env bash
# Run from the repository root using C:/msys64/usr/bin/bash.exe.
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${1:-out/t-locale}"
if [ "${LXRT_SKIP_BUILD:-0}" != 1 ]; then
  C:/msys64/usr/bin/bash.exe lxrt/build.sh "$out"
fi
root="$(cygpath -m "$PWD")"
wslroot="$(wsl -e wslpath -u "$root" | tr -d '\r')"
failed=0
for name in ${LXRT_TESTS:-ctype locale wchar iconv math x87}; do
  C:/msys64/usr/bin/bash.exe tools/lxtest.sh -o "$out" -n "tests/locale/$name.c" -- -fno-builtin
  "$root/$out/$name.exe" > "$out/$name.native.txt" || failed=1
  # math's reference run exits 1 under qemu-i386 (its remquo quotient bug, see below).
  wsl -e bash -lc "cd '$wslroot' && GCONV_PATH=sysroot/usr/lib/i386-linux-gnu/gconv LD_LIBRARY_PATH=sysroot/lib/i386-linux-gnu:sysroot/usr/lib/i386-linux-gnu qemu-i386 -L sysroot '$out/$name'" > "$out/$name.glibc.txt" || [ "$name" = math ] || failed=1
  if [ "$name" = math ]; then
    # Self-checking: the native run must report 0 failures. Its output is also compared with
    # glibc except remquo quotients and the failure tally, where qemu-i386's x87 emulation
    # returns quotient 0 for remquo(7, 2) (correct low quotient bits: 4).
    grep -q '^math failures=0$' "$out/$name.native.txt" || failed=1
    grep -v '^remquo\|^math failures=' "$out/$name.glibc.txt" > "$out/$name.glibc.cmp"
    grep -v '^remquo\|^math failures=' "$out/$name.native.txt" > "$out/$name.native.cmp"
    if cmp -s "$out/$name.glibc.cmp" "$out/$name.native.cmp" &&
       grep -q '^math failures=0$' "$out/$name.native.txt"; then
      printf '%s: identical (self-check passed)\n' "$name"
    else
      diff -u "$out/$name.glibc.txt" "$out/$name.native.txt" || true
      failed=1
    fi
    continue
  fi
  if cmp -s "$out/$name.native.txt" "$out/$name.glibc.txt"; then
    printf '%s: identical\n' "$name"
  else
    diff -u "$out/$name.glibc.txt" "$out/$name.native.txt" || true
    failed=1
  fi
done
exit "$failed"
