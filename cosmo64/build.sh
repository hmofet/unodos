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
#   ./build.sh calib    -> the TOUCH CALIBRATION payload (build/calib.bin)
#   ./build.sh usb      -> the USB HOST PROBE      (build/usbprobe.bin)
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
# /merge:.unodrv=.data: the UNO_DRIVER registration tables xhci.c and usbhid.c
# emit are 16 initialised bytes in a section of their own, and lld places an
# unknown section AFTER .data -- whose virtual extent is the shell's 72 MB of
# .bss. flatten.py ships everything up to the last non-zero byte, so those 16
# bytes made the payload 76 MB, which is the size that hung LK's decompressor
# at the splash in M1. Nothing here reads the table (no device manager), so
# it goes inside .data's initialised part where it costs nothing. flatten.py
# trips on any shipped image over 16 MB so this cannot recur silently.
LINK="-nostdlib -Wl,--image-base,0x40080000 -e _start -Wl,-Xlink=/merge:.unodrv=.data"

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
    $CC $BASECF -mstrict-align -c log.c -o build/log.o && \
    $CC $BASECF -mstrict-align -c msdc.c -o build/msdc.o && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/m0.exe build/entry.o build/cpu.o build/m0.o \
        build/videolfb.o build/mmu.o build/log.o build/msdc.o"
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
  # the URC host client, for qharness.py's QHARNESS_URC gate (pc64/tools is
  # otherwise excluded above)
  scp -q ../pc64/tools/unoauto_remote.py "$QUILL:$QDIR/cosmo64/"

  # The Tier-1 portable core (dependency survey 2026-08-31; the nine themes
  # are all named by kThemes[] in pc64_uui.c, so all nine link).
  UNOUI="unoui unoui_input unoui_anim unoui_wmanim"
  THEMES="theme_aurora theme_unodos theme_macos7 theme_macplus theme_win31 \
          theme_amiga theme_c64 theme_apple2 theme_next"
  PCORE="fb pc64_libc pc64_math pc64_font pc64_icons pc64_qoi pc64_uui_apps \
         mac_compat pc64_io pc64_write pc64_clock pc64_files pc64_uui \
         fat pc64_fs hid_kbd net"
  # M6: unoautomate + the URC remote channel, compiled UNCHANGED. The two
  # files that carry the privilege gate are built -DUNO_DEBUG (per file, like
  # the usb renames -- no shared header changes layout under it) because the
  # production arming path needs an account on a FAT volume this device does
  # not mount yet; urc.c explains, and URC_PIN=<6 digits> ./build.sh shell
  # keeps the production auth rules with a token from the build instead.
  # unoauto_compat.c is deliberately absent: urc.c supplies its symbols with
  # a real clock and a log that reaches the eMMC.
  URC="unoauto unoauto_probe unoauto_screen netdisc unostorage"
  URCDBG="unoauto_gate unoauto_remote"
  C64="videolfb display platform input stubs i2c kbd touch log msdc blk \
       ssusb pci usb netup urc"
  if [ -n "$URC_PIN" ]; then
    printf '#define C64_URC_PIN "%s"\n' "$URC_PIN" > urc_pin.h
    echo "[shell] URC gate: production auth with the build-time PIN"
  else
    rm -f urc_pin.h
    echo "[shell] URC gate: open (no PIN) -- set URC_PIN=<6 digits> to close it"
  fi
  # stage_quill ran before the header was decided: re-stage it, or remove a
  # stale one so a PIN never lingers on quill from an earlier build
  if [ -f urc_pin.h ]; then scp -q urc_pin.h "$QUILL:$QDIR/cosmo64/";
  else ssh "$QUILL" "rm -f $QDIR/cosmo64/urc_pin.h"; fi

  # KBDTEST=1: compile the scripted key pad (QEMU gate proof, never shipped)
  [ -n "$KBDTEST" ] && BASECF="$BASECF -DC64_KBDTEST"
  # TOUCHDBG=1: paint the raw touch report on the panel edge (bring-up aid)
  [ -n "$TOUCHDBG" ] && BASECF="$BASECF -DC64_TOUCHDBG"
  # BLKTEST=1: put fat.c + pc64_fs.c through a format/write/read/delete round
  # trip over a RAM transport, so the QEMU gate covers the storage stack the
  # virt board's missing MSDC otherwise leaves untested (36 MiB of .bss --
  # never ship a BLKTEST image)
  [ -n "$BLKTEST" ] && BASECF="$BASECF -DC64_BLKTEST"
  # -Wno-error=implicit-function-declaration: mingw-gcc merely warns on the
  # declared-later-in-the-same-file pattern pc64_uui.c uses; clang 16+ errors.
  # The linker still catches genuinely missing functions.
  # -mstrict-align FOR EVERYTHING on this device: the hardware bisect of
  # 2026-09-01 showed unaligned accesses wedge the core silently (no EL1
  # fault -- consistent with GenieZone's stage-2 imposing Device-type memory),
  # and each build compiled without it died at its first merged wide load
  # while the fully strict-align m0 runs the same path clean.
  # -DFB_MAX_W/-DFB_MAX_H: fb.h's ceiling sizes fb[] and unoui's cached
  # desktop background, and it defaults to a PC monitor's 1920x1200. This
  # panel's native desktop is 2160x1080 -- wider AND shorter -- so the default
  # would have clipped the desktop this port now starts in.
  SHCF="$BASECF -mstrict-align -Wno-error=implicit-function-declaration \
        -DFB_MAX_W=2160 -DFB_MAX_H=1080 \
        -DUNO_COLOR=1 -DUNO_PC64 -DUNO_UUI -Dmain=uno_main \
        -I$QDIR/pc64/include -I$QDIR/pc64 -I$QDIR/unoui -I$QDIR/uno3d \
        -I$QDIR/pc64/bearssl/inc -I$QDIR/unosound -I$QDIR/unomedia \
        -I$QDIR/unoacpi -I$QDIR/unoacpi/uacpi/include -I$QDIR/cosmo64"

  # M5: ax88179.c joins them on the same terms. Its bulk calls are renamed as
  # well as its control ones, because uno_usb_bulk_in/out put the CALLER's
  # pointer straight into the TRB and the driver's tx[]/g_rx[] are ordinary
  # cached .bss. Moving the driver's own statics into .xdma with the xhci
  # pragma would also work and would then make it parse every received frame
  # out of Device memory a byte at a time; usb.c bounces instead, so the
  # staging area is uncached and the parse is not.
  #
  # M4: the USB lane's xhci.c and usbhid.c, compiled UNCHANGED for this
  # platform. -DUNO_XHCI turns the real driver on (it is inert stubs without
  # it); -include c64_usbglue.h moves every DMA buffer in xhci.c into the
  # ".xdma" section mmu.c maps uncached (C64_XDMA) and routes uno_dbg_log to
  # the eMMC log; the two -D renames send usbhid.c's control transfers
  # through usb.c's bounce buffer, because it passes a STACK buffer and the
  # controller cannot see through the cache to it. The tripwire after the
  # link fails the build if xhci.o still owns a .bss: a DMA structure left in
  # write-back memory does not fail, it corrupts.
  USBCF="$SHCF -DUNO_XHCI -include c64_usbglue.h"
  OBJDUMP="$LMBIN/llvm-objdump"

  ssh "$QUILL" "set -e; cd $QDIR/cosmo64 && \
    for f in $UNOUI; do $CC $SHCF -c ../unoui/\$f.c -o build/u_\$f.o; done && \
    for f in $THEMES; do $CC $SHCF -c ../unoui/themes/\$f.c -o build/t_\$f.o; done && \
    for f in $PCORE; do $CC $SHCF -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    for f in $URC; do $CC $SHCF -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    for f in $URCDBG; do $CC $SHCF -DUNO_DEBUG -c ../pc64/\$f.c -o build/p_\$f.o; done && \
    $CC $USBCF -DC64_XDMA -c ../pc64/xhci.c -o build/p_xhci.o && \
    $CC $USBCF -Duno_usb_get_config=c64_usb_get_config \
        -Duno_usb_control=c64_usb_control \
        -Duno_usb_setup_intr_in=c64_usb_setup_intr_in \
        -c ../pc64/usbhid.c -o build/p_usbhid.o && \
    $CC $USBCF -Duno_usb_get_config=c64_usb_get_config \
        -Duno_usb_control=c64_usb_control \
        -Duno_usb_bulk_in=c64_usb_bulk_in \
        -Duno_usb_bulk_out=c64_usb_bulk_out \
        -c ../pc64/ax88179.c -o build/p_ax88179.o && \
    if $OBJDUMP -h build/p_xhci.o | grep -Eq '\.bss +0*[1-9a-f]'; then \
        echo 'BUILD TRIPWIRE: xhci.o still has a .bss -- DMA memory would be cached' >&2; exit 1; fi && \
    $OBJDUMP -h build/p_xhci.o | grep -q '\.xdma' || { echo 'BUILD TRIPWIRE: no .xdma section in xhci.o' >&2; exit 1; } && \
    for f in $C64; do $CC $SHCF -mstrict-align -c \$f.c -o build/c_\$f.o; done && \
    $CC $SHCF -mstrict-align -c mmu.c -o build/mmu_sa.o && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/shell.exe build/entry.o build/cpu.o build/mmu_sa.o \
        build/u_*.o build/t_*.o build/p_*.o build/c_*.o && \
    $OBJDUMP -h build/shell.exe | grep -E 'xdma|\.data|\.text'"
  scp -q "$QUILL:$QDIR/cosmo64/build/shell.exe" build/
  FLATTEN_IMGSZ=shipped "$PY" flatten.py build/shell.exe build/shell.bin
  OUT=build/shell.bin
  ;;
