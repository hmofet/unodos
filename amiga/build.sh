#!/bin/sh
# UnoDOS/68K Amiga build. Requires vasmm68k_mot + exe2adf (see amiga/README.md).
# Usage: ./build.sh [test]    ("test" auto-launches apps at boot)
#
# Pipeline (kept consistent in ONE pass so a test artifact never links against
# a stale API): kernel -> mkapi (vars-offset export + symbol check) -> assemble
# each disk-loaded app (-Fbin at its fixed slot) -> bootable ADF + DF1 FAT12
# data disk carrying the .APP binaries.
set -e
cd "$(dirname "$0")"

VASM="${VASM:-/c/Users/arin/amiga-tools/vasmm68k_mot.exe}"
EXE2ADF="${EXE2ADF:-/c/Users/arin/amiga-tools/exe2adf.exe}"
PY="${PY:-python3}"
VFLAGS="-opt-allbra"            # auto-extend out-of-range branches

mkdir -p build
echo "[1/6] generating data from x86 tree assets..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i)

if [ "$1" = "test" ]; then
    DEF="-DAUTOTEST=1"; EXE=build/UnoDOS68K_test; ADF=build/unodos68k_test.adf
    LABEL="UnoDOS Test"
else
    DEF=""; EXE=build/UnoDOS68K; ADF=build/unodos68k.adf; LABEL="UnoDOS 68K"
fi
# allow a second -D for the focused autotest variants:  ./build.sh test THEME
if [ -n "$2" ]; then DEF="$DEF -DAUTOTEST_$2=1"; fi

echo "[2/6] assembling kernel.asm (cpu 68000)..."
"$VASM" -Fhunkexe -nosym $VFLAGS $DEF -L build/kernel.lst -o "$EXE" kernel.asm

echo "[3/6] exporting the kernel API (vars offsets + symbol check)..."
"$PY" mkapi.py build/kernel.lst build/kernel_api.inc

echo "[4/6] assembling disk-loaded apps (-Fbin at fixed slots)..."
APPS=""
for app in files:FILES theme:THEME dostris:DOSTRIS pacman:PACMAN \
           notepad:NOTEPAD music:MUSIC outlast:OUTLAST tracker:TRACKER \
           paint:PAINT; do
  src="${app%%:*}_app.asm"; out="build/${app##*:}.APP"
  if [ -f "$src" ]; then
    "$VASM" -Fbin $VFLAGS $DEF -L "build/${app%%:*}_app.lst" -o "$out" "$src"
    echo "  $src -> $out"
    APPS="$APPS $out"
  fi
done

echo "[5/6] packing bootable ADF (kernel)..."
"$EXE2ADF" -i "$EXE" -a "$ADF" -l "$LABEL"

echo "[6/6] building DF1 FAT12 data disk (text files + app binaries)..."
# put the .TXT data files AND the .APP binaries on the data disk
"$PY" mkfat.py build/unodos-data.adf disk/CANON.TXT disk/HELLO.TXT disk/README.TXT $APPS

echo "done: $ADF + build/unodos-data.adf (DF1)"
