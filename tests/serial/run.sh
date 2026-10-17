#!/usr/bin/env bash
# COM-port backend test against a linked pair of Windows serial ports (null modem / virtual pair).
#   C:/msys64/usr/bin/bash.exe tests/serial/run.sh COM1 COM2
# Not part of tests/run-all.sh: it needs two ports that are connected to each other.
set -euo pipefail
cd "$(dirname "$0")/../.."
[ $# -eq 2 ] || { echo "usage: $0 <port A> <port B>  (two linked COM ports)" >&2; exit 2; }
out=out/t-serial
mkdir -p "$out"
cp out/win/lxrt.dll "$out/"
cat > "$out/phoenixtools.ini" <<EOF
[serial]
/dev/ttyUSB0 = $1
/dev/ttyACM0 = $2
/dev/ttyACM9 = COM250
EOF
C:/msys64/usr/bin/bash.exe tools/lxtest.sh -o "$out" tests/serial/serial.c
