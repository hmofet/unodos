#!/bin/sh
# UnoDOS / Sega Master System build (Z80, sjasmplus).
# Usage: ./build.sh            -> build/unodos.sms       (interactive)
#        ./build.sh test       -> build/unodos_test.sms  (AUTOTEST: scripted pad)
set -e
cd "$(dirname "$0")"

SJASM="${SJASM:-/c/Users/arin/z80-tools/sjasmplus-1.23.1.win/sjasmplus.exe}"
PY="${PY:-python}"

mkdir -p build
echo "[1/2] generating tiles + palette from the shared assets..."
(cd .. && "$PY" sms/mkdata.py)

FLAGS=""
OUT=build/unodos.sms
case "$1" in
  test)    FLAGS="-DAUTOTEST=1";                       OUT=build/unodos_test.sms ;;
  dostris) FLAGS="-DAUTOTEST=1 -DAUTOTEST_DOSTRIS=1";  OUT=build/unodos_dt.sms ;;
  music)   FLAGS="-DAUTOTEST=1 -DAUTOTEST_MUSIC=1";    OUT=build/unodos_mu.sms ;;
  paint)   FLAGS="-DAUTOTEST=1 -DAUTOTEST_PAINT=1";    OUT=build/unodos_pt.sms ;;
  tracker) FLAGS="-DAUTOTEST=1 -DAUTOTEST_TRACKER=1";  OUT=build/unodos_tk.sms ;;
  outlast) FLAGS="-DAUTOTEST=1 -DAUTOTEST_OUTLAST=1";  OUT=build/unodos_ol.sms ;;
  pacman)  FLAGS="-DAUTOTEST=1 -DAUTOTEST_PACMAN=1";   OUT=build/unodos_pm.sms ;;
esac

echo "[2/2] assembling kernel.asm (Z80, raw 32KB ROM)..."
"$SJASM" $FLAGS --raw="$OUT" kernel.asm
echo "done: sms/$OUT ($(wc -c < "$OUT") bytes)"
