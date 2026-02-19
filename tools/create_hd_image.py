#!/usr/bin/env python3
"""
Create a bootable FAT16 hard drive image for UnoDOS.

Usage:
  python3 create_hd_image.py output.img

This creates a 64MB hard drive image with:
- MBR with partition table
- FAT16 partition with UnoDOS boot code
- Kernel and apps pre-installed

Files needed in build/ directory:
- mbr.bin     (512 bytes)
- vbr.bin     (512 bytes)
- stage2_hd.bin (2048 bytes)
- kernel.bin  (28KB)

Files from apps/ directory automatically included:
- CLOCK.BIN, BROWSER.BIN, MOUSE.BIN, MUSIC.BIN, LAUNCHER.BIN
"""

import sys
import struct
import os

# Disk geometry for 64MB image (CHS compatible)
SECTOR_SIZE = 512
SECTORS_PER_TRACK = 63
HEADS = 16
CYLINDERS = 130                    # ~64MB (130 * 16 * 63 * 512 = ~66MB)
TOTAL_SECTORS = CYLINDERS * HEADS * SECTORS_PER_TRACK  # 131,040 sectors

# FAT16 parameters
RESERVED_SECTORS = 5               # VBR + 4 sectors for stage2_hd
SECTORS_PER_CLUSTER = 4            # 2KB clusters
FAT_COPIES = 2
ROOT_ENTRIES = 512                 # 32 sectors for root dir
PARTITION_START = 63               # Partition at sector 63 (standard CHS alignment)
PARTITION_SECTORS = TOTAL_SECTORS - PARTITION_START

# Calculate FAT size
# Each cluster is 4 sectors, so we have PARTITION_SECTORS / 4 clusters
# Each FAT entry is 2 bytes, so we need (clusters * 2) / 512 sectors for FAT
NUM_CLUSTERS = PARTITION_SECTORS // SECTORS_PER_CLUSTER
FAT_SIZE = (NUM_CLUSTERS * 2 + SECTOR_SIZE - 1) // SECTOR_SIZE

# Root directory sectors
ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE

# Data area start (within partition)
DATA_START = RESERVED_SECTORS + (FAT_COPIES * FAT_SIZE) + ROOT_DIR_SECTORS


def format_fat_filename(filename):
    """Convert filename to 8.3 FAT format (11 bytes, space-padded)."""
    parts = filename.upper().split('.')
    name = parts[0][:8].ljust(8)
    ext = (parts[1] if len(parts) > 1 else '')[:3].ljust(3)
    return (name + ext).encode('ascii')


