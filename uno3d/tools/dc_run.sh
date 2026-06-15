#!/bin/bash
# Isolated Flycast capture for uno3d on its OWN display (:98), so it doesn't
# collide with the dreamcast port's rig on :99. Does NOT pkill other flycasts.
#   dc_run.sh <content.cdi> <out.png> [seconds]
CONTENT="$1"; OUT="$2"; SECS="${3:-14}"
FLY=/root/emu/squashfs-root/usr/bin/flycast
CFGDIR=/root/.config/flycast98
export DISPLAY=:98 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe HOME=/root XDG_CONFIG_HOME=$CFGDIR
mkdir -p "$CFGDIR/flycast" /root/.local/share/flycast
cat > "$CFGDIR/flycast/emu.cfg" <<CFG
[config]
Dreamcast.HleBootRom = yes
Dreamcast.Region = 1
Dreamcast.Broadcast = 0
Dynarec.Enabled = yes
pvr.rend = 0
rend.EmulateFramebuffer = yes
rend.vsync = no
rend.DelayFrameSwapping = no
[window]
fullscreen = no
width = 640
height = 480
CFG
Xvfb :98 -screen 0 640x480x24 >/tmp/xvfb98.log 2>&1 &
XVFB=$!; sleep 2
"$FLY" "$CONTENT" >/tmp/flycast98.log 2>&1 &
FLYPID=$!
sleep "$SECS"
import -display :98 -window root "$OUT" 2>/tmp/import98.log \
  || { xwd -display :98 -root -silent >/tmp/s98.xwd 2>>/tmp/import98.log && convert xwd:/tmp/s98.xwd "$OUT" 2>>/tmp/import98.log; }
kill -9 $FLYPID 2>/dev/null; kill -9 $XVFB 2>/dev/null
echo "=== flycast98.log tail ==="; tail -8 /tmp/flycast98.log
echo "=== out ==="; ls -la "$OUT" 2>&1
exit 0
