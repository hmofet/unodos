#!/bin/bash
# Assemble UnoDosFlasherCapture.app from winid.swift + capture + Info.plist,
# ad-hoc sign it so it has a stable TCC identity, and print the one-time
# Screen Recording grant step. Run this once on the Mac (no admin needed).
set -e
BASE="$(cd "$(dirname "$0")" && pwd)"
APP="$BASE/UnoDosFlasherCapture.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"

echo "[1/4] compiling winid…"
swiftc -O -o "$APP/Contents/MacOS/winid" "$BASE/winid.swift"

echo "[2/4] installing script + Info.plist…"
cp "$BASE/capture"     "$APP/Contents/MacOS/capture"
chmod +x "$APP/Contents/MacOS/capture"
cp "$BASE/Info.plist"  "$APP/Contents/Info.plist"

echo "[3/4] ad-hoc signing (stable TCC identity)…"
codesign --force --deep -s - "$APP"

echo "[4/4] done: $APP"
echo
echo "ONE-TIME: grant it Screen Recording so it can capture headlessly:"
echo "  System Settings > Privacy & Security > Screen Recording > + (add app),"
echo "  press Cmd+Shift+G and paste:  $APP"
echo "  (turn its switch on; if prompted to quit & reopen, that is fine)"
echo
echo "Then capture any time with:  open \"$APP\""
echo "Output PNG:                  \$HOME/unodos-flasher-shots/flasher-macos.png"
