#!/bin/sh
# unoscript fs-path host gate: build the pure path helpers (unoscript_path.c)
# against the assertions in tools/unoscript_path_test.c and run them.  Seconds,
# no QEMU, no UEFI - run it after every edit to unoscript_path.c.  These are the
# security-critical traversal / home-scope rules of the u.fs.* surface.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/unoscript_path_test"
${CC:-cc} -O1 -Wall -Wextra -Werror -I. -o "$OUT" \
    tools/unoscript_path_test.c unoscript_path.c
"$OUT"
