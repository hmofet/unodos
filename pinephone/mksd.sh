#!/bin/sh
# UnoDOS / PinePhone (Allwinner A64) — build a SELF-BOOTING microSD card.
#
# The kernel build (build.sh) only produces a flat payload, build/unodos.bin, that
# must be loaded at 0x40080000 AFTER something has brought up DRAM and the DSI panel.
# This script builds that "something" — mainline U-Boot + ARM Trusted Firmware for the
# A64 — plus a boot script, and assembles a bootable SD image:
#
#   BROM -> SPL (8 KB offset) -> U-Boot (lights the DSI panel) -> distro_bootcmd
#        -> finds boot.scr on the FAT partition -> loads unodos.bin -> go 0x40080000
#
# Run this ON A LINUX BOX (it needs parted/mkfs.vfat/losetup/mkimage/dtc and, to write,
# sudo + a card reader). It is NOT a WSL/Windows script like build.sh. Tested on the
# "devbuntu" flashing box.
#
# Usage:
#   ./mksd.sh fw                 # build U-Boot + ATF -> build/u-boot-sunxi-with-spl.bin
#   ./mksd.sh image              # assemble bootable image -> build/pinephone-unodos.img
#   ./mksd.sh write /dev/sdX     # dd the image onto a card (guarded; confirms removable)
#   ./mksd.sh all   /dev/sdX     # fw + image + write
#
# Env overrides:
#   WORK=<dir>     scratch dir for toolchain + U-Boot/ATF checkouts (default ~/pine-uboot)
#   IMG_MB=<n>     image size in MiB (default 960; the FAT partition spans it)
#   CROSS=<prefix> use an existing aarch64 cross prefix instead of auto-fetching one
#
# The one real-hardware unknown is cache coherency at `go` — see boot.cmd and the
# README "Self-booting microSD" section.
set -e
cd "$(dirname "$0")"
HERE=$(pwd)

WORK="${WORK:-$HOME/pine-uboot}"
IMG_MB="${IMG_MB:-960}"
UBOOT_REPO="https://github.com/u-boot/u-boot.git"
ATF_REPO="https://github.com/ARM-software/arm-trusted-firmware.git"
# kernel.org crosstool: a 'nolibc' aarch64 gcc, ideal for bare-metal U-Boot/ATF.
TC_VER="14.2.0"
TC_TARBALL="x86_64-gcc-${TC_VER}-nolibc-aarch64-linux.tar.xz"
TC_URL="https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/x86_64/${TC_VER}/${TC_TARBALL}"

FW="$HERE/build/u-boot-sunxi-with-spl.bin"   # SPL + U-Boot FIT (incl. BL31)
IMG="$HERE/build/pinephone-unodos.img"

# --- resolve a cross-compiler prefix (auto-fetch a userspace one; no sudo/apt) ---
resolve_cross() {
  if [ -n "$CROSS" ]; then return; fi
  if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    CROSS=aarch64-linux-gnu-; return
  fi
  TC_DIR="$WORK/gcc-${TC_VER}-nolibc/aarch64-linux/bin"
  if [ ! -x "$TC_DIR/aarch64-linux-gcc" ]; then
    echo "[tc] fetching $TC_TARBALL ..."
    mkdir -p "$WORK"; ( cd "$WORK" && curl -fsSL "$TC_URL" -o cross.tar.xz && tar xf cross.tar.xz && rm -f cross.tar.xz )
  fi
  CROSS="$TC_DIR/aarch64-linux-"
}

# --- swig + pyelftools (U-Boot host tools) via a venv, to avoid system pip/PEP-668 ---
ensure_pytools() {
  VENV="$WORK/venv"
  [ -x "$VENV/bin/swig" ] && { . "$VENV/bin/activate"; return; }
  python3 -m venv "$VENV"; . "$VENV/bin/activate"
  pip install --quiet --upgrade pip
  pip install --quiet swig pyelftools setuptools
}

