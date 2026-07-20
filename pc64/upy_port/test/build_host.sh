#!/bin/sh
# Host build+run of the MicroPython port validation (M1). Uses host gcc/glibc.
#   sh pc64/upy_port/test/build_host.sh
set -e
cd "$(dirname "$0")/../.."             # upy_port/test -> pc64/
TOP=upy
PORT=upy_port
B=build/upyhost
mkdir -p "$B"

# the compile flags shared by codegen (as CPP) and compilation
CF="-std=c11 -O1 -I$PORT -I$TOP -I$B -DUPY_HOST -Wno-array-bounds"

# every source we compile is also QSTR-scanned
SRCS="$(ls $TOP/py/*.c) $PORT/test/upy_host_test.c"

echo "[1/3] codegen (qstr/module/version headers)..."
python3 $PORT/mkupy.py --top "$TOP" --port "$PORT" --build "$B" \
    --cpp "gcc -E" --cflags "$CF" -- $SRCS

echo "[2/3] compiling..."
OBJ=""
for s in $SRCS; do
    o="$B/$(echo "$s" | tr '/.' '__').o"
    gcc $CF -c -o "$o" "$s"
    OBJ="$OBJ $o"
done

echo "[3/3] linking + running..."
gcc -o "$B/upy_host_test" $OBJ -lm
"$B/upy_host_test"
