#!/bin/sh
# UnoDOS / WonderSwan build (NEC V30MZ, nasm 16-bit) -> a 64 KB .ws ROM.
# Usage: ./build.sh [nav|app|clock|theme|music|dostris]
set -e
cd "$(dirname "$0")"
PY="${PY:-python}"
NASM="${NASM:-/c/Users/arin/nasm-tools/nasm-2.16.01/nasm.exe}"
mkdir -p build
echo "[1/3] generating tiles + data..."
(cd .. && "$PY" ws/mkdata.py)
AT=0; SCENE=0; OUT=build/unodos.ws
case "$1" in
  nav)     AT=1; SCENE=1; OUT=build/unodos_nav.ws ;;
  app)     AT=1; SCENE=2; OUT=build/unodos_app.ws ;;
  clock)   AT=1; SCENE=3; OUT=build/unodos_clock.ws ;;
  theme)   AT=1; SCENE=4; OUT=build/unodos_theme.ws ;;
  music)   AT=1; SCENE=5; OUT=build/unodos_music.ws ;;
  dostris) AT=1; SCENE=6; OUT=build/unodos_dt.ws ;;
esac
echo "[2/3] assembling kernel.asm (AUTOTEST=$AT SCENE=$SCENE)..."
"$NASM" -f bin kernel.asm -DAUTOTEST=$AT -DSCENE=$SCENE -o build/kernel.ws -l build/kernel.lst
echo "[3/3] patching WonderSwan header checksum..."
"$PY" -c "
d=bytearray(open('build/kernel.ws','rb').read())
s=0
for i in range(len(d)-2): s=(s+d[i])&0xFFFF
d[-2]=s&0xFF; d[-1]=(s>>8)&0xFF
open('$OUT','wb').write(d)
print('wrote $OUT', len(d),'bytes; checksum', hex(s))
"
echo "done: ws/$OUT"
