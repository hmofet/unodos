#!/bin/sh
# UnoDOS/SNES build. 65816 via cc65 (ca65 + ld65), LoROM .sfc output.
# Usage:
#   ./build.sh            -> build/unodos.sfc       (interactive: live joypad)
#   ./build.sh test       -> build/unodos_test.sfc  (AUTOTEST: synthetic input)
set -e
cd "$(dirname "$0")"

CA65="${CA65:-/c/Users/arin/snes-tools/bin/ca65.exe}"
LD65="${LD65:-/c/Users/arin/snes-tools/bin/ld65.exe}"
PY="${PY:-python}"

mkdir -p build

echo "[1/4] generating tiles/palette from the shared font..."
(cd .. && "$PY" snes/mkdata.py)

FLAGS=""
OUT=build/unodos.sfc
case "$1" in
  test|autotest) FLAGS="-D AUTOTEST=1"; OUT=build/unodos_test.sfc ;;
  drag)    FLAGS="-D AUTOTEST=1 -D AUTOTEST_DRAG=1"; OUT=build/unodos_dg.sfc ;;
  dostris) FLAGS="-D AUTOTEST=1 -D AUTOTEST_DOSTRIS=1"; OUT=build/unodos_dt.sfc ;;
esac

echo "[2/4] assembling kernel.asm (ca65 --cpu 65816)..."
"$CA65" --cpu 65816 $FLAGS -o build/kernel.o kernel.asm

echo "[3/4] linking (ld65 LoROM)..."
"$LD65" -C lorom.cfg -o "$OUT" build/kernel.o

echo "[4/4] patching cartridge checksum..."
"$PY" - "$OUT" <<'PYEOF'
import sys
p = sys.argv[1]
d = bytearray(open(p, "rb").read())
assert len(d) == 0x8000, f"expected 32 KB, got {len(d)}"
# self-consistent SNES checksum: complement=$FFFF, checksum=$0000 placeholder
d[0x7FDC], d[0x7FDD] = 0xFF, 0xFF
d[0x7FDE], d[0x7FDF] = 0x00, 0x00
s = sum(d) & 0xFFFF
d[0x7FDE], d[0x7FDF] = s & 0xFF, s >> 8
c = s ^ 0xFFFF
d[0x7FDC], d[0x7FDD] = c & 0xFF, c >> 8
open(p, "wb").write(d)
print(f"  checksum {s:04X}, complement {c:04X}")
PYEOF

echo "done: $OUT"