cmd_fw() {
  resolve_cross; ensure_pytools
  mkdir -p "$WORK" "$HERE/build"

  # ARM Trusted Firmware -> bl31.bin (the A64 secure monitor U-Boot loads).
  if [ ! -d "$WORK/arm-trusted-firmware" ]; then
    git clone --depth 1 "$ATF_REPO" "$WORK/arm-trusted-firmware"
  fi
  echo "[fw] building ATF bl31 (sun50i_a64)..."
  make -s -C "$WORK/arm-trusted-firmware" CROSS_COMPILE="$CROSS" PLAT=sun50i_a64 DEBUG=0 bl31
  BL31="$WORK/arm-trusted-firmware/build/sun50i_a64/release/bl31.bin"

  # Mainline U-Boot (pinephone_defconfig => CONFIG_VIDEO/PANEL/BACKLIGHT=y, lights DSI).
  if [ ! -d "$WORK/u-boot" ]; then
    git clone --depth 1 "$UBOOT_REPO" "$WORK/u-boot"
  fi
  cd "$WORK/u-boot"
  make -s CROSS_COMPILE="$CROSS" pinephone_defconfig
  # Drop the EFI capsule host tool (wants gnutls headers; unneeded here) and add the
  # `cache` command so boot.scr can disable caches before `go` (coherency, see README).
  ./scripts/config --disable TOOLS_MKEFICAPSULE
  ./scripts/config --disable EFI_CAPSULE_ON_DISK
  ./scripts/config --disable EFI_CAPSULE_FIRMWARE_FIT
  ./scripts/config --disable EFI_CAPSULE_FIRMWARE_RAW
  ./scripts/config --disable EFI_CAPSULE_AUTHENTICATE
  ./scripts/config --enable  CMD_CACHE
  make -s CROSS_COMPILE="$CROSS" olddefconfig >/dev/null
  echo "[fw] building U-Boot..."
  make -s CROSS_COMPILE="$CROSS" BL31="$BL31" SCP=/dev/null -j"$(nproc)"
  cp u-boot-sunxi-with-spl.bin "$FW"
  cd "$HERE"
  echo "[fw] done: $FW ($(wc -c < "$FW") bytes)"
}

cmd_image() {
  [ -f "$FW" ] || { echo "missing $FW — run './mksd.sh fw' first"; exit 1; }
  [ -f "$HERE/build/unodos.bin" ] || { echo "missing build/unodos.bin — run './build.sh' first"; exit 1; }
  command -v mkimage >/dev/null || { echo "need u-boot-tools (mkimage)"; exit 1; }

  echo "[img] compiling boot.scr from boot.cmd..."
  mkimage -A arm64 -O u-boot -T script -C none -d "$HERE/boot.cmd" "$HERE/build/boot.scr" >/dev/null

  # Build the whole image on a loop device, then we only ever write the card once.
  # (Doing parted/mkfs directly on a USB device fights write-blocker udev rules.)
  echo "[img] assembling ${IMG_MB} MiB image..."
  rm -f "$IMG"; truncate -s "$((IMG_MB*1024*1024))" "$IMG"
  sudo parted -s "$IMG" mklabel msdos
  sudo parted -s "$IMG" mkpart primary fat32 1MiB 100%
  sudo parted -s "$IMG" set 1 lba on
  LOOP=$(sudo losetup -fP --show "$IMG")
  sudo mkfs.vfat -F 32 -n UNODOS "${LOOP}p1" >/dev/null
  MP=$(mktemp -d)
  sudo mount "${LOOP}p1" "$MP"
  sudo cp "$HERE/build/unodos.bin" "$MP/unodos.bin"
  sudo cp "$HERE/build/boot.scr"   "$MP/boot.scr"
  sudo sync; sudo umount "$MP"; rmdir "$MP"
  sudo losetup -d "$LOOP"
  # SPL+U-Boot live in the 8 KB .. 1 MiB gap before the partition.
  sudo dd if="$FW" of="$IMG" bs=1024 seek=8 conv=notrunc,fsync status=none
  echo "[img] done: $IMG"
}

cmd_write() {
  DEV="$1"
  [ -b "$DEV" ] || { echo "usage: ./mksd.sh write /dev/sdX  (block device)"; exit 1; }
  [ -f "$IMG" ] || { echo "missing $IMG — run './mksd.sh image' first"; exit 1; }
  # Guard: refuse anything that isn't a removable/USB disk.
  RM=$(lsblk -ndo RM "$DEV"); TRAN=$(lsblk -ndo TRAN "$DEV")
  echo "target $DEV: RM=$RM TRAN=$TRAN MODEL='$(lsblk -ndo MODEL "$DEV" | xargs)' SIZE=$(lsblk -ndo SIZE "$DEV")"
  [ "$RM" = "1" ] || [ "$TRAN" = "usb" ] || { echo "ABORT: $DEV is not removable/USB"; exit 1; }
  # Re-arm any read-only write-block on exit (devbuntu policy; harmless elsewhere).
  trap 'sudo blockdev --setro "$DEV" 2>/dev/null || true' EXIT
  sudo blockdev --setrw "$DEV" 2>/dev/null || true
  echo "[write] dd $IMG -> $DEV ..."
  sudo dd if="$IMG" of="$DEV" bs=4M conv=fsync status=progress
  sudo sync
  echo "[write] done. Eject, insert into the PinePhone, power on."
}

case "${1:-}" in
  fw)    cmd_fw ;;
  image) cmd_image ;;
  write) cmd_write "$2" ;;
  all)   cmd_fw; cmd_image; cmd_write "$2" ;;
  *) echo "usage: ./mksd.sh {fw|image|write /dev/sdX|all /dev/sdX}"; exit 1 ;;
esac
