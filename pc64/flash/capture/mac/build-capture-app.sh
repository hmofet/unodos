#!/bin/bash
# Assemble UnoDosFlasherCapture.app: compile capture.swift into the bundle's
# executable (the capture runs IN-PROCESS via ScreenCaptureKit under this
# bundle's own Screen Recording grant), then ad-hoc sign it. Run once on the Mac.
set -e
BASE="$(cd "$(dirname "$0")" && pwd)"
APP="$BASE/UnoDosFlasherCapture.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"

echo "[1/3] compiling capture.swift (ScreenCaptureKit)…"
swiftc -O -framework ScreenCaptureKit -framework AppKit -o "$APP/Contents/MacOS/capture" "$BASE/capture.swift"

echo "[2/3] installing Info.plist…"
cp "$BASE/Info.plist" "$APP/Contents/Info.plist"

echo "[3/3] ad-hoc signing…"
codesign --force -s - "$APP"

echo "done: $APP"
echo
echo "ONE-TIME: grant it Screen Recording (its capture runs under this identity):"
echo "  System Settings > Privacy & Security > Screen Recording > + ,"
echo "  press Cmd+Shift+G and paste:  $APP  (turn its switch on)"
echo
echo "Then capture with:  open \"$APP\"   ->  \$HOME/unodos-flasher-shots/flasher-macos.png"
