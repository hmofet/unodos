#!/bin/sh
# unonet host gate: N concurrent, non-blocking TLS sessions (tools/tls_conc_test.c).
#
# Builds the real tls.c + tls_entropy.c + BearSSL natively against a POSIX
# netsock shim, stands up tools/tls_echo_server.py on the pinned test key, and
# runs N handshakes from one pump loop. Seconds, no QEMU. Run it after every
# edit to tls.c - the QEMU spectest area has a NULL NIC and cannot reach TLS at
# all, and nettest.py is a screenshot, so this is the only assertion in the tree
# that a handshake still completes.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/tls_conc_test"
LIB="${TMPDIR:-/tmp}/uno_bearssl_host.a"
CC="${CC:-cc}"

# the throwaway key/cert are gitignored; regenerate + re-pin on a fresh clone.
# (pinned_key.h IS committed, so a regenerate here means rebuilding pc64 too.)
if [ ! -f tls_test/server.crt ]; then
    sh tls_test/gen.sh
    echo "NOTE: regenerated tls_test cert; rebuild pc64 if the pin changed"
fi

# BearSSL, once, cached. 294 files: this is the only slow part, and only the
# first run pays it. Delete $LIB to force a rebuild.
if [ ! -f "$LIB" ]; then
    echo "[gate] building BearSSL for the host (first run only)..."
    OBJ="${TMPDIR:-/tmp}/uno_bearssl_host_obj"
    rm -rf "$OBJ"; mkdir -p "$OBJ"
    for c in $(find bearssl/src -name '*.c' | sort); do
        $CC -O1 -w -Ibearssl/inc -Ibearssl/src -c "$c" -o "$OBJ/$(echo "$c" | tr / _).o"
    done
    ar rcs "$LIB" "$OBJ"/*.o
    rm -rf "$OBJ"
fi

$CC -O1 -Wall -Wextra -Werror -I. -Ibearssl/inc -Ibearssl/src \
    -o "$OUT" tools/tls_conc_test.c tls.c tls_entropy.c tls_ca.c "$LIB"

# Start the server on an OS-chosen port and wait for it to SAY it is listening,
# rather than sleeping: a fixed port collides on a shared build box, and a fixed
# sleep is how a gate becomes flaky on a loaded one.
LOG="${TMPDIR:-/tmp}/tls_conc_port.$$"
rm -f "$LOG"
python3 tools/tls_echo_server.py tls_test/server.crt tls_test/server.key 0 > "$LOG" &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -f "$LOG"' EXIT INT TERM

PORT=""
i=0
while [ $i -lt 100 ]; do
    PORT=$(sed -n 's/^listening //p' "$LOG" 2>/dev/null | head -1)
    [ -n "$PORT" ] && break
    kill -0 $SRV 2>/dev/null || { echo "tls_echo_server died before listening"; exit 1; }
    sleep 0.1
    i=$((i + 1))
done
[ -n "$PORT" ] || { echo "tls_echo_server never reported a port"; exit 1; }

"$OUT" "$PORT"
