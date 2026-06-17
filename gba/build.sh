#!/bin/sh
# UnoDOS / Game Boy Advance build (ARM7TDMI, arm-none-eabi via WSL) -> .gba ROM.
# Usage: ./build.sh           -> build/unodos.gba        (interactive)
#        ./build.sh nav        -> build/unodos_nav.gba    (AUTOTEST: directional select)
#        ./build.sh app|clock|theme|music|dostris         (AUTOTEST: launch that app/game)
set -e
cd "$(dirname "$0")"

PY="${PY:-python}"
REPO_WSL="/mnt/c/Users/arin/Documents/Github/unodos/gba"

mkdir -p build
echo "[1/3] generating gfx data (font + icons + palettes + tables)..."
(cd .. && "$PY" gba/mkdata.py)

DEFS=""
OUT=build/unodos.gba
case "$1" in
  nav)     DEFS="--defsym AUTOTEST=1 --defsym AT_NAV=1";     OUT=build/unodos_nav.gba ;;
  app)     DEFS="--defsym AUTOTEST=1 --defsym AT_APP=1";     OUT=build/unodos_app.gba ;;
  clock)   DEFS="--defsym AUTOTEST=1 --defsym AT_CLOCK=1";   OUT=build/unodos_clock.gba ;;
  theme)   DEFS="--defsym AUTOTEST=1 --defsym AT_THEME=1";   OUT=build/unodos_theme.gba ;;
  music)   DEFS="--defsym AUTOTEST=1 --defsym AT_MUSIC=1";   OUT=build/unodos_music.gba ;;
  dostris) DEFS="--defsym AUTOTEST=1 --defsym AT_DOSTRIS=1"; OUT=build/unodos_dt.gba ;;
esac

echo "[2/3] assembling (ARM7TDMI) + linking via WSL..."
wsl bash -lc "cd $REPO_WSL && \
  arm-none-eabi-as -mcpu=arm7tdmi $DEFS kernel.s -o build/kernel.o && \
  arm-none-eabi-as -mcpu=arm7tdmi build/gfx.s -o build/gfx.o && \
  arm-none-eabi-ld -T link.ld build/kernel.o build/gfx.o -o build/unodos.elf && \
  arm-none-eabi-objcopy -O binary build/unodos.elf $OUT"

echo "[3/3] fixing the GBA header checksum..."
"$PY" gbafix.py "$OUT"
echo "done: gba/$OUT ($(wc -c < "$OUT") bytes)"
