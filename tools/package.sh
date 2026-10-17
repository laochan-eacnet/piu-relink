#!/usr/bin/env bash
# Publish an Explorer-launchable game directory plus the standard-library LAN server:
#   <dest>/piu.exe (GUI subsystem), guest DLLs/hooks/CURL, lxrt/lxgfx/lxsnd, merged phoenixtools.ini,
#   server/*.py, start-server.cmd, empty card1.txt/card2.txt only when not already present.
# Existing settings (including legacy runtime INI migration), runtime/, databases, card inputs and data junction are retained.
#   C:/msys64/usr/bin/bash.exe tools/package.sh [dest] [data-source]
# Defaults: dest = out/dist/piu, data-source = <repo>/data.
set -euo pipefail
cd "$(dirname "$0")/.."
dest="${1:-out/dist/piu}"
data_status="existing data retained"
SUBSYSTEM=gui DATA=data PRESERVE_CONFIG=1 C:/msys64/usr/bin/bash.exe tools/mkgame.sh "$dest"
python tools/package.py "$dest"
# lexists also preserves a junction whose target is temporarily unavailable.
if ! python -c "import os, sys; sys.exit(not os.path.lexists(sys.argv[1]))" "$dest/data"; then
  src="$(realpath "${2:-data}")"
  # A junction needs no privilege and keeps the 35 GB tree in place.
  python -c "import _winapi, sys; _winapi.CreateJunction(sys.argv[1], sys.argv[2])" \
    "$(cygpath -w "$src")" "$(cygpath -w "$(realpath "$dest")")\\data"
  data_status="data -> $src"
fi
echo "package: $dest ($data_status; server: start-server.cmd)"
