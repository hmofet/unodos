#!/bin/sh
# unoui scrolling-list host gate: build the toolkit core + the default theme
# against the assertions in tools/list_test.c and run them.  Seconds, no QEMU -
# run it after every edit to the UI_LIST paths (painter, hit test, input).
set -e
cd "$(dirname "$0")/.."
FB=../ps2
[ -f "$FB/build/font_data.h" ] || ( cd "$FB" && ${PY:-python3} mkfont_c.py )
mkdir -p build
${CC:-cc} -O1 -Wall -Wextra -I. -I"$FB" -o build/list_test \
    tools/list_test.c unoui.c unoui_input.c themes/theme_unodos.c \
    "$FB/fb.c" "$FB/fb_aa.c"
./build/list_test
