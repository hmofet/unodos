#!/usr/bin/env python3
"""
Create a FAT12 floppy image with applications for UnoDOS app loader testing.

Usage:
  Single file:   python3 create_app_test.py output.img clock.bin HELLO.BIN
  Multiple files: python3 create_app_test.py output.img launcher.bin HELLO.BIN clock.bin CLOCK.BIN

Arguments after output.img come in pairs: source_file FAT_FILENAME
If only one file is provided without FAT_FILENAME, the name is derived from the source.
"""

import sys
import struct
import os

def format_fat_filename(filename):
    """Convert filename to 8.3 FAT format (11 bytes, space-padded)."""
    parts = filename.upper().split('.')
    name = parts[0][:8].ljust(8)
    ext = (parts[1] if len(parts) > 1 else '')[:3].ljust(3)
    return name + ext

def create_fat12_floppy(output_path, files):
    """
    Create a 1.44MB FAT12 floppy with multiple app binaries.
    files: list of tuples (bin_path, fat_filename)
    """
    # Read all app binaries
    apps = []
    for bin_path, fat_filename in files:
        with open(bin_path, 'rb') as f:
            app_data = f.read()

        # Format FAT filename
        if fat_filename is None:
            basename = os.path.basename(bin_path)
            name, ext = os.path.splitext(basename)
            fat_filename = name.upper()[:8].ljust(8) + ext.upper()[1:4].ljust(3)
        else:
            fat_filename = format_fat_filename(fat_filename)

        apps.append({
            'data': app_data,
            'size': len(app_data),
            'fat_name': fat_filename
        })
        print(f"App: '{fat_filename.strip()}' - {len(app_data)} bytes")

    # FAT12 parameters for 1.44MB floppy
    SECTOR_SIZE = 512
    SECTORS_PER_CLUSTER = 1
    RESERVED_SECTORS = 1
    NUM_FATS = 2
    ROOT_ENTRIES = 224
    TOTAL_SECTORS = 2880
    SECTORS_PER_FAT = 9

    # Calculate data start
    ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE
    DATA_START_SECTOR = RESERVED_SECTORS + (NUM_FATS * SECTORS_PER_FAT) + ROOT_DIR_SECTORS

    # Create empty floppy image
    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    # Build boot sector (sector 0)
    boot_sector = bytearray(SECTOR_SIZE)
    boot_sector[0:3] = b'\xEB\x3C\x90'  # JMP short 0x3E, NOP
    boot_sector[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', boot_sector, 11, SECTOR_SIZE)
    boot_sector[13] = SECTORS_PER_CLUSTER
    struct.pack_into('<H', boot_sector, 14, RESERVED_SECTORS)
    boot_sector[16] = NUM_FATS
    struct.pack_into('<H', boot_sector, 17, ROOT_ENTRIES)
    struct.pack_into('<H', boot_sector, 19, TOTAL_SECTORS)
    boot_sector[21] = 0xF0
    struct.pack_into('<H', boot_sector, 22, SECTORS_PER_FAT)
    struct.pack_into('<H', boot_sector, 24, 18)  # Sectors per track
    struct.pack_into('<H', boot_sector, 26, 2)   # Number of heads
    boot_sector[38] = 0x29
    struct.pack_into('<L', boot_sector, 39, 0x12345678)
    boot_sector[43:54] = b'APPTESTDSK '
    boot_sector[54:62] = b'FAT12   '
    boot_sector[510] = 0x55
    boot_sector[511] = 0xAA
    image[0:SECTOR_SIZE] = boot_sector

    # Build FAT table
    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    fat[0] = 0xF0
    fat[1] = 0xFF
    fat[2] = 0xFF

    # Allocate clusters for each app
    current_cluster = 2
    data_offset = DATA_START_SECTOR * SECTOR_SIZE

    for app in apps:
        clusters_needed = (app['size'] + SECTOR_SIZE - 1) // SECTOR_SIZE
        if clusters_needed == 0:
            clusters_needed = 1

        app['start_cluster'] = current_cluster
        app['data_offset'] = data_offset

        # Allocate clusters in FAT
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

        # Copy app data to data area
        image[data_offset:data_offset + len(app['data'])] = app['data']

        current_cluster += clusters_needed
        data_offset += clusters_needed * SECTOR_SIZE

    # Copy FAT to both areas
    fat1_start = RESERVED_SECTORS * SECTOR_SIZE
    fat2_start = (RESERVED_SECTORS + SECTORS_PER_FAT) * SECTOR_SIZE
    image[fat1_start:fat1_start + len(fat)] = fat
    image[fat2_start:fat2_start + len(fat)] = fat

    # Build root directory
    root_dir_start = (RESERVED_SECTORS + NUM_FATS * SECTORS_PER_FAT) * SECTOR_SIZE

    # Volume label
    vol_entry = bytearray(32)
    vol_entry[0:11] = b'APPTESTDSK '
    vol_entry[11] = 0x08
    image[root_dir_start:root_dir_start + 32] = vol_entry

    # File entries
    for i, app in enumerate(apps):
        entry = bytearray(32)
        entry[0:11] = app['fat_name'].encode('ascii')
        entry[11] = 0x20  # Archive attribute
        struct.pack_into('<H', entry, 26, app['start_cluster'])
        struct.pack_into('<L', entry, 28, app['size'])
        offset = root_dir_start + (i + 1) * 32
        image[offset:offset + 32] = entry

    # Write image
    with open(output_path, 'wb') as f:
        f.write(image)

    print(f"Created {output_path} ({len(image)} bytes)")
    for app in apps:
        print(f"  {app['fat_name'].strip()} at cluster {app['start_cluster']}, size {app['size']} bytes")


def parse_args(args):
    """Parse command line arguments into file list.
    Format: output.img file1.bin [FAT_NAME1] [file2.bin FAT_NAME2] ...
    FAT names are identified by NOT containing a path separator and having .BIN extension.
    """
    if len(args) < 2:
        return None, []

    output_path = args[0]
    files = []

    # Remaining args come in pairs: source_file FAT_NAME
    # Or single: source_file (FAT name derived from source)
    i = 1
    while i < len(args):
        bin_path = args[i]
        fat_name = None

        # Check if this looks like a real file path (contains / or exists)
        is_file = os.path.exists(bin_path) or '/' in bin_path

        if not is_file:
            # This might be a FAT name that was misplaced, skip
            i += 1
            continue

        # Check if next arg is a FAT name (doesn't exist as file, no path separator)
        if i + 1 < len(args):
            next_arg = args[i + 1]
            next_is_fat_name = not os.path.exists(next_arg) and '/' not in next_arg
            if next_is_fat_name:
                fat_name = next_arg
                i += 2
            else:
                i += 1
        else:
            i += 1

        files.append((bin_path, fat_name))

    return output_path, files


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        print(f"Usage: {sys.argv[0]} output.img app.bin [FAT_NAME] [app2.bin FAT_NAME2] ...")
        sys.exit(1)

    output_path, files = parse_args(sys.argv[1:])

    if not files:
        print("Error: No input files specified")
        sys.exit(1)

    create_fat12_floppy(output_path, files)