# ---------------------------------------------------------------------------
calib )
  # The touch calibration payload: draws targets in RAW PANEL PIXELS and logs
  # the controller's RAW report, so the calibration path carries none of the
  # transform it exists to measure. Every cosmo64 driver it needs is
  # self-contained (cosmo64.h only), so this builds with the plain flags -- no
  # pc64 tree, no unoui, no 67 MB of .bss.
  echo "[calib] cross-compiling the calibration payload on quill..."
  stage_quill
  CAL="calib videolfb mmu log msdc i2c kbd touch"
  ssh "$QUILL" "set -e; cd $QDIR/cosmo64 && \
    for f in $CAL; do $CC $BASECF -mstrict-align -c \$f.c -o build/k_\$f.o; done && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/calib.exe build/entry.o build/cpu.o build/k_*.o"
  scp -q "$QUILL:$QDIR/cosmo64/build/calib.exe" build/
  "$PY" flatten.py build/calib.exe build/calib.bin
  OUT=build/calib.bin
  ;;
usb )
  # The USB host probe: reports what state LK leaves the SSUSB controller in,
  # which is what decides whether M4 is an adoption (like the eMMC) or a full
  # bring-up (like the SD card). Self-contained -- cosmo64.h only, no pc64
  # tree, no unoui -- and it needs no input drivers, so it links even less
  # than calib.
  echo "[usb] cross-compiling the USB host probe on quill..."
  stage_quill
  USBP="usbprobe videolfb mmu log msdc"
  ssh "$QUILL" "set -e; cd $QDIR/cosmo64 && \
    for f in $USBP; do $CC $BASECF -mstrict-align -c \$f.c -o build/b_\$f.o; done && \
    $CC -c entry.s -o build/entry.o && \
    $CC -c cpu.s -o build/cpu.o && \
    $CC $LINK -o build/usbprobe.exe build/entry.o build/cpu.o build/b_*.o"
  scp -q "$QUILL:$QDIR/cosmo64/build/usbprobe.exe" build/
  "$PY" flatten.py build/usbprobe.exe build/usbprobe.bin
  OUT=build/usbprobe.bin
  ;;
* )
  echo "usage: ./build.sh [shell|calib|usb]" >&2
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
