#!/bin/sh
# unoui build - the portable UI toolkit + the write-once demo, host software
# path. Renders the SAME demo window under every theme -> one PPM each -> PNGs
# -> a tiled contact sheet. The exact same unoui.c + themes/*.c build into the
# PS2/Dreamcast ports (they already link fb.c); only the glue main differs.
#
#   ./build.sh            build + render + contact sheet -> build/themes.png
#
# Reuses the shared software framebuffer (../ps2/fb.c) and its 8x8 font, the
# same way uno3d's host target does.
set -e
cd "$(dirname "$0")"
mkdir -p build
CC="${CC:-gcc}"
PY="${PY:-python3}"
FB=../ps2

# the prebuilt shared font header the fb text primitives need
if [ ! -f "$FB/build/font_data.h" ]; then
    ( cd "$FB" && $PY mkfont_c.py )
fi

THEMES="themes/theme_unodos.c themes/theme_macos7.c themes/theme_macplus.c \
        themes/theme_win31.c  themes/theme_amiga.c  themes/theme_c64.c \
        themes/theme_apple2.c themes/theme_next.c"
CORE="unoui.c unoui_input.c $THEMES $FB/fb.c $FB/fb_aa.c"

rm -f build/*.ppm

# 1) the THEME contact sheet (static write-once window under every theme)
# shellcheck disable=SC2086
$CC -O2 -Wall -I. -I"$FB" $CORE unoui_demo.c host_unoui.c -o build/host_unoui
./build/host_unoui build
$PY tools/tile.py build/themes.png 4 \
    build/00_*.ppm build/01_*.ppm build/02_*.ppm build/03_*.ppm \
    build/04_*.ppm build/05_*.ppm build/06_*.ppm build/07_*.ppm

# 2) the INTERACTIVE storyboard (one scripted event stream -> states)
# shellcheck disable=SC2086
$CC -O2 -Wall -I. -I"$FB" $CORE unoui_app.c host_unoui_input.c -o build/host_unoui_input
./build/host_unoui_input build
# wider crop: both windows span x18..618
$PY tools/tile.py build/storyboard.png 3 18,0,600,410 $(ls build/in_*.ppm | sort)

# per-frame PNGs for inspection
for p in build/*.ppm; do
    $PY "$FB/tools/ppm2png.py" "$p" "${p%.ppm}.png" >/dev/null
done

echo "done: build/themes.png + build/storyboard.png  (+ per-frame PNGs)"
