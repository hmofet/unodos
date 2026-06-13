#!/bin/sh
# UnoDOS/AppleII build: boot0+RWTS + kernel -> bootable 140K .dsk image.
# Requires dasm (6502 cross-assembler) and python3 (py65 for harness.py).
# Usage: ./build.sh [test]    ("test" auto-launches SysInfo + Clock at boot)
set -e
cd "$(dirname "$0")"

PY="${PY:-python3}"
DASM="${DASM:-/c/Users/arin/apple2-tools/dasm.exe}"

mkdir -p build
echo "[1/4] generating shared data (font/icons) from x86 tree assets..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i)

echo "[2/4] converting the shared font to the hi-res 7px convention..."
"$PY" mkfont.py

case "$1" in
  test)
    DEF="-DAUTOTEST=1"; DSK=build/unodos_apple2_test.dsk ;;
  *)
    DEF=""; DSK=build/unodos_apple2.dsk ;;
esac

echo "[3/4] assembling boot.s and kernel.s..."
"$DASM" boot.s -f3 -obuild/boot.bin -lbuild/boot.lst
"$DASM" kernel.s -f3 $DEF -obuild/kernel.bin -lbuild/kernel.lst

echo "[4/4] packing the 35-track DOS-order disk image..."
"$PY" mkdsk.py build/boot.bin build/kernel.bin "$DSK"
echo "done: $DSK"
