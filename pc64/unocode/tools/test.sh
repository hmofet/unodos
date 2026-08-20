#!/bin/sh
# UnoCode host tests - the JSONC parser and the regex engine, built natively.
#
# These two files are pure logic (no framebuffer, no toolkit, no filesystem),
# so they can be tested on the build host in a second instead of through a
# QEMU boot.  Everything else in UnoCode goes through them.
#
#   sh tools/test.sh
set -e
cd "$(dirname "$0")/.."
CC="${CC:-gcc}"
mkdir -p build
$CC -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -I. -I.. -I../../unoui -I../../ps2 \
    uc_json.c uc_rx.c uc_util.c tools/uc_test.c -o build/uc_test
./build/uc_test
