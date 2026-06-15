#!/bin/bash
# runshot.sh <APP> <outname> [settle]
# Stages <APP>.APPL, runs it in Executor on WSLg :0, grabs the executor content
# window into /root/cap/<outname>_full.png (no heavy processing here).
APP="$1"; OUT="$2"; SETTLE="${3:-8}"
B=/mnt/c/Users/arin/Documents/Github/unodos/mac/build
mkdir -p /root/unotest/.rsrc /root/unotest/.finf /root/cap
cp "$B/$APP.APPL" /root/unotest/ 2>/dev/null
cp "$B/.rsrc/$APP.APPL" /root/unotest/.rsrc/ 2>/dev/null
cp "$B/.finf/$APP.APPL" /root/unotest/.finf/ 2>/dev/null

setsid bash -c "
export DISPLAY=:0
sleep $SETTLE
CW=\$(xwininfo -root -tree 2>/dev/null | grep -E '0x[0-9a-f]+ .executor.' | grep -v Selection | head -1 | grep -oE '0x[0-9a-f]+')
import -window \$CW /root/cap/${OUT}_full.png 2>/dev/null
" </dev/null >/dev/null 2>&1 &

export DISPLAY=:0
cd /root/unotest
timeout $((SETTLE + 5)) /opt/Executor2000-0.1.0-Linux/bin/executor -bpp 8 -size 640x480 "$APP.APPL" >/dev/null 2>&1
echo "ran $APP"
