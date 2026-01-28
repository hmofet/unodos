# UnoDOS Bootloader Architecture

This document describes the bootloader architecture for both floppy disk and hard disk/USB boot paths.

## Overview

UnoDOS supports two boot methods:

| Method | Media | Filesystem | Boot Chain |
|--------|-------|------------|------------|
| Floppy | 1.44MB/360KB floppy | FAT12 | Boot Sector → Stage2 → Kernel |
| HDD/USB | FAT16 partition | FAT16 | MBR → VBR → Stage2_HD → Kernel |

---

## Memory Map

```
Address         Size    Contents
─────────────────────────────────────────────────
0x0000:0x0000   1KB     Interrupt Vector Table (IVT)
0x0000:0x0400   256B    BIOS Data Area (BDA)
0x0000:0x0600   512B    Relocated MBR (HDD boot only)
0x0000:0x7C00   512B    Boot Sector / VBR
0x0800:0x0000   2KB     Stage2 loader
0x0900:0x0000   512B    Sector buffer (Stage2_HD)
0x0920:0x0000   512B    FAT cache (Stage2_HD)
0x1000:0x0000   28KB    Kernel
0x2000:0x0000   4KB     Shell/Launcher segment
0x3000:0x0000   4KB     User application segment
0xA000:0x0000   64KB    EGA/VGA graphics memory
0xB800:0x0000   16KB    CGA video memory
```

---

## Floppy Boot Path

### Disk Layout (1.44MB)

```
Sector  Contents
──────────────────────────
0       Boot sector (512B)
1-4     Stage2 (2KB)
5       Reserved
6-61    Kernel (28KB = 56 sectors)
62+     FAT12 filesystem data
```

### Boot Sector (`boot/boot.asm`)

**Loaded by:** BIOS at 0x0000:0x7C00
**Size:** 512 bytes (1 sector)

**Responsibilities:**
1. Set up segment registers (DS=ES=0)
2. Display loading message
3. Load Stage2 from sectors 1-4 to 0x0800:0x0000
4. Verify Stage2 signature (0x4E55 "UN")
5. Jump to Stage2 entry point

**Disk Access:** CHS mode via INT 13h AH=02h

### Stage2 (`boot/stage2.asm`)

**Loaded at:** 0x0800:0x0000
**Size:** 2KB (4 sectors)

**Responsibilities:**
1. Display progress dots while loading
2. Load kernel from sectors 6-61 to 0x1000:0x0000
3. Handle track/head boundaries (18 sectors/track, 2 heads)
4. Verify kernel signature (0x4B55 "UK")
5. Pass boot drive in DL
6. Jump to kernel at 0x1000:0x0002

**Disk Access:** CHS mode, 1 sector at a time with progress

---

## HDD/USB Boot Path

### Disk Layout

```
Sector  Contents
──────────────────────────
0       MBR (partition table)
...
63      VBR (partition start, FAT16 boot sector)
64-67   Stage2_HD (2KB in reserved sectors)
68+     FAT16 structures (FAT, root dir, data)
```

### Partition Layout (FAT16)

```
Offset (sectors)  Contents
────────────────────────────────────
0                 VBR (Volume Boot Record)
1-4               Stage2_HD (reserved sectors)
5-132             FAT #1 (128 sectors)
133-260           FAT #2 (copy)
261-292           Root Directory (512 entries)
293+              Data Area (clusters)
```

### MBR (`boot/mbr.asm`)

**Loaded by:** BIOS at 0x0000:0x7C00
**Size:** 512 bytes

**Responsibilities:**
1. Print 'M' debug marker
2. Relocate self to 0x0000:0x0600
3. Scan partition table for bootable partition (0x80 flag)
4. Load VBR from partition start LBA to 0x7C00
5. Try LBA mode first (INT 13h AH=42h), fall back to CHS
6. Verify VBR signature (0xAA55)
7. Jump to VBR, passing drive number in DL

**Disk Access:** LBA preferred, CHS fallback

### VBR (`boot/vbr.asm`)

**Loaded at:** 0x0000:0x7C00
**Size:** 512 bytes

**Structure:**
```
Offset  Size  Contents
──────────────────────────
0x00    3     Jump instruction
0x03    8     OEM name ("UNODOS  ")
0x0B    25    BIOS Parameter Block (BPB)
0x24    26    Extended BPB
0x3E    448   Boot code
0x1FE   2     Signature (0xAA55)
```

