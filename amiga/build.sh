#!/bin/sh
# UnoDOS/68K Amiga build. Requires vasmm68k_mot + exe2adf (see amiga/README.md).
# Usage: ./build.sh [test]    ("test" auto-launches both apps at boot)
set -e
cd "$(dirname "$0")"

VASM="${VASM:-/c/Users/arin/amiga-tools/vasmm68k_mot.exe}"
EXE2ADF="${EXE2ADF:-/c/Users/arin/amiga-tools/exe2adf.exe}"
PY="${PY:-python3}"

mkdir -p build
echo "[1/3] generating data from x86 tree assets..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i)

if [ "$1" = "test" ]; then
    DEF="-DAUTOTEST=1"; EXE=build/UnoDOS68K_test; ADF=build/unodos68k_test.adf
    LABEL="UnoDOS Test"
else
    DEF=""; EXE=build/UnoDOS68K; ADF=build/unodos68k.adf; LABEL="UnoDOS 68K"
fi

echo "[2/3] assembling kernel.asm (cpu 68000)..."
"$VASM" -Fhunkexe -nosym $DEF -o "$EXE" kernel.asm

echo "[3/3] packing bootable ADF..."
"$EXE2ADF" -i "$EXE" -a "$ADF" -l "$LABEL"
echo "done: $ADF"
