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

# Each variant gets a CLEAN build dir. They share build/, and an object from
# one configuration linked into the next failed at link with an undefined
# reference the source did not contain (a UW_ENGINE pc64_spectest.o carried
# into a non-engine link). A gate that can fail for a reason the tree does
# not hold is worse than no gate.
fresh() { rm -rf build; }

step "production build"
fresh; ./build.sh >/dev/null
step "debug build"
fresh; UNO_DEBUG=1 ./build.sh >/dev/null
step "unoweb-engine build (BROWSER_ENGINE=uw)"
fresh; BROWSER_ENGINE=uw ./build.sh >/dev/null
step "unoweb-engine debug build"
fresh; BROWSER_ENGINE=uw UNO_DEBUG=1 ./build.sh >/dev/null

# Run a host test, print only its last line, and FAIL THE GATE if it failed.
# `test | tail -1` cannot do that: a pipeline's exit status is the LAST
# command's, so piping a test into tail throws the test's status away and
# `set -e` never sees it - every check below was reporting rather than gating.
# On failure the whole output is printed, since that is when you want it.
hosttest() {
    if ! _o=$("$@" 2>&1); then
        printf '%s\n' "$_o"
        printf 'gate: %s FAILED\n' "$1"
        exit 1
    fi
    printf '%s\n' "$_o" | tail -1
}

step "unoweb golden tests"
(cd ../unoweb/test && make -s >/dev/null)
(cd ../unoweb/test && hosttest ./run_tests)

step "csslib host tests"
(cd .. && sh csslib/test/build-host-test.sh >/dev/null 2>&1)
hosttest ../csslib/test/build/css_host_test.exe
hosttest ../csslib/test/build/css_cascade_test.exe

step "quickjs host tests"
sh quickjs/test/build-host-test.sh >/dev/null 2>&1
hosttest ./build/qjs_host_test.exe
hosttest ./build/qjs_dispatch_test.exe
hosttest ./build/webjs_test.exe

step "cookie jar tests"
hosttest ./build/cookie_test.exe
hosttest ./build/cache_test.exe
hosttest ./build/framing_test.exe
hosttest ./build/fetch_test.exe

# The TLS gates are host builds against a real echo server, and they are the
# only assertion in the tree that a handshake completes: SPECTEST's network
# area runs a NULL NIC and cannot reach TLS at all.
# Key bindings: the defaults ARE what every app on this machine reads from the
# keyboard, so a change to them is a change to input everywhere.  Runs twice,
# with and without the bindings module, because hid_kbd.c's weak fallback is
# what the legacy core links.
step "key binding host gate"
hosttest sh tools/binds_test.sh

step "unonet TLS host gates"
hosttest sh tools/tls_entropy_test.sh
hosttest sh tools/tls_conc_test.sh

# WiFi security: the SAE exchange, the RSN element we put on the air, and the
# P-256 arithmetic under both.  Belongs in the gate for the same reason the
# TLS ones do - SPECTEST's network area is a NULL NIC, so nothing on the QEMU
# side can reach a supplicant, and the only alternative to a host gate here is
# carrying a laptop to an access point.
step "WiFi supplicant host gate (WPA2-PSK + WPA3-SAE)"
hosttest sh tools/sae_test.sh

if [ "${QUICK:-0}" != "0" ]; then
    printf '\nQUICK=1: skipping the QEMU conformance run\n'
    exit 0
fi

# The QEMU suite needs the DEBUG image staged, and the loop above left the
# uw-debug one in build/esp.
step "restaging the plain debug image for SPECTEST"
fresh; UNO_DEBUG=1 ./build.sh >/dev/null
step "SPECTEST in QEMU"
python3 tools/spectest_qemu.py | tail -3
