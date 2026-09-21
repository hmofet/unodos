#!/bin/sh
# Smoke test for the desktop apps: replay tests/*.txt against the built
# binaries, then check the documents they saved.  Usage:
#   tests/smoke.sh <dir with the binaries> <exe suffix: "" or ".exe">
# On Linux, run it under xvfb-run.  Frames land in $OUT (default ./smoke-out).
set -e
BIN=$1; EXT=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$PWD/smoke-out}
rm -rf "$OUT"; mkdir -p "$OUT/docs"
name() {   # the binary's file name on this platform
    case "$EXT" in .exe) echo "$1";; *) if [ -x "$BIN/$1" ]; then echo "$1"; else echo "$1" | tr 'A-Z' 'a-z'; fi;; esac
}
run() {
    exe="$BIN/$(name "$1")$EXT"
    [ -d "$BIN/$1.app" ] && exe="$BIN/$1.app/Contents/MacOS/$1"
    ( cd "$OUT" && "$exe" --size 1000x680 --dir "$OUT/docs" --script "$HERE/$2" )
    echo "ok: $1 $2"
}
magic() { od -An -tx1 -N4 "$1" | tr -d ' \n'; }
run UnoWord word_save.txt
[ "$(magic "$OUT/docs/Document1")" = "d0cf11e0" ] || { echo "FAIL: UnoWord did not save a .doc"; exit 1; }
# a document in a SECOND folder, for word_open.txt's plain Save to return to
mkdir -p "$OUT/other" && cp "$OUT/docs/Document1" "$OUT/other/Letter.doc"
run UnoWord word_open.txt
[ ! -e "$OUT/docs/Letter.doc" ] || { echo "FAIL: Save wrote to volume 0, not back to other/"; exit 1; }
cmp -s "$OUT/docs/Document1" "$OUT/other/Letter.doc" &&
    { echo "FAIL: the edited Letter.doc was not saved back to other/"; exit 1; }
[ "$(magic "$OUT/other/Letter.doc")" = "d0cf11e0" ] || { echo "FAIL: other/Letter.doc is not a .doc"; exit 1; }
run UnoCalc calc.txt
run UnoShow show.txt
[ "$(magic "$OUT/docs/Book1.xls")" = "d0cf11e0" ] || { echo "FAIL: UnoCalc did not save a .xls"; ls "$OUT/docs"; exit 1; }
n=$(ls "$OUT"/*.ppm | wc -l)
[ "$n" -ge 9 ] || { echo "FAIL: only $n frames written"; exit 1; }
echo "smoke: PASS ($n frames in $OUT)"
