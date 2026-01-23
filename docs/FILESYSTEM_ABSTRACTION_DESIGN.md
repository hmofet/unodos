# Filesystem Abstraction Layer Design (v3.10.0)

## Overview

The filesystem abstraction layer enables UnoDOS to support multiple filesystem types through a driver interface, similar to VFS (Virtual File System) in Unix/Linux.

**Architecture:** Built-in FAT12 + pluggable driver mechanism for FAT16/FAT32

## Three-Tier Deployment Architecture

### Tier 1: Single 360KB Floppy (Minimal System)
- **Boot**: Boot sector → Stage2 → Kernel
- **Filesystem**: FAT12 only (built-in)
- **Content**: Kernel + minimal apps (~340KB available for apps)
- **Driver loading**: None (no space/need)

### Tier 2: Multi-Floppy System
- **Boot**: Floppy 1 (360KB) boots OS
- **Additional floppies**: Floppy 2, 3, etc. contain drivers/apps
- **Driver loading**: Load FAT16/FAT32 drivers from Floppy 2 into memory
- **Use case**: User inserts driver disk, OS loads module, can then read other disks

### Tier 3: Hard Disk Installation
- **Boot**: Floppy boots installer or minimal OS
- **Installation**: Copy OS files to HDD (FAT16/FAT32)
- **Bootloader**: Install boot sector on HDD
- **Driver loading**: Load FAT16/FAT32 driver at boot to access HDD
- **Tools needed**: Bootloader writer, file copy utility

## Filesystem Driver Interface

### Driver Structure

Each filesystem driver implements a function table:

```asm
; Filesystem driver interface (64 bytes total)
fs_driver:
    db 'FAT12   '           ; 8 bytes: Driver name (padded)
    dw 0x0001               ; 2 bytes: Version (1.0)
    dw driver_flags         ; 2 bytes: Capability flags
    dw fs_detect            ; 2 bytes: Detect if this FS is on disk
    dw fs_mount             ; 2 bytes: Mount filesystem
    dw fs_unmount           ; 2 bytes: Unmount filesystem
    dw fs_open              ; 2 bytes: Open file
    dw fs_read              ; 2 bytes: Read from file
    dw fs_close             ; 2 bytes: Close file
    dw fs_list_dir          ; 2 bytes: List directory
    dw fs_get_info          ; 2 bytes: Get file info (size, date)
    times 34 db 0           ; Reserved for future functions
```

**Capability flags:**
- Bit 0: Read support
- Bit 1: Write support (future)
- Bit 2: Directory support
- Bit 3: Long filename support (future)

### Driver Registration

Kernel maintains a registry of up to 4 filesystem drivers:

```asm
fs_driver_registry:
    dw 0, 0     ; Driver 0: FAT12 (built-in, always present)
    dw 0, 0     ; Driver 1: FAT16 (loadable)
    dw 0, 0     ; Driver 2: FAT32 (loadable)
    dw 0, 0     ; Driver 3: (reserved)

; Each entry: segment:offset pointer to driver structure
```

**Function: fs_register_driver**
- Input: ES:BX = pointer to driver structure
- Returns: AX = driver ID (0-3), or 0xFFFF if registry full
- Copies driver pointer to next free slot

## Public API Functions (Added to API Table)

Apps call these high-level functions, which dispatch to the appropriate driver:

### 1. fs_mount (API offset 44)
**Purpose:** Mount a filesystem on a drive
- Input:
  - AL = drive number (0=A:, 1=B:, 0x80=HDD0, etc)
  - AH = driver ID (0=auto-detect, 1-3=force specific driver)
- Returns:
  - CF = 0 on success, CF = 1 on error
  - AX = error code if CF=1
  - BX = mount handle (used in subsequent calls)
- Process:
  1. If AH=0 (auto-detect), try each registered driver's fs_detect
  2. If AH>0, use specified driver
  3. Call driver's fs_mount function
  4. Store mount info in mount table

### 2. fs_open (API offset 48)
**Purpose:** Open a file for reading
- Input:
  - BX = mount handle (from fs_mount)
  - DS:SI = pointer to filename (null-terminated, 8.3 format: "FILENAME.EXT")
- Returns:
  - CF = 0 on success, CF = 1 on error
  - AX = file handle (0-15), or error code if CF=1
- Process:
  1. Look up mount handle to find driver
  2. Call driver's fs_open function
  3. Store file handle in file table

### 3. fs_read (API offset 52)
**Purpose:** Read data from file
- Input:
  - AX = file handle
  - ES:DI = buffer to read into
  - CX = number of bytes to read
