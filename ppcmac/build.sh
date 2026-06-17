#!/bin/sh
# UnoDOS / PowerPC Mac (32-bit PowerPC) build -> unodos.elf + flat unodos.bin.
# Usage: ./build.sh           -> build/unodos.bin        (interactive M1 boot)
#        ./build.sh nav        -> build/unodos_nav.bin    (AUTOTEST: directional select)
#        ./build.sh app|clock|theme|music|dostris          (AUTOTEST: launch that app/game)
set -e
cd "$(dirname "$0")"

PY="${PY:-python}"
REPO_WSL="/mnt/c/Users/arin/Documents/Github/unodos/ppcmac"
AS=powerpc-linux-gnu-as
LD=powerpc-linux-gnu-ld
OC=powerpc-linux-gnu-objcopy

mkdir -p build
echo "[1/3] generating gfx data (font + icons + palettes + tables)..."
(cd .. && "$PY" ppcmac/mkdata.py)

DEFS=""
OUT=build/unodos.bin
case "$1" in
  nav)     DEFS="--defsym AUTOTEST=1 --defsym AT_NAV=1";     OUT=build/unodos_nav.bin ;;
  app)     DEFS="--defsym AUTOTEST=1 --defsym AT_APP=1";     OUT=build/unodos_app.bin ;;
  clock)   DEFS="--defsym AUTOTEST=1 --defsym AT_CLOCK=1";   OUT=build/unodos_clock.bin ;;
  theme)   DEFS="--defsym AUTOTEST=1 --defsym AT_THEME=1";   OUT=build/unodos_theme.bin ;;
  music)   DEFS="--defsym AUTOTEST=1 --defsym AT_MUSIC=1";   OUT=build/unodos_music.bin ;;
  dostris) DEFS="--defsym AUTOTEST=1 --defsym AT_DOSTRIS=1"; OUT=build/unodos_dt.bin ;;
esac

echo "[2/3] assembling (PowerPC, big-endian) + linking via WSL..."
wsl bash -lc "cd $REPO_WSL && \
  $AS -mregnames $DEFS kernel.s -o build/kernel.o && \
  $AS -mregnames build/gfx.s -o build/gfx.o && \
  $LD -T link.ld build/kernel.o build/gfx.o -o build/kernel.elf && \
  $OC -O binary build/kernel.elf $OUT"

echo "[3/3] done: ppcmac/$OUT ($(wc -c < "$OUT") bytes)"
