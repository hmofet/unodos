#!/bin/sh
# unonet TLS-entropy host gate: build tls_entropy.c natively against a
# synthetic CPU (tools/tls_entropy_test.c) and run each scenario in its own
# process - the source is cached in a file-static, so scenarios must not share
# one. Seconds, no QEMU, no UEFI; run it after every edit to tls_entropy.c.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/tls_entropy_test"

# BearSSL's SHA-256 does the conditioning; take it from the in-tree copy so the
# gate exercises the same implementation the OS links.
${CC:-cc} -O1 -Wall -Wextra -Werror -DTLS_ENT_HOSTTEST -I. -Ibearssl/inc -Ibearssl/src \
    -o "$OUT" tools/tls_entropy_test.c tls_entropy.c \
    bearssl/src/hash/sha2small.c bearssl/src/codec/dec32be.c bearssl/src/codec/enc32be.c

rc=0
for scen in rdrand jitter deadrand frozen steplock coarse; do
    "$OUT" "$scen" || rc=1
done
[ "$rc" -eq 0 ] && echo ">> tls entropy gate OK" || echo ">> tls entropy gate FAILED"
exit $rc
