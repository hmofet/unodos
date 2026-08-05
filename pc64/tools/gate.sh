#!/bin/sh
# gate.sh - the merge gate in one command (AGENTS.md section 3, item 2+3).
#
#   cd pc64 && sh tools/gate.sh
#
# Builds every variant that CAN break independently, then runs the QEMU
# conformance suite. The uw variants are here because of a real incident:
# BROWSER_ENGINE=uw was link-broken for two days (um_inflate compiled into
# both unomedia halves) and nothing noticed, because no gate ever built it.
# A variant nobody builds is a variant that is already broken.
#
#   QUICK=1 sh tools/gate.sh    builds only, no QEMU run
set -e
cd "$(dirname "$0")/.."

step() { printf '\n=== %s\n' "$1"; }

step "production build"
./build.sh >/dev/null
step "debug build"
UNO_DEBUG=1 ./build.sh >/dev/null
step "unoweb-engine build (BROWSER_ENGINE=uw)"
BROWSER_ENGINE=uw ./build.sh >/dev/null
step "unoweb-engine debug build"
BROWSER_ENGINE=uw UNO_DEBUG=1 ./build.sh >/dev/null

step "unoweb golden tests"
(cd ../unoweb/test && make -s >/dev/null && ./run_tests | tail -1)

step "csslib host tests"
(cd .. && sh csslib/test/build-host-test.sh >/dev/null 2>&1)
../csslib/test/build/css_host_test.exe | tail -1
../csslib/test/build/css_cascade_test.exe | tail -1

step "quickjs host tests"
sh quickjs/test/build-host-test.sh >/dev/null 2>&1
./build/qjs_host_test.exe | tail -1
./build/qjs_dispatch_test.exe | tail -1

if [ "${QUICK:-0}" != "0" ]; then
    printf '\nQUICK=1: skipping the QEMU conformance run\n'
    exit 0
fi

# The QEMU suite needs the DEBUG image staged, and the loop above left the
# uw-debug one in build/esp.
step "restaging the plain debug image for SPECTEST"
UNO_DEBUG=1 ./build.sh >/dev/null
step "SPECTEST in QEMU"
python3 tools/spectest_qemu.py | tail -3
