#!/bin/sh
# UnoDOS / Raspberry Pi (AArch64) build -> kernel8.img (flat bare-metal image).
# Usage: ./build.sh           -> build/kernel8.img        (interactive M1 boot)
#        ./build.sh nav        -> build/kernel8_nav.img    (AUTOTEST: directional select)
#        ./build.sh app|clock|theme|music|dostris          (AUTOTEST: launch that app/game)
set -e
cd "$(dirname "$0")"

PY="${PY:-python}"
command -v "$PY" >/dev/null 2>&1 || PY=python3
REPO_WSL="/mnt/c/Users/arin/Documents/Github/unodos-logo/rpi"
AS=aarch64-linux-gnu-as
LD=aarch64-linux-gnu-ld
OC=aarch64-linux-gnu-objcopy

mkdir -p build
echo "[1/3] generating gfx data (font + icons + palettes + tables)..."
(cd .. && "$PY" rpi/mkdata.py)

DEFS=""
OUT=build/kernel8.img
case "$1" in
  nav)     DEFS="--defsym AUTOTEST=1 --defsym AT_NAV=1";     OUT=build/kernel8_nav.img ;;
  app)     DEFS="--defsym AUTOTEST=1 --defsym AT_APP=1";     OUT=build/kernel8_app.img ;;
  clock)   DEFS="--defsym AUTOTEST=1 --defsym AT_CLOCK=1";   OUT=build/kernel8_clock.img ;;
  theme)   DEFS="--defsym AUTOTEST=1 --defsym AT_THEME=1";   OUT=build/kernel8_theme.img ;;
  music)   DEFS="--defsym AUTOTEST=1 --defsym AT_MUSIC=1";   OUT=build/kernel8_music.img ;;
  dostris) DEFS="--defsym AUTOTEST=1 --defsym AT_DOSTRIS=1"; OUT=build/kernel8_dt.img ;;
  tracker) DEFS="--defsym AUTOTEST=1 --defsym AT_TRACKER=1"; OUT=build/kernel8_tk.img ;;
  outlast) DEFS="--defsym AUTOTEST=1 --defsym AT_OUTLAST=1"; OUT=build/kernel8_ol.img ;;
  pacman)  DEFS="--defsym AUTOTEST=1 --defsym AT_PACMAN=1";  OUT=build/kernel8_pm.img ;;
  paint)   DEFS="--defsym AUTOTEST=1 --defsym AT_PAINT=1";   OUT=build/kernel8_pt.img ;;
  store)   DEFS="--defsym AUTOTEST=1 --defsym AT_STORE=1";   OUT=build/kernel8_st.img ;;
  files)   DEFS="--defsym AUTOTEST=1 --defsym AT_FILES=1";   OUT=build/kernel8_fi.img ;;
esac

echo "[2/3] assembling (AArch64) + linking..."
if command -v "$AS" >/dev/null 2>&1; then
  # already inside WSL (or a box with the cross binutils): run directly
  $AS -march=armv8-a $DEFS kernel.s -o build/kernel.o
  $AS -march=armv8-a build/gfx.s -o build/gfx.o
  $LD -T link.ld build/kernel.o build/gfx.o -o build/kernel.elf
  $OC -O binary build/kernel.elf $OUT
else
  # Windows Git Bash: shell into WSL for the toolchain
  wsl bash -lc "cd $REPO_WSL && \
    $AS -march=armv8-a $DEFS kernel.s -o build/kernel.o && \
    $AS -march=armv8-a build/gfx.s -o build/gfx.o && \
    $LD -T link.ld build/kernel.o build/gfx.o -o build/kernel.elf && \
    $OC -O binary build/kernel.elf $OUT"
fi

echo "[3/3] done: rpi/$OUT ($(wc -c < "$OUT") bytes)"
