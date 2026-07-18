#!/usr/bin/env python3
"""Build a UEFI-bootable USB image for UnoDOS/pc64: GPT + one EFI System
Partition (FAT32) holding the whole build/esp/ tree - /EFI/BOOT/BOOTX64.EFI plus
the bundled docs and TrueType fonts.

pc64/build.sh only produces the ESP as a *directory* (build/esp/), which QEMU can
fake with `-drive fat:rw:build/esp`.  Real UEFI firmware needs a partition table:
it finds the OS by scanning GPT for an EF00 (EFI System) partition and running
/EFI/BOOT/BOOTX64.EFI from the removable-media default path.  This packs that
directory into a raw disk image you can write to a USB stick with dd, Rufus,
balenaEtcher, or the bundled Windows flasher (pc64/flash/).

Run under WSL / Linux (needs sgdisk + mtools).

  python3 tools/mkuefi.py            # 128 MiB dev image -> build/unodos-uefi.img
  python3 tools/mkuefi.py 2048       # 2 GiB release image (documents get room)
  UNO_DISK_MIB=512 python3 tools/mkuefi.py

Prereq: ./build.sh  (produces build/esp/EFI/BOOT/BOOTX64.EFI).
"""
import os
import re
import subprocess
import sys

PC64 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(PC64, "build")
ESP_DIR = os.path.join(BUILD, "esp")                 # the tree build.sh emits
DISK = os.path.join(BUILD, "unodos-uefi.img")        # output raw image
FAT = "/tmp/uno_esp.img"                             # scratch partition image
SECTOR = 512

_mib = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("UNO_DISK_MIB", "128"))
DISK_SECTORS = _mib * 2048                           # MiB -> 512-byte sectors


def run(cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    boot = os.path.join(ESP_DIR, "EFI", "BOOT", "BOOTX64.EFI")
    if not os.path.exists(boot):
        sys.exit("build/esp/EFI/BOOT/BOOTX64.EFI not found - run ./build.sh first")

    # 1. blank image sized to the requested capacity
    with open(DISK, "wb") as f:
        f.truncate(DISK_SECTORS * SECTOR)

    # 2. GPT with a single EFI System Partition spanning the disk (LBA 2048 -> end)
    run(["sgdisk", "--zap-all", DISK])
    run(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNO-ESP", DISK])

    info = subprocess.run(["sgdisk", "-i", "1", DISK],
                          check=True, capture_output=True, text=True).stdout
    first = int(re.search(r"First sector:\s*(\d+)", info).group(1))
    last = int(re.search(r"Last sector:\s*(\d+)", info).group(1))
    part_sectors = last - first + 1

    # 3. format the partition as FAT32 (-F) and copy the ESP tree into it.
    #    mformat's -h/-s geometry matches what real firmware expects on removable
    #    media; -F forces FAT32 so the volume is valid at release sizes too.
    run(["mformat", "-C", "-i", FAT, "-T", str(part_sectors),
         "-h", "64", "-s", "32", "-F", "-v", "UNODOS", "::"])
    # copy every top-level entry of build/esp/ into the volume root.  mcopy -s
    # recurses into directories (so EFI/BOOT/BOOTX64.EFI lands correctly); plain
    # files (HELLO.MD, the .TTFs, PAGE.HTML) copy straight to ::/.
    for name in sorted(os.listdir(ESP_DIR)):
        src = os.path.join(ESP_DIR, name)
        if os.path.isdir(src):
            run(["mcopy", "-s", "-i", FAT, src, "::/"])
        else:
            run(["mcopy", "-i", FAT, src, "::/" + name])

    # 4. splice the formatted partition back into the disk image at its LBA
    with open(FAT, "rb") as f:
        data = f.read()
    with open(DISK, "r+b") as f:
        f.seek(first * SECTOR)
        f.write(data)
    os.remove(FAT)

    efi_bytes = os.path.getsize(boot)
    print("unodos-uefi.img: %d MiB, GPT + ESP (FAT32) at LBA %d (%d sectors), "
          "/EFI/BOOT/BOOTX64.EFI (%d B)" % (_mib, first, part_sectors, efi_bytes))


if __name__ == "__main__":
    main()
