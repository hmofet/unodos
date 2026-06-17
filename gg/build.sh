#!/bin/sh
# UnoDOS / Sega Game Gear build (Z80, sjasmplus) -> a 32KB .gg cartridge.
# Usage: ./build.sh           -> build/unodos.gg        (interactive)
#        ./build.sh nav        -> build/unodos_nav.gg    (AUTOTEST: directional select)
#        ./build.sh app|clock|theme|music|dostris        (AUTOTEST: launch that app/game)
set -e
cd "$(dirname "$0")"

SJASM="${SJASM:-/c/Users/arin/z80-tools/sjasmplus-1.23.1.win/sjasmplus.exe}"
PY="${PY:-python}"

mkdir -p build
echo "[1/2] generating tiles + palette + data from the shared assets..."
(cd .. && "$PY" gg/mkdata.py)

FLAGS=""
OUT=build/unodos.gg
case "$1" in
  nav)     FLAGS="-DAUTOTEST=1 -DAT_NAV=1";     OUT=build/unodos_nav.gg ;;
  app)     FLAGS="-DAUTOTEST=1 -DAT_APP=1";     OUT=build/unodos_app.gg ;;
  clock)   FLAGS="-DAUTOTEST=1 -DAT_CLOCK=1";   OUT=build/unodos_clock.gg ;;
  theme)   FLAGS="-DAUTOTEST=1 -DAT_THEME=1";   OUT=build/unodos_theme.gg ;;
  music)   FLAGS="-DAUTOTEST=1 -DAT_MUSIC=1";   OUT=build/unodos_music.gg ;;
  dostris) FLAGS="-DAUTOTEST=1 -DAT_DOSTRIS=1"; OUT=build/unodos_dt.gg ;;
esac

echo "[2/2] assembling kernel.asm (Z80, raw 32KB ROM)..."
"$SJASM" $FLAGS --raw="$OUT" kernel.asm
echo "done: gg/$OUT ($(wc -c < "$OUT") bytes)"
