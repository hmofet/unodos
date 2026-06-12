#!/bin/sh
# UnoDOS/MacPlus build: boot blocks + kernel -> bootable 800K .dsk image.
# Requires vasmm68k_mot (same toolchain as the Amiga/Genesis ports).
# Usage: ./build.sh [test]    ("test" auto-launches both apps at boot)
set -e
cd "$(dirname "$0")"

VASM="${VASM:-/c/Users/arin/amiga-tools/vasmm68k_mot.exe}"
PY="${PY:-python3}"

mkdir -p build
echo "[1/4] generating shared data (font/icons) from x86 tree assets..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i)

case "$1" in
  test)
    DEF="-DAUTOTEST=1"; KERN=build/kernel_test.bin; DSK=build/unodos_macplus_test.dsk ;;
  mac2)
    # Mac II class: 640x480 1-bit, 80 bytes/row (ROM-assisted input mode)
    DEF="-DSCRW=640 -DSCRH=480 -DROWB=80"; KERN=build/kernel_mac2.bin; DSK=build/unodos_mac2.dsk ;;
  mac2test)
    DEF="-DAUTOTEST=1 -DSCRW=640 -DSCRH=480 -DROWB=80"; KERN=build/kernel_mac2_test.bin; DSK=build/unodos_mac2_test.dsk ;;
  *)
    DEF=""; KERN=build/kernel.bin; DSK=build/unodos_macplus.dsk ;;
esac

echo "[2/5] assembling boot blocks..."
"$VASM" -Fbin -m68000 -nosym -o build/boot.bin boot.asm

echo "[3/5] assembling kernel.asm (cpu 68000)..."
"$VASM" -Fbin -m68000 -nosym $DEF -o "$KERN" kernel.asm

echo "      assembling the disk-loaded demo app (DEMO.APP)..."
"$VASM" -Fbin -m68000 -nosym -o build/demo.bin demo_app.asm

echo "[4/5] packing bootable 800K image..."
"$PY" mkdisk.py build/boot.bin "$KERN" "$DSK"

echo "[5/5] writing the UnoDOS FAT12 filesystem (Files/Notepad/DEMO.APP)..."
"$PY" mkfs.py "$DSK" disk/README.TXT disk/HELLO.TXT build/demo.bin:DEMO.APP
echo "done: $DSK"
