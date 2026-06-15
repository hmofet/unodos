#!/bin/sh
# UnoDOS/C64 build: assemble the 6510 kernel and pack it into a .prg + .d64.
# Requires dasm (6502/6510 cross-assembler) and python3 (py65 for harness.py).
# Usage: ./build.sh [test]   ("test" auto-opens SysInfo + Clock at boot)
set -e
cd "$(dirname "$0")"

PY="${PY:-python3}"
DASM="${DASM:-/c/Users/arin/apple2-tools/dasm.exe}"

mkdir -p build
echo "[1/4] generating VIC bitmap tables + shared 8x8 font..."
"$PY" mktables.py
"$PY" mkfont.py

case "$1" in
  test)
    DEF="-DAUTOTEST=1"; PRG=build/unodos_c64_test.prg; D64=build/unodos_c64_test.d64 ;;
  *)
    DEF=""; PRG=build/unodos_c64.prg; D64=build/unodos_c64.d64 ;;
esac

echo "[2/4] assembling kernel.s..."
"$DASM" kernel.s -f3 $DEF -obuild/kernel.bin -lbuild/kernel.lst

echo "[3/4] packing .prg + .d64..."
"$PY" mkprg.py build/kernel.bin "$PRG" "$D64"

echo "[4/4] done: $PRG (+ .d64)"
