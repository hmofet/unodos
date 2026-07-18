#!/bin/bash
# Trigger a headless macOS flasher capture. Requires the one-time build + the
# Screen Recording grant (see build-capture-app.sh). Safe to run per release.
set -u
BASE="$(cd "$(dirname "$0")" && pwd)"
APP="$BASE/UnoDosFlasherCapture.app"
OUT="$HOME/unodos-flasher-shots/flasher-macos.png"

[ -d "$APP" ] || { echo "not built yet - run build-capture-app.sh"; exit 1; }
rm -f "$OUT"
open "$APP"

for i in $(seq 1 40); do
  [ -f "$OUT" ] && { sleep 1; break; }
  sleep 1
done

if [ -f "$OUT" ]; then
  echo "captured: $OUT ($(stat -f%z "$OUT") bytes)"
else
  echo "no screenshot produced - check ~/unodos-flasher-shots/capture.log"
  echo "(most likely Screen Recording is not granted to $APP yet)"
  exit 1
fi
