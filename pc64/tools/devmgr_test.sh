#!/bin/sh
# unodevices phase-1 host gate: build uno_devmgr.c against a synthetic PCI
# config space and run the assertions in tools/devmgr_test.c.  Seconds, no
# QEMU, no UEFI - run it after every edit to uno_devmgr.c.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/devmgr_test"
${CC:-cc} -O1 -Wall -Wextra -Werror -I. -o "$OUT" tools/devmgr_test.c uno_devmgr.c
"$OUT"
