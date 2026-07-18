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
#   ./build.sh uui-host the unoui/Aurora shell rendered on the host (software
#                       fb -> shots/aurora.png). No KOS needed; the pixels are
#                       byte-identical to what `dc uui` presents on hardware.
#   ./build.sh dc uui   the unoui shell (modern desktop + Aurora theme) ELF
#                       (build/unodos-dc-uui.elf) + a bootable image
#                       (build/unodos-dc-uui.cdi via mkdcdisc, else .iso). The DC
#                       analogue of `ps2 ee uui`: fb + fb_aa + dc_uui + unoui +
#                       themes, Aurora FULL (32-bit internal -> RGB565 out).
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
  uui-host)
    # host render of the unoui/Aurora shell (dc_uui.c's scene) -> shots/aurora.png.
    # The SAME portable software path the DC runs (unoui + theme_aurora + fb_aa
    # over fb.c); only the present differs (console = fb->565, host = fb->PPM).
    echo "[2/2] rendering the unoui/Aurora shell on the host (software fb)..."
    CC="${CC:-gcc}"
    "$CC" -O2 -Wall -Wno-unused-value -Wno-multichar -DUNO_COLOR=1 -DUNO_UUI -DUNO_BG_CACHE \
        -I. -Ibuild -I../unoui \
        host_uui.c fb.c fb_aa.c ../unoui/unoui.c ../unoui/unoui_input.c \
        ../unoui/themes/theme_aurora.c -o build/host_uui
    ./build/host_uui shots/aurora.ppm
    "$PY" tools/ppm2png.py shots/aurora.ppm shots/aurora.png
    echo "done: shots/aurora.png" ;;
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

    # --- `dc uui`: the unoui shell (modern desktop + Aurora), the DC analogue of
    # ps2's `ee uui`. Self-contained kos-cc compile of fb + fb_aa + dc_uui +
    # unoui + the themes (the Makefile builds the legacy Mac core instead), then
    # a bootable image for the emulator. -ffunction/-fdata-sections + --gc-sections
    # keeps the static desktop demo lean; -DUNO_BG_CACHE bakes the Aurora bg once.
    if [ "$2" = "uui" ]; then
      echo "[2/2] building the unoui shell ELF (KallistiOS + unoui + Aurora)..."
      INCS="-I. -Ibuild -I../unoui"
      CF="-O2 -Wall -Wno-unused-value -Wno-multichar -ffunction-sections -fdata-sections \
          -DUNO_COLOR=1 -DUNO_DC -DUNO_UUI -DUNO_BG_CACHE"
      OBJS=""
      for s in fb fb_aa dc_uui; do
        kos-cc $CF $INCS -c "$s.c" -o "build/$s.o"; OBJS="$OBJS build/$s.o"
      done
      for u in unoui unoui_input; do
        kos-cc $CF $INCS -c "../unoui/$u.c" -o "build/$u.o"; OBJS="$OBJS build/$u.o"
      done
      for t in ../unoui/themes/*.c; do
        b=$(basename "$t" .c)
        kos-cc $CF $INCS -c "$t" -o "build/th_$b.o"; OBJS="$OBJS build/th_$b.o"
      done
      kos-cc -Wl,--gc-sections -o build/unodos-dc-uui.elf $OBJS $KOS_LIBS
      echo "done: build/unodos-dc-uui.elf (unoui shell + Aurora)"
      # a bootable image: prefer mkdcdisc, else the scramble + genisoimage path.
      if command -v mkdcdisc >/dev/null 2>&1; then
        mkdcdisc -e build/unodos-dc-uui.elf -o build/unodos-dc-uui.cdi -n "UnoDOS Aurora" -N \
          && echo "done: build/unodos-dc-uui.cdi"
      else
        "$KOS_CC_BASE/bin/$KOS_CC_PREFIX-objcopy" -R .stack -O binary \
          build/unodos-dc-uui.elf build/uui_unscr.bin
        "$KOS_BASE/utils/scramble/scramble" build/uui_unscr.bin build/1ST_READ.BIN
        [ -f build/IP.BIN ] || "$KOS_BASE/utils/makeip/makeip" -f build/IP.BIN
        rm -rf build/uui_iso && mkdir -p build/uui_iso && cp build/1ST_READ.BIN build/uui_iso/
        genisoimage -C 0,11702 -V UNODOS -G build/IP.BIN -joliet -rock -l \
          -o build/unodos-dc-uui.iso build/uui_iso && echo "done: build/unodos-dc-uui.iso"
      fi
      exit 0
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
