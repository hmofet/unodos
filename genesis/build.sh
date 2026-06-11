#!/bin/sh
# UnoDOS/Genesis build. Same vasm as the Amiga port (it's all 68000).
# Usage: ./build.sh   -> build/unodos.gen
set -e
cd "$(dirname "$0")"

VASM="${VASM:-/c/Users/arin/amiga-tools/vasmm68k_mot.exe}"
PY="${PY:-python3}"

mkdir -p build
echo "[1/2] generating font tiles from the shared x86 font..."
(cd .. && "$PY" genesis/mkfont.py)

echo "[2/2] assembling boot.asm (cpu 68000, flat binary)..."
"$VASM" -Fbin -m68000 -nosym -o build/unodos.gen boot.asm
echo "done: build/unodos.gen"
