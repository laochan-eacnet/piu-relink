#!/usr/bin/env bash
# Run from Windows with C:/msys64/usr/bin/bash.exe tests/fs/run.sh [outdir].
# Includes glibc 2.31 header probes and WSL's newer i686 glibc/stat@2.33 ABI.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/env.sh
out="${1:-out/t-fs}"
C:/msys64/usr/bin/bash.exe lxrt/build.sh "$out"
linux_root="$(wsl -e wslpath -u "$(cygpath -w "$PWD")" | tr -d '\r')"
for test in layouts files mappings patterns stat233 symlinks; do
    C:/msys64/usr/bin/bash.exe tools/lxtest.sh -n -o "$out" "tests/fs/$test.c" -- -Wno-deprecated-declarations
    "$(cygpath -m "$PWD/$out/$test.exe")" > "$out/$test.windows.txt"
    wsl -e bash -lc "cd '$linux_root' && LD_LIBRARY_PATH=sysroot/lib/i386-linux-gnu qemu-i386 -L sysroot '$out/$test'" > "$out/$test.linux.txt"
    if [ "$test" = symlinks ] && [ "$(tr -d '\r\n' < "$out/$test.windows.txt")" = 'symlinks unavailable EPERM' ]; then
        echo 'symlinks: Windows privilege unavailable (required EPERM policy passed)'
    else
        diff -u "$out/$test.linux.txt" "$out/$test.windows.txt"
        echo "$test: Windows output matches qemu-i386"
    fi
done
for test in layouts stat233; do
    wsl -e bash -lc "cd '$linux_root' && i686-linux-gnu-gcc -O2 -fno-pie -no-pie -fno-stack-protector tests/fs/$test.c -o '$out/$test-modern' && qemu-i386 -L /usr/i686-linux-gnu '$out/$test-modern'" > "$out/$test-modern.linux.txt"
    python relink/relink.py "$out/$test-modern" -o "$out/$test-modern.exe"
    "$(cygpath -m "$PWD/$out/$test-modern.exe")" > "$out/$test-modern.windows.txt"
    diff -u "$out/$test-modern.linux.txt" "$out/$test-modern.windows.txt"
    echo "$test-modern: Windows output matches modern i686 glibc"
done
