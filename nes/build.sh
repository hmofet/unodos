#!/bin/sh
# UnoDOS / NES build (6502, dasm) -> iNES NROM-256 (.nes).
# Usage: ./build.sh            -> build/unodos.nes
set -e
cd "$(dirname "$0")"

PY="${PY:-python}"
DASM="${DASM:-/c/Users/arin/apple2-tools/dasm.exe}"

mkdir -p build
echo "[1/3] generating CHR (font + icons) + palette from the shared assets..."
(cd .. && "$PY" nes/mkdata.py)

echo "[2/3] assembling kernel.s (6502, 32KB PRG)..."
"$DASM" kernel.s -f3 -obuild/prg.bin -lbuild/kernel.lst -sbuild/kernel.sym

echo "[3/3] packing iNES ROM..."
"$PY" mkines.py build/prg.bin build/chr.bin build/unodos.nes
echo "done: nes/build/unodos.nes"
