#!/bin/bash
# UnoDOS Boot Floppy Writer for Linux
# Run as root: sudo ./boot.sh [device]
# Usage: sudo ./boot.sh /dev/fd0

DEVICE="${1:-/dev/fd0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Pulling latest from GitHub..."
cd "$PROJECT_DIR"
git fetch origin 2>/dev/null
git reset --hard origin/master 2>/dev/null
if [ $? -ne 0 ]; then
    echo "Git failed, using local version"
else
    echo "Updated!"
fi

# Find image
IMAGE="$PROJECT_DIR/build/unodos-144.img"
if [ ! -f "$IMAGE" ]; then
    IMAGE="$PROJECT_DIR/build/unodos.img"
fi
if [ ! -f "$IMAGE" ]; then
    echo "ERROR: No image found in build directory"
    exit 1
fi

echo "Writing $(basename "$IMAGE") to $DEVICE..."

dd if="$IMAGE" of="$DEVICE" bs=512 status=progress
sync

if [ $? -ne 0 ]; then
    echo "FAILED! Run as root."
    exit 1
fi

echo "Done! Boot floppy ready."
