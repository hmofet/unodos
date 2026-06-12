#!/bin/bash
# stage_run.sh <AppName> [bpp] [size] — stage an .APPL (+sidecars) and run it
set -e
export DISPLAY=:0
APP="$1"
BPP="${2:-8}"
SIZE="${3:-640x480}"
B=/mnt/c/Users/arin/Documents/Github/unodos/mac/build
mkdir -p /root/unotest/.rsrc /root/unotest/.finf
cp "$B/$APP.APPL" /root/unotest/
cp "$B/.rsrc/$APP.APPL" /root/unotest/.rsrc/
cp "$B/.finf/$APP.APPL" /root/unotest/.finf/
cd /root/unotest
exec /opt/Executor2000-0.1.0-Linux/bin/executor -bpp "$BPP" -size "$SIZE" "$APP.APPL" >/tmp/exec.log 2>&1
