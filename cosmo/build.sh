#!/bin/sh
# UnoDOS / Cosmo Communicator (MediaTek MT6771, AArch64) build.
# Emits build/unodos.bin (flat payload) and build/unodos-boot.img (LK-loadable
# Android boot image for slot p38, the UNODOS slot -- p42 is TRIXIE's since
# 2026-08-28, do NOT dd there). WSL is dead on amanuensis, so the AArch64
# toolchain runs on quill over SSH (see the machine notes); mkdata + mkbootimg
# run locally in Git Bash.
#   ./build.sh              -> interactive build (launcher, first light)
#   ./build.sh nav|app|clock|theme|dostris|paint|pacman|outlast|tracker|fs
#                          -> AUTOTEST builds the harness drives
set -e
cd "$(dirname "$0")"

PY="${PY:-python3}"
QUILL="${QUILL:-arin@192.168.2.114}"
AMAN="${AMAN:-arin@192.168.2.113}"
REPO_WIN="C:/Users/arin/Documents/Github/unodos-cosmo"     # this worktree on amanuensis
QDIR="/work/unodos-cosmo"                                    # build dir on quill

mkdir -p build
echo "[1/4] generating gfx data (font + icons + palettes + tables)..."
(cd .. && "$PY" cosmo/mkdata.py)

DEFS=""
OUT=build/unodos.bin
case "$1" in
  nav)     DEFS="--defsym AUTOTEST=1 --defsym AT_NAV=1";     OUT=build/unodos_nav.bin ;;
  app)     DEFS="--defsym AUTOTEST=1 --defsym AT_APP=1";     OUT=build/unodos_app.bin ;;
  clock)   DEFS="--defsym AUTOTEST=1 --defsym AT_CLOCK=1";   OUT=build/unodos_clock.bin ;;
  theme)   DEFS="--defsym AUTOTEST=1 --defsym AT_THEME=1";   OUT=build/unodos_theme.bin ;;
  dostris) DEFS="--defsym AUTOTEST=1 --defsym AT_DOSTRIS=1"; OUT=build/unodos_dt.bin ;;
  paint)   DEFS="--defsym AUTOTEST=1 --defsym AT_PAINT=1";   OUT=build/unodos_paint.bin ;;
  pacman)  DEFS="--defsym AUTOTEST=1 --defsym AT_PACMAN=1";  OUT=build/unodos_pm.bin ;;
  outlast) DEFS="--defsym AUTOTEST=1 --defsym AT_OUTLAST=1"; OUT=build/unodos_ol.bin ;;
  tracker) DEFS="--defsym AUTOTEST=1 --defsym AT_TRACKER=1"; OUT=build/unodos_tk.bin ;;
  fs)      DEFS="--defsym AUTOTEST=1 --defsym AT_FS=1";      OUT=build/unodos_fs.bin ;;
esac

# SCALE=1|2 ./build.sh  -> integer scale-up of the UI on the panel. The interactive
# build defaults to 2 (960x1280 rect on the 1080x2160 panel; 1:1 was a small centred
# window, first-light photo 2026-08-31). AUTOTEST builds default to 1: their
# instruction budgets are calibrated for the 1:1 present (~3x cheaper), and the
# scaled blit is gated by the default build's eye check.
case "$1" in
  "") ;;
  *)  SCALE="${SCALE:-1}" ;;
esac
if [ -n "$SCALE" ]; then
  DEFS="$DEFS --defsym FB_SCALE=$SCALE"
  echo "    (UI scale: FB_SCALE=$SCALE)"
fi

# ROT=90|180|270|0 ./build.sh  -> override the panel rotation fb_present applies.
# Default 270, derived from the Cosmo's own LK (MTK_LCM_PHYSICAL_ROTATION in
# project/k71v1_64_bsp.mk, and the 270 branch of LK's console blit). If the first
# photograph comes out sideways or upside-down, the fix is one rebuild.
if [ -n "$ROT" ]; then
  DEFS="$DEFS --defsym FB_ROT=$ROT"
  echo "    (panel rotation overridden: FB_ROT=$ROT)"
fi

# BEACON=1 ./build.sh  -> blink the stage count on the vibrator through the PMIC.
# Off by default: the PWRAP/MT6358 register facts are read from LK source and have
# never been executed on this device (see the beacon comment in kernel.s).
if [ -n "$BEACON" ]; then
  DEFS="$DEFS --defsym BEACON=1"
  echo "    (BEACON build: vibrator stage pulses enabled)"
fi

# BANDDBG=1 ./build.sh -> paint LK's vram regions distinct colours at boot and hold
# ~12 s (page0 RED / page1 GREEN / page2 BLUE / tail MAGENTA), then boot normally.
# Diagnoses which memory a stale on-panel artefact is scanned from; a region still
# showing noise over the colours is outside videolfb vram (a second OVL layer).
if [ -n "$BANDDBG" ]; then
  DEFS="$DEFS --defsym BANDDBG=1"
  echo "    (BANDDBG build: vram colour-map + 12 s hold at boot)"
fi

echo "[2/4] cross-assembling (AArch64) on quill..."
# stage this worktree's cosmo/ + generated contract onto quill, build there.
ssh "$QUILL" "mkdir -p $QDIR/cosmo/build $QDIR/unodef/gen/cosmo"
scp -q kernel.s link.ld *.inc.s build/gfx.s build/gfxequ.inc "$QUILL:$QDIR/cosmo/"
scp -q build/gfx.s build/gfxequ.inc "$QUILL:$QDIR/cosmo/build/" 2>/dev/null || true
scp -q ../unodef/gen/cosmo/sys_gen.inc "$QUILL:$QDIR/unodef/gen/cosmo/"
ssh "$QUILL" "command -v aarch64-linux-gnu-as >/dev/null || \
  sudo apt-get install -y binutils-aarch64-linux-gnu >/dev/null 2>&1"
ssh "$QUILL" "cd $QDIR/cosmo && \
  aarch64-linux-gnu-as -march=armv8-a $DEFS kernel.s -o build/kernel.o && \
  aarch64-linux-gnu-as -march=armv8-a build/gfx.s -o build/gfx.o && \
  aarch64-linux-gnu-ld -T link.ld build/kernel.o build/gfx.o -o build/kernel.elf && \
  aarch64-linux-gnu-objcopy -O binary build/kernel.elf build/$(basename $OUT)"
scp -q "$QUILL:$QDIR/cosmo/build/$(basename $OUT)" "$OUT"
echo "    payload: cosmo/$OUT ($(wc -c < "$OUT") bytes)"

echo "[3/4] wrapping in an LK boot image..."
IMG="${OUT%.bin}-boot.img"
"$PY" mkbootimg.py "$OUT" "$IMG"

echo "[4/4] done."
echo "    -> cosmo/$IMG"
echo "    install:  scp $IMG the-cosmo:  then in Gemian (as root):  dd if=$(basename "$IMG") of=/dev/mmcblk0p38 bs=1M conv=fsync"
