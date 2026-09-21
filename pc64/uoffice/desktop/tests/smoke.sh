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
run() {       # run APP SCRIPT [DOCUMENT]
    exe="$BIN/$(name "$1")$EXT"
    [ -d "$BIN/$1.app" ] && exe="$BIN/$1.app/Contents/MacOS/$1"
    ( cd "$OUT" && "$exe" --size 1000x680 --dir "$OUT/docs" --script "$HERE/$2" $3 ) ||
        { echo "FAIL: $1 $2 (exit $?)"; exit 1; }
    echo "ok: $1 $2"
}
magic() { od -An -tx1 -N4 "$1" | tr -d ' \n'; }
run UnoWord word_save.txt
run UnoWord word_clip.txt
run UnoWord word_keys.txt
# Ctrl+B before typing: the new paragraph was saved bold
if command -v unzip >/dev/null 2>&1; then
    unzip -p "$OUT/docs/Keys.docx" word/document.xml | grep -q "<w:b/>" ||
        { echo "FAIL: text typed after Ctrl+B was not saved bold"; exit 1; }
fi
[ "$(magic "$OUT/docs/Document1.doc")" = "d0cf11e0" ] || { echo "FAIL: UnoWord did not save a .doc"; exit 1; }
# a document in a SECOND folder, for word_open.txt's plain Save to return to
mkdir -p "$OUT/other" && cp "$OUT/docs/Document1.doc" "$OUT/other/Letter.doc"
run UnoWord word_open.txt
[ ! -e "$OUT/docs/Letter.doc" ] || { echo "FAIL: Save wrote to volume 0, not back to other/"; exit 1; }
cmp -s "$OUT/docs/Document1.doc" "$OUT/other/Letter.doc" &&
    { echo "FAIL: the edited Letter.doc was not saved back to other/"; exit 1; }
[ "$(magic "$OUT/other/Letter.doc")" = "d0cf11e0" ] || { echo "FAIL: other/Letter.doc is not a .doc"; exit 1; }
# opened from the command line, as a double click does
run UnoWord word_arg.txt other/Letter.doc
# the save-changes guard: Cancel, No, Yes-in-place, Yes-on-close
cp "$OUT/other/Letter.doc" "$OUT/letter.before"
cp "$OUT/docs/Document1.doc" "$OUT/doc1.before"
run UnoWord word_guard.txt
cmp -s "$OUT/letter.before" "$OUT/other/Letter.doc" &&
    { echo "FAIL: Yes did not save Letter.doc before the next file opened"; exit 1; }
cmp -s "$OUT/doc1.before" "$OUT/docs/Document1.doc" &&
    { echo "FAIL: Yes on close did not save Document1.doc"; exit 1; }
run UnoCalc calc.txt
run UnoCalc calc_clip.txt
run UnoShow show.txt
[ "$(magic "$OUT/docs/Book1.xls")" = "d0cf11e0" ] || { echo "FAIL: UnoCalc did not save a .xls"; ls "$OUT/docs"; exit 1; }
run UnoCalc calc_arg.txt docs/Book1.xls
run UnoCalc calc_guard.txt
[ "$(magic "$OUT/docs/Guard.xls")" = "d0cf11e0" ] || { echo "FAIL: closing with Yes did not Save As first"; exit 1; }
n0=$(ls "$OUT/docs" | wc -l)
run UnoShow show_guard.txt
[ "$(ls "$OUT/docs" | wc -l)" = "$n0" ] || { echo "FAIL: No wrote a file anyway"; exit 1; }
n=$(ls "$OUT"/*.ppm | wc -l)
[ "$n" -ge 9 ] || { echo "FAIL: only $n frames written"; exit 1; }
echo "smoke: PASS ($n frames in $OUT)"
