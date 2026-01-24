#!/usr/bin/env python3
"""Create a bootable FAT12 test floppy with TEST.TXT included"""
import subprocess
import struct

def create_bootable_test():
    # First create the test FAT12 image
    subprocess.run(['python3', 'tools/create_multicluster_test.py', 'build/unodos-test.img'], check=True)
    
    # Build UnoDOS
    subprocess.run(['make', 'floppy144'], check=True)
    
    # Read UnoDOS boot sectors (sectors 0-4 = boot + stage2)
    with open('build/unodos-144.img', 'rb') as f:
        unodos_boot = f.read(512 * 5)  # First 5 sectors
    
    # Read the test image
    with open('build/unodos-test.img', 'rb') as f:
        test_img = bytearray(f.read())
    
    # Preserve the BPB from the test image (bytes 3-61 of sector 0)
    bpb = test_img[3:62]
    
    # Copy UnoDOS boot code but preserve FAT12 BPB
    # Boot sector: bytes 0-2 (jmp), 3-61 (BPB), 62-509 (boot code), 510-511 (signature)
    boot_sector = bytearray(unodos_boot[:512])
    boot_sector[3:62] = bpb  # Preserve BPB from FAT12 image
    
    # Write combined image
    test_img[0:512] = boot_sector  # Boot sector with BPB
    test_img[512:512*5] = unodos_boot[512:512*5]  # Stage 2 loader (sectors 1-4)
    
    with open('build/unodos-test.img', 'wb') as f:
        f.write(test_img)
    
    print("Created build/unodos-test.img - bootable UnoDOS with TEST.TXT")
    print("Use: make test-combined")

if __name__ == '__main__':
    create_bootable_test()
