#!/bin/sh
# unoui editable-text geometry gate: build the toolkit core + the default theme
# against the assertions in tools/edit_test.c and run them.  Seconds, no QEMU -
# run it after every edit to the caret / hit-test / reveal paths.
#
# It links pc64/fb.c rather than ps2/fb.c (which the other host gates use)
# because only pc64's framebuffer has the pluggable text provider, and a
# PROPORTIONAL font is the whole point: the bug this pins down is invisible
# under a fixed 8 px bitmap font.
set -e
cd "$(dirname "$0")/.."
FB=../pc64
[ -f "$FB/build/font_data.h" ] || ( cd "$FB" && ${PY:-python3} mkfont_c.py )
mkdir -p build
${CC:-cc} -O1 -Wall -Wextra -I. -I"$FB" -o build/edit_test \
    tools/edit_test.c unoui.c unoui_input.c themes/theme_unodos.c \
    "$FB/fb.c"
./build/edit_test
