#!/usr/bin/env bash
# Run every lxrt regression against out/win/lxrt.dll (build it first: lxrt/build.sh).
#   C:/msys64/usr/bin/bash.exe tests/run-all.sh
# Suites with run.sh compare byte-for-byte with qemu-i386 + glibc 2.31; the remaining programs are
# self-checking (exit status 0 = pass).
set -uo pipefail
cd "$(dirname "$0")/.."
fail=0
check() { local name="$1"; shift; if "$@" > "out/test/$name.out" 2>&1; then echo "PASS $name"; else echo "FAIL $name (out/test/$name.out)"; fail=1; fi; }
mkdir -p out/test
lxtest() { C:/msys64/usr/bin/bash.exe tools/lxtest.sh "$@"; }
check hello lxtest tests/hello.c -- -fstack-protector-all :: 0
check binder lxtest tests/binder/libtlsm.so.c tests/binder/fopenhook.preload.so.c tests/binder/binder.c
check cxx lxtest tests/cxx/libthrow.so.cc tests/cxx/cxx.cc
check drivepath lxtest tests/path/drivepath.c
for t in tests/net/*.c tests/misc/*.c; do
  check "$(basename "${t%.c}")" lxtest "$t"
done
for s in stdio fs thread locale; do
  check "suite-$s" C:/msys64/usr/bin/bash.exe "tests/$s/run.sh"
done
exit $fail
