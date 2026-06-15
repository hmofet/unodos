#!/bin/bash
# ===========================================================================
# Build self-driving UNO_AUTOTEST_<app> Dreamcast images that GENUINELY load
# the app module .KLF from the CD at runtime, and screenshot each in Flycast.
#
# Unlike the older tools/capture_apps.sh (which baked apps into the ELF), every
# variant here:
#   1. builds an APP-FREE autotest ELF (the core auto-launches one app, which
#      triggers uno_load_module -> library_open of /cd/UNODOS/APPS/APPNN.KLF);
#   2. builds the 11 .KLF modules (make modules);
#   3. packages BOTH the ELF (as 1ST_READ.BIN) AND the .KLF data track into one
#      .cdi via mkdcdisc -e <elf> -d cd_root/UNODOS, so the modules are read
#      off the CD by the running kernel - real load-from-storage.
#   4. boots it headless in Flycast (tools/emu_run.sh) -> shots/dc_native_<tag>.png
#
# Run from a KOS-aware shell under WSL:
#   source /root/KallistiOS/environ.sh
#   bash tools/capture_native.sh
# ===========================================================================
set -e
cd /mnt/c/Users/arin/Documents/Github/unodos/dreamcast
source /root/KallistiOS/environ.sh

CF="-O2 -Wall -Wno-multichar -Wno-unused-value -DUNO_DC -DUNO_COLOR=1 -I. -Ibuild"
SRC="dc_main.c fb.c mac_compat.c mac_io.c unodos.c dc_modload.c"

# 1) the 11 .KLF modules (shared by every variant's data track)
echo "[modules] building app_00.klf .. app_10.klf"
make modules >/dev/null 2>&1

# stage the CD data tree once (the modules every variant loads from /cd)
rm -rf build/cd_root && mkdir -p build/cd_root/UNODOS/APPS
for i in 00 01 02 03 04 05 06 07 08 09 10; do
  cp build/app_$i.klf build/cd_root/UNODOS/APPS/APP$i.KLF
done

build_run() {  # $1=AUTOTEST suffix  $2=tag  $3=secs
  echo "=== $2 (loads APP module from CD) ==="
  kos-cc $CF -DUNO_AUTOTEST_$1 -o build/n_$2.elf $SRC $KOS_LIBS 2>&1 | grep -i error || true
  mkdcdisc -e build/n_$2.elf -d build/cd_root/UNODOS -o build/n_$2.cdi -n UNODOS -N >/dev/null 2>&1
  bash tools/emu_run.sh build/n_$2.cdi shots/dc_native_$2.png "${3:-16}" >/dev/null 2>&1
  ls -la shots/dc_native_$2.png
}

build_run PACMAN  pacman  18
build_run THEME   theme   16
build_run DOSTRIS dostris 16
build_run FILES   files   16
build_run PAINT   paint   16
build_run TRACKER tracker 16

echo "=== ALL DONE ==="
