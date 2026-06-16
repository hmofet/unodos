#!/bin/sh
# Host build + run of the uno2d tall-vtable worked example (CONTRACT-ARCH Phase 6).
set -e
CC="${CC:-gcc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"; mkdir -p build
"$CC" -std=c11 -Wall -Wextra -O1 -I unodef/gen/c -I uno2d uno2d/uno2d.c uno2d/uno2d_test.c -o build/uno2d_test
./build/uno2d_test
