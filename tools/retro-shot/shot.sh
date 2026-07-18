#!/bin/sh
# shot.sh <core> <rom> <out.png> [frames] - render a ROM headlessly to PNG.
# core: one of genesis snes gb  (or a full .so path)
set -e
C="$1"; ROM="$2"; OUT="$3"; FR="${4:-600}"
case "$C" in
  genesis|gen|sms|gg) SO=~/uno-emu/cores/genesis_plus_gx_libretro.so ;;
  snes|sfc)           SO=~/uno-emu/cores/snes9x_libretro.so ;;
  gb|gbc)             SO=~/uno-emu/cores/gambatte_libretro.so ;;
  *)                  SO="$C" ;;
esac
TMP=$(mktemp --suffix=.ppm)
~/uno-emu/retro_shot "$SO" "$ROM" "$TMP" "$FR"
python3 ~/uno-emu/ppm2png.py "$TMP" "$OUT"
rm -f "$TMP"
