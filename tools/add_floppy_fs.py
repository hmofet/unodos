#!/usr/bin/env python3
"""
Add FAT12 filesystem to UnoDOS floppy image after the OS sectors.

The floppy layout is:
  Sector 0: Boot sector
  Sectors 1-4: Stage2
  Sector 5: Reserved
  Sectors 6-61: Kernel (28KB = 56 sectors)
  Sectors 62+: FAT12 filesystem with apps

This script writes a FAT12 filesystem starting at sector 62.
"""

import sys
import struct
import os

SECTOR_SIZE = 512
OS_SECTORS = 78  # Boot + Stage2 + Reserved + Kernel (expanded Build 281)
FS_START_SECTOR = 78

def format_fat_filename(filename):
    """Convert filename to 8.3 FAT format (11 bytes, space-padded)."""
    parts = filename.upper().split('.')
    name = parts[0][:8].ljust(8)
    ext = (parts[1] if len(parts) > 1 else '')[:3].ljust(3)
    return name + ext

def add_fat12_filesystem(image_path, files):
    """Add FAT12 filesystem to existing floppy image."""

    # Read existing image
    with open(image_path, 'rb') as f:
        image = bytearray(f.read())

    total_sectors = len(image) // SECTOR_SIZE
    fs_sectors = total_sectors - FS_START_SECTOR

    print(f"Image: {len(image)} bytes ({total_sectors} sectors)")
    print(f"OS area: sectors 0-{OS_SECTORS-1}")
    print(f"Filesystem: sectors {FS_START_SECTOR}-{total_sectors-1} ({fs_sectors} sectors)")

    # FAT12 parameters for the filesystem area
    SECTORS_PER_CLUSTER = 1
    RESERVED_SECTORS = 1  # Boot sector (within filesystem)
    NUM_FATS = 2
    ROOT_ENTRIES = 224
    SECTORS_PER_FAT = 9

    # Calculate offsets within filesystem
    ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE
    DATA_START_SECTOR = RESERVED_SECTORS + (NUM_FATS * SECTORS_PER_FAT) + ROOT_DIR_SECTORS

    fs_offset = FS_START_SECTOR * SECTOR_SIZE

    # Create boot sector for the filesystem
    boot_sector = bytearray(SECTOR_SIZE)
    boot_sector[0:3] = b'\xEB\x3C\x90'  # JMP short, NOP
    boot_sector[3:11] = b'UNODOS  '      # OEM name
    struct.pack_into('<H', boot_sector, 11, SECTOR_SIZE)  # Bytes per sector
    boot_sector[13] = SECTORS_PER_CLUSTER
    struct.pack_into('<H', boot_sector, 14, RESERVED_SECTORS)
    boot_sector[16] = NUM_FATS
    struct.pack_into('<H', boot_sector, 17, ROOT_ENTRIES)
    struct.pack_into('<H', boot_sector, 19, fs_sectors)  # Total sectors in FS
    boot_sector[21] = 0xF0  # Media descriptor
    struct.pack_into('<H', boot_sector, 22, SECTORS_PER_FAT)
    struct.pack_into('<H', boot_sector, 24, 18)  # Sectors per track (1.44MB)
    struct.pack_into('<H', boot_sector, 26, 2)   # Heads
    boot_sector[38] = 0x29  # Extended boot signature
    struct.pack_into('<L', boot_sector, 39, 0x12345678)  # Volume serial
    boot_sector[43:54] = b'UNODOS     '  # Volume label
    boot_sector[54:62] = b'FAT12   '     # Filesystem type
    boot_sector[510] = 0x55
    boot_sector[511] = 0xAA

    image[fs_offset:fs_offset + SECTOR_SIZE] = boot_sector

    # Initialize FAT
    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    fat[0] = 0xF0
    fat[1] = 0xFF
    fat[2] = 0xFF

    # Allocate clusters for files
    current_cluster = 2
    data_offset = fs_offset + DATA_START_SECTOR * SECTOR_SIZE
    file_info = []

    for bin_path, fat_name in files:
        if not os.path.exists(bin_path):
            print(f"Warning: {bin_path} not found, skipping")
            continue

        with open(bin_path, 'rb') as f:
            file_data = f.read()

        file_size = len(file_data)
        clusters_needed = (file_size + SECTOR_SIZE - 1) // SECTOR_SIZE
        if clusters_needed == 0:
            clusters_needed = 1

        start_cluster = current_cluster

        # Allocate in FAT
        for i in range(clusters_needed):
            cluster = current_cluster + i
            if i == clusters_needed - 1:
                next_val = 0xFFF  # EOF
            else:
                next_val = cluster + 1

            # FAT12 packing
            byte_offset = (cluster * 3) // 2
            if cluster % 2 == 0:
                fat[byte_offset] = next_val & 0xFF
                fat[byte_offset + 1] = (fat[byte_offset + 1] & 0xF0) | ((next_val >> 8) & 0x0F)
            else:
                fat[byte_offset] = (fat[byte_offset] & 0x0F) | ((next_val << 4) & 0xF0)
                fat[byte_offset + 1] = (next_val >> 4) & 0xFF

        # Write file data
        image[data_offset:data_offset + file_size] = file_data

        file_info.append({
            'name': fat_name,
            'cluster': start_cluster,
            'size': file_size
        })

        print(f"  {fat_name}: cluster {start_cluster}, {file_size} bytes")

        current_cluster += clusters_needed
        data_offset += clusters_needed * SECTOR_SIZE

    # Write FAT to both areas
    fat1_offset = fs_offset + RESERVED_SECTORS * SECTOR_SIZE
    fat2_offset = fs_offset + (RESERVED_SECTORS + SECTORS_PER_FAT) * SECTOR_SIZE
    image[fat1_offset:fat1_offset + len(fat)] = fat
    image[fat2_offset:fat2_offset + len(fat)] = fat

    # Build root directory
    root_offset = fs_offset + (RESERVED_SECTORS + NUM_FATS * SECTORS_PER_FAT) * SECTOR_SIZE

    # Volume label entry
    vol_entry = bytearray(32)
    vol_entry[0:11] = b'UNODOS     '
    vol_entry[11] = 0x08  # Volume label attribute
    image[root_offset:root_offset + 32] = vol_entry

    # File entries
    for i, info in enumerate(file_info):
        entry = bytearray(32)
        entry[0:11] = format_fat_filename(info['name']).encode('ascii')
        entry[11] = 0x20  # Archive attribute
        struct.pack_into('<H', entry, 26, info['cluster'])
        struct.pack_into('<I', entry, 28, info['size'])

        entry_offset = root_offset + (i + 1) * 32
        image[entry_offset:entry_offset + 32] = entry

    # Write updated image
    with open(image_path, 'wb') as f:
        f.write(image)

    print(f"\nUpdated {image_path}")
    return True

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        print(f"\nUsage: {sys.argv[0]} floppy.img file1.bin FILE1.BIN [file2.bin FILE2.BIN ...]")
        print("\nExample:")
        print(f"  {sys.argv[0]} unodos-144.img build/launcher.bin LAUNCHER.BIN")
        sys.exit(1)

    image_path = sys.argv[1]

    # Parse file pairs
    files = []
    i = 2
    while i < len(sys.argv):
        if i + 1 >= len(sys.argv):
            print(f"Error: Missing FAT name for {sys.argv[i]}")
            sys.exit(1)
        files.append((sys.argv[i], sys.argv[i+1]))
        i += 2

    if not os.path.exists(image_path):
        print(f"Error: {image_path} not found")
        sys.exit(1)

    add_fat12_filesystem(image_path, files)
