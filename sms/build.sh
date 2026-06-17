#!/bin/sh
# UnoDOS / Sega Master System build (Z80, sjasmplus).
# Usage: ./build.sh           -> build/unodos.sms
set -e
cd "$(dirname "$0")"

SJASM="${SJASM:-/c/Users/arin/z80-tools/sjasmplus-1.23.1.win/sjasmplus.exe}"
PY="${PY:-python}"

mkdir -p build
echo "[1/2] generating tiles + palette from the shared assets..."
(cd .. && "$PY" sms/mkdata.py)

echo "[2/2] assembling kernel.asm (Z80, raw 32KB ROM)..."
"$SJASM" --raw=build/unodos.sms kernel.asm
echo "done: sms/build/unodos.sms ($(wc -c < build/unodos.sms) bytes)"
