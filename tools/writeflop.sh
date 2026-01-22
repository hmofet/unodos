#!/bin/bash
# UnoDOS Floppy Writer
# Writes UnoDOS image to a physical 1.44MB or 360KB floppy disk

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# Default values
IMAGE=""
DEVICE=""
VERIFY=1

usage() {
    echo "UnoDOS Floppy Writer"
    echo ""
    echo "Usage: $0 [OPTIONS] <device>"
    echo ""
    echo "Options:"
    echo "  -i, --image <file>   Specify image file (default: auto-detect based on device)"
    echo "  -1, --144            Use 1.44MB image (unodos-144.img)"
    echo "  -3, --360            Use 360KB image (unodos.img)"
    echo "  -n, --no-verify      Skip verification after write"
    echo "  -h, --help           Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 /dev/fd0              Write to first floppy drive (auto-detect size)"
    echo "  $0 -1 /dev/sdb           Write 1.44MB image to USB floppy"
    echo "  $0 --360 /dev/fd0        Write 360KB image to floppy"
    echo ""
    echo "WARNING: This will DESTROY all data on the target device!"
    exit 1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--image)
            IMAGE="$2"
            shift 2
            ;;
        -1|--144)
            IMAGE="$BUILD_DIR/unodos-144.img"
            shift
            ;;
        -3|--360)
            IMAGE="$BUILD_DIR/unodos.img"
            shift
            ;;
        -n|--no-verify)
            VERIFY=0
            shift
            ;;
        -h|--help)
            usage
            ;;
        -*)
            echo "Unknown option: $1"
            usage
            ;;
        *)
            DEVICE="$1"
            shift
            ;;
    esac
done

# Check device specified
if [[ -z "$DEVICE" ]]; then
    echo "Error: No device specified"
    usage
fi

# Check device exists
if [[ ! -b "$DEVICE" ]]; then
    echo "Error: $DEVICE is not a block device"
    exit 1
fi

# Auto-detect image if not specified
if [[ -z "$IMAGE" ]]; then
    # Check if 1.44MB image exists, prefer it for compatibility
    if [[ -f "$BUILD_DIR/unodos-144.img" ]]; then
        IMAGE="$BUILD_DIR/unodos-144.img"
    elif [[ -f "$BUILD_DIR/unodos.img" ]]; then
        IMAGE="$BUILD_DIR/unodos.img"
    else
        echo "Error: No image found. Run 'make' or 'make floppy144' first."
        exit 1
    fi
fi

# Check image exists
if [[ ! -f "$IMAGE" ]]; then
    echo "Error: Image file not found: $IMAGE"
    echo "Run 'make' to build it first."
    exit 1
fi

IMAGE_SIZE=$(stat -c%s "$IMAGE")
IMAGE_NAME=$(basename "$IMAGE")

echo "========================================"
echo "UnoDOS Floppy Writer"
echo "========================================"
echo ""
echo "Image:  $IMAGE_NAME ($IMAGE_SIZE bytes)"
echo "Device: $DEVICE"
echo ""

# Safety check - don't write to hard drives
if [[ "$DEVICE" =~ ^/dev/sd[a-z]$ ]] || [[ "$DEVICE" =~ ^/dev/nvme ]] || [[ "$DEVICE" =~ ^/dev/hd ]]; then
    # Check if it's actually a floppy/small device
    DEVICE_SIZE=$(blockdev --getsize64 "$DEVICE" 2>/dev/null || echo "0")
    if [[ "$DEVICE_SIZE" -gt 2000000 ]]; then
        echo "WARNING: $DEVICE appears to be larger than a floppy disk!"
        echo "Device size: $DEVICE_SIZE bytes"
        echo ""
        read -p "Are you ABSOLUTELY SURE you want to write to this device? (type 'YES' to confirm): " CONFIRM
        if [[ "$CONFIRM" != "YES" ]]; then
            echo "Aborted."
            exit 1
        fi
    fi
fi

echo "WARNING: All data on $DEVICE will be destroyed!"
read -p "Continue? (y/N): " CONFIRM
if [[ "$CONFIRM" != "y" && "$CONFIRM" != "Y" ]]; then
    echo "Aborted."
    exit 1
fi

echo ""
echo "Writing image to $DEVICE..."

# Unmount if mounted
if mount | grep -q "^$DEVICE"; then
    echo "Unmounting $DEVICE..."
    sudo umount "$DEVICE" 2>/dev/null || true
fi

# Write the image
sudo dd if="$IMAGE" of="$DEVICE" bs=512 status=progress conv=fdatasync

echo ""
echo "Write complete!"

# Verify if requested
if [[ "$VERIFY" -eq 1 ]]; then
    echo ""
    echo "Verifying..."

    TEMP_FILE=$(mktemp)
    trap "rm -f $TEMP_FILE" EXIT

    SECTORS=$((IMAGE_SIZE / 512))
    sudo dd if="$DEVICE" of="$TEMP_FILE" bs=512 count=$SECTORS status=none

    if cmp -s "$IMAGE" "$TEMP_FILE"; then
        echo "Verification PASSED!"
    else
        echo "Verification FAILED! The disk may be bad."
        exit 1
    fi
fi

echo ""
echo "========================================"
echo "Success! Floppy is ready to boot."
echo "========================================"
