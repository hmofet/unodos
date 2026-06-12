#!/bin/sh
# UnoDOS/Genesis build. Same vasm as the Amiga port (it's all 68000).
# Usage: ./build.sh [variant]
#   ./build.sh            -> build/unodos.gen (interactive)
#   ./build.sh test       -> build/unodos_test.gen (AUTOTEST composite)
#   ./build.sh notepad    -> AUTOTEST_NOTEPAD (demo text + 6 up-arrows)
#   ./build.sh music      -> AUTOTEST_MUSIC
#   ./build.sh kbd        -> AUTOTEST_KBD (soft-keyboard click path)
#   ./build.sh ps2        -> AUTOTEST_PS2 (synthetic PS/2 streams)
#   ./build.sh click      -> AUTOTEST_CLICK (click-latch double-click)
set -e
cd "$(dirname "$0")"

VASM="${VASM:-/c/Users/arin/amiga-tools/vasmm68k_mot.exe}"
PY="${PY:-python3}"

mkdir -p build
echo "[1/2] generating tiles/song/keymaps from the shared assets..."
(cd .. && "$PY" genesis/mkdata.py)

FLAGS=""
OUT=build/unodos.gen
case "$1" in
  test)    FLAGS="-DAUTOTEST=1";                       OUT=build/unodos_test.gen ;;
  notepad) FLAGS="-DAUTOTEST=1 -DAUTOTEST_NOTEPAD=1";  OUT=build/unodos_np.gen ;;
  music)   FLAGS="-DAUTOTEST=1 -DAUTOTEST_MUSIC=1";    OUT=build/unodos_mu.gen ;;
  kbd)     FLAGS="-DAUTOTEST=1 -DAUTOTEST_KBD=1";      OUT=build/unodos_kb.gen ;;
  ps2)     FLAGS="-DAUTOTEST=1 -DAUTOTEST_PS2=1";      OUT=build/unodos_p2.gen ;;
  click)   FLAGS="-DAUTOTEST=1 -DAUTOTEST_CLICK=1";    OUT=build/unodos_ck.gen ;;
  dostris) FLAGS="-DAUTOTEST=1 -DAUTOTEST_DOSTRIS=1";  OUT=build/unodos_dt.gen ;;
  outlast) FLAGS="-DAUTOTEST=1 -DAUTOTEST_OUTLAST=1";  OUT=build/unodos_ol.gen ;;
  pacman)  FLAGS="-DAUTOTEST=1 -DAUTOTEST_PACMAN=1";   OUT=build/unodos_pm.gen ;;
esac

echo "[2/2] assembling kernel.asm (cpu 68000, flat binary)..."
"$VASM" -Fbin -m68000 -nosym $FLAGS -o "$OUT" kernel.asm
echo "done: $OUT"
