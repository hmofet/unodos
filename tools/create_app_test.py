#!/usr/bin/env python3
"""
Create a FAT12 floppy image with an application for UnoDOS app loader testing.
Usage: python3 create_app_test.py output.img app.bin [FAT_FILENAME]
If FAT_FILENAME not provided, derives from input filename (e.g., clock.bin -> CLOCK.BIN)
"""

import sys
import struct
import os

def create_fat12_floppy(output_path, app_bin_path, fat_filename=None):
    """Create a 1.44MB FAT12 floppy with the app binary."""

    # Read the app binary
    with open(app_bin_path, 'rb') as f:
        app_data = f.read()

    app_size = len(app_data)

    # Derive FAT filename from input path if not provided
    if fat_filename is None:
        basename = os.path.basename(app_bin_path)
        name, ext = os.path.splitext(basename)
        fat_filename = name.upper()[:8].ljust(8) + ext.upper()[1:4].ljust(3)
    else:
        # Ensure proper 8.3 format
        parts = fat_filename.upper().split('.')
        name = parts[0][:8].ljust(8)
        ext = (parts[1] if len(parts) > 1 else '')[:3].ljust(3)
        fat_filename = name + ext

    print(f"App size: {app_size} bytes")
    print(f"FAT filename: '{fat_filename}'")

    # FAT12 parameters for 1.44MB floppy
    SECTOR_SIZE = 512
    SECTORS_PER_CLUSTER = 1
    RESERVED_SECTORS = 1
    NUM_FATS = 2
    ROOT_ENTRIES = 224
    TOTAL_SECTORS = 2880
    SECTORS_PER_FAT = 9
    SECTORS_PER_TRACK = 18
    NUM_HEADS = 2

    # Calculate cluster for data
    ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE
    DATA_START_SECTOR = RESERVED_SECTORS + (NUM_FATS * SECTORS_PER_FAT) + ROOT_DIR_SECTORS

    # Create empty floppy image
    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    # Build boot sector (sector 0)
    boot_sector = bytearray(SECTOR_SIZE)

    # Jump instruction
    boot_sector[0:3] = b'\xEB\x3C\x90'  # JMP short 0x3E, NOP

    # OEM name
    boot_sector[3:11] = b'MSDOS5.0'

    # BIOS Parameter Block (BPB)
    struct.pack_into('<H', boot_sector, 11, SECTOR_SIZE)          # Bytes per sector
    boot_sector[13] = SECTORS_PER_CLUSTER                          # Sectors per cluster
    struct.pack_into('<H', boot_sector, 14, RESERVED_SECTORS)      # Reserved sectors
    boot_sector[16] = NUM_FATS                                     # Number of FATs
    struct.pack_into('<H', boot_sector, 17, ROOT_ENTRIES)          # Root directory entries
    struct.pack_into('<H', boot_sector, 19, TOTAL_SECTORS)         # Total sectors (16-bit)
    boot_sector[21] = 0xF0                                         # Media descriptor (1.44MB floppy)
    struct.pack_into('<H', boot_sector, 22, SECTORS_PER_FAT)       # Sectors per FAT
    struct.pack_into('<H', boot_sector, 24, SECTORS_PER_TRACK)     # Sectors per track
    struct.pack_into('<H', boot_sector, 26, NUM_HEADS)             # Number of heads
    struct.pack_into('<L', boot_sector, 28, 0)                     # Hidden sectors
    struct.pack_into('<L', boot_sector, 32, 0)                     # Total sectors (32-bit, not used)
    boot_sector[36] = 0x00                                         # Drive number
    boot_sector[37] = 0x00                                         # Reserved
    boot_sector[38] = 0x29                                         # Extended boot signature
    struct.pack_into('<L', boot_sector, 39, 0x12345678)            # Volume serial number
    boot_sector[43:54] = b'APPTESTDSK '                            # Volume label (11 bytes)
    boot_sector[54:62] = b'FAT12   '                               # Filesystem type

    # Boot signature
    boot_sector[510] = 0x55
    boot_sector[511] = 0xAA

    # Copy boot sector to image
    image[0:SECTOR_SIZE] = boot_sector

    # Build FAT tables
    # First 3 bytes: media descriptor + 0xFF 0xFF
    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    fat[0] = 0xF0  # Media descriptor
    fat[1] = 0xFF
    fat[2] = 0xFF

    # Calculate clusters needed for app
    clusters_needed = (app_size + SECTOR_SIZE * SECTORS_PER_CLUSTER - 1) // (SECTOR_SIZE * SECTORS_PER_CLUSTER)
    if clusters_needed == 0:
        clusters_needed = 1

    print(f"Clusters needed: {clusters_needed}")

    # Allocate clusters for app (starting at cluster 2)
    for i in range(clusters_needed):
        cluster = 2 + i
        if i == clusters_needed - 1:
            # Last cluster - mark as EOF
            next_val = 0xFFF
        else:
            next_val = cluster + 1

        # FAT12 packing: each entry is 12 bits
        # Entry at index N starts at byte offset (N * 3) / 2
        byte_offset = (cluster * 3) // 2
        if cluster % 2 == 0:
            # Even cluster: low 12 bits
            fat[byte_offset] = next_val & 0xFF
            fat[byte_offset + 1] = (fat[byte_offset + 1] & 0xF0) | ((next_val >> 8) & 0x0F)
        else:
            # Odd cluster: high 12 bits
            fat[byte_offset] = (fat[byte_offset] & 0x0F) | ((next_val << 4) & 0xF0)
            fat[byte_offset + 1] = (next_val >> 4) & 0xFF

    # Copy FAT to both FAT areas
    fat1_start = RESERVED_SECTORS * SECTOR_SIZE
    fat2_start = (RESERVED_SECTORS + SECTORS_PER_FAT) * SECTOR_SIZE
    image[fat1_start:fat1_start + len(fat)] = fat
    image[fat2_start:fat2_start + len(fat)] = fat

    # Build root directory
    root_dir_start = (RESERVED_SECTORS + NUM_FATS * SECTORS_PER_FAT) * SECTOR_SIZE

    # First entry: Volume label
    vol_entry = bytearray(32)
    vol_entry[0:11] = b'APPTESTDSK '  # Volume label
    vol_entry[11] = 0x08  # Volume label attribute
    image[root_dir_start:root_dir_start + 32] = vol_entry

    # Second entry: Application file
    file_entry = bytearray(32)
    file_entry[0:11] = fat_filename.encode('ascii')  # Filename in 8.3 format
    file_entry[11] = 0x20  # Archive attribute
    file_entry[12:22] = b'\x00' * 10  # Reserved
    struct.pack_into('<H', file_entry, 22, 0x0000)  # Creation time
    struct.pack_into('<H', file_entry, 24, 0x0000)  # Creation date
    struct.pack_into('<H', file_entry, 26, 2)       # Starting cluster
    struct.pack_into('<L', file_entry, 28, app_size)  # File size
    image[root_dir_start + 32:root_dir_start + 64] = file_entry

    # Copy app data to data area
    data_area_start = DATA_START_SECTOR * SECTOR_SIZE
    image[data_area_start:data_area_start + len(app_data)] = app_data

    # Write image to file
    with open(output_path, 'wb') as f:
        f.write(image)

    print(f"Created {output_path} ({len(image)} bytes)")
    print(f"  {fat_filename.strip()} at cluster 2, size {app_size} bytes")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} output.img app.bin [FAT_FILENAME]")
        sys.exit(1)

    fat_name = sys.argv[3] if len(sys.argv) > 3 else None
    create_fat12_floppy(sys.argv[1], sys.argv[2], fat_name)
