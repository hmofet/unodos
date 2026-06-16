#!/bin/sh
# Host build + run of the unofs worked example (CONTRACT-ARCH Phase 3).
# Needs a C compiler (gcc/clang) and build/unodos-144.img (run `make` first).
# Usage:  sh unofs/build.sh        (from anywhere; resolves the repo root)
set -e
CC="${CC:-gcc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build
"$CC" -std=c11 -Wall -Wextra -O1 -I unodef/gen/c -I unofs \
      unofs/unofs_core.c unofs/block_file.c unofs/unofs_test.c -o build/unofs_test
./build/unofs_test
