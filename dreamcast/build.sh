#!/bin/sh
# UnoDOS/Dreamcast build.
#
#   ./build.sh [host]   software-framebuffer splash via the host compiler,
#                       renders shots/m0_splash.png. VERIFIED on the PC (run in
#                       WSL on this Windows box: gcc 13 + python3 there).
#   ./build.sh desktop [FEATURE]
#                       the FULL UnoDOS desktop / WM / apps via the Mac-compat
#                       shim over fb.*, built with the host compiler, rendered to
#                       shots/m1_<tag>.png. FEATURE bakes in -DUNO_AUTOTEST_<F>
#                       (PACMAN/PAINT/THEME/DOSTRIS/TRACKER/FILES/OUTLAST/FAT12,
#                       or "stack"); empty = the bare desktop. VERIFIED.
#   ./build.sh dc [FEATURE]
#                       the Dreamcast ELF (build/unodos-dc.elf) via KallistiOS.
#                       Needs the KOS environment ($KOS_BASE) - sources
#                       $KOS_BASE/../environ.sh if KOS_BASE is unset. UNVERIFIED
#                       here (no sh-elf-gcc / DC emulator on the dev machine).
#   ./build.sh iso [FEATURE]
#                       dc + a bootable selfboot CD image (build/unodos-dc.iso)
#                       via the KOS tools + genisoimage.
#   ./build.sh cdi [FEATURE]
#                       iso, converted to .cdi if cdi4dc/mkdcdisc is present.
#
# All targets regenerate build/font_data.h from the shared font first.
set -e
cd "$(dirname "$0")"
PY="${PY:-python3}"
mkdir -p build shots uno_disk

echo "[1/2] exporting the shared font to a C array..."
(cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i >/dev/null)
"$PY" mkfont_c.py

case "$1" in
  desktop)
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
    [ -f uno_disk/README.TXT ] || printf 'UnoDOS Dreamcast milestone 2\rNotepad reads this file\rfrom the VMU volume.' > uno_disk/README.TXT
    UNO_OUT="shots/m1_$TAG.ppm" ./build/host_desktop
    "$PY" tools/ppm2png.py "shots/m1_$TAG.ppm" "shots/m1_$TAG.png"
    echo "done: shots/m1_$TAG.png" ;;
  dc|cdi|iso)
    echo "[2/2] building the Dreamcast ELF (KallistiOS)..."
    # source the KOS environment if not already present
    if [ -z "$KOS_BASE" ]; then
      for E in /opt/toolchains/dc/kos/environ.sh "$HOME/KallistiOS/environ.sh" \
               "$HOME/dc/kos/environ.sh"; do
        [ -f "$E" ] && { . "$E"; break; }
      done
    fi
    if [ -z "$KOS_BASE" ]; then
      echo "ERROR: KallistiOS not found. Install it and source environ.sh, or set KOS_BASE." >&2
      echo "       (see README.md - Toolchain). The host targets above need no KOS." >&2
      exit 1
    fi
    EXTRA_DEF=""
    [ -n "$2" ] && EXTRA_DEF="-DUNO_AUTOTEST_$2"
    make clean >/dev/null 2>&1 || true
    case "$1" in
      cdi) make cdi EXTRA_DEF="$EXTRA_DEF"; echo "done: build/unodos-dc.cdi (or .iso) ${2:+(AUTOTEST_$2)}" ;;
      iso) make iso EXTRA_DEF="$EXTRA_DEF"; echo "done: build/unodos-dc.iso ${2:+(AUTOTEST_$2)}" ;;
      *)   make EXTRA_DEF="$EXTRA_DEF";     echo "done: build/unodos-dc.elf ${2:+(AUTOTEST_$2)}" ;;
    esac ;;
  *)
    echo "[2/2] building + running the host splash..."
    CC="${CC:-gcc}"
    "$CC" -O2 -Wall -o build/host_splash fb.c uno_splash.c host_main.c
    ./build/host_splash shots/m0_splash.ppm "${2:-320}" "${3:-240}"
    "$PY" tools/ppm2png.py shots/m0_splash.ppm shots/m0_splash.png
    echo "done: shots/m0_splash.png" ;;
esac
