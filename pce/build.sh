#!/bin/sh
# UnoDOS / PC Engine build (HuC6280, ca65) -> .pce HuCard ROM.
set -e
cd "$(dirname "$0")"
PY="${PY:-python}"
CA65="${CA65:-/c/Users/arin/snes-tools/bin/ca65}"
LD65="${LD65:-/c/Users/arin/snes-tools/bin/ld65}"
mkdir -p build
echo "[1/3] generating tiles + palette + data..."
(cd .. && "$PY" pce/mkdata.py)
DEF=""; OUT=build/unodos.pce
case "$1" in
  nav)     DEF="-DAUTOTEST -DAT_NAV";     OUT=build/unodos_nav.pce ;;
  app)     DEF="-DAUTOTEST -DAT_APP";     OUT=build/unodos_app.pce ;;
  clock)   DEF="-DAUTOTEST -DAT_CLOCK";   OUT=build/unodos_clock.pce ;;
  theme)   DEF="-DAUTOTEST -DAT_THEME";   OUT=build/unodos_theme.pce ;;
  music)   DEF="-DAUTOTEST -DAT_MUSIC";   OUT=build/unodos_music.pce ;;
  dostris) DEF="-DAUTOTEST -DAT_DOSTRIS"; OUT=build/unodos_dt.pce ;;
esac
echo "[2/3] assembling (HuC6280)..."
"$CA65" --cpu huc6280 $DEF kernel.s -o build/kernel.o
echo "[3/3] linking..."
"$LD65" -C pce.cfg build/kernel.o -o "$OUT"
echo "done: pce/$OUT ($(wc -c < "$OUT") bytes)"
