#!/bin/sh
# SAE / WPA3 host gate: the whole authentication exchange, the RSN element we
# put on the air, and the P-256 arithmetic underneath - natively, in seconds,
# with no QEMU and no radio.  Run it after every edit to pc64/wifi_sae.c or
# pc64/wifi_wpa.c.
#
# Two passes:
#   1. tools/sae_test.c   - self-consistency, structure, and rejection cases.
#   2. tools/sae_test.py  - an INDEPENDENT reimplementation of the password
#                           element derivations in Python, diffed against the
#                           C.  Pass 1 alone cannot catch a field arithmetic
#                           layer that is self-consistently wrong, and a
#                           hand-rolled Montgomery multiplier is exactly the
#                           kind of code that fails that way.
#
# Built WITH build.sh's sanitizer set, deliberately.  A harness compiled
# without them tests different code from the one the debug OS runs - the
# UnoAmp EQ defect passed a clean host harness while resetting the box on
# every run, because correct arithmetic and DEFINED arithmetic are not the
# same property.  -fsanitize-undefined-trap-on-error is left OFF on purpose:
# on the host we want the file and line, not a SIGILL.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CC=${CC:-cc}
SAN="-fsanitize=signed-integer-overflow,bounds,shift,integer-divide-by-zero,null"

# BearSSL is a big vendor tree and none of it changes between runs; build it
# once into an archive rather than paying for ~100 translation units on every
# invocation of a gate that is supposed to be instant.  Delete build/ to force
# a rebuild after a vendor bump.
LIB=build/bearssl-host.a
if [ ! -f "$LIB" ]; then
    echo "building the BearSSL host archive (once)..."
    rm -rf build/bsslobj && mkdir -p build/bsslobj
    for c in $(find bearssl/src -name '*.c' | sort); do
        o=build/bsslobj/$(echo "$c" | tr / _ | sed 's/\.c$/.o/')
        $CC -O1 -c -Ibearssl/inc -Ibearssl/src -o "$o" "$c" &
        while [ "$(jobs -p | wc -l)" -ge 8 ]; do wait -n 2>/dev/null || wait; done
    done
    wait
    ar rcs "$LIB" build/bsslobj/*.o
fi

$CC -O1 -Wall -Wextra $SAN -Ibearssl/inc -I. \
    -o build/sae_test \
    tools/sae_test.c wifi_sae.c wifi_wpa.c "$LIB"

./build/sae_test
echo
echo "-- cross-checking the P-256 arithmetic against an independent Python model"
./build/sae_test vectors > build/sae_vectors.txt
python3 tools/sae_test.py build/sae_vectors.txt
