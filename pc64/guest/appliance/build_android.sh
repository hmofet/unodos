#!/bin/bash
# Build ANDROID.IMG - the Android runtime the appliance's Waydroid container
# runs (docs/ANDROID-APPLIANCE-PLAN.md P1).  Everything it packs is
# third-party (LineageOS, Apache-2.0 plus GPL kernel bits) and therefore
# NEVER enters the repo: this script is ours, its OUTPUT is a build artefact
# staged onto the ESP as EFI\UNODOS\VM\ANDROID.IMG.
#
#     ./build_android.sh [workdir]        # default /work/unodos-guest
#
# Produces <workdir>/android.img, ~2.6 GB.
#
# WHY A SEPARATE IMAGE AND NOT PART OF rootfs.img.  Three reasons and the
# first is the one that matters: an Android system image is 1.8 GB, and
# ROOTFS.IMG is a browser appliance that has no business growing by that much
# for machines that will never run an Android app.  Second, it keeps the
# proprietary case separable - a GApps build differs only in this file, and
# it is one file the user can delete.  Third, two images are two build
# scripts, so this lane and the appliance lane never edit the same file.
#
# WHERE IT IS MOUNTED, and why that path and no other.  Waydroid's own
# defaults list `/usr/share/waydroid-extra/images` as a PREINSTALLED images
# path (tools/config/__init__.py).  When system.img and vendor.img are found
# there, `waydroid init` adopts that path AND tolerates a failed OTA fetch
# rather than treating it as fatal - which is exactly what an appliance with
# no route to sourceforge needs.  Mounting the image anywhere else means an
# init that tries to download 1 GB at boot and dies when it cannot.
set -e
WORK=${1:-/work/unodos-guest}
HERE="$(cd "$(dirname "$0")" && pwd)"
CACHE="$WORK/android-cache"

SYSTEM_JSON=${SYSTEM_JSON:-https://ota.waydro.id/system/lineage/waydroid_x86_64/VANILLA.json}
VENDOR_JSON=${VENDOR_JSON:-https://ota.waydro.id/vendor/waydroid_x86_64/MAINLINE.json}

# GApps is PROPRIETARY and never the default.  `--gapps` switches the system
# channel to the GAPPS build, which the user is then choosing to fetch onto
# their own machine; nothing here redistributes it and CI never runs it.
for a in "$@"; do
    if [ "$a" = "--gapps" ]; then
        SYSTEM_JSON=https://ota.waydro.id/system/lineage/waydroid_x86_64/GAPPS.json
        echo "build_android: GAPPS build selected - proprietary, user-supplied"
    fi
done

mkdir -p "$CACHE"
cd "$CACHE"

fetch() {                       # fetch <json-url> <what>
    local json="$1" what="$2" url name sha
    read -r url name sha <<EOF
$(curl -fsSL "$json" | python3 -c 'import json,sys
d = json.load(sys.stdin)["response"][0]
print(d["url"], d["filename"], d["id"])')
EOF
    [ -n "$name" ] || { echo "build_android: no $what in $json" >&2; exit 1; }
    echo "  $what: $name"
    [ -f "$name" ] || curl -fL --retry 3 -o "$name" "$url"
    # The OTA index calls it `id`; it is the sha256 of the zip.  Checked every
    # run, not just after a download: a cache is a place a truncated file
    # lives forever otherwise.
    echo "$sha  $name" | sha256sum -c - >/dev/null
    echo "$name"
}

echo "build_android: fetching images (cached in $CACHE)"
SYS_ZIP=$(fetch "$SYSTEM_JSON" system | tail -1)
VEN_ZIP=$(fetch "$VENDOR_JSON" vendor | tail -1)

rm -rf "$CACHE/stage"
mkdir -p "$CACHE/stage"
echo "build_android: extracting"
unzip -o -q "$SYS_ZIP" -d "$CACHE/stage"
unzip -o -q "$VEN_ZIP" -d "$CACHE/stage"
[ -f "$CACHE/stage/system.img" ] || { echo "no system.img in $SYS_ZIP" >&2; exit 1; }
[ -f "$CACHE/stage/vendor.img" ] || { echo "no vendor.img in $VEN_ZIP" >&2; exit 1; }
ls -la "$CACHE/stage"

# Pack with mke2fs -d, in a container, exactly as build_rootfs.sh does: no
# loop devices and no host root.  Sized from the payload with 12% of slack -
# an ext4 with no free blocks at all refuses to mount rw on some kernels and
# this one is mounted ro anyway, so the slack is for the filesystem's own
# metadata rather than for growth.
BYTES=$(du -sb "$CACHE/stage" | cut -f1)
MIB=$(( BYTES / 1048576 * 112 / 100 + 64 ))
echo "build_android: packing ${MIB} MiB ext4"
docker run --rm -v "$CACHE":/c -v "$WORK":/out alpine:3.20 sh -c "
    apk add --no-cache e2fsprogs >/dev/null &&
    rm -f /out/android.img &&
    truncate -s ${MIB}M /out/android.img &&
    mke2fs -q -t ext4 -L uno-android -d /c/stage /out/android.img"

ls -la "$WORK/android.img"
echo "android runtime: $WORK/android.img"
echo
echo "stage it beside the other payload files:"
echo "    cp $WORK/android.img build/android.img"
echo "    UNO_DISK_MIB=4200 python3 tools/vm_stage.py"
