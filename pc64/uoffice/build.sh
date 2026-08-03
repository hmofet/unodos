#!/bin/sh
# uoffice host gate - build and run the chrome storyboard without booting the
# OS, over the same sources the app module compiles freestanding.
#
#   ./build.sh            build + run + storyboard -> build/uochrome.png
#
# Links the shared software framebuffer (../../ps2/fb.c + fb_aa.c) exactly as
# unoui's harness does, so what the storyboard shows is what pc64 draws.
#
# Sanitizers are build.sh's own set plus ASan, and -fno-sanitize-recover: the
# UnoAmp EQ lesson (docs/OFFICE97-PLAN.md §9) is that a harness built WITHOUT
# the OS's flags is testing different code, and that mistake cost a day.
set -e
cd "$(dirname "$0")"
mkdir -p build
CC="${CC:-gcc}"
PY="${PY:-python3}"
FB=../../ps2
UI=../../unoui

SAN="-fsanitize=address,undefined \
     -fsanitize=signed-integer-overflow,bounds,shift,integer-divide-by-zero,null \
     -fno-sanitize-recover=all"

# the prebuilt shared font header the fb text primitives need
if [ ! -f "$FB/build/font_data.h" ]; then
    ( cd "$FB" && $PY mkfont_c.py )
fi

rm -f build/*.ppm

# shellcheck disable=SC2086
CORE="uochrome.c uoicons.c uodlg.c uobars.c uofile.c $FB/fb.c $FB/fb_aa.c"
CC_ALL="$CC -O1 -g -std=c99 -Wall -Wextra -Werror -I. -I$FB -I$UI $SAN"

# shellcheck disable=SC2086
$CC_ALL $CORE ../tools/uochrome_test.c -o build/uochrome_test
# shellcheck disable=SC2086
$CC_ALL $CORE ../tools/uodlg_test.c    -o build/uodlg_test
# shellcheck disable=SC2086
$CC_ALL $CORE ../tools/uobars_test.c   -o build/uobars_test

./build/uochrome_test build
./build/uodlg_test build
./build/uobars_test build

if [ -f "$UI/tools/tile.py" ]; then
    # shellcheck disable=SC2046
    $PY "$UI/tools/tile.py" build/uochrome.png 3 $(ls build/uoc_*.ppm | sort)
    # shellcheck disable=SC2046
    $PY "$UI/tools/tile.py" build/uodlg.png 3 $(ls build/uod_*.ppm | sort)
    # shellcheck disable=SC2046
    $PY "$UI/tools/tile.py" build/uobars.png 3 $(ls build/uob_*.ppm | sort)
    echo "storyboards: build/uochrome.png build/uodlg.png build/uobars.png"
fi
