# UnoDOS Boot Architecture

This document describes the boot process and architecture of UnoDOS, from power-on to the graphical welcome screen.

## Overview

UnoDOS uses a three-stage boot architecture:

1. **Stage 1 (Boot Sector)**: 512 bytes, loaded by BIOS from the first sector
2. **Stage 2 (Loader)**: 2KB, loaded by Stage 1, loads and verifies the kernel
3. **Kernel**: 16KB, loaded by Stage 2, contains the main operating system

This design separates the bootloader from the OS code, allowing the kernel to grow independently while maintaining a simple, reliable boot process.

## Boot Process Flow

```
Power On
    │
    ▼
┌─────────────────────────────────────────────────────────┐
│  BIOS POST (Power-On Self Test)                         │
│  - Memory test                                          │
│  - Hardware initialization                              │
│  - Build interrupt vector table                         │
└─────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────┐
│  BIOS Bootstrap                                         │
│  - Read first sector (512 bytes) from boot device       │
│  - Load to address 0x0000:0x7C00                        │
│  - Verify boot signature (0xAA55 at offset 510)         │
│  - Jump to 0x0000:0x7C00                                │
└─────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────┐
│  Stage 1: Boot Sector (boot/boot.asm)                   │
│  - Set up segment registers (DS=ES=0x0000)              │
│  - Set up stack (SS:SP = 0x0000:0x7C00)                 │
│  - Display boot messages                                │
│  - Load Stage 2 from sectors 2-5 (2KB)                  │
│  - Verify Stage 2 signature ("UN" at offset 0)          │
│  - Jump to Stage 2 at 0x0800:0x0002                     │
└─────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────┐
│  Stage 2: Loader (boot/stage2.asm)                      │
│  - Display "Loading kernel" message                     │
│  - Load kernel from sectors 6-37 (16KB) with progress   │
│  - Verify kernel signature ("UK" at offset 0)           │
│  - Jump to kernel at 0x1000:0x0002                      │
└─────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────┐
│  Kernel (kernel/kernel.asm)                             │
│  - Detect conventional memory (INT 12h)                 │
│  - Switch to CGA 320x200 graphics mode                  │
│  - Draw welcome screen                                  │
│  - Enter main loop (clock + character demo)             │
└─────────────────────────────────────────────────────────┘
```

## Memory Map

### During Boot (Real Mode, 16-bit)

```
Linear Address    Segment:Offset    Description
─────────────────────────────────────────────────────────
0x00000-0x003FF   0000:0000-03FF    Interrupt Vector Table (1KB)
0x00400-0x004FF   0000:0400-04FF    BIOS Data Area (256 bytes)
0x00500-0x07BFF   0000:0500-7BFF    Free (available for use)
0x07C00-0x07DFF   0000:7C00-7DFF    Boot Sector (512 bytes)
0x07E00-0x07FFF   0000:7E00-7FFF    Stack area
0x08000-0x087FF   0800:0000-07FF    Stage 2 Loader (2KB)
0x10000-0x13FFF   1000:0000-3FFF    Kernel (16KB)
0x14000-0x9FFFF   ----:----         Free conventional memory
0xA0000-0xBFFFF   ----:----         Video memory (EGA/VGA)
0xB8000-0xBFFFF   B800:0000-7FFF    CGA video memory (used by UnoDOS)
0xC0000-0xFFFFF   ----:----         ROM area (BIOS, adapters)
```

### Disk Layout

```
Sector    Offset      Content                 Size
────────────────────────────────────────────────────
1         0x0000      Boot sector             512 bytes
2-5       0x0200      Stage 2 Loader          2KB (4 sectors)
6-37      0x0A00      Kernel                  16KB (32 sectors)
38+       0x4A00      Future: File system     Remaining space
```

## Stage 1: Boot Sector

**File**: `boot/boot.asm`
**Size**: 512 bytes (must be exactly this size)
**Load Address**: 0x0000:0x7C00

### Responsibilities

1. Initialize CPU state (segment registers, stack)
2. Load Stage 2 (4 sectors) using BIOS INT 13h
3. Validate Stage 2 signature ("UN")
4. Transfer control to Stage 2

### Key Code

```nasm
; Load Stage 2 using BIOS INT 13h
mov ah, 0x02        ; BIOS read sectors function
mov al, 4           ; Number of sectors to read (2KB)
mov ch, 0           ; Cylinder 0
mov cl, 2           ; Starting sector (1-indexed)
mov dh, 0           ; Head 0
mov dl, [boot_drive]; Drive number
int 0x13            ; Call BIOS

; Signature validation
mov ax, [es:0x0000]
cmp ax, 0x4E55      ; 'UN' in little-endian
jne boot_error

; Jump to Stage 2
jmp 0x0800:0x0002
```

## Stage 2: Loader

**File**: `boot/stage2.asm`
**Size**: 2KB (4 sectors)
**Load Address**: 0x0800:0x0000

### Responsibilities

1. Display "Loading kernel" message
2. Load kernel sector-by-sector with progress indicator (dots)
3. Handle disk geometry (track/head/sector advancement)
4. Validate kernel signature ("UK")
5. Transfer control to kernel

### Progress Indicator

