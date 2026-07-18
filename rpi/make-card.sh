#!/bin/sh
# ============================================================================
# make-card.sh — build a COMPLETE bootable UnoDOS/Pi microSD image.
#
# A Raspberry Pi boots from a FAT partition that must contain the closed-source
# VideoCore firmware (the GPU is the boot CPU) PLUS our kernel + config.txt.
# This script gathers everything and emits one MBR+FAT32 image you can flash:
#
#     bash rpi/make-card.sh
#     # -> rpi/build/unodos-rpi.img   (dd this to the SD card)
#
# Flash with:  sudo dd if=rpi/build/unodos-rpi.img of=/dev/sdX bs=4M conv=fsync
# (replace /dev/sdX with your card; ON THE devbuntu BOX honour the USB write
# blocker: blockdev --setrw /dev/sdX before dd, --setro after.)
#
# Runs unprivileged: mkfs.vfat + mtools format/copy a partition-sized FAT image,
# then sfdisk lays an MBR and we dd the FAT into the partition slot. No mount,
# no root. Needs: aarch64-linux-gnu binutils, mkfs.vfat (dosfstools), mtools,
# sfdisk (util-linux), curl. Run inside WSL/devbuntu.
# ============================================================================
set -e
cd "$(dirname "$0")"

FW_DIR=firmware
IMG=build/unodos-rpi.img
PART=build/_part.fat
IMG_MB=257                 # 1 MiB MBR gap + 256 MiB FAT partition
PART_MB=256
FW_BASE=https://raw.githubusercontent.com/raspberrypi/firmware/master/boot

# Closed VideoCore blobs (cannot be self-built): Pi 3 = bootcode/start/fixup,
# Pi 4 = start4/fixup4 (Pi 4 has no bootcode.bin — it's in the SoC).
# DTBs + the disable-bt overlay are REQUIRED for dtoverlay=disable-bt to take
# effect (it reroutes the PL011 to GPIO14/15 for the serial console; without
# them the overlay is silently ignored and GPIO14/15 stays on the mini-UART).
FW_FILES="bootcode.bin start.elf fixup.dat start4.elf fixup4.dat \
          bcm2710-rpi-3-b.dtb bcm2710-rpi-3-b-plus.dtb bcm2710-rpi-zero-2-w.dtb \
          bcm2711-rpi-4-b.dtb"
OVERLAYS="disable-bt.dtbo"

PY="${PY:-python3}"
AS=aarch64-linux-gnu-as
LD=aarch64-linux-gnu-ld
OC=aarch64-linux-gnu-objcopy

echo "[1/4] building kernels (Pi 3 + Pi 4)..."
mkdir -p build
( cd .. && "$PY" rpi/mkdata.py )               # font/icons/palettes -> build/gfx.s
# Pi 3 (BCM2837, PERIPH 0x3F000000 default)
$AS -march=armv8-a kernel.s -o build/kernel.o
$AS -march=armv8-a build/gfx.s -o build/gfx.o
$LD -T link.ld build/kernel.o build/gfx.o -o build/kernel.elf
$OC -O binary build/kernel.elf build/kernel8.img
# Pi 4 (BCM2711, PERIPH 0xFE000000)
$AS -march=armv8-a --defsym PERIPH=0xFE000000 kernel.s -o build/k4.o
$LD -T link.ld build/k4.o build/gfx.o -o build/k4.elf
$OC -O binary build/k4.elf build/kernel8-pi4.img

echo "[2/4] fetching VideoCore firmware + DTBs + overlay (cached in $FW_DIR/)..."
mkdir -p "$FW_DIR" "$FW_DIR/overlays"
for f in $FW_FILES; do
  if [ ! -s "$FW_DIR/$f" ]; then
    echo "      downloading $f"
    curl -fsSL "$FW_BASE/$f" -o "$FW_DIR/$f"
  fi
done
for o in $OVERLAYS; do
  if [ ! -s "$FW_DIR/overlays/$o" ]; then
    echo "      downloading overlays/$o"
    curl -fsSL "$FW_BASE/overlays/$o" -o "$FW_DIR/overlays/$o"
  fi
done

echo "[3/4] formatting a $PART_MB MiB FAT32 partition image..."
rm -f "$PART"
# block-count for mkfs.vfat is in 1 KiB blocks
mkfs.vfat -F 32 -n UNODOS -C "$PART" $((PART_MB * 1024)) >/dev/null
mcopy -i "$PART" config.txt ::config.txt
mcopy -i "$PART" build/kernel8.img ::kernel8.img
mcopy -i "$PART" build/kernel8-pi4.img ::kernel8-pi4.img
for f in $FW_FILES; do mcopy -i "$PART" "$FW_DIR/$f" "::$f"; done
mmd   -i "$PART" ::overlays
for o in $OVERLAYS; do mcopy -i "$PART" "$FW_DIR/overlays/$o" "::overlays/$o"; done

echo "[4/4] assembling MBR image + partition..."
rm -f "$IMG"
truncate -s ${IMG_MB}M "$IMG"
# one bootable FAT32-LBA partition starting at 1 MiB (sector 2048)
printf '2048,,c,*\n' | sfdisk --quiet "$IMG"
dd if="$PART" of="$IMG" bs=1M seek=1 conv=notrunc status=none
rm -f "$PART"

echo "done: rpi/$IMG ($(wc -c < "$IMG") bytes)"
echo "contents:"; mdir -i "$IMG@@1M"
