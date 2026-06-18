#!/bin/sh
# UnoDOS / Raspberry Pi (AArch64) build -> kernel8.img (flat bare-metal image).
# Usage: ./build.sh           -> build/kernel8.img        (interactive M1 boot)
#        ./build.sh nav        -> build/kernel8_nav.img    (AUTOTEST: directional select)
#        ./build.sh app|clock|theme|music|dostris          (AUTOTEST: launch that app/game)
set -e
cd "$(dirname "$0")"

PY="${PY:-python}"
REPO_WSL="/mnt/c/Users/arin/Documents/Github/unodos/rpi"
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
  paint)   DEFS="--defsym AUTOTEST=1 --defsym AT_PAINT=1";   OUT=build/kernel8_paint.img ;;
  pacman)  DEFS="--defsym AUTOTEST=1 --defsym AT_PACMAN=1";  OUT=build/kernel8_pm.img ;;
  outlast) DEFS="--defsym AUTOTEST=1 --defsym AT_OUTLAST=1"; OUT=build/kernel8_ol.img ;;
  tracker) DEFS="--defsym AUTOTEST=1 --defsym AT_TRACKER=1"; OUT=build/kernel8_tk.img ;;
  fs)      DEFS="--defsym AUTOTEST=1 --defsym AT_FS=1";      OUT=build/kernel8_fs.img ;;
  fs2)     DEFS="--defsym AUTOTEST=1 --defsym AT_FS2=1";     OUT=build/kernel8_fs2.img ;;
  fsview)  DEFS="--defsym AUTOTEST=1 --defsym AT_FSVIEW=1";  OUT=build/kernel8_fsv.img ;;
esac

echo "[2/3] assembling (AArch64) + linking via WSL..."
wsl bash -lc "cd $REPO_WSL && \
  $AS -march=armv8-a $DEFS kernel.s -o build/kernel.o && \
  $AS -march=armv8-a build/gfx.s -o build/gfx.o && \
  $LD -T link.ld build/kernel.o build/gfx.o -o build/kernel.elf && \
  $OC -O binary build/kernel.elf $OUT"

echo "[3/3] done: rpi/$OUT ($(wc -c < "$OUT") bytes)"
