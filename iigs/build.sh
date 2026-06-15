#!/bin/sh
# UnoDOS/Apple IIGS build. 65C816 via cc65 (ca65 + ld65); ProDOS 800K .po.
# Usage:
#   ./build.sh            -> build/unodos_iigs.po   (+ block-0 boot, kernel)
#   ./build.sh test       -> AUTOTEST build (synthetic input; M1+)
set -e
cd "$(dirname "$0")"

CA65="${CA65:-/c/Users/arin/snes-tools/bin/ca65.exe}"
LD65="${LD65:-/c/Users/arin/snes-tools/bin/ld65.exe}"
PY="${PY:-python}"

mkdir -p build

FLAGS=""
case "$1" in
  test|autotest) FLAGS="-D AUTOTEST=1" ;;
esac

echo "[1/5] generating font + palette from the shared assets..."
(cd .. && "$PY" iigs/mkdata.py)

echo "[2/5] assembling boot stage (ca65 --cpu 65816)..."
"$CA65" --cpu 65816 $FLAGS -o build/boot.o boot.s
"$LD65" -C boot.cfg -o build/boot.bin build/boot.o

echo "[3/5] assembling kernel..."
"$CA65" --cpu 65816 $FLAGS -o build/kernel.o kernel.s
"$LD65" -C kernel.cfg -o build/kernel.bin build/kernel.o

echo "[4/5] packing the 800K ProDOS image..."
"$PY" mkdsk.py build/boot.bin build/kernel.bin build/unodos_iigs.po

echo "[5/5] rendering the desktop via the harness..."
"$PY" harness.py build/unodos_iigs.po build/m1.png --frames 4

echo "done: build/unodos_iigs.po + build/m1.png"
