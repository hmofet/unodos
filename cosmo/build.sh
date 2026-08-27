#!/bin/sh
# UnoDOS / Cosmo Communicator (MediaTek MT6771, AArch64) build.
# Emits build/unodos.bin (flat payload) and build/unodos-boot.img (LK-loadable
# Android boot image for slot p42). WSL is dead on amanuensis, so the AArch64
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

# BEACON=1 ./build.sh  -> blink the stage count on the vibrator through the PMIC.
# Off by default: the PWRAP/MT6358 register facts are read from LK source and have
# never been executed on this device (see the beacon comment in kernel.s).
if [ -n "$BEACON" ]; then
  DEFS="$DEFS --defsym BEACON=1"
  echo "    (BEACON build: vibrator stage pulses enabled)"
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
echo "    install:  scp $IMG the-cosmo:  then in Gemian:  sudo dd if=$(basename "$IMG") of=/dev/mmcblk0p42 bs=1M"
