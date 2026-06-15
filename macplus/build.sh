#!/bin/sh
# UnoDOS/MacPlus build: boot blocks + kernel -> bootable 800K .dsk image, plus
# the disk-loaded app binaries packed onto the FAT12 volume.
#
# Pipeline (kept consistent in ONE pass so apps never link against a stale API):
#   1. shared data (font/icons)
#   2. boot blocks
#   3. kernel.asm  (-L listing -> symbol table)
#   4. mkapi.py    kernel listing -> build/kernel_api.inc (app link addresses)
#   5. each app: mkapp.py de-PC's kernel refs -> assemble at its slot org
#   6. mkdisk.py packs boot+kernel; mkfs.py writes the FAT12 volume incl. *.APP
#
# Requires vasmm68k_mot (Amiga/Genesis toolchain) + python3.
# Usage: ./build.sh [test|mac2|mac2test]
set -e
cd "$(dirname "$0")"

VASM="${VASM:-/c/Users/arin/amiga-tools/vasmm68k_mot.exe}"
PY="${PY:-python3}"

mkdir -p build
echo "[1/6] generating shared data (font/icons) from x86 tree assets..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i)

case "$1" in
  test)
    DEF="-DAUTOTEST=1"; KERN=build/kernel_test.bin; DSK=build/unodos_macplus_test.dsk ;;
  mac2)
    DEF="-DSCRW=640 -DSCRH=480 -DROWB=80"; KERN=build/kernel_mac2.bin; DSK=build/unodos_mac2.dsk ;;
  mac2test)
    DEF="-DAUTOTEST=1 -DSCRW=640 -DSCRH=480 -DROWB=80"; KERN=build/kernel_mac2_test.bin; DSK=build/unodos_mac2_test.dsk ;;
  *)
    DEF=""; KERN=build/kernel.bin; DSK=build/unodos_macplus.dsk ;;
esac

echo "[2/6] assembling boot blocks..."
"$VASM" -Fbin -m68000 -nosym -o build/boot.bin boot.asm

echo "[3/6] assembling kernel.asm (cpu 68000)..."
# -L listing carries the symbol table mkapi.py reads (no -nosym here).
"$VASM" -Fbin -m68000 $DEF -L build/kernel.lst -o "$KERN" kernel.asm
cp "$KERN" build/kernel_api_src.bin >/dev/null 2>&1 || true

echo "[4/6] exporting the kernel API for disk-loaded apps..."
"$PY" mkapi.py build/kernel.lst build/kernel_api.inc

echo "[5/6] assembling disk-loaded apps (each at its fixed slot org)..."
# slot ids must match diskapp.i proc_slot / app_names.
for app in files:0 dostris:1 pacman:2 outlast:3 paint:4 music:5 tracker:6 theme:7 demo:8; do
  name="${app%%:*}"; id="${app##*:}"
  src="$name.app.asm"
  [ -f "$src" ] || continue
  "$PY" mkapp.py build/kernel_api.inc "$src" "build/$name.gen.asm"
  "$VASM" -Fbin -m68000 $DEF -o "build/$name.bin" "build/$name.gen.asm"
  printf "  %-12s -> build/%s.bin (slot %s, %s bytes)\n" "$src" "$name" "$id" "$(wc -c <build/$name.bin)"
done

echo "[6/6] packing bootable 800K image + FAT12 volume (apps + text)..."
"$PY" mkdisk.py build/boot.bin "$KERN" "$DSK"
APPS=""
for a in files dostris pacman outlast paint music tracker theme demo; do
  # FAT 8.3 name: uppercase basename (<=8 chars) + .APP; mkfs.py pads to 8.3.
  [ -f "build/$a.bin" ] && APPS="$APPS build/$a.bin:$(echo $a | tr a-z A-Z).APP"
done
"$PY" mkfs.py "$DSK" disk/README.TXT disk/HELLO.TXT $APPS
echo "done: $DSK"
