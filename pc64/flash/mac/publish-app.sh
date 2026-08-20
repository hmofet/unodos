#!/bin/bash
# publish-app.sh - build the REDISTRIBUTABLE macOS flasher: UnoDosFlasher-macOS.zip.
# The macOS twin of `build-flasher.ps1 -Publish`: a publishable image carries no
# Intel WiFi firmware, no id shareware WAD and no Winamp skin. Scrubbing the
# inputs is the belt, UNO_NOFW=1 is the suspenders (it also DELETES firmware a
# prior build staged), and the gate below is the proof - it inspects both the
# built ESP tree and the packed disk image itself before anything is zipped.
#
# Run from a Mac with the build-app.sh prereqs (mingw-w64, gptfdisk, mtools,
# python3, Swift) plus the "Slate Local Signing" identity - publishing an
# ad-hoc-signed app is refused. The mba build server runs this via
# `ssh mba build unodos-flasher --github`.
set -euo pipefail
cd "$(dirname "$0")"
PC64="$(cd ../.. && pwd)"
IMG="$PC64/build/unodos-uefi.img"
ZIP="UnoDosFlasher-macOS.zip"

# GCC 14+ turns two long-standing warnings into errors; older GCC accepts the
# flags. The pointer one is um_set_alloc(malloc, ...): unsigned long vs size_t,
# identical on quill's mingw (a warning there) and benign at runtime.
export UNO_EXTRA="${UNO_EXTRA:--std=gnu17 -Wno-implicit-function-declaration -Wno-incompatible-pointer-types}"
export UNO_NOFW=1

echo "[publish] scrubbing unredistributable inputs, fetching Freedoom..."
rm -rf "$PC64/fw-blobs" "$PC64/wads/DOOM1.WAD"
find "$PC64/wads" -name '*.WSZ' -delete 2>/dev/null || true
# `sh`, not ./ - fetch-wad.sh has no exec bit in a fresh checkout (same reason
# build-app.sh runs `sh build.sh`).
( cd "$PC64" && sh tools/fetch-wad.sh )

# build.sh populates build/esp INCREMENTALLY, so firmware or a shareware WAD
# staged by an earlier local build would leak into this image. Start clean.
rm -rf "$PC64/build/esp"

./build-app.sh release
rm -f "$ZIP"

# ---- publish gate: refuse to ship anything unredistributable -----------------
# Mirrors build-flasher.ps1's gate. Checks BOTH the ESP tree build.sh staged and
# the packed image mkuefi.py cut from it (the app embeds a gzip of that image).
fail() { echo "PUBLISH GATE: $*" >&2; exit 1; }

check_wad() { # $1 = a DOOM1.WAD to vet
    local len; len=$(wc -c < "$1" | tr -d ' ')
    if [ "$len" = "11159840" ] || [ "$len" = "4196020" ]; then
        fail "$1 is id Software's WAD ($len bytes), not Freedoom"
    fi
    if ! grep -aq -e "BSD-3-Clause" -e "Freedoom" "$1"; then
        fail "$1 does not look like Freedoom (no licence marker found)"
    fi
}

ESP="$PC64/build/esp"
[ -d "$ESP" ] || fail "$ESP missing after the build"
hit=$(find "$ESP" \( -iname '*.uco' -o -iname '*.pnv' \) -print -quit)
[ -n "$hit" ] && fail "ESP tree still contains Intel firmware: $hit"
hit=$(find "$ESP" -type d -iname firmware -print -quit)
[ -n "$hit" ] && fail "ESP tree still contains a FIRMWARE directory: $hit"
hit=$(find "$ESP" -iname '*.wsz' -print -quit)
[ -n "$hit" ] && fail "ESP tree contains a Winamp skin: $hit"
[ -f "$ESP/DOOM1.WAD" ] && check_wad "$ESP/DOOM1.WAD"

# The same three checks against the image itself, via mtools. The ESP partition
# offset comes from the GPT (sgdisk prints the first sector; LBA = 512 bytes).
first=$(sgdisk -i 1 "$IMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
[ -n "$first" ] || fail "could not read the ESP partition offset from $IMG"
MIMG="$IMG@@$((first * 512))"
listing=$(mdir -i "$MIMG" -/ -b ::/)
echo "$listing" | grep -qi '\.uco$'   && fail "image contains Intel firmware (*.UCO)"
echo "$listing" | grep -qi '\.pnv$'   && fail "image contains Intel firmware (*.PNV)"
echo "$listing" | grep -qi 'firmware' && fail "image contains a FIRMWARE path"
echo "$listing" | grep -qi '\.wsz$'   && fail "image contains a Winamp skin (*.WSZ)"
if echo "$listing" | grep -qi '^::/DOOM1\.WAD$'; then
    tmpwad=$(mktemp)
    mcopy -n -i "$MIMG" ::/DOOM1.WAD "$tmpwad"
    check_wad "$tmpwad"
    rm -f "$tmpwad"
fi
echo "[publish] gate passed: no firmware, no shareware WAD, no skins (tree + image)."

# A published app must carry the real signing identity, not an ad-hoc fallback.
auth=$(codesign -dvv UnoDosFlasher.app 2>&1 | sed -n 's/^Authority=//p' | head -1)
[ "$auth" = "Slate Local Signing" ] || fail "app is not signed with 'Slate Local Signing' (got: ${auth:-ad-hoc})"
codesign --verify --strict UnoDosFlasher.app

# ditto preserves the bundle structure + signatures the way Finder's zip does.
ditto -c -k --keepParent UnoDosFlasher.app "$ZIP"
echo "[publish] built $(pwd)/$ZIP ($(du -h "$ZIP" | cut -f1))"