**Responsibilities:**
1. Parse BPB for filesystem parameters
2. Query drive geometry (INT 13h AH=08h)
3. Calculate CHS for Stage2 location
4. Load Stage2_HD from reserved sectors to 0x0800:0x0000
5. Try CHS first, fall back to LBA if needed
6. Verify Stage2 signature (0x5355 "US")
7. Jump to Stage2_HD

**Disk Access:** CHS preferred, LBA fallback (opposite of MBR)

### Stage2_HD (`boot/stage2_hd.asm`)

**Loaded at:** 0x0800:0x0000
**Size:** 2KB (4 sectors)

**Responsibilities:**
1. Parse BPB from VBR (still at 0x7C00)
2. Calculate FAT, root directory, and data area locations
3. Search root directory for "KERNEL  BIN"
4. Follow FAT cluster chain to load entire kernel
5. Support multi-cluster files
6. Verify kernel signature
7. Jump to kernel

**Key Calculations:**
```
FAT_start     = partition_LBA + reserved_sectors
Root_start    = FAT_start + (num_FATs × FAT_size)
Data_start    = Root_start + (root_entries × 32 / 512)
Cluster_LBA   = Data_start + (cluster - 2) × sectors_per_cluster
```

**Disk Access:** LBA preferred (INT 13h AH=42h), CHS fallback

---

## Kernel Loading

Both boot paths load the kernel to the same location:

**Load Address:** 0x1000:0x0000 (linear 0x10000)
**Entry Point:** 0x1000:0x0002 (after 2-byte signature)
**Maximum Size:** 28KB (56 sectors on floppy, cluster chain on HDD)

### Kernel Signature

```
Offset  Value   Meaning
──────────────────────────
0x0000  0x4B55  "UK" signature (little-endian)
0x0002  ...     Entry point code
```

### Kernel Entry State

When the kernel entry point executes:

| Register | Value |
|----------|-------|
| CS | 0x1000 |
| IP | 0x0002 |
| DL | Boot drive number |
| DS | Undefined (set by kernel) |
| ES | Undefined (set by kernel) |
| SS:SP | Set by bootloader |

---

## Differences Between Boot Paths

| Aspect | Floppy | HDD/USB |
|--------|--------|---------|
| Filesystem | FAT12 | FAT16 |
| Partition table | None | MBR |
| Kernel location | Fixed sectors | FAT file |
| Disk access | CHS only | LBA + CHS fallback |
| Drive geometry | Fixed (18/2/80) | Queried from BIOS |
| Stage2 size | 2KB | 2KB |
| CPU requirement | 8086 | 386+ (for now) |

---

## USB Boot Considerations

USB drives present special challenges:

1. **Geometry:** USB drives often report 0/0 geometry, requiring LBA mode
2. **BIOS Emulation:** Some BIOSes emulate USB as HDD 0x80, others as floppy 0x00
3. **Sector Size:** Some USB drives report non-512 byte sectors
4. **LBA Support:** USB drives universally support LBA mode (INT 13h extensions)

### USB Debug Features (Build 065+)

- Geometry debug: `(XX/YY)` shows queried values
- LBA indicator: `L` shows when LBA mode is used
- Stage2 signature check: `[XX]` shows first byte loaded

---

## Error Handling

Each stage performs signature verification:

| Stage | Signature | Error Message |
|-------|-----------|---------------|
| Boot/MBR | 0xAA55 | (BIOS won't boot) |
| Stage2 | 0x4E55 "UN" | "Bad stage2" |
| Stage2_HD | 0x5355 "US" | "Invalid stage2" |
| Kernel | 0x4B55 "UK" | "Invalid kernel!" / "Bad kernel!" |

---

## File Locations

```
boot/
├── boot.asm        # Floppy boot sector
├── stage2.asm      # Floppy stage2 loader
├── mbr.asm         # HDD Master Boot Record
├── vbr.asm         # HDD Volume Boot Record
└── stage2_hd.asm   # HDD stage2 loader (FAT16)

kernel/
└── kernel.asm      # Main kernel (same for both boot paths)

tools/
└── create_hd_image.py  # Creates FAT16 HDD image
```

---

## Building

### Floppy Image
```bash
make floppy144      # Creates build/unodos-144.img
```

### HDD Image
```bash
make hd-image       # Creates build/unodos-hd.img
```

### Testing
```bash
make run144         # Test floppy in QEMU
make run-hd         # Test HDD in QEMU
```
