#!/bin/sh
# unoui tabbed-document host gate: build the toolkit core + the default theme
# against the assertions in tools/tabs_test.c and run them.  Seconds, no QEMU -
# run it after every edit to the UI_TABS paths (layout, painter, hit test,
# input).  The sweep in there is what stops the painter and the hit test
# drifting apart again.
set -e
cd "$(dirname "$0")/.."
FB=../ps2
[ -f "$FB/build/font_data.h" ] || ( cd "$FB" && ${PY:-python3} mkfont_c.py )
mkdir -p build
${CC:-cc} -O1 -Wall -Wextra -I. -I"$FB" -o build/tabs_test \
    tools/tabs_test.c unoui.c unoui_input.c themes/theme_unodos.c \
    "$FB/fb.c" "$FB/fb_aa.c"
./build/tabs_test
