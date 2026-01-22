# UnoDOS 3 Boot Architecture

This document describes the boot process and architecture of UnoDOS 3, from power-on to the graphical welcome screen.

## Overview

UnoDOS 3 uses a two-stage boot loader design:

1. **Stage 1 (Boot Sector)**: 512 bytes, loaded by BIOS from the first sector of the floppy disk
2. **Stage 2 (Second Stage Loader)**: 8KB, loaded by Stage 1, contains the main initialization code

This design overcomes the 512-byte limitation of the boot sector while keeping the system simple enough to run on original PC XT hardware.

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
│  - Display "Booting UnoDOS..." message                  │
│  - Load Stage 2 from sectors 2-17 (8KB)                 │
│  - Verify Stage 2 signature ("UN" at offset 0)          │
│  - Jump to Stage 2 at 0x0800:0x0002                     │
└─────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────┐
│  Stage 2: Second Stage Loader (boot/stage2.asm)         │
│  - Set up segment registers (DS=ES=0x0800)              │
│  - Detect conventional memory (INT 12h)                 │
│  - Detect video adapter (INT 11h)                       │
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
0x08000-0x09FFF   0800:0000-1FFF    Stage 2 Loader (8KB)
0x0A000-0x9FFFF   ----:----         Free conventional memory
0xA0000-0xBFFFF   ----:----         Video memory (EGA/VGA)
0xB0000-0xB7FFF   B000:0000-7FFF    MDA video memory
0xB8000-0xBFFFF   B800:0000-7FFF    CGA video memory
0xC0000-0xFFFFF   ----:----         ROM area (BIOS, adapters)
```

### CGA Video Memory Layout (Mode 4: 320x200, 4 colors)

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

## Stage 1: Boot Sector

**File**: `boot/boot.asm`
**Size**: 512 bytes (must be exactly this size)
**Load Address**: 0x0000:0x7C00

### Responsibilities

1. **Initialize CPU State**
   - Set segment registers (CS, DS, ES, SS)
   - Set up stack pointer

2. **Load Stage 2**
   - Use BIOS INT 13h (disk services)
   - Read 16 sectors (8KB) starting from sector 2
   - Load to address 0x0800:0x0000

3. **Validate Stage 2**
   - Check for "UN" signature at start of Stage 2
   - Halt with error if invalid

4. **Transfer Control**
   - Far jump to 0x0800:0x0002 (skip signature)

### Disk Layout

```
Sector    Offset      Content
────────────────────────────────────
1         0x0000      Boot sector (512 bytes)
2-17      0x0200      Stage 2 (8KB, 16 sectors)
18+       0x2200      Future: File system, data
```

### Key Code Sections

```nasm
; Load Stage 2 using BIOS INT 13h
mov ah, 0x02        ; BIOS read sectors function
mov al, 16          ; Number of sectors to read
mov ch, 0           ; Cylinder 0
mov cl, 2           ; Starting sector (1-indexed)
mov dh, 0           ; Head 0
mov dl, 0           ; Drive A:
mov bx, 0x0000      ; ES:BX = destination
int 0x13            ; Call BIOS

; Signature validation
mov ax, [es:0x0000]
cmp ax, 0x4E55      ; 'UN' in little-endian
jne boot_error

