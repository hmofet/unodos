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

# `sh ./build.sh`, not `./build.sh`. build.sh is tracked mode 100644, so on any
# fresh checkout it is NOT executable and `./build.sh` dies with exit 126
# ("Permission denied") at the very first step - the gate cannot run at all
# until someone chmods it, and nothing says so. The exec bit is set in the
# index in the same commit, but invoking through `sh` means the gate no longer
# DEPENDS on a mode bit, which a checkout on a filesystem that has none (or a
# copied tree) would lose again.

step "production build"
fresh; sh ./build.sh >/dev/null
step "debug build"
fresh; UNO_DEBUG=1 sh ./build.sh >/dev/null
step "unoweb-engine build (BROWSER_ENGINE=uw)"
fresh; BROWSER_ENGINE=uw sh ./build.sh >/dev/null
step "unoweb-engine debug build"
fresh; BROWSER_ENGINE=uw UNO_DEBUG=1 sh ./build.sh >/dev/null

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

# UnoCode's own two: the JSONC parser and the regex engine, which every theme,
# keybinding, snippet, extension manifest and grammar in the editor is read
# through.  They come across with the vendored core (pc64/UNOCODE-UPSTREAM.md)
# and run here so that a SYNC is gated by one command - upstream's gate never
# compiles this kernel, so this is where a bad drop has to be caught.
step "unocode host tests"
hosttest sh unocode/tools/test.sh

# Studio's AI client reads every provider's reply through this extractor and
# writes every request through this emitter, and it was ungated - a manual
# test nobody ran. It is one gcc call with no dependencies, so there was never
# a reason for it not to be here.
# HOSTCC, not CC: build.sh's CC defaults to the mingw cross-compiler, which
# targets the freestanding OS and cannot produce something this gate can run.
step "studio JSON tests"
"${HOSTCC:-gcc}" -O1 -o build/json_test tools/json_test.c apps/studio_json.c
hosttest ./build/json_test

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
fresh; UNO_DEBUG=1 sh ./build.sh >/dev/null
step "SPECTEST in QEMU"
python3 tools/spectest_qemu.py | tail -3
