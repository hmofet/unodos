#!/bin/bash
# Build UnoDosFlasher (SwiftPM) and assemble UnoDosFlasher.app: the SwiftUI GUI +
# the privileged raw-disk writer (writer.c) + the single embedded gzip disk image.
# The macOS twin of pc64/flash/build-flasher.ps1, ported from the Writer's Unlock
# mac flasher and simplified to pc64's UEFI-only, single-image world.
#
# Unlike the Writer's Unlock build (whose images come from a separate Linux job),
# the pc64 image is produced right here: build.sh -> mkuefi.py -> gzip. Pass
# --skip-build to reuse an already-built pc64/build/unodos-uefi.img.
#
# Prereqs (this Mac): x86_64-w64-mingw32-gcc (brew mingw-w64), sgdisk (brew
# gptfdisk), mtools (brew mtools), python3, Swift toolchain. If build.sh needs
# extra compiler flags on your GCC, pass them via UNO_EXTRA (e.g. GCC 14+ wants
# `UNO_EXTRA="-std=gnu17 -Wno-implicit-function-declaration"`).
#
# Usage: ./build-app.sh [release|debug] [--skip-build]
set -euo pipefail
cd "$(dirname "$0")"

CONFIG="release"
SKIP_BUILD=0
for a in "$@"; do
  case "$a" in
    debug|release) CONFIG="$a" ;;
    --skip-build)  SKIP_BUILD=1 ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done

PC64="$(cd ../.. && pwd)"
IMG="$PC64/build/unodos-uefi.img"
SIZE_MIB="${UNO_DISK_MIB:-512}"
VERSION="$(tr -d '[:space:]' < VERSION 2>/dev/null)"; VERSION="${VERSION:-1.0.0}"
APPNAME="UnoDosFlasher"

# --- 1. build the bootable pc64 image (BOOTX64.EFI -> ESP -> GPT/FAT32 image) ---
if [ "$SKIP_BUILD" -eq 0 ]; then
  echo "[img] building pc64 (build.sh)…"
  ( cd "$PC64" && UNO_EXTRA="${UNO_EXTRA:-}" sh build.sh )
  echo "[img] packing UEFI disk image (mkuefi.py $SIZE_MIB MiB)…"
  ( cd "$PC64" && python3 tools/mkuefi.py "$SIZE_MIB" )
fi
[ -f "$IMG" ] || { echo "ERROR: $IMG not found — run without --skip-build." >&2; exit 1; }

# --- 2. gzip the image (the writer streams it decompressed to the raw device) ---
mkdir -p staging
echo "[img] gzipping $(basename "$IMG") ($(du -h "$IMG" | cut -f1))…"
gzip -c "$IMG" > staging/unodos.img.gz
echo "[img] compressed to $(du -h staging/unodos.img.gz | cut -f1)"

# --- 3. build the universal (arm64 + x86_64) GUI executable ---------------------
ARCHS="--arch arm64 --arch x86_64"
swift build -c "$CONFIG" $ARCHS
BIN="$(swift build -c "$CONFIG" $ARCHS --show-bin-path)"

APP="$APPNAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/images"
cp "$BIN/$APPNAME" "$APP/Contents/MacOS/$APPNAME"

# Privileged raw writer — root-exec'd via Authorization Services. zlib (-lz) ships
# with macOS. Lives in Resources/ and is signed alongside the app below.
cc -O2 -Wall -arch arm64 -arch x86_64 -o "$APP/Contents/Resources/uno-writer" writer.c -lz

echo "Architectures: app=$(lipo -archs "$APP/Contents/MacOS/$APPNAME") writer=$(lipo -archs "$APP/Contents/Resources/uno-writer")"

# Embedded disk image (single, gzip), mirroring the Windows exe's embedded resource.
cp staging/unodos.img.gz "$APP/Contents/Resources/images/unodos.img.gz"

# App icon — iconset (if generated) else derive from the Windows .ico; best-effort.
if [ ! -d "$APPNAME.iconset" ] && [ -f ../unodos.ico ]; then
  echo "[icon] deriving iconset from ../unodos.ico via sips…"
  mkdir -p "$APPNAME.iconset"
  if sips -s format png ../unodos.ico --out staging/icon-base.png >/dev/null 2>&1; then
    for sz in 16 32 128 256 512; do
      sips -z $sz $sz staging/icon-base.png --out "$APPNAME.iconset/icon_${sz}x${sz}.png" >/dev/null 2>&1 || true
      d=$((sz*2))
      sips -z $d $d staging/icon-base.png --out "$APPNAME.iconset/icon_${sz}x${sz}@2x.png" >/dev/null 2>&1 || true
    done
  fi
fi
if [ -d "$APPNAME.iconset" ]; then
  iconutil -c icns "$APPNAME.iconset" -o "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null || true
fi

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>UnoDOS Flasher</string>
  <key>CFBundleDisplayName</key><string>UnoDOS USB Installer</string>
  <key>CFBundleIdentifier</key><string>org.unodos.pc64.flasher</string>
  <key>CFBundleExecutable</key><string>UnoDosFlasher</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>__VERSION__</string>
  <key>CFBundleShortVersionString</key><string>__VERSION__</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
</dict>
</plist>
PLIST
sed -i '' "s/__VERSION__/$VERSION/g" "$APP/Contents/Info.plist"

# --- 4. code signing (same stable local identity as the Slate Mac app) ----------
# Sign the nested writer first, then the app. Ad-hoc (`--sign -`) has no stable
# designated requirement, so an "Always Allow" keychain grant breaks across
# launches; the persistent self-signed identity keeps it stable.
SIGN_ID="Slate Local Signing"
LOGIN_KC="$HOME/Library/Keychains/login.keychain-db"
_got_identity() { [ "$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=//p' | head -1)" = "$SIGN_ID" ]; }
_sign_all() {
  codesign --force --sign "$SIGN_ID" "$APP/Contents/Resources/uno-writer" 2>/dev/null || return 1
  codesign --force --deep --sign "$SIGN_ID" "$APP" 2>/dev/null || return 1
}

if security find-certificate -c "$SIGN_ID" >/dev/null 2>&1; then
  _sign_all || true
  if ! _got_identity; then
    if [ -n "${SLATE_KEYCHAIN_PW:-}" ]; then
      security unlock-keychain -p "$SLATE_KEYCHAIN_PW" "$LOGIN_KC" 2>/dev/null || true
    elif [ -t 0 ]; then
      echo "Unlocking the login keychain for code signing — enter your macOS login password:"
      security unlock-keychain "$LOGIN_KC" || true
    fi
    _sign_all || true
  fi
  if _got_identity; then
    codesign --verify --strict "$APP"
    echo "Signed with '$SIGN_ID'"
  else
    echo "ERROR: could not sign with '$SIGN_ID'. Unlock the login keychain or pass SLATE_KEYCHAIN_PW." >&2
    exit 1
  fi
else
  echo "WARNING: '$SIGN_ID' identity not found; falling back to ad-hoc." >&2
  codesign --force --sign - "$APP/Contents/Resources/uno-writer" >/dev/null 2>&1 || true
  codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true
fi

rm -rf staging
echo "Built $(pwd)/$APP  (v$VERSION)"