- Returns:
  - CF = 0 on success, CF = 1 on error
  - AX = actual bytes read, or error code if CF=1
- Process:
  1. Look up file handle to find driver
  2. Call driver's fs_read function

### 4. fs_close (API offset 56)
**Purpose:** Close an open file
- Input:
  - AX = file handle
- Returns:
  - CF = 0 on success, CF = 1 on error
  - AX = error code if CF=1
- Process:
  1. Look up file handle to find driver
  2. Call driver's fs_close function
  3. Mark file handle as free

### 5. fs_list_dir (API offset 60)
**Purpose:** List directory contents (future - not in v3.10.0)
- Input:
  - BX = mount handle
  - DS:SI = directory path (or NULL for root)
  - ES:DI = buffer for directory entries
  - CX = max entries
- Returns:
  - CF = 0 on success, CF = 1 on error
  - AX = number of entries returned, or error code if CF=1

### 6. fs_register_driver (API offset 64)
**Purpose:** Register a loadable filesystem driver (Tier 2/3)
- Input:
  - ES:BX = pointer to driver structure
- Returns:
  - CF = 0 on success, CF = 1 on error
  - AX = driver ID (0-3), or error code if CF=1

## Data Structures

### Mount Table (4 entries, 16 bytes each)
```asm
mount_table:
    ; Entry 0
    db 0                ; Drive number (0xFF = free)
    db 0                ; Driver ID
    dw 0                ; Driver segment
    dw 0                ; Driver offset
    dw 0, 0, 0, 0, 0    ; Driver-specific data (10 bytes)
    ; Entries 1-3 follow same format
```

### File Handle Table (16 entries, 32 bytes each)
```asm
file_table:
    ; Entry 0
    db 0                ; Status (0=free, 1=open)
    db 0                ; Mount handle
    dw 0                ; Driver function pointers (cached)
    dw 0                ; Current position (32-bit)
    dw 0
    dw 0                ; File size (32-bit)
    dw 0
    times 22 db 0       ; Driver-specific data
    ; Entries 1-15 follow same format
```

## FAT12 Driver Implementation

### BPB (BIOS Parameter Block) Structure

Located at sector 0 (boot sector) of the floppy:

```
Offset | Size | Description
-------|------|------------
0x00   | 3    | Jump instruction (EB 3C 90)
0x03   | 8    | OEM name
0x0B   | 2    | Bytes per sector (512)
0x0D   | 1    | Sectors per cluster (1)
0x0E   | 2    | Reserved sectors (1)
0x10   | 1    | Number of FATs (2)
0x11   | 2    | Root directory entries (224)
0x13   | 2    | Total sectors (720 or 2880)
0x15   | 1    | Media descriptor (0xFD or 0xF0)
0x16   | 2    | Sectors per FAT (3 or 9)
0x18   | 2    | Sectors per track (9 or 18)
0x1A   | 2    | Number of heads (2)
0x1C   | 4    | Hidden sectors (0)
0x20   | 4    | Large sector count (0)
```

### Disk Layout

**360KB 5.25" Floppy (720 sectors):**
- Sector 0: Boot sector (BPB)
- Sectors 1-3: FAT #1 (3 sectors = 1536 bytes)
- Sectors 4-6: FAT #2 (copy)
- Sectors 7-20: Root directory (14 sectors = 224 entries)
- Sectors 21-719: Data area (699 sectors = 354 clusters)

**1.44MB 3.5" Floppy (2880 sectors):**
- Sector 0: Boot sector (BPB)
- Sectors 1-9: FAT #1 (9 sectors = 4608 bytes)
- Sectors 10-18: FAT #2 (copy)
- Sectors 19-32: Root directory (14 sectors = 224 entries)
- Sectors 33-2879: Data area (2847 sectors = 2847 clusters)

### Directory Entry Structure (32 bytes)

```
Offset | Size | Description
-------|------|------------
0x00   | 8    | Filename (padded with spaces)
0x08   | 3    | Extension (padded with spaces)
0x0B   | 1    | Attributes (0x10=dir, 0x20=archive)
0x0C   | 10   | Reserved
0x16   | 2    | Time (5:6:5 bits = hour:min:sec/2)
0x18   | 2    | Date (7:4:5 bits = year-1980:month:day)
0x1A   | 2    | Starting cluster
0x1C   | 4    | File size in bytes
```

### FAT12 Functions to Implement

