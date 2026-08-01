#!/bin/sh
# unossh wire + KEX host gate: RFC 7748 X25519 vectors, the mpint rules, the
# reader's bounds checks, packet padding and key derivation.  Seconds, no QEMU.
#
# Built WITH build.sh's sanitizer set for the same reason ed25519test.sh is.
# Links the whole portable BearSSL tree the way build.sh does (same skip list),
# because the EC code reaches into the i31 bigint helpers and picking files by
# hand is a guessing game that only shows up at link time.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
SAN="-fsanitize=signed-integer-overflow,bounds,shift,integer-divide-by-zero,null"
BSSL_SKIP=" ghash_pclmul sysrng aes_x86ni aes_x86ni_cbcdec aes_x86ni_cbcenc
            aes_x86ni_ctr aes_x86ni_ctrcbc chacha20_sse2 "
# bearssl/src/ssl is excluded: this gate needs hashes, EC and bigints, and the
# TLS engine would only drag in br_prng_seeder_system, which pc64 overrides.
BSRC=""
for f in $(find bearssl/src -name '*.c' -not -path 'bearssl/src/ssl/*' | sort); do
    base=$(basename "$f" .c)
    case "$BSSL_SKIP" in *" $base "*) continue;; esac
    BSRC="$BSRC $f"
done
# shellcheck disable=SC2086
${CC:-cc} -O1 -Wall -Wextra $SAN -Ibearssl/inc -Ibearssl/src \
    -o build/sshwiretest \
    tools/sshwiretest.c unossh_wire.c $BSRC
./build/sshwiretest
