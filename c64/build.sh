#!/bin/sh
# UnoDOS/C64 build: assemble the 6510 kernel and pack it into a .prg + .d64.
# Requires dasm (6502/6510 cross-assembler) and python3 (py65 for harness.py).
# Usage: ./build.sh [test]   ("test" = -DAUTOTEST=1: launcher auto-selects an icon)
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

echo "[2/5] assembling kernel.s..."
"$DASM" kernel.s -f3 $DEF -obuild/kernel.bin -lbuild/kernel.lst -sbuild/kernel.sym

echo "[3/5] exporting the kernel API for disk-loaded apps..."
"$PY" mkapi.py build/kernel.sym build/kernel_api.inc

echo "[4/5] assembling disk-loaded apps (org \$5000)..."
for app in sysinfo:0 clock:1 files:2 theme:3 dostris:4 music:5 pacman:6 tracker:7 paint:8 outlast:9; do
  name="${app%%:*}"; id="${app##*:}"
  if [ -f "$name.s" ]; then
    "$DASM" "$name.s" -f3 $DEF -obuild/app$id.bin -lbuild/$name.lst
    echo "  $name.s -> build/app$id.bin (app id $id)"
  fi
done

echo "[5/5] packing .prg + .d64..."
"$PY" mkprg.py build/kernel.bin "$PRG" "$D64"

echo "done: $PRG (+ .d64) + disk apps"
