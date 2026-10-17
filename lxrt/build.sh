#!/usr/bin/env bash
# Build lxrt.dll. Usage (repo root): C:/msys64/usr/bin/bash.exe lxrt/build.sh [outdir]
# outdir defaults to out/win (or $LXRT_OUT); objects go to <outdir>/obj-lxrt.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
out="${1:-${LXRT_OUT:-out/win}}"
obj="$out/obj-lxrt"
mkdir -p "$out" "$obj"
# Every source declaring LX_SYMTAB(group, ...) contributes a symbol group.
grep -ho '^LX_SYMTAB([a-z0-9_]*' lxrt/*.c | sed 's/LX_SYMTAB(/X(/; s/$/)/' | sort -u > "$obj/symtab_list.h"
objs=()
for src in lxrt/*.c lxrt/*.S; do
  [ -e "$src" ] || continue
  o="$obj/$(basename "${src%.*}").o"
  "${WIN_CC[@]}" -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion \
    -Ilxrt -I"$obj" -c "$src" -o "$o"
  objs+=("$o")
done
"${WIN_CC[@]}" -shared -o "$out/lxrt.dll" "${objs[@]}" "${WIN_LDFLAGS[@]}" \
  -Wl,/implib:"$obj/lxrt.lib" \
  -lkernel32 -luser32 -lshell32 -ladvapi32 -lws2_32 -liphlpapi -lsynchronization -lbcrypt -lmincore
echo "built $out/lxrt.dll"
