#!/bin/sh
# unoui MDI host gate: build the toolkit core + the default theme against the
# assertions in tools/mdi_test.c and run them.  Seconds, no QEMU - run it after
# every edit to the UI_MDI paths (layout, z-order, draw, hit test, input).
# The containment checks are the ones that matter: a child that escapes its
# container paints and takes clicks over whatever is beside it.
set -e
cd "$(dirname "$0")/.."
FB=../ps2
[ -f "$FB/build/font_data.h" ] || ( cd "$FB" && ${PY:-python3} mkfont_c.py )
mkdir -p build
${CC:-cc} -O1 -Wall -Wextra -I. -I"$FB" -o build/mdi_test \
    tools/mdi_test.c unoui.c unoui_input.c themes/theme_unodos.c \
    "$FB/fb.c" "$FB/fb_aa.c"
./build/mdi_test