def lba_to_chs(lba):
    """Convert LBA to CHS for partition table."""
    cylinder = lba // (HEADS * SECTORS_PER_TRACK)
    head = (lba // SECTORS_PER_TRACK) % HEADS
    sector = (lba % SECTORS_PER_TRACK) + 1  # 1-based

    # CHS encoding for partition table
    if cylinder > 1023:
        cylinder = 1023  # Max for CHS

    return (head, (sector & 0x3F) | ((cylinder >> 2) & 0xC0), cylinder & 0xFF)


def create_mbr(mbr_bin_path):
    """Create MBR with partition table."""
    mbr = bytearray(SECTOR_SIZE)

    # Load MBR boot code
    with open(mbr_bin_path, 'rb') as f:
        boot_code = f.read(446)
    mbr[0:len(boot_code)] = boot_code

    # Create partition table entry at 0x1BE
    # One bootable FAT16 partition starting at sector 1
    start_chs = lba_to_chs(PARTITION_START)
    end_chs = lba_to_chs(PARTITION_START + PARTITION_SECTORS - 1)

    partition = bytearray(16)
    partition[0] = 0x80             # Bootable
    partition[1] = start_chs[0]     # Start head
    partition[2] = start_chs[1]     # Start sector + cyl high
    partition[3] = start_chs[2]     # Start cyl low
    partition[4] = 0x06             # FAT16 (DOS 3.31+)
    partition[5] = end_chs[0]       # End head
    partition[6] = end_chs[1]       # End sector + cyl high
    partition[7] = end_chs[2]       # End cyl low
    struct.pack_into('<I', partition, 8, PARTITION_START)
    struct.pack_into('<I', partition, 12, PARTITION_SECTORS)

    mbr[0x1BE:0x1BE+16] = partition

    # Empty entries for partitions 2-4
    for i in range(3):
        mbr[0x1CE + i*16:0x1CE + (i+1)*16] = bytes(16)

    # Boot signature
    mbr[510] = 0x55
    mbr[511] = 0xAA

    return mbr


def create_vbr(vbr_bin_path):
    """Create VBR with BPB for FAT16."""
    vbr = bytearray(SECTOR_SIZE)

    # Load VBR boot code
    with open(vbr_bin_path, 'rb') as f:
        vbr_template = f.read()

    # Copy the template (it has jump, OEM, and boot code)
    vbr[0:len(vbr_template)] = vbr_template

    # Update BPB fields (these may already be set in template, but ensure correct)
    struct.pack_into('<H', vbr, 0x0B, SECTOR_SIZE)          # Bytes per sector
    vbr[0x0D] = SECTORS_PER_CLUSTER                          # Sectors per cluster
    struct.pack_into('<H', vbr, 0x0E, RESERVED_SECTORS)     # Reserved sectors
    vbr[0x10] = FAT_COPIES                                   # Number of FATs
    struct.pack_into('<H', vbr, 0x11, ROOT_ENTRIES)         # Root entries
    struct.pack_into('<H', vbr, 0x13, 0)                    # Total sectors 16 (0 = use 32-bit)
    vbr[0x15] = 0xF8                                         # Media type (fixed disk)
    struct.pack_into('<H', vbr, 0x16, FAT_SIZE)             # Sectors per FAT
    struct.pack_into('<H', vbr, 0x18, SECTORS_PER_TRACK)    # Sectors per track
    struct.pack_into('<H', vbr, 0x1A, HEADS)                # Number of heads
    struct.pack_into('<I', vbr, 0x1C, PARTITION_START)      # Hidden sectors
    struct.pack_into('<I', vbr, 0x20, PARTITION_SECTORS)    # Total sectors 32

    # Extended BPB
    vbr[0x24] = 0x80                # Drive number
    vbr[0x25] = 0                   # Reserved
    vbr[0x26] = 0x29                # Extended boot signature
    struct.pack_into('<I', vbr, 0x27, 0x12345678)           # Volume serial
    vbr[0x2B:0x36] = b'UNODOS     '                         # Volume label
    vbr[0x36:0x3E] = b'FAT16   '                            # FS type

    # Boot signature
    vbr[510] = 0x55
    vbr[511] = 0xAA

    return vbr


def create_fat16_image(output_path, files):
    """Create complete FAT16 hard drive image."""
    print(f"Creating {TOTAL_SECTORS * SECTOR_SIZE // 1024 // 1024}MB FAT16 image...")
    print(f"  Partition: {PARTITION_SECTORS} sectors")
    print(f"  Clusters: {NUM_CLUSTERS}")
    print(f"  FAT size: {FAT_SIZE} sectors")
    print(f"  Data start: sector {DATA_START}")

    # Create empty image
    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    # Write MBR at sector 0
    mbr = create_mbr('build/mbr.bin')
    image[0:SECTOR_SIZE] = mbr
    print("  MBR written")

    # Partition offset in image
    part_offset = PARTITION_START * SECTOR_SIZE

    # Write VBR at partition start
    vbr = create_vbr('build/vbr.bin')
    image[part_offset:part_offset + SECTOR_SIZE] = vbr
    print("  VBR written")

    # Write stage2_hd at reserved sectors 1-4
    with open('build/stage2_hd.bin', 'rb') as f:
        stage2 = f.read()
    stage2_offset = part_offset + SECTOR_SIZE
    image[stage2_offset:stage2_offset + len(stage2)] = stage2
    print(f"  stage2_hd written ({len(stage2)} bytes)")

    # Initialize FAT (first two entries are reserved)
    fat_offset = part_offset + RESERVED_SECTORS * SECTOR_SIZE
    fat = bytearray(FAT_SIZE * SECTOR_SIZE)
    struct.pack_into('<H', fat, 0, 0xFFF8)  # Media type marker
    struct.pack_into('<H', fat, 2, 0xFFFF)  # Reserved

    # Root directory offset
    root_offset = fat_offset + (FAT_COPIES * FAT_SIZE * SECTOR_SIZE)

    # Data area offset
    data_offset = root_offset + (ROOT_DIR_SECTORS * SECTOR_SIZE)

    # Current cluster for allocation (starts at 2)
    current_cluster = 2
    root_entry_index = 0

    # Add volume label
    vol_entry = bytearray(32)
    vol_entry[0:11] = b'UNODOS     '
    vol_entry[11] = 0x08  # Volume label attribute
    image[root_offset:root_offset + 32] = vol_entry
    root_entry_index = 1

    # Add each file
    for bin_path, fat_name in files:
        if not os.path.exists(bin_path):
            print(f"  Warning: {bin_path} not found, skipping")
            continue

        with open(bin_path, 'rb') as f:
            file_data = f.read()

        file_size = len(file_data)
        clusters_needed = (file_size + SECTORS_PER_CLUSTER * SECTOR_SIZE - 1) // (SECTORS_PER_CLUSTER * SECTOR_SIZE)
        if clusters_needed == 0:
            clusters_needed = 1

        # Check if we have space
        if current_cluster + clusters_needed > NUM_CLUSTERS:
            print(f"  Error: No space for {fat_name}")
            continue

        start_cluster = current_cluster

        # Allocate clusters in FAT
        for i in range(clusters_needed):
            cluster = current_cluster + i
            if i == clusters_needed - 1:
                next_val = 0xFFFF  # EOF
            else:
                next_val = cluster + 1

            struct.pack_into('<H', fat, cluster * 2, next_val)

        # Write file data to data area
        file_offset = data_offset + (start_cluster - 2) * SECTORS_PER_CLUSTER * SECTOR_SIZE
        image[file_offset:file_offset + file_size] = file_data

        # Create directory entry
        dir_entry = bytearray(32)
        dir_entry[0:11] = format_fat_filename(fat_name)
        dir_entry[11] = 0x20  # Archive attribute
        struct.pack_into('<H', dir_entry, 26, start_cluster)
        struct.pack_into('<I', dir_entry, 28, file_size)

        entry_offset = root_offset + root_entry_index * 32
        image[entry_offset:entry_offset + 32] = dir_entry

        print(f"  {fat_name}: cluster {start_cluster}, {file_size} bytes")

        current_cluster += clusters_needed
        root_entry_index += 1

    # Copy FAT to both areas
    image[fat_offset:fat_offset + len(fat)] = fat
    image[fat_offset + FAT_SIZE * SECTOR_SIZE:fat_offset + FAT_SIZE * SECTOR_SIZE + len(fat)] = fat
    print("  FAT written")

    # Write image
    with open(output_path, 'wb') as f:
        f.write(image)

    print(f"\nCreated {output_path} ({len(image)} bytes)")
    return True


def find_apps():
    """Find app binaries to include."""
    files = []

    # Always include kernel first
    kernel_path = 'build/kernel.bin'
    if os.path.exists(kernel_path):
        files.append((kernel_path, 'KERNEL.BIN'))
    else:
        print("Error: build/kernel.bin not found!")
        return None

    # Look for apps with their actual build names
    # Format: (build_name, fat_name)
    app_mappings = [
        ('launcher.bin', 'LAUNCHER.BIN'),
        ('clock.bin', 'CLOCK.BIN'),
        ('browser.bin', 'BROWSER.BIN'),
        ('mouse_test.bin', 'MOUSE.BIN'),
        ('music.bin', 'MUSIC.BIN'),
        ('mkboot.bin', 'MKBOOT.BIN'),
        ('settings.bin', 'SETTINGS.BIN'),
        ('tetris.bin', 'TETRIS.BIN'),
        ('demo.bin', 'DEMO.BIN'),
        ('apitest.bin', 'APITEST.BIN'),
        ('notepad.bin', 'TEXT.BIN'),
    ]

    for build_name, fat_name in app_mappings:
        app_path = f'build/{build_name}'
        if os.path.exists(app_path):
            files.append((app_path, fat_name))
        else:
            print(f"  Warning: {app_path} not found, skipping {fat_name}")

    return files


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        print(f"Usage: {sys.argv[0]} output.img")
        sys.exit(1)

    output_path = sys.argv[1]

    # Check required files exist
    required = ['build/mbr.bin', 'build/vbr.bin', 'build/stage2_hd.bin', 'build/kernel.bin']
    missing = [f for f in required if not os.path.exists(f)]
    if missing:
        print("Error: Missing required files:")
        for f in missing:
            print(f"  {f}")
        print("\nRun 'make hd-image' to build all required files first.")
        sys.exit(1)

    # Find apps
    files = find_apps()
    if files is None:
        sys.exit(1)

    # Create image
    if create_fat16_image(output_path, files):
        print("\nImage created successfully!")
        print("\nTo test in QEMU:")
        print(f"  qemu-system-i386 -hda {output_path}")
    else:
        sys.exit(1)
