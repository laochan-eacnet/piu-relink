#!/usr/bin/env bash
# Compile an i386 Linux test program, relink it to PE and run it under lxrt.
#   C:/msys64/usr/bin/bash.exe tools/lxtest.sh [-o outdir] [-n] tests/foo.c [-- extra compiler args] [:: program args]
# -o outdir   where lxrt.dll lives and test binaries go (default out/test; must contain lxrt.dll,
#             e.g. built with `lxrt/build.sh <outdir>`)
# -n          build only, don't run
# Shared objects: a source named *.so.c is built as lib<name>.so (soname = file name) and relinked
# as a DLL; list them before the main program, e.g. tests/libtls.so.c tests/tls.c.
# Output of the program goes to stdout; exit status is the program's.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
out=out/test
run=1
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2 ;;
    -n) run=0; shift ;;
    *) break ;;
  esac
done
mkdir -p "$out"
srcs=(); cflags=(); args=(); mode=src
for a in "$@"; do
  case "$mode:$a" in
    *:--) mode=cflags ;;
    *:::) mode=args ;;
    src:*) srcs+=("$a") ;;
    cflags:*) cflags+=("$a") ;;
    args:*) args+=("$a") ;;
  esac
done
libs=(); preloads=(); main=""; base=0x40000000
for s in "${srcs[@]}"; do
  name="$(basename "$s")"
  case "$s" in
    *.so.c|*.so.cc)
      so="${name%.c}"; so="${so%.cc}"
      cc=("${LNX_CC[@]}"); [[ "$s" == *.cc ]] && cc=("${LNX_CXX[@]}")
      "${cc[@]}" -O2 -g -fPIC -shared -fstack-protector-strong -Wl,-soname,"$so" "${cflags[@]}" "$s" -o "$out/$so" \
        -L"$out" "${libs[@]/#/-l:}" -ldl
      if [[ "$so" == *.preload.so ]]; then
        # LD_PRELOAD-style: not linked into the program, only interposed at bind time
        python relink/relink.py "$out/$so" -o "$out/$so.dll" --base "$base" --preload-flag
        preloads+=(--preload "$so")
      else
        python relink/relink.py "$out/$so" -o "$out/$so.dll" --base "$base"
        libs+=("$so")
      fi
      base=$(printf '0x%x' $((base + 0x01000000))) ;;
    *)
      main="$s" ;;
  esac
done
exe="${main##*/}"; exe="${exe%.*}"
cc=("${LNX_CC[@]}"); [[ "$main" == *.cc || "$main" == *.cpp ]] && cc=("${LNX_CXX[@]}")
"${cc[@]}" -O2 -g -no-pie -Wl,--image-base=0x8048000 -fstack-protector-strong "${cflags[@]}" "$main" \
  -o "$out/$exe" -L"$out" "${libs[@]/#/-l:}" -lpthread -ldl -lm
python relink/relink.py "$out/$exe" -o "$out/$exe.exe" "${preloads[@]}"
# relinked guest runtime libraries (C++ tests)
for g in libstdc++.so.6.dll libgcc_s.so.1.dll; do
  if [ -f "out/win/$g" ] && [ "$out" != out/win ]; then cp "out/win/$g" "$out/"; fi
done
if [ "$out" = out/test ]; then cp out/win/lxrt.dll "$out/"; fi
[ -f "$out/lxrt.dll" ] || { echo "lxtest: $out/lxrt.dll missing (run lxrt/build.sh $out)" >&2; exit 2; }
if [ "$run" = 1 ]; then
  "$(cygpath -m "$PWD/$out/$exe.exe" 2>/dev/null || echo "$PWD/$out/$exe.exe")" "${args[@]}"
fi
