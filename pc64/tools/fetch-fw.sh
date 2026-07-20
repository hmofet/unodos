#!/bin/sh
# ===========================================================================
# fetch-fw.sh - DEVELOPER-side helper: populate pc64/fw-blobs/ so build.sh can
# bundle the Intel WiFi firmware into a *test* image (see build.sh, the "[fw]"
# step).  fw-blobs/ is gitignored - the firmware is never committed.
#
# End users don't run this; they run tools/uno-wifi-fw.py to put the blob on a
# flashed stick.  This script is the same idea for the build machine: it grabs
# the Debian firmware-iwlwifi package and drops the IWL*.UCO/.PNV names the
# iwlwifi driver loads into fw-blobs/.
#
#   sh tools/fetch-fw.sh            # from the pc64/ dir (or anywhere; paths are absolute-ish)
# Needs: curl, ar (binutils), tar, xz.
# ===========================================================================
set -e
here=$(cd "$(dirname "$0")/.." && pwd)     # -> pc64/
dst="$here/fw-blobs"
pool="http://ftp.debian.org/debian/pool/non-free-firmware/f/firmware-nonfree"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "Finding the latest firmware-iwlwifi .deb ..."
deb=$(curl -s "$pool/" | grep -oE 'firmware-iwlwifi_[0-9][0-9a-z.+~-]*_all\.deb' \
      | grep -v bpo | grep -v '~' | sort -u | tail -1)
[ -n "$deb" ] || { echo "could not find the package"; exit 1; }
echo "  $deb"
curl -sL "$pool/$deb" -o "$tmp/fw.deb"

cd "$tmp"
ar x fw.deb
if   [ -f data.tar.xz ];  then tar -xf data.tar.xz  --wildcards './usr/lib/firmware/intel/iwlwifi/iwlwifi-*'
elif [ -f data.tar.zst ]; then zstd -dq data.tar.zst -o data.tar && tar -xf data.tar --wildcards './usr/lib/firmware/intel/iwlwifi/iwlwifi-*'
elif [ -f data.tar.gz ];  then tar -xf data.tar.gz  --wildcards './usr/lib/firmware/intel/iwlwifi/iwlwifi-*'
else echo "unknown data.tar compression"; exit 1; fi
fwd=usr/lib/firmware/intel/iwlwifi

mkdir -p "$dst"
pick() { ls "$fwd"/$1 2>/dev/null | sort -V | tail -1; }
cp "$(pick 'iwlwifi-QuZ-a0-hr-b0-*.ucode')" "$dst/IWLAX201.UCO"   # AX201 (Qu/QuZ)
cp "$(pick 'iwlwifi-cc-a0-*.ucode')"        "$dst/IWLAX200.UCO"   # AX200 (discrete)
cp "$(pick 'iwlwifi-ty-a0-gf-a0-*.ucode')"  "$dst/IWLAX210.UCO"   # AX210 (Ty)
cp "$fwd/iwlwifi-ty-a0-gf-a0.pnvm"          "$dst/IWLAX210.PNV"
so=$(pick 'iwlwifi-so-a0-gf-a0-*.ucode')                          # AX211/AX411 (So)
[ -n "$so" ] && { cp "$so" "$dst/IWLAX211.UCO"; cp "$fwd/iwlwifi-so-a0-gf-a0.pnvm" "$dst/IWLAX211.PNV"; } || true
p9=$(pick 'iwlwifi-9000-pu-b0-jf-b0-*.ucode'); [ -n "$p9" ] && cp "$p9" "$dst/IWL9000.UCO" || true
p92=$(pick 'iwlwifi-9260-th-b0-jf-b0-*.ucode'); [ -n "$p92" ] && cp "$p92" "$dst/IWL9260.UCO" || true
p7=$(pick 'iwlwifi-7260-*.ucode'); [ -n "$p7" ] && cp "$p7" "$dst/IWL7260.UCO" || true
p8=$(pick 'iwlwifi-8000C-*.ucode'); [ -n "$p8" ] && cp "$p8" "$dst/IWL8000.UCO" || true
# WiFi-7 (Bz/Gl/Sc) is best-effort in the driver; fetch on demand with
# tools/uno-wifi-fw.py --card be200|be201|be211 rather than bundling it.

echo "Populated $dst:"
ls -la "$dst"
echo "Now 'UNO_STUDIO=0 ./build.sh' bundles these into build/esp/FIRMWARE/ for a test stick."