Stage 2 prints a dot (`.`) after each sector is loaded, providing visual feedback during the kernel load. This is especially useful on slow floppy drives.

```
Loading kernel................................ OK
```

### Key Code

```nasm
.load_loop:
    ; Read one sector
    mov ah, 0x02
    mov al, 1
    int 0x13

    ; Print progress dot
    mov ah, 0x0E
    mov al, '.'
    int 0x10

    ; Advance to next sector
    ...
    loop .load_loop
```

## Kernel

**File**: `kernel/kernel.asm`
**Size**: 16KB (32 sectors, expandable)
**Load Address**: 0x1000:0x0000 (linear 0x10000 = 64KB mark)

### Responsibilities

1. Detect system resources (memory, video)
2. Initialize CGA graphics mode
3. Draw welcome screen with bordered window
4. Display RAM information and clock
5. Run main event loop

### Key Functions

| Function | Description |
|----------|-------------|
| `detect_memory` | Get conventional memory size via INT 12h |
| `setup_graphics` | Initialize CGA mode and palette |
| `draw_hello_gfx` | Draw the welcome screen |
| `draw_clock` | Read RTC and display time |
| `draw_ram_info` | Display memory statistics |
| `char_demo_loop` | Display ASCII character grid |
| `plot_pixel_white` | Set a pixel to white (color 3) |
| `draw_char` | Draw 8x8 character bitmap |
| `draw_char_small` | Draw 4x6 character bitmap |

## Font System

Two bitmap fonts are included:

**8x8 Font** (`boot/font8x8.asm`)
- 95 characters (ASCII 32-126)
- 8 bytes per character
- Used for main titles

**4x6 Font** (`boot/font4x6.asm`)
- 95 characters (ASCII 32-126)
- 6 bytes per character
- Used for small text (version, clock, RAM info)

## CGA Video Memory

### Mode 4: 320x200, 4 colors

```
Offset      Scanlines    Description
─────────────────────────────────────────────
0x0000      0,2,4,...    Even scanlines (100 lines)
0x2000      1,3,5,...    Odd scanlines (100 lines)
```

Each byte contains 4 pixels (2 bits per pixel):
```
Bit 7-6: Pixel 0 (leftmost)
Bit 5-4: Pixel 1
Bit 3-2: Pixel 2
Bit 1-0: Pixel 3 (rightmost)
```

Color palette (Palette 1):
- 00: Background color (blue)
- 01: Cyan
- 10: Magenta
- 11: White

## BIOS Interrupts Used

| Interrupt | Function | Description |
|-----------|----------|-------------|
| INT 10h, AH=00h | Set Video Mode | Switch to CGA mode 4 |
| INT 10h, AH=0Bh | Set Color Palette | Configure background/palette |
| INT 10h, AH=0Eh | Teletype Output | Print characters (text mode) |
| INT 11h | Equipment List | Detect video adapter type |
| INT 12h | Memory Size | Get conventional memory in KB |
| INT 13h, AH=00h | Reset Disk | Reset floppy controller |
| INT 13h, AH=02h | Read Sectors | Load data from floppy |
| INT 1Ah, AH=02h | Read RTC Time | Get hours, minutes, seconds |

## Segment Register Usage

| Register | Boot | Stage 2 | Kernel | Purpose |
|----------|------|---------|--------|---------|
| CS | 0x0000 | 0x0800 | 0x1000 | Code segment |
| DS | 0x0000 | 0x0800 | 0x1000 | Data segment |
| ES | varies | varies | 0xB800 | Video memory |
| SS | 0x0000 | 0x0000 | 0x0000 | Stack segment |
| SP | 0x7C00 | 0x7C00 | 0x7C00 | Stack pointer |

## Signatures

| Component | Signature | Hex Value | Purpose |
|-----------|-----------|-----------|---------|
| Boot sector | 0xAA55 | - | BIOS requirement |
| Stage 2 | "UN" | 0x4E55 | Loader verification |
| Kernel | "UK" | 0x4B55 | Kernel verification |

## Error Handling

### Stage 1 Errors
- **Disk read error**: Display error message, halt
- **Invalid Stage 2 signature**: Display error message, halt

### Stage 2 Errors
- **Disk read error**: Display error message, halt
- **Invalid kernel signature**: Display error message, halt

### Kernel Errors
- **RTC not available**: Display "--:--" instead of time

## Design Rationale

### Why Three Stages?

1. **Stage 1** (512 bytes): BIOS limitation - can only load first sector
2. **Stage 2** (2KB): Minimal loader, handles disk I/O complexity
3. **Kernel** (16KB+): Full OS code, can grow independently

### Why Separate Kernel?

- Kernel can exceed 8KB limit without modifying bootloader
- Cleaner architecture for future development
- Boot progress indicator during kernel load
- Foundation for loading multiple kernels (debug/release)

### Why 0x1000:0x0000 for Kernel?

- Above Stage 2 (0x0800)
- Linear address 0x10000 = 64KB mark
- Leaves room for kernel growth (up to ~550KB)
- Below video memory (0xA0000)

---

*Document version: 2.0 (2026-01-22) - Updated for v3.2.0 three-stage architecture*
