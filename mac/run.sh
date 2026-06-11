#!/bin/bash
# Run a UnoDOS/Mac build under Executor (ROM-free, no System install needed).
# Run inside WSL/Linux with WSLg (or any X/Wayland display) for the GUI.
#
#   ./run.sh color   - UnoDOS7 (Color QuickDraw, 8-bit, 640x480)
#   ./run.sh mono    - UnoDOSClassic (1-bit, 512x342)
#   append "test" to auto-launch the apps:  ./run.sh color test
#
# IMPORTANT: point Executor at the .APPL (auto-launches our app). The .dsk
# shows Executor's Browser instead, and triggers a PBClose hang on exit.
set -e
cd "$(dirname "$0")"
EXEC="${EXEC:-/opt/Executor2000-0.1.0-Linux/bin/executor}"
BUILD="${BUILD:-build}"

case "$1" in
  color) APP=UnoDOS7;       BPP=8; SIZE=640x480 ;;
  mono)  APP=UnoDOSClassic; BPP=1; SIZE=512x342 ;;
  *) echo "usage: $0 {color|mono} [test]"; exit 1 ;;
esac
[ "$2" = "test" ] && APP="${APP}Test"

exec "$EXEC" -bpp "$BPP" -size "$SIZE" "$BUILD/$APP.APPL"
