#!/bin/sh
# Host gate for key bindings: build uno_binds.c and hid_kbd.c natively against
# an in-memory filesystem and run tools/binds_test.c.  Seconds, no QEMU, no
# UEFI - run it after every edit to either file, or to the defaults.
#
# TWICE, and the second run is the point: hid_kbd.c carries a WEAK
# uno_bind_bits() so it still links in a build without the bindings module (the
# legacy core is exactly that build).  That fallback has to keep behaving
# exactly as this machine always did, and the only way to know is to link it
# without uno_binds.c and run the same assertions.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/binds_test"
CC=${CC:-cc}
FLAGS="-O1 -Wall -Wextra -Werror -I."

echo "with uno_binds.c linked:"
$CC $FLAGS -DBINDS_LINKED -o "$OUT" tools/binds_test.c uno_binds.c hid_kbd.c
"$OUT"

echo "with the weak fallback only (the legacy core's build):"
$CC $FLAGS -o "$OUT-weak" tools/binds_test.c hid_kbd.c
"$OUT-weak"
