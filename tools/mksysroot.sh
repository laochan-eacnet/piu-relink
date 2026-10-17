#!/usr/bin/env bash
# Build the i386 Linux test sysroot (Ubuntu 20.04 "focal", glibc 2.31, GCC 10 libstdc++)
# from .deb packages. Run inside WSL: tools/mksysroot.sh
# Output: <repo>/sysroot (symlinks dereferenced so Windows clang/lld can read it).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/sysroot"
work="${TMPDIR:-/tmp}/piurelink-sysroot"
mirror="http://archive.ubuntu.com/ubuntu"
pkgs=(libc6 libc6-dev linux-libc-dev libgcc-10-dev libstdc++-10-dev libstdc++6 libgcc-s1 libcrypt1 libcrypt-dev)

rm -rf "$work" && mkdir -p "$work/debs" "$work/root"
cd "$work"
for suite in focal focal-updates; do
  for comp in main universe; do
    curl -fsSL "$mirror/dists/$suite/$comp/binary-i386/Packages.gz" | gunzip >> Packages
  done
done
python3 - "${pkgs[@]}" > urls <<'EOF'
import sys
want = set(sys.argv[1:])
best = {}
def vkey(v):
    import re
    return [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', v)]
for para in open('Packages').read().split('\n\n'):
    f = dict(l.split(': ', 1) for l in para.splitlines() if ': ' in l and not l.startswith(' '))
    n = f.get('Package')
    if n in want and (n not in best or vkey(f['Version']) > vkey(best[n][0])):
        best[n] = (f['Version'], f['Filename'])
missing = want - best.keys()
if missing:
    sys.exit(f"missing packages: {missing}")
for n, (v, fn) in sorted(best.items()):
    print(fn)
EOF
while read -r fn; do
  curl -fsSL -o "debs/$(basename "$fn")" "$mirror/$fn"
done < urls
for d in debs/*.deb; do
  dpkg-deb -x "$d" root
done
# Absolute .deb symlinks (usr/lib/i386-linux-gnu/libm.so -> /lib/i386-linux-gnu/libm.so.6) must
# resolve inside the sysroot, not against the host, before `cp -L` dereferences them.
find root -type l | while read -r l; do
  t="$(readlink "$l")"
  case "$t" in /*) ln -sfn "$PWD/root$t" "$l" ;; esac
done
find root -xtype l -delete
# netfilter headers differ only in case (xt_TCPMSS.h vs xt_tcpmss.h); NTFS cannot hold both.
rm -rf root/usr/include/linux/netfilter*
rm -rf "$out" && mkdir -p "$out"
cp -rL root/usr root/lib "$out"/
ls debs > "$out/PACKAGES"
echo "sysroot: $out"