; Jump to Stage 2
jmp 0x0800:0x0002
```

## Stage 2: Second Stage Loader

**File**: `boot/stage2.asm`
**Size**: Up to 8KB (currently ~6KB)
**Load Address**: 0x0800:0x0000

### Responsibilities

1. **System Detection**
   - Memory size (INT 12h)
   - Video adapter type (INT 11h equipment list)

2. **Video Initialization**
   - Set CGA Mode 4 (320x200, 4 colors) via INT 10h
   - Configure color palette
   - Clear video memory

3. **Graphics Rendering**
   - Draw welcome screen with bordered window
   - Render text using custom bitmap fonts
   - Display RAM information
   - Display real-time clock

4. **Main Loop**
   - Update clock display
   - Run character demonstration

### Key Functions

| Function | Description |
|----------|-------------|
| `detect_memory` | Get conventional memory size via INT 12h |
| `detect_video` | Identify video adapter from equipment list |
| `setup_graphics` | Initialize CGA mode and palette |
| `draw_hello_gfx` | Draw the welcome screen |
| `draw_clock` | Read RTC and display time |
| `draw_ram_info` | Display memory statistics |
| `char_demo_loop` | Cycle through ASCII characters |
| `plot_pixel_white` | Set a pixel to white (color 3) |
| `draw_char` | Draw 8x8 character bitmap |
| `draw_char_small` | Draw 4x6 character bitmap |

### Font System

Two bitmap fonts are included:

**8x8 Font** (`font8x8.asm`)
- 95 characters (ASCII 32-126)
- 8 bytes per character
- Used for main titles

**4x6 Font** (`font4x6.asm`)
- 95 characters (ASCII 32-126)
- 6 bytes per character
- Used for small text (version, clock, RAM info)

Character lookup:
```nasm
; Calculate font offset: (ASCII - 32) * bytes_per_char
sub al, 32          ; Convert ASCII to index
mov bl, 6           ; Bytes per char (4x6 font)
mul bl              ; AX = index * 6
add ax, font_4x6    ; Add base address
```

## BIOS Interrupts Used

| Interrupt | Function | Description |
|-----------|----------|-------------|
| INT 10h, AH=00h | Set Video Mode | Switch to CGA mode 4 |
| INT 10h, AH=0Bh | Set Color Palette | Configure background/palette |
| INT 10h, AH=0Eh | Teletype Output | Print characters (text mode) |
| INT 11h | Equipment List | Detect video adapter type |
| INT 12h | Memory Size | Get conventional memory in KB |
| INT 13h, AH=02h | Read Sectors | Load data from floppy |
| INT 1Ah, AH=02h | Read RTC Time | Get hours, minutes, seconds |

## Segment Register Usage

| Register | Stage 1 Value | Stage 2 Value | Purpose |
|----------|---------------|---------------|---------|
| CS | 0x0000 | 0x0800 | Code segment |
| DS | 0x0000 | 0x0800 | Data segment |
| ES | 0x0800 (load) | 0xB800 (video) | Extra segment |
| SS | 0x0000 | 0x0000 | Stack segment |
| SP | 0x7C00 | 0x7C00 | Stack pointer |

## Error Handling

### Stage 1 Errors
- **Disk read error**: Display error message, halt
- **Invalid signature**: Display error message, halt

### Stage 2 Errors
- **RTC not available**: Display "--:--" instead of time

## Future Extensions

The boot architecture is designed to support future additions:

1. **Kernel Loading**: Stage 2 can load additional code from disk
2. **File System**: FAT12 support for loading files
3. **Memory Management**: Protected mode transition (on 286+)
4. **Driver Loading**: Modular driver architecture

## Technical Notes

### Why Two Stages?

The BIOS only loads the first 512 bytes from the boot device. This is not enough space for meaningful initialization code, fonts, and graphics routines. The two-stage approach:

1. Keeps Stage 1 minimal (just loads Stage 2)
2. Gives Stage 2 room to grow (8KB currently, expandable)
3. Allows signature validation before executing untrusted code

### Why 0x0800:0x0000?

Stage 2 is loaded at segment 0x0800 (linear address 0x8000) because:

1. It's above the boot sector and stack area
2. It's below the typical start of BIOS data areas
3. It leaves room for future kernel code above 0xA000

### CGA Interlacing

CGA Mode 4 uses interlaced memory: even scanlines start at offset 0x0000, odd scanlines at 0x2000. This is a hardware limitation of the original CGA adapter and must be accounted for in all graphics code.

---

*Document version: 1.0 (2026-01-22)*
