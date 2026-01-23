#!/usr/bin/env python3
"""
Create a test FAT12 image with a file larger than 512 bytes
to test multi-cluster reading in UnoDOS v3.10.0+
"""

import struct
import sys

def create_test_image(output_file):
    """Create a FAT12 image with a multi-cluster test file"""

    # Create 360KB blank image (720 sectors × 512 bytes)
    image = bytearray(720 * 512)

    # Boot sector (simplified - just enough for FAT12 mount to work)
    # Jump instruction
    image[0:3] = b'\xEB\x3C\x90'

    # OEM name
    image[3:11] = b'UNODOS  '

    # BPB (BIOS Parameter Block)
    struct.pack_into('<H', image, 0x0B, 512)        # Bytes per sector
    struct.pack_into('<B', image, 0x0D, 1)          # Sectors per cluster
    struct.pack_into('<H', image, 0x0E, 1)          # Reserved sectors
    struct.pack_into('<B', image, 0x10, 2)          # Number of FATs
    struct.pack_into('<H', image, 0x11, 224)        # Root directory entries
    struct.pack_into('<H', image, 0x13, 720)        # Total sectors (360KB)
    struct.pack_into('<B', image, 0x15, 0xF0)       # Media descriptor (floppy)
    struct.pack_into('<H', image, 0x16, 9)          # Sectors per FAT
    struct.pack_into('<H', image, 0x18, 18)         # Sectors per track
    struct.pack_into('<H', image, 0x1A, 2)          # Number of heads

    # Boot signature
    struct.pack_into('<H', image, 510, 0xAA55)

    # FAT1 starts at sector 1 (offset 0x200)
    fat1_offset = 512

    # FAT2 starts at sector 10 (offset 0x1400)
    fat2_offset = 512 * 10

    # Root directory starts at sector 19 (offset 0x2600)
    root_offset = 512 * 19

    # Data area starts at sector 33 (offset 0x4200)
    data_offset = 512 * 33

    # Initialize FAT with media descriptor
    image[fat1_offset:fat1_offset + 3] = b'\xF0\xFF\xFF'
    image[fat2_offset:fat2_offset + 3] = b'\xF0\xFF\xFF'

    # Create test file content (1024 bytes = 2 clusters)
    # Make it easily recognizable
    file_content = bytearray()
    file_content += b'CLUSTER 1: ' + (b'A' * 500) + b'\n'
    file_content += b'CLUSTER 2: ' + (b'B' * 500) + b'\n'
    file_size = len(file_content)

    print(f"Test file size: {file_size} bytes ({(file_size + 511) // 512} clusters)")

    # Write file to data area
    # First cluster (cluster 2) at sector 33
    cluster1_offset = data_offset
    image[cluster1_offset:cluster1_offset + 512] = file_content[0:512]

    # Second cluster (cluster 3) at sector 34
    cluster2_offset = data_offset + 512
    image[cluster2_offset:cluster2_offset + (file_size - 512)] = file_content[512:file_size]

    # Set up FAT chain: cluster 2 → cluster 3 → EOF
    # Cluster 2 entry at FAT offset (2 * 3) / 2 = 3
    # For cluster 2 (even): next cluster = 3
    # Byte 3-4 should contain 0x003 in lower 12 bits
    set_fat_entry(image, fat1_offset, 2, 3)
    set_fat_entry(image, fat2_offset, 2, 3)

    # Cluster 3 entry at FAT offset (3 * 3) / 2 = 4.5 = 4
    # For cluster 3 (odd): EOF marker = 0xFFF
    # Byte 4-5 should contain 0xFFF in upper 12 bits
    set_fat_entry(image, fat1_offset, 3, 0xFFF)
    set_fat_entry(image, fat2_offset, 3, 0xFFF)

    # Create directory entry for TEST.TXT
    # Each entry is 32 bytes
    entry_offset = root_offset

    # Filename (8.3 format, space-padded)
    image[entry_offset:entry_offset + 11] = b'TEST    TXT'

    # Attributes (0x20 = archive)
    image[entry_offset + 0x0B] = 0x20

    # Starting cluster (offset 0x1A)
    struct.pack_into('<H', image, entry_offset + 0x1A, 2)

    # File size (offset 0x1C)
    struct.pack_into('<I', image, entry_offset + 0x1C, file_size)

    # Write image to file
    with open(output_file, 'wb') as f:
        f.write(image)

    print(f"Created {output_file}")
    print(f"FAT chain: cluster 2 → cluster 3 → EOF")
    print(f"File content preview: {file_content[:50]}")

def set_fat_entry(image, fat_offset, cluster, value):
    """Set a FAT12 entry (12-bit value)"""
    byte_offset = fat_offset + (cluster * 3) // 2

    if cluster % 2 == 0:
        # Even cluster: lower 12 bits
        word = struct.unpack_from('<H', image, byte_offset)[0]
        word = (word & 0xF000) | (value & 0x0FFF)
        struct.pack_into('<H', image, byte_offset, word)
    else:
        # Odd cluster: upper 12 bits
        word = struct.unpack_from('<H', image, byte_offset)[0]
        word = (word & 0x000F) | ((value & 0x0FFF) << 4)
        struct.pack_into('<H', image, byte_offset, word)

if __name__ == '__main__':
    output = 'build/test-fat12-multi.img'
    if len(sys.argv) > 1:
        output = sys.argv[1]

    create_test_image(output)
    print(f"\nTest with: make test-fat12-multi")