**1. fat12_detect**
- Read sector 0
- Check BPB signature (bytes per sector = 512, FAT count = 2)
- Validate media descriptor (0xF0 or 0xFD)
- Return CF=0 if valid FAT12, CF=1 if not

**2. fat12_mount**
- Read and parse BPB
- Calculate:
  - Root directory start sector = reserved + (FATs × sectors_per_FAT)
  - Data area start sector = root_dir_start + root_dir_sectors
  - Total clusters = (total_sectors - data_start) / sectors_per_cluster
- Cache BPB data in driver-specific mount data
- Return mount handle

**3. fat12_open**
- Search root directory for filename (8.3 format)
- Parse directory entry
- Get starting cluster and file size
- Store in file handle
- Return file handle

**4. fat12_read**
- Calculate current cluster from file position
- Read cluster data using BIOS INT 13h
- Follow FAT chain if reading spans multiple clusters
- Handle FAT12 cluster values:
  - 0x000 = free cluster
  - 0x002-0xFEF = used cluster, value = next cluster
  - 0xFF0-0xFF6 = reserved
  - 0xFF7 = bad cluster
  - 0xFF8-0xFFF = end of chain
- Update file position
- Return bytes read

**5. fat12_close**
- Mark file handle as free
- No other cleanup needed (read-only)

### FAT12 Cluster Chain Reading

FAT12 uses 12-bit entries, which creates a challenge: entries don't align on byte boundaries.

**Example FAT12 entries:**
```
Byte offset:  0    1    2    3    4    5
Byte value:   F0   FF   03   40   00   05
             |----|----|----|----|----|----|
Cluster 0:    F0F (reserved)
Cluster 1:    FF0 3 = 0x3FF (end of chain)
Cluster 2:    004 (next cluster = 4)
Cluster 3:    005 (next cluster = 5)
```

**Algorithm to read cluster N:**
1. Calculate byte offset: offset = (N × 3) / 2
2. Read 2 bytes from FAT at that offset
3. If N is even: value = (bytes & 0x0FFF)
4. If N is odd: value = (bytes >> 4)

## Size Estimate

| Component | Estimated Size |
|-----------|----------------|
| Abstraction layer | ~400 bytes |
| FAT12 driver | ~1,200 bytes |
| Mount table (4 entries × 16 bytes) | 64 bytes |
| File table (16 handles × 32 bytes) | 512 bytes |
| BPB cache | 32 bytes |
| FAT cache (partial) | 512 bytes |
| **Total** | **~2,720 bytes** |

**Remaining after implementation:** 21,928 - 2,720 = ~19,208 bytes (78% free)

## Implementation Order

1. ✓ Design abstraction layer (this document)
2. Define data structures (mount table, file table, driver interface)
3. Implement abstraction layer functions (fs_mount, fs_open, fs_read, fs_close)
4. Implement FAT12 driver (detect, mount, open, read, close)
5. Add disk I/O wrapper (BIOS INT 13h for sector reading)
6. Test by reading a file from floppy
7. Add functions to API table

## Testing Plan

### Test 1: Detect FAT12
- Insert UnoDOS floppy (known FAT12)
- Call fs_mount with auto-detect
- Verify FAT12 driver is selected

### Test 2: Mount and Parse BPB
- Mount drive A:
- Verify BPB values are correct (bytes_per_sector=512, etc)

### Test 3: Open File
- Create test file TEST.TXT on floppy
- Call fs_open("TEST.TXT")
- Verify file handle returned, starting cluster and size are correct

### Test 4: Read File
- Read 128 bytes from TEST.TXT
- Display on screen using gfx_draw_string
- Verify content matches file

### Test 5: Close File
- Call fs_close
- Verify file handle marked as free

## Future Enhancements (v3.11.0+)

### Tier 2: Multi-Floppy Support
- **Driver loading app**: Reads FAT16 driver from Floppy 2
- **fs_register_driver**: Loads driver into memory
- **Driver activation**: Can now mount FAT16 disks

### Tier 3: HDD Installation
- **Installer app**: Copies OS files to HDD
- **Bootloader writer**: Installs boot sector on HDD
- **HDD boot**: Boot from floppy, load FAT16/FAT32 driver, mount HDD

### Write Support (v4.0.0+)
- fs_write function
- fs_create function
- fs_delete function
- FAT table modification
- Directory entry creation

### Long Filename Support (v4.1.0+)
- VFAT long filename entries
- Unicode support (basic)

---

*Document created: 2026-01-23*
*UnoDOS v3.10.0 - Filesystem Abstraction Layer*
