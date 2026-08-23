#!/bin/sh
# Host gate for unopkg: build pc64_pkg.c natively against a stand-in volume
# layer and run tools/pkg_test.c over a REAL Android package.  Seconds, no
# QEMU, no UEFI - run it after every edit to pc64_pkg.c or foreign_shim.c.
#
#     tools/pkg_test.sh <app.apk> [FSHIM.UNO]
#
# The template defaults to build/esp/PKG/FSHIM.UNO, so the ordinary sequence
# is ./build.sh once and then this as often as you like.
#
# A REAL package is the point.  Every reader in pc64_pkg.c fails on the
# CONTENTS of a file rather than on anything about the machine, and a shipping
# APK exercises what a fixture would not think to: thousands of central-
# directory entries, a deflated manifest, a UTF-8 string pool, and an
# android:label that is a resource reference rather than a string.
set -e
cd "$(dirname "$0")/.."

APK="$1"
TPL="${2:-build/esp/PKG/FSHIM.UNO}"
if [ -z "$APK" ] || [ ! -f "$APK" ]; then
    echo "usage: tools/pkg_test.sh <app.apk> [FSHIM.UNO]" >&2
    exit 2
fi
if [ ! -f "$TPL" ]; then
    echo "no shim template at $TPL - run ./build.sh first" >&2
    exit 2
fi

OUT="${TMPDIR:-/tmp}/pkg_test"
ROOT="${TMPDIR:-/tmp}/pkg_test_root"
CC=${CC:-cc}
# -Wno-incompatible-pointer-types: um_set_alloc(malloc, free) is the tree's
# idiom in five other files (pc64_media.c, unoamp_skin.c, ...); this gate is
# not the place to diverge from it.
FLAGS="-O1 -Wall -Wextra -I. -I../unomedia -I../unoui -I../unosound -Wno-incompatible-pointer-types"

rm -rf "$ROOT"; mkdir -p "$ROOT"
$CC $FLAGS -o "$OUT" tools/pkg_test.c pc64_pkg.c \
    ../unomedia/um_inflate.c ../unomedia/unomedia.c
"$OUT" "$ROOT" "$APK" "$TPL"
