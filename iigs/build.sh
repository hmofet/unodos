#!/bin/sh
# UnoDOS/Apple IIGS build. 65C816 via cc65 (ca65 + ld65); ProDOS 800K .po.
#
# The kernel (SHR renderer, WM, ADB input, FAT12 storage, DOC audio engine,
# scheduler, draw primitives, SysInfo + Clock) is assembled to $00:2000.  Every
# other app is a SEPARATE binary, assembled at its own fixed bank-0 load region
# (sys.inc SLOT_*), packed onto the FAT12 volume as <NAME>.APP, and read at
# runtime by the kernel loader via fat_read_file.  The kernel exports its API
# entry points (mkapi.py -> build/kernel_api.inc) so the apps link to it purely
# by address (the C64 disk-app pattern, ld65 flavour).
#
# Usage:
#   ./build.sh            -> build/unodos_iigs.po   (boot + kernel + app .APPs)
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

echo "[1/7] generating font + palette from the shared assets..."
(cd .. && "$PY" iigs/mkdata.py)

echo "[2/7] assembling boot stage (ca65 --cpu 65816)..."
"$CA65" --cpu 65816 $FLAGS -o build/boot.o boot.s
"$LD65" -C boot.cfg -o build/boot.bin build/boot.o

echo "[3/7] assembling kernel..."
"$CA65" --cpu 65816 $FLAGS -o build/kernel.o kernel.s
"$LD65" -C kernel.cfg -o build/kernel.bin -Ln build/kernel.lbl build/kernel.o

echo "[4/7] exporting the kernel API for disk-loaded apps..."
"$PY" mkapi.py build/kernel.lbl build/kernel_api.inc

# assemble one disk app: $1=source.s  $2=org-symbol  $3=out.bin
build_app() {
  src="$1"; org="$2"; out="$3"
  "$CA65" --cpu 65816 $FLAGS -I build -o "build/${src%.s}.o" "$src"
  # per-app flat-binary config at the app's fixed ORG (sys.inc SLOT_*)
  cat > "build/app_${src%.s}.cfg" <<EOF
MEMORY { APP: start=$org, size=\$0800, type=ro, file=%O, fill=no; }
SEGMENTS { CODE: load=APP, type=ro, start=$org; RODATA: load=APP, type=ro; }
EOF
  "$LD65" -C "build/app_${src%.s}.cfg" -o "$out" "build/${src%.s}.o"
  echo "    $src -> $out ($(stat -c%s "$out") bytes @ $org)"
}

echo "[5/7] assembling disk-loaded apps (one fixed region each)..."
build_app filesnp.s '$4000' build/FILESNP.APP
build_app theme.s   '$4800' build/THEME.APP
build_app music.s   '$5000' build/MUSIC.APP
build_app dostris.s '$5800' build/DOSTRIS.APP
build_app paint.s   '$6000' build/PAINT.APP
build_app tracker.s '$6800' build/TRACKER.APP
build_app pacman.s  '$7000' build/PACMAN.APP
build_app outlast.s '$7800' build/OUTLAST.APP

echo "[6/7] packing the 800K ProDOS image + FAT12 volume (apps as .APP files)..."
"$PY" mkdsk.py build/boot.bin build/kernel.bin build/unodos_iigs.po
"$PY" mkfs.py build/unodos_iigs.po \
    disk/WELCOME.TXT disk/ABOUT.TXT \
    build/FILESNP.APP build/THEME.APP build/MUSIC.APP build/DOSTRIS.APP \
    build/PAINT.APP build/TRACKER.APP build/PACMAN.APP build/OUTLAST.APP

echo "[7/7] rendering the desktop via the harness..."
"$PY" harness.py build/unodos_iigs.po build/m1.png --frames 4

echo "done: build/unodos_iigs.po + build/m1.png"
