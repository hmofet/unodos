#!/bin/bash
cd /mnt/c/Users/arin/Documents/Github/unodos/dreamcast
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe HOME=/root
pkill -9 -f flycast 2>/dev/null; pkill -9 -f Xvfb 2>/dev/null; sleep 1
mkdir -p ~/.config/flycast
cat > ~/.config/flycast/emu.cfg <<CFG
[config]
Dreamcast.HleBootRom = yes
Dreamcast.Region = 1
Dynarec.Enabled = no
rend.EmulateFramebuffer = yes
rend.vsync = no
[window]
fullscreen = no
width = 640
height = 480
CFG
Xvfb :99 -screen 0 640x480x24 >/tmp/xvfb.log 2>&1 &
XV=$!
sleep 2
/root/emu/squashfs-root/usr/bin/flycast build/minhost.cdi >build/min_fly.log 2>&1 &
FP=$!
sleep 12
import -window root shots/dbg_min.png 2>/dev/null
kill -9 $FP $XV 2>/dev/null
