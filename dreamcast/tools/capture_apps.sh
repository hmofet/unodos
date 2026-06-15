#!/bin/bash
# Build a self-driving UNO_AUTOTEST_<app> .cdi for each app and screenshot it in
# Flycast via emu_run.sh -> shots/dc_<app>.png. Needs KallistiOS sourced + the
# emulator rig (see emu_run.sh). Run from a KOS-aware shell under WSL.
exec > /mnt/c/Users/arin/Documents/Github/unodos/dreamcast/tools/capture_apps.log 2>&1
source /root/KallistiOS/environ.sh
cd /mnt/c/Users/arin/Documents/Github/unodos/dreamcast
CF="-O2 -Wall -Wno-multichar -Wno-unused-value -DUNO_DC -DUNO_COLOR=1 -I. -Ibuild"
SRC="dc_main.c fb.c mac_compat.c mac_io.c unodos.c"
build_run() {  # $1=define-suffix  $2=tag  $3=secs
  echo "=== $2 ==="
  kos-cc $CF -DUNO_AUTOTEST_$1 -o build/v_$2.elf $SRC $KOS_LIBS 2>&1 | grep -i error
  ~/mkdcdisc/build/mkdcdisc -e build/v_$2.elf -o build/v_$2.cdi -n UNODOS -N >/dev/null 2>&1
  bash tools/emu_run.sh build/v_$2.cdi shots/dc_$2.png "${3:-16}" >/dev/null 2>&1
  ls -la shots/dc_$2.png
}
build_run PACMAN  pacman 16
build_run PAINT   paint  16
build_run THEME   theme  16
build_run DOSTRIS dostris 16
build_run TRACKER tracker 16
build_run FILES   files  16
build_run OUTLAST outlast 16
# stack = Music + Files + Notepad (plain UNO_AUTOTEST)
echo "=== stack ==="
kos-cc $CF -DUNO_AUTOTEST -o build/v_stack.elf $SRC $KOS_LIBS 2>&1 | grep -i error
~/mkdcdisc/build/mkdcdisc -e build/v_stack.elf -o build/v_stack.cdi -n UNODOS -N >/dev/null 2>&1
bash tools/emu_run.sh build/v_stack.cdi shots/dc_stack.png 16 >/dev/null 2>&1
ls -la shots/dc_stack.png
echo "=== ALL DONE ==="
