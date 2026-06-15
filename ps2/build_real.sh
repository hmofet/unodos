#!/bin/sh
# ===========================================================================
# Build + run the REAL refactored UnoDOS core (unodos.c) on the host (WSL gcc).
# ===========================================================================
# This is the genuine proof of the runtime-app-loading architecture: the SAME
# unodos.c that the PS2/DC/Mac ports compile, with ALL app code removed and the
# pointer-based loader (app_loader.c) #included.  Each of the 11 apps is built
# as a SEPARATE module (.so) into apps_store/ and dlopen'd at runtime by
# host_modload.c.  The kernel object has ZERO app symbols (proven by nm below);
# every window's draw/key/tick comes from a loaded module.
#
#   build/uno_realcore   - the app-free kernel, -rdynamic so modules resolve it
#   apps_store/appNN.so  - the 11 app modules ("storage")
#   shots/real_*.png     - per-app screenshots (desktop + each app as a module)
# ===========================================================================
set -e
cd "$(dirname "$0")"
PY="${PY:-python3}"
CC="${CC:-gcc}"
mkdir -p build apps_store shots tools

echo "[1/6] export the shared 8x8 font"
( cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i >/dev/null )
"$PY" mkfont_c.py >/dev/null

echo "[2/6] refactor unodos.c -> the app-free core (from the pristine original)"
# tools/unodos_orig_ps2.c is the pristine pre-refactor core kept for reproducible
# regeneration; if absent, seed it from the current unodos.c.
[ -f tools/unodos_orig_ps2.c ] || cp unodos.c tools/unodos_orig_ps2.c
"$PY" tools/refactor_core.py tools/unodos_orig_ps2.c unodos.c

echo "[3/6] build the REAL core (no app code, -rdynamic)"
$CC -O2 -DUNO_COLOR=1 -DUNO_HOST -I. -rdynamic \
    -o build/uno_realcore \
    unodos.c fb.c mac_compat.c mac_io.c uno_splash.c host_modload.c host_desktop.c -ldl

echo "[3b] PROOF: the core object has zero app symbols"
$CC -O2 -DUNO_COLOR=1 -DUNO_HOST -I. -c unodos.c -o build/unodos.o
# match app function/data symbols by their distinctive prefixes; exclude the
# kernel's repaint_all (contains "paint") and the kept music synth primitives.
APP_SYMS=$(nm build/unodos.o \
  | grep -iE ' (T|t|D|d|B|b) (pacman_|dostris_|theme_|sysinfo_|files_|notepad_|tracker_|tk_|paint_|pt_|outlast_|ol_|clock_draw|music_draw|music_key|music_tick|music_select|dt_|pm_|gPm|gDt|gOl|gPt|gNBuf|gTk|kThemes|kSongs|kCanon)' \
  | grep -ivE ' repaint_all' || true)
if [ -n "$APP_SYMS" ]; then
    echo "  !! FAIL: app symbols still in core object:"; echo "$APP_SYMS"; exit 1
fi
echo "  OK: nm build/unodos.o shows no app symbols (pacman_/dostris_/theme_/...)."

echo "[4/6] build each of the 11 apps as a separate module (.so) into apps_store/"
build_mod() { # <id> <src>
  $CC -O2 -DUNO_COLOR=1 -DUNO_HOST -I. -fPIC -shared \
      -o "apps_store/app$1.so" "apps/$2"
  N=$(nm -D "apps_store/app$1.so" | grep -c ' T uno_app_main' || true)
  echo "    apps_store/app$1.so  <- apps/$2   (uno_app_main exports: $N)"
}
build_mod 00 sysinfo.c
build_mod 01 clock.c
build_mod 02 files.c
build_mod 03 notepad.c
build_mod 04 music.c
build_mod 05 dostris.c
build_mod 06 outlast.c
build_mod 07 pacman.c
build_mod 08 tracker.c
build_mod 09 paint.c
build_mod 10 theme.c

echo "[5/6] run the core: dlopen each module from storage + dispatch + shoot"
ppm2png() { "$PY" tools/ppm2png.py "$1" "$2"; }

# desktop stack (several apps as modules at once)
UNO_OUT=shots/real_desktop.ppm ./build/uno_realcore
ppm2png shots/real_desktop.ppm shots/real_desktop.png

# one shot per app, each loaded purely as a module (id passed without the
# leading zero so /bin/sh arithmetic in the core's atoi() is unambiguous)
NAMES="0:sysinfo 1:clock 2:files 3:notepad 4:music 5:dostris 6:outlast 7:pacman 8:tracker 9:paint 10:theme"
for pair in $NAMES; do
    id=${pair%%:*}; name=${pair##*:}
    UNO_APP=$id UNO_OUT=shots/real_$name.ppm ./build/uno_realcore
    ppm2png shots/real_$name.ppm shots/real_$name.png
done

echo "[6/6] done.  Shots in shots/real_*.png; modules in apps_store/."
