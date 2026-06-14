#!/bin/sh
# UnoDOS/PS2 build.
#
#   ./build.sh [host]   software-framebuffer splash via the host compiler,
#                       renders shots/m0_splash.png. VERIFIED on the PC (run
#                       in WSL on this Windows box: gcc 13 + python3 13 there).
#   ./build.sh ee       the PS2 ELF via PS2SDK (ee-gcc + gsKit). Needs $PS2SDK
#                       (the ps2dev Docker image or a PS2SDK install). UNVERIFIED
#                       here - no ee-gcc / PCSX2 / BIOS on the dev machine.
#
# Both regenerate build/font_data.h from the shared font first.
set -e
cd "$(dirname "$0")"
PY="${PY:-python3}"
mkdir -p build shots

echo "[1/2] exporting the shared font to a C array..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i >/dev/null)
"$PY" mkfont_c.py

case "$1" in
  desktop)
    # M1+ : the full UnoDOS desktop / WM / apps via the Mac-compat shim
    # (mac_compat.* + mac_io.*) over fb.*, built with the host compiler.
    #   ./build.sh desktop            -> plain desktop  -> shots/m1_desktop.png
    #   ./build.sh desktop PACMAN     -> AUTOTEST_PACMAN -> shots/m1_pacman.png
    # FEATURE matches the UNO_AUTOTEST_<FEATURE> hooks in unodos.c (PACMAN,
    # PAINT, THEME, DOSTRIS, TRACKER, FILES, OUTLAST, FAT12) or AUTOTEST (the
    # Music+Files+Notepad stack). Empty = the bare desktop.
    echo "[2/2] building the host desktop (Mac-compat shim)..."
    CC="${CC:-gcc}"
    FEAT="$2"
    DEF=""; TAG="desktop"
    if [ -n "$FEAT" ]; then
      if [ "$FEAT" = "stack" ]; then DEF="-DUNO_AUTOTEST"; TAG="stack";
      else DEF="-DUNO_AUTOTEST_$FEAT"; TAG=$(echo "$FEAT" | tr 'A-Z' 'a-z'); fi
    fi
    "$CC" -O2 -DUNO_COLOR=1 -DUNO_HOST $DEF -I. \
        -o build/host_desktop fb.c mac_compat.c mac_io.c unodos.c host_desktop.c
    mkdir -p uno_disk
    [ -f uno_disk/README.TXT ] || printf 'UnoDOS PS2 milestone 2\rNotepad reads this file\rfrom the Files volume.' > uno_disk/README.TXT
    UNO_OUT="shots/m1_$TAG.ppm" ./build/host_desktop
    "$PY" tools/ppm2png.py "shots/m1_$TAG.ppm" "shots/m1_$TAG.png"
    echo "done: shots/m1_$TAG.png" ;;
  ee|ee-splash)
    # ./build.sh ee [FEATURE]  -> the full desktop ELF (build/unodos-ps2.elf).
    #                             FEATURE bakes in -DUNO_AUTOTEST_<FEATURE> for a
    #                             self-driving PCSX2 screenshot (PACMAN, PAINT,
    #                             THEME, ...); empty = the interactive desktop.
    # ./build.sh ee-splash     -> the M0 hello-GS splash ELF (reference).
    echo "[2/2] building the PS2 ELF (PS2SDK)..."
    # default to the prebuilt ps2dev toolchain layout (this machine: WSL,
    # ~/ps2dev/ps2dev from the ps2dev v2.0.0 release). Override PS2DEV to point
    # elsewhere.
    : "${PS2DEV:=$HOME/ps2dev/ps2dev}"
    export PS2DEV
    export PS2SDK="${PS2SDK:-$PS2DEV/ps2sdk}"
    export GSKIT="${GSKIT:-$PS2DEV/gsKit}"
    export PATH="$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2SDK/bin:$PATH"
    make clean >/dev/null 2>&1 || true
    if [ "$1" = "ee-splash" ]; then
        make splash
        echo "done: build/unodos-ps2-splash.elf"
    else
        EXTRA_DEF=""
        [ -n "$2" ] && EXTRA_DEF="-DUNO_AUTOTEST_$2"
        make EXTRA_DEF="$EXTRA_DEF"
        echo "done: build/unodos-ps2.elf ${2:+(AUTOTEST_$2)}"
    fi ;;
  *)
    echo "[2/2] building + running the host splash..."
    CC="${CC:-gcc}"
    "$CC" -O2 -Wall -o build/host_splash fb.c uno_splash.c host_main.c
    ./build/host_splash shots/m0_splash.ppm "${2:-320}" "${3:-224}"
    "$PY" tools/ppm2png.py shots/m0_splash.ppm shots/m0_splash.png
    echo "done: shots/m0_splash.png" ;;
esac
