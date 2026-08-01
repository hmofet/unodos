#!/bin/sh
# Ed25519 host gate: RFC 8032 section 7.1 vectors plus rejection cases.
# Seconds, no QEMU, no OS - run it after every edit to ed25519.c.
#
# Built WITH build.sh's sanitizer set, deliberately. A harness compiled without
# them tests different code from the one the debug OS runs: the UnoAmp EQ
# defect passed a clean host harness while resetting the box on every run,
# because correct arithmetic and DEFINED arithmetic are not the same property.
# -fsanitize-undefined-trap-on-error is left OFF here on purpose - on the host
# we want the file and line, not a SIGILL.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
SAN="-fsanitize=signed-integer-overflow,bounds,shift,integer-divide-by-zero,null"
${CC:-cc} -O1 -Wall -Wextra $SAN -Ibearssl/inc -Ibearssl/src \
    -o build/ed25519test \
    tools/ed25519test.c ed25519.c bearssl/src/hash/sha2big.c bearssl/src/codec/dec64be.c bearssl/src/codec/enc64be.c
./build/ed25519test
