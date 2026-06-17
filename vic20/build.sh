#!/bin/sh
# UnoDOS / VIC-20 build (6502, dasm) -> a .prg (load $1201, +8K expansion).
# Usage: ./build.sh [nav|app|clock|theme|music|dostris]
set -e
cd "$(dirname "$0")"
PY="${PY:-python}"
DASM="${DASM:-/c/Users/arin/apple2-tools/dasm.exe}"
mkdir -p build
echo "[1/3] generating charset + data..."
(cd .. && "$PY" vic20/mkdata.py)
A=0; NAV=0; APP=0; CLK=0; THM=0; MUS=0; DOS=0
OUT=build/unodos.prg
case "$1" in
  nav)     A=1; NAV=1; OUT=build/unodos_nav.prg ;;
  app)     A=1; APP=1; OUT=build/unodos_app.prg ;;
  clock)   A=1; CLK=1; OUT=build/unodos_clock.prg ;;
  theme)   A=1; THM=1; OUT=build/unodos_theme.prg ;;
  music)   A=1; MUS=1; OUT=build/unodos_music.prg ;;
  dostris) A=1; DOS=1; OUT=build/unodos_dt.prg ;;
esac
DEF="-DAUTOTEST=$A -DAT_NAV=$NAV -DAT_APP=$APP -DAT_CLOCK=$CLK -DAT_THEME=$THM -DAT_MUSIC=$MUS -DAT_DOSTRIS=$DOS"
echo "[2/3] assembling kernel.s..."
"$DASM" kernel.s -f3 $DEF -obuild/kernel.bin -lbuild/kernel.lst -sbuild/kernel.sym
echo "[3/3] packing .prg (load \$1201)..."
"$PY" -c "import sys; d=open('build/kernel.bin','rb').read(); open('$OUT','wb').write(bytes([0x01,0x12])+d); print('wrote $OUT', len(d)+2,'bytes')"
echo "done: vic20/$OUT"
