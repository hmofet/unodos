#!/bin/sh
# Build + run the runtime-app-loading DEMONSTRATOR on the host (WSL gcc):
#   - the kernel (demo_kernel.c + fb.c + mac_compat.c + the loader) with NO app
#     code, exporting its symbols (-rdynamic) so loaded modules resolve them;
#   - each app compiled to a SEPARATE .so in apps_store/ (the "storage");
#   - run: the kernel dlopen's each module from apps_store/ at runtime and
#     dispatches through the AppInterface pointers, rendering shots/*.ppm.
set -e
cd "$(dirname "$0")"
PY="${PY:-python3}"
CC="${CC:-gcc}"
mkdir -p build apps_store shots

echo "[1/4] export the shared 8x8 font"
( cd .. && "$PY" amiga/mkdata.py amiga/gen_data.i >/dev/null )
"$PY" mkfont_c.py >/dev/null

echo "[2/4] build the kernel (no app code, -rdynamic)"
$CC -O2 -DUNO_COLOR=1 -DUNO_HOST -I. -rdynamic \
    -o build/uno_modkernel \
    fb.c mac_compat.c demo_kernel.c host_modload.c host_desktop.c -ldl

echo "[3/4] build each app as a separate module (.so) into apps_store/"
build_mod() { # <id> <src>
  $CC -O2 -DUNO_COLOR=1 -DUNO_HOST -I. -fPIC -shared \
      -o "apps_store/app$1.so" "apps/$2"
  echo "    apps_store/app$1.so  <- apps/$2"
}
build_mod 00 sysinfo.c
build_mod 02 files.c
build_mod 05 dostris.c
build_mod 07 pacman.c
build_mod 10 theme.c

echo "[4/4] run: kernel loads modules from storage + dispatches"
ls -la apps_store/*.so | awk '{print "    "$5" bytes  "$9}'
UNO_OUT=shots/m_modular.ppm ./build/uno_modkernel
"$PY" - <<'PYEOF'
# PPM -> PNG for easy viewing
import struct
try:
    from PIL import Image
    im = Image.open("shots/m_modular.ppm"); im.save("shots/m_modular.png")
    print("wrote shots/m_modular.png")
except Exception as e:
    print("(PNG convert skipped: %s)" % e)
PYEOF
echo "done"
