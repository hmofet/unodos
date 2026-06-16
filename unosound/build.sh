#!/bin/sh
# Host build + run of the unosound voice/score floor (CONTRACT-ARCH Phase 9).
set -e
CC="${CC:-gcc}"; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"; mkdir -p build
"$CC" -std=c11 -Wall -Wextra -O1 -I unosound unosound/unosound.c unosound/unosound_test.c -lm -o build/unosound_test
./build/unosound_test
