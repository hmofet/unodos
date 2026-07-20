#!/bin/sh
# UnoDOS / PinePhone (Allwinner A64, AArch64) build -> unodos.bin (flat payload).
# Usage: ./build.sh           -> build/unodos.bin        (interactive M1 boot)
#        ./build.sh nav        -> build/unodos_nav.bin    (AUTOTEST: directional select)
#        ./build.sh app|clock|theme|music|dostris          (AUTOTEST: launch that app/game)
set -e
cd "$(dirname "$0")"

PY="${PY:-python}"
# WSL path of THIS directory (works from Git Bash /c/... or from WSL /mnt/c/...)
REPO_WSL="$(pwd | sed 's#^/\([a-zA-Z]\)/#/mnt/\1/#')"
AS=aarch64-linux-gnu-as
LD=aarch64-linux-gnu-ld
OC=aarch64-linux-gnu-objcopy

mkdir -p build
echo "[1/3] generating gfx data (font + icons + palettes + tables)..."
(cd .. && "$PY" pinephone/mkdata.py)

DEFS=""
OUT=build/unodos.bin
case "$1" in
  nav)     DEFS="--defsym AUTOTEST=1 --defsym AT_NAV=1";     OUT=build/unodos_nav.bin ;;
  app)     DEFS="--defsym AUTOTEST=1 --defsym AT_APP=1";     OUT=build/unodos_app.bin ;;
  clock)   DEFS="--defsym AUTOTEST=1 --defsym AT_CLOCK=1";   OUT=build/unodos_clock.bin ;;
  theme)   DEFS="--defsym AUTOTEST=1 --defsym AT_THEME=1";   OUT=build/unodos_theme.bin ;;
  music)   DEFS="--defsym AUTOTEST=1 --defsym AT_MUSIC=1";   OUT=build/unodos_music.bin ;;
  dostris) DEFS="--defsym AUTOTEST=1 --defsym AT_DOSTRIS=1"; OUT=build/unodos_dt.bin ;;
  tracker) DEFS="--defsym AUTOTEST=1 --defsym AT_TRACKER=1"; OUT=build/unodos_tk.bin ;;
  outlast) DEFS="--defsym AUTOTEST=1 --defsym AT_OUTLAST=1"; OUT=build/unodos_ol.bin ;;
  pacman)  DEFS="--defsym AUTOTEST=1 --defsym AT_PACMAN=1";  OUT=build/unodos_pm.bin ;;
  paint)   DEFS="--defsym AUTOTEST=1 --defsym AT_PAINT=1";   OUT=build/unodos_pt.bin ;;
esac

echo "[2/3] assembling (AArch64) + linking via WSL..."
wsl bash -lc "cd $REPO_WSL && \
  $AS -march=armv8-a $DEFS kernel.s -o build/kernel.o && \
  $AS -march=armv8-a build/gfx.s -o build/gfx.o && \
  $LD -T link.ld build/kernel.o build/gfx.o -o build/kernel.elf && \
  $OC -O binary build/kernel.elf $OUT"

echo "[3/3] done: pinephone/$OUT ($(wc -c < "$OUT") bytes)"
