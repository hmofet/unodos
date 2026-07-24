#!/bin/sh
# unodevices PCH-TCO host gate: build uno_hw_wdt.c against a synthetic ICH9-style
# south bridge (tools/hwwdt_test.c) and run each scenario in its own process
# (the driver caches discovery in file-statics, so scenarios must not share one).
# Seconds, no QEMU, no UEFI - run it after every edit to uno_hw_wdt.c.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/hwwdt_test"
${CC:-cc} -O1 -Wall -Wextra -Werror -DUNO_DEBUG -DHWWDT_HOSTTEST -I. \
    -o "$OUT" tools/hwwdt_test.c uno_hw_wdt.c

rc=0
for scen in present locked foreign noacpi norcba nolpc; do
    echo "== scenario: $scen =="
    "$OUT" "$scen" || rc=1
done
exit $rc
