#!/bin/bash
# Build an appliance rootfs on quill, inside docker (the tree needs root-owned
# files and device-free packing; a container gives both without touching the
# host).  Produces <workdir>/rootfs-<app>.img, and copies it to rootfs.img -
# staged as EFI\UNODOS\VM\ROOTFS.IMG by tools/vm_stage.py.
#
#     ./build_rootfs.sh [workdir]                  # chromium, the default
#     UNO_APP=gimp ./build_rootfs.sh [workdir]     # GIMP, multi-window
#
# WHICH APPLIANCE IS A PARAMETER.  `apps/<name>.app` is the whole definition;
# see apps/README.md.  The per-app image is kept under its own name as well as
# under rootfs.img, because the two builds are twenty minutes apart and
# rebuilding the one you are not testing is the sort of thing that gets done
# by accident at midnight.
set -e
WORK=${1:-/work/unodos-guest}
HERE="$(cd "$(dirname "$0")" && pwd)"
APP=${UNO_APP:-chromium}

[ -f "$HERE/apps/$APP.app" ] || {
    echo "no such appliance: $APP" >&2
    echo "have: $(cd "$HERE/apps" && ls *.app | sed 's/\.app$//' | tr '\n' ' ')" >&2
    exit 1; }

# --privileged is NOT used, and the build has to survive without it: an
# appliance whose build needs a privileged container is one that cannot be
# built in CI.  What it costs is that an app file's chroot steps (GIMP primes
# its plug-in cache that way) are best-effort - they say so when they cannot
# run, and the appliance pays the cost at first boot instead.
docker run --rm \
    -e UNO_APP="$APP" \
    --cap-add SYS_ADMIN --security-opt apparmor=unconfined \
    -v "$WORK":/out \
    -v "$HERE":/scripts:ro \
    alpine:3.20 sh /scripts/rootfs_inner.sh

mv "$WORK/rootfs.img" "$WORK/rootfs-$APP.img"
cp "$WORK/rootfs-$APP.img" "$WORK/rootfs.img"
ls -la "$WORK/rootfs-$APP.img" "$WORK/rootfs.img"
