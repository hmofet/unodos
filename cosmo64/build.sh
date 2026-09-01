#!/bin/sh
# UnoDOS pc64-on-ARM (Cosmo Communicator, MT6771) build.
#
# Toolchain: llvm-mingw's aarch64-w64-mingw32-clang on quill -- PE/COFF and
# LLP64, the same object format and data model as the x86 pc64, per the
# toolchain decision in research/pc64-arm-port-plan.md (hmofet/cosmo). Images
# link at LK's load address so the flat payload needs no relocation;
# flatten.py lays the PE out (and stamps an ARM64 Image header, so the same
# binary boots under `qemu -kernel`) and the asm port's mkbootimg.py wraps it
# for the p38 slot.
#
#   ./build.sh          -> the m0/m1 TEST payload  (build/m0.bin + boot img)
#   ./build.sh shell    -> the pc64 SHELL          (build/shell.bin + boot img)
#
# Verify on quill (real QEMU; see qharness.py):
#   scp qharness.py build/<x>.bin quill:/work/unodos-cosmo64/ &&
#   ssh quill 'cd /work/unodos-cosmo64 && python3 qharness.py <x>.bin /tmp/x.png 3'
set -e
cd "$(dirname "$0")"

PY="${PY:-python3}"
QUILL="${QUILL:-arin@192.168.2.114}"
QDIR="/work/unodos-pc64arm"
LMBIN="/opt/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/bin"
CC="$LMBIN/aarch64-w64-mingw32-clang"

# Baseline flags (see m0.c / README for the history):
#   -fsigned-char   pc64/include/limits.h hardcodes signed char
#   -fno-builtin    pc64_libc.c defines memset/memcpy; the idiom recognizer
#                   must not rewrite their own loops into calls to themselves
# -mstrict-align is applied ONLY to code that can run before the MMU is on
# (mmu.c, and everything in the m0 payload); the shell's fb blits run MMU-on.
BASECF="-O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-stack-check \
        -nostdinc -fno-builtin -fsigned-char"
LINK="-nostdlib -Wl,--image-base,0x40080000 -e _start"

