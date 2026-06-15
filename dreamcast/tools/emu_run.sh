#!/bin/bash
# UnoDOS/Dreamcast headless emulator capture rig.
#
#   emu_run.sh <content.cdi|none> <out.png> [seconds]
#
# Boots <content> in Flycast under Xvfb + Mesa llvmpipe (no GPU), waits, then
# screenshots the virtual display. REIOS (HLE BIOS) runs the disc - no Sega BIOS
# file needed. Two flags are load-bearing (see ../README.md "Rig gotchas"):
#   rend.EmulateFramebuffer = yes  - UnoDOS draws straight to VRAM
#   rend.vsync = no                - Xvfb reports 0 Hz; vsync would gate the
#                                    game-frame buffer swap, leaving it black
# Paths assume this machine's layout (Flycast AppImage extracted under ~/emu,
# the repo at /mnt/c/...); adjust FLY/TOOLS for another host.
CONTENT="$1"; OUT="$2"; SECS="${3:-12}"
FLY=/root/emu/squashfs-root/usr/bin/flycast
TOOLS=/mnt/c/Users/arin/Documents/Github/unodos/dreamcast/tools
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe HOME=/root
pkill -9 -f flycast 2>/dev/null; pkill -9 -f Xvfb 2>/dev/null; sleep 1
mkdir -p ~/.config/flycast ~/.local/share/flycast ~/emu/flash
cat > ~/.config/flycast/emu.cfg <<CFG
[config]
Dreamcast.HleBootRom = yes
Dreamcast.Region = 1
Dreamcast.Broadcast = 0
Dreamcast.Language = 1
Dynarec.Enabled = yes
pvr.rend = 0
rend.EmulateFramebuffer = yes
rend.vsync = no
rend.DelayFrameSwapping = no
rend.ShowFPS = yes
[window]
fullscreen = no
width = 640
height = 480
CFG
Xvfb :99 -screen 0 640x480x24 >/tmp/xvfb.log 2>&1 &
XVFB=$!; sleep 2
if [ "$CONTENT" = "none" ]; then
  "$FLY" >"$TOOLS/flycast.log" 2>&1 &
else
  "$FLY" "$CONTENT" >"$TOOLS/flycast.log" 2>&1 &
fi
FLYPID=$!
sleep "$SECS"
import -window root "$OUT" 2>"$TOOLS/import.log" || { xwd -root -silent >/tmp/s.xwd 2>>"$TOOLS/import.log" && convert xwd:/tmp/s.xwd "$OUT" 2>>"$TOOLS/import.log"; }
kill -9 $FLYPID 2>/dev/null; kill -9 $XVFB 2>/dev/null
echo "=== flycast.log tail ==="; tail -20 "$TOOLS/flycast.log"
echo "=== out ==="; ls -la "$OUT" 2>&1; file "$OUT" 2>&1
