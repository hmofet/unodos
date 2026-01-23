#!/usr/bin/env python3
"""
Dump FAT12 directory entries from a floppy image or device
This helps debug why UnoDOS can't find files
"""

import sys
import struct

def dump_fat12_dir(filename):
    with open(filename, 'rb') as f:
        # Read boot sector
        f.seek(0)
        boot = f.read(512)

        # Parse BPB
        bytes_per_sector = struct.unpack('<H', boot[0x0B:0x0D])[0]
        sectors_per_cluster = boot[0x0D]
        reserved_sectors = struct.unpack('<H', boot[0x0E:0x10])[0]
        num_fats = boot[0x10]
        root_entries = struct.unpack('<H', boot[0x11:0x13])[0]
        sectors_per_fat = struct.unpack('<H', boot[0x16:0x18])[0]

        print(f"FAT12 Filesystem Info:")
        print(f"  Bytes per sector: {bytes_per_sector}")
        print(f"  Sectors per cluster: {sectors_per_cluster}")
        print(f"  Reserved sectors: {reserved_sectors}")
        print(f"  Number of FATs: {num_fats}")
        print(f"  Root directory entries: {root_entries}")
        print(f"  Sectors per FAT: {sectors_per_fat}")
        print()

        # Calculate root directory location
        root_dir_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
        root_dir_start = reserved_sectors + (num_fats * sectors_per_fat)

        print(f"Root directory starts at sector {root_dir_start}")
        print(f"Root directory spans {root_dir_sectors} sectors")
        print()

        # Read root directory
        f.seek(root_dir_start * bytes_per_sector)
        root_dir = f.read(root_dir_sectors * bytes_per_sector)

        print("Directory Entries:")
        print("=" * 80)

        for i in range(root_entries):
            offset = i * 32
            entry = root_dir[offset:offset + 32]

            # Check if entry is free
            if entry[0] == 0x00:
                print(f"Entry {i}: [END OF DIRECTORY]")
                break
            if entry[0] == 0xE5:
                print(f"Entry {i}: [DELETED]")
                continue

            # Parse entry
            filename = entry[0:11].decode('ascii', errors='replace')
            attributes = entry[11]
            cluster = struct.unpack('<H', entry[26:28])[0]
            filesize = struct.unpack('<I', entry[28:32])[0]

            # Decode attributes
            attr_str = []
            if attributes & 0x01: attr_str.append("RO")
            if attributes & 0x02: attr_str.append("HIDDEN")
            if attributes & 0x04: attr_str.append("SYSTEM")
            if attributes & 0x08: attr_str.append("VOLUME")
            if attributes & 0x0F == 0x0F: attr_str.append("LFN")
            if attributes & 0x10: attr_str.append("DIR")
            if attributes & 0x20: attr_str.append("ARCHIVE")

            attr_display = f"0x{attributes:02X} ({', '.join(attr_str) if attr_str else 'NONE'})"

            print(f"Entry {i:2d}: '{filename}' attr={attr_display} cluster={cluster} size={filesize}")

            # Show hex dump of first 11 bytes (filename)
            hex_dump = ' '.join(f"{b:02X}" for b in entry[0:11])
            print(f"           Filename bytes: {hex_dump}")
            print()

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: dump_fat12.py <floppy_image_or_device>")
        print("Example: dump_fat12.py /dev/fd0")
        print("Example: dump_fat12.py A:")
        print("Example: dump_fat12.py test.img")
        sys.exit(1)

    dump_fat12_dir(sys.argv[1])