stage_quill() {
  ssh "$QUILL" "mkdir -p $QDIR/cosmo64/build $QDIR/pc64/build $QDIR/unoui/themes $QDIR/uno3d"
  scp -q ./*.s ./*.c ./*.h ./*.py "$QUILL:$QDIR/cosmo64/"
}

mkdir -p build

case "$1" in
# ---------------------------------------------------------------------------
"" )
  echo "[m0] cross-compiling the test payload on quill..."
  stage_quill
  ssh "$QUILL" "cd $QDIR/cosmo64 && \
    $CC $BASECF -mstrict-align -c m0.c -o build/m0.o && \
    $CC $BASECF -mstrict-align -c videolfb.c -o build/videolfb.o && \
    $CC $BASECF -mstrict-align -c mmu.c -o build/mmu.o && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/m0.exe build/entry.o build/cpu.o build/m0.o \
        build/videolfb.o build/mmu.o"
  scp -q "$QUILL:$QDIR/cosmo64/build/m0.exe" build/
  "$PY" flatten.py build/m0.exe build/m0.bin
  OUT=build/m0.bin
  ;;
# ---------------------------------------------------------------------------
shell )
  echo "[shell] staging sources on quill (pc64 core + unoui + cosmo64)..."
  stage_quill
  # the pc64 tree minus the big vendored stacks a shell-only build never sees
  # (tar over ssh: Git Bash has no rsync)
  (cd .. && tar czf - \
      --exclude=pc64/upy --exclude=pc64/unocode --exclude=pc64/quickjs \
      --exclude=pc64/shots --exclude=pc64/flash --exclude=pc64/remote \
      --exclude=pc64/build --exclude=pc64/tools \
      pc64 unoui uno3d unosound unomedia unoacpi) | ssh "$QUILL" "tar xzf - -C $QDIR"
  scp -q ../pc64/build/font_data.h ../pc64/build/world_map.h "$QUILL:$QDIR/pc64/build/"

  # The Tier-1 portable core (dependency survey 2026-08-31; the nine themes
  # are all named by kThemes[] in pc64_uui.c, so all nine link).
  UNOUI="unoui unoui_input unoui_anim unoui_wmanim"
  THEMES="theme_aurora theme_unodos theme_macos7 theme_macplus theme_win31 \
          theme_amiga theme_c64 theme_apple2 theme_next"
  PCORE="fb pc64_libc pc64_math pc64_font pc64_icons pc64_qoi pc64_uui_apps \
         mac_compat pc64_io pc64_write pc64_clock pc64_files pc64_uui"
  C64="videolfb display platform input stubs i2c kbd touch"

  # KBDTEST=1: compile the scripted key pad (QEMU gate proof, never shipped)
  [ -n "$KBDTEST" ] && BASECF="$BASECF -DC64_KBDTEST"
  # TOUCHDBG=1: paint the raw touch report on the panel edge (bring-up aid)
  [ -n "$TOUCHDBG" ] && BASECF="$BASECF -DC64_TOUCHDBG"
  # -Wno-error=implicit-function-declaration: mingw-gcc merely warns on the
  # declared-later-in-the-same-file pattern pc64_uui.c uses; clang 16+ errors.
  # The linker still catches genuinely missing functions.
  # -mstrict-align FOR EVERYTHING on this device: the hardware bisect of
  # 2026-09-01 showed unaligned accesses wedge the core silently (no EL1
  # fault -- consistent with GenieZone's stage-2 imposing Device-type memory),
  # and each build compiled without it died at its first merged wide load
  # while the fully strict-align m0 runs the same path clean.
  SHCF="$BASECF -mstrict-align -Wno-error=implicit-function-declaration \
        -DUNO_COLOR=1 -DUNO_PC64 -DUNO_UUI -Dmain=uno_main \
        -I$QDIR/pc64/include -I$QDIR/pc64 -I$QDIR/unoui -I$QDIR/uno3d \
        -I$QDIR/pc64/bearssl/inc -I$QDIR/unosound -I$QDIR/unomedia \
        -I$QDIR/unoacpi -I$QDIR/unoacpi/uacpi/include -I$QDIR/cosmo64"

  ssh "$QUILL" "set -e; cd $QDIR/cosmo64 && \
    for f in $UNOUI; do $CC $SHCF -c ../unoui/\$f.c -o build/u_\$f.o; done && \
    for f in $THEMES; do $CC $SHCF -c ../unoui/themes/\$f.c -o build/t_\$f.o; done && \
    for f in $PCORE; do $CC $SHCF -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    for f in $C64; do $CC $SHCF -mstrict-align -c \$f.c -o build/c_\$f.o; done && \
    $CC $SHCF -mstrict-align -c mmu.c -o build/mmu_sa.o && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/shell.exe build/entry.o build/cpu.o build/mmu_sa.o \
        build/u_*.o build/t_*.o build/p_*.o build/c_*.o"
  scp -q "$QUILL:$QDIR/cosmo64/build/shell.exe" build/
  FLATTEN_IMGSZ=shipped "$PY" flatten.py build/shell.exe build/shell.bin
  OUT=build/shell.bin
  ;;
* )
  echo "usage: ./build.sh [shell]" >&2
  exit 1
  ;;
esac

echo "[boot image] wrapping..."
# mkbootimg.py is the asm port's (cosmo/ lane, in-tree since the 2026-09-01
# merge) -- consumed, not edited.
MKBOOTIMG="${MKBOOTIMG:-../cosmo/mkbootimg.py}"
[ -f "$MKBOOTIMG" ] || { echo "mkbootimg.py not found at $MKBOOTIMG -- set MKBOOTIMG" >&2; exit 1; }
"$PY" "$MKBOOTIMG" "$OUT" build/pc64arm-boot.img
echo "    -> cosmo64/build/pc64arm-boot.img  (from $OUT)"
echo "    install:  scp build/pc64arm-boot.img the-cosmo:  then as root:"
echo "              dd if=pc64arm-boot.img of=/dev/mmcblk0p38 bs=1M conv=fsync"
