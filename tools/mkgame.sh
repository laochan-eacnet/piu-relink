#!/usr/bin/env bash
# Relink piu + guest libraries into a runnable Windows directory.
#   C:/msys64/usr/bin/bash.exe tools/mkgame.sh [outdir]      (default out/game)
# Environment:
#   SUBSYSTEM=console|gui   PE subsystem of piu.exe (default console; gui = no console window,
#                           stdout/stderr go to piu.log beside the EXE)
#   DATA=<path>             Windows path of the game data tree for /piu/data, absolute or relative
#                           to the EXE directory (default: this repo's data/)
#   PRESERVE_CONFIG=1      merge newly introduced defaults without replacing existing settings
# Inputs: piu, lib/ (game libs), phoenixtools/bin/, out/linux/ffmpeg/ (rebuilt FFmpeg),
#         out/win/{lxrt,lxgfx,lxsnd,lxusb}.dll (native runtime + bridges).
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
out="${1:-out/game}"
case "${SUBSYSTEM:-console}" in console) subsystem=3 ;; gui) subsystem=2 ;; *) echo "mkgame: SUBSYSTEM must be console or gui" >&2; exit 1 ;; esac
mkdir -p "$out/runtime"
rl() { python relink/relink.py "$@"; }

# phoenixtools patch sites (fixed vaddrs in piu): the canary rewrite must never touch them.
protect=()
for a in $(grep -ohE '0x0[0-9A-Fa-f]{7}u' phoenixtools/src/phoenix/patch.c phoenixtools/src/phoenix/addresses.h | sort -u); do
  protect+=(--protect "${a%u}:16")
done

# Guest DLLs: soname, source file, preferred ImageBase (non-overlapping; .reloc handles clashes).
guest=(
  "phoenixtools.so      phoenixtools/bin/phoenixtools.so      0x10000000 preload"
  "libcurl.so.4         phoenixtools/bin/lib/network/libcurl.so.4 0x10400000"
  "libmad.so.0          lib/libmad.so.0                       0x10500000"
  "libz.so.1            lib/libz.so.1                         0x10600000"
  "libpng16.so.16       lib/libpng16.so.16                    0x10700000"
  "libfreetype.so.6     lib/libfreetype.so.6                  0x10800000"
  "libftgl.so.2         lib/libftgl.so.2                      0x10A00000"
  "libgcc_s.so.1        lib/libgcc_s.so.1                     0x10B00000"
  "libstdc++.so.6       lib/libstdc++.so.6                    0x10C00000"
  "libavutil.so.56      out/linux/ffmpeg/libavutil.so.56      0x11000000"
  "libswresample.so.3   out/linux/ffmpeg/libswresample.so.3   0x11400000"
  "libswscale.so.5      out/linux/ffmpeg/libswscale.so.5      0x11600000"
  "libavcodec.so.58     out/linux/ffmpeg/libavcodec.so.58     0x12000000"
  "libavformat.so.58    out/linux/ffmpeg/libavformat.so.58    0x13000000"
)
for g in "${guest[@]}"; do
  read -r so src base flag <<<"$g"
  if [ ! -f "$src" ]; then echo "mkgame: missing $src" >&2; exit 1; fi
  extra=(); [ "${flag:-}" = preload ] && extra=(--preload-flag)
  rl "$src" -o "$out/$so.dll" --name "$so" --base "$base" "${extra[@]}"
done
rl piu -o "$out/piu.exe" --exe --preload phoenixtools.so --subsystem "$subsystem" "${protect[@]}" -v

for n in lxrt lxgfx lxsnd lxusb; do cp "out/win/$n.dll" "$out/"; done

write_config() {
  if [ "${PRESERVE_CONFIG:-0}" = 1 ]; then
    python tools/package.py --merge-config "$out/$1"
  else
    cat > "$out/$1"
  fi
}

# Unified configuration: guest hooks plus native runtime, beside piu.exe.
data_rel="${DATA:-$(python -c "import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]).replace(os.sep, '/'))" "$PWD/data" "$PWD/$out")}"
{
  sed -e 's#^plain_root = .*#plain_root = /piu/data#' phoenixtools/phoenixtools.ini
  cat <<EOF

# lxrt guest filesystem mounts: <guest path> = <Windows path, relative to this directory or absolute>
[mounts]
/piu/data = $data_rel
# Read-only game data: stat results and directory listings are cached for the process lifetime.
[immutable]
/piu/data = yes
# Window: mode = windowed | borderless | fullscreen. width/height: windowed = client size,
# fullscreen = display mode to switch to; 0 = automatic (game size / desktop size).
# The game always renders at its own size (1280x720); other sizes are scaled, aspect kept.
[display]
mode = windowed
width = 0
height = 0
# WASAPI: mode = shared | exclusive (exclusive falls back to shared if the device refuses).
[audio]
mode = shared

# Map guest serial devices to Windows COM ports; leave unneeded devices unmapped.
# The cabinet light board runs at 9600 baud; RFID readers run at 115200 baud.
# RFID readers require [input] virtual_card_reader = false.
[serial]
# Cabinet light board (9600 baud).
# /dev/ttyUSB0 = COM3
# P1 RFID reader (115200 baud).
# /dev/ttyACM0 = COM4
# P2 RFID reader (115200 baud).
# /dev/ttyACM1 = COM5

# Developer diagnostics are disabled by default; relative paths start beside piu.exe.
[debug]
# Verbose lxrt log: symbol binding, init order, every open() and failed path lookup.
trace = false
# Log each distinct failed file lookup (stat/open/opendir) once; cheap enough during play.
trace_missing = false
# Append runtime diagnostics to this file; empty disables file logging.
log =
# Guard guest heap blocks and log overflows with allocation/free callers.
heapcheck = false
# Save rendered frames in this directory; empty disables snapshots.
snapshot_dir =
# Capture every Nth rendered frame when snapshots are enabled.
snapshot_every = 60
# Render without presenting frames to the window.
no_present = false
# Capture audio to this WAV file; empty disables capture.
audio_capture =
# Log audio playback statistics.
audio_stats = false
EOF
} | write_config phoenixtools.ini
# A reset also removes the obsolete runtime config; preservation migrates it above.
rm -f "$out/lxrt.ini"
echo "game directory: $out"
