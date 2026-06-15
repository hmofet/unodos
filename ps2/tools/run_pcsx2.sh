#!/bin/bash
# ===========================================================================
# Headless PCSX2 launch + screenshot for the UnoDOS/PS2 ELF, under Xvfb in WSL.
#   tools/run_pcsx2.sh <abs-elf> <out-png> [seconds]
# Renders via the lavapipe (llvmpipe) SOFTWARE Vulkan ICD - no GPU needed.
# Uses a privately-named X server (/opt/pcsx2/MyVfb) on a high display so a
# stray `pkill Xvfb` from another job can't tear our display down mid-capture.
# Pre-reqs (caller sets up once): PCSX2 2.7 AppImage extracted to /opt/pcsx2,
# 4 MB PS2 BIOS in data/PCSX2/bios, SetupWizardIncomplete=false, Renderer=13.
# ===========================================================================
ELF="${1:?elf}"; OUT="${2:?out png}"; WAIT="${3:-30}"
DISP=":${DISP_NUM:-71}"
PCSX2_DIR=/opt/pcsx2/squashfs-root/usr/bin
DATA=/opt/pcsx2/data
XVFB=/opt/pcsx2/MyVfb
# uniquely-named copies of the X server + emulator so a stray `pkill Xvfb` /
# `pkill pcsx2` from a parallel job can't tear us down mid-capture.
EMU="$PCSX2_DIR/unodos2"
[ -x "$EMU" ] || cp "$PCSX2_DIR/pcsx2-qt" "$EMU"

sleep 1
rm -f "$DATA/PCSX2/logs/emulog.txt" "$OUT"

"$XVFB" "$DISP" -screen 0 1024x768x24 -nolisten tcp >/opt/pcsx2/xvfb.log 2>&1 &
XPID=$!
sleep 3

export DISPLAY="$DISP"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1

( cd "$PCSX2_DIR" && ./unodos2 -datapath "$DATA" -fastboot -elf "$ELF" \
    >/opt/pcsx2/boot.log 2>&1 ) &
PPID2=$!

sleep "$WAIT"
echo "=== mapped windows ==="
DISPLAY="$DISP" xwininfo -root -tree 2>/dev/null | grep -iE '0x[0-9a-f]+ "' | head
DISPLAY="$DISP" import -window root "$OUT" 2>/dev/null
echo "screenshot rc=$? -> $OUT"
DISPLAY="$DISP" identify "$OUT" 2>/dev/null
echo "=== emulog tail ==="
grep -ivE 'qt.text|OpenType|Couldn.t find' "$DATA/PCSX2/logs/emulog.txt" 2>/dev/null | tail -14
kill -9 $PPID2 2>/dev/null
pkill -9 -f 'unodos2 -datapath' 2>/dev/null
kill -9 $XPID 2>/dev/null
true
