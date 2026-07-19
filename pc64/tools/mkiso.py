#!/usr/bin/env python3
"""Pack build/esp into a hybrid UEFI-bootable ISO (build/unodos-pc64.iso).

One artifact, every delivery path:
  - VM hypervisors (QEMU/VirtualBox/VMware/Hyper-V): attach as a CD - the
    El Torito EFI entry points at an embedded FAT image and OVMF/real UEFI
    firmware boots it.
  - dd / BalenaEtcher / Rufus (dd mode): the ISO also carries an MBR+GPT
    partition table whose EFI System Partition is that same FAT image, so a
    raw sector-copy to a USB stick boots on real UEFI machines.
  - Rufus (file-copy mode): the ISO9660 tree contains the full ESP layout
    (EFI/BOOT/BOOTX64.EFI, APPS/, fonts, docs), so extracting onto a
    FAT-formatted stick boots too.

Needs xorriso + mtools (WSL/Linux).  Run after ./build.sh:

  python3 tools/mkiso.py
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)

ESP = "build/esp"
FATIMG = "build/iso_efiboot.img"
ISO = "build/unodos-pc64.iso"


def total_size(root):
    n = 0
    for d, _, fs in os.walk(root):
        for f in fs:
            n += os.path.getsize(os.path.join(d, f))
    return n


def main():
    if not os.path.isdir(ESP):
        sys.exit("no %s - run ./build.sh first" % ESP)

    # ---- the FAT (ESP) image the firmware actually boots -------------------
    # FAT32 always (-F): mformat's auto pick makes small images FAT12, which
    # UnoDOS's own FAT stack rightly refuses (and must not mis-mount) - 33 MiB
    # is FAT32's practical floor.
    need = total_size(ESP)
    mib = max(33, (need * 12 // 10) // (1024 * 1024) + 1)
    with open(FATIMG, "wb") as f:
        f.truncate(mib * 1024 * 1024)
    subprocess.run(["mformat", "-i", FATIMG, "-F", "-v", "UNODOS", "::"], check=True)
    subprocess.run(["mcopy", "-i", FATIMG, "-s"]
                   + [os.path.join(ESP, e) for e in sorted(os.listdir(ESP))]
                   + ["::/"], check=True)

    # ---- the hybrid ISO ----------------------------------------------------
    # El Torito EFI entry -> appended partition 2 (the FAT image), which the
    # MBR/GPT also declares as an EFI System Partition (0xef) - the modern
    # Ubuntu-style layout: CD boot and dd-to-USB boot from one file. The
    # ISO9660 tree additionally carries the whole ESP for file-copy tools.
    if os.path.exists(ISO):
        os.remove(ISO)
    subprocess.run([
        "xorriso", "-as", "mkisofs",
        "-r", "-J", "-joliet-long",
        "-V", "UNODOS",
        "-o", ISO,
        "-partition_offset", "16",
        "-append_partition", "2", "0xef", FATIMG,
        "-appended_part_as_gpt",
        "-e", "--interval:appended_partition_2:all::",
        "-no-emul-boot",
        ESP,
    ], check=True)
    print("%s: %d KB (ESP image %d MiB inside)"
          % (ISO, os.path.getsize(ISO) // 1024, mib))


if __name__ == "__main__":
    main()
