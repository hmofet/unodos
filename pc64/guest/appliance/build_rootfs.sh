#!/bin/bash
# Build the Chromium appliance rootfs on quill, inside docker (the tree needs
# root-owned files and device-free packing; a container gives both without
# touching the host).  Produces <workdir>/rootfs.img - staged as
# EFI\UNODOS\VM\ROOTFS.IMG by tools/vm_stage.py.
#
#     ./build_rootfs.sh [workdir]        # default /work/unodos-guest
set -e
WORK=${1:-/work/unodos-guest}
HERE="$(cd "$(dirname "$0")" && pwd)"

docker run --rm \
    -v "$WORK":/out \
    -v "$HERE":/scripts:ro \
    alpine:3.20 sh /scripts/rootfs_inner.sh
ls -la "$WORK/rootfs.img"
