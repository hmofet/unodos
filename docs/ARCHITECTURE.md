# UnoDOS Boot Architecture

This document describes the boot process and architecture of UnoDOS, from power-on to the graphical desktop.

## Overview

UnoDOS uses a three-stage boot architecture:

1. **Stage 1 (Boot Sector)**: 512 bytes, loaded by BIOS from the first sector
2. **Stage 2 (Loader)**: 2KB, loaded by Stage 1, loads and verifies the kernel
3. **Kernel**: 28KB, loaded by Stage 2, contains the main operating system

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
│  - Load kernel from sectors 6-61 (28KB) with progress   │
│  - Verify kernel signature ("UK" at offset 0)           │
│  - Jump to kernel at 0x1000:0x0002                      │
└─────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────┐
│  Kernel (kernel/kernel.asm)                             │
│  Phase 1: Early init (text mode, BIOS output)           │
│  - Install INT 0x80 handler (API dispatch)              │
│  - Install PS/2 mouse driver (INT 0x74/IRQ12)           │
│  - Print version/build to serial/text output            │
│                                                         │
│  Phase 2: Interrupt handlers + graphics                 │
│  - Install INT 09h handler (keyboard driver)            │
│  - Switch to CGA 320x200 graphics mode                  │
│  - Draw welcome screen with version/build info          │
│  - Show mouse cursor                                    │
│                                                         │
│  Phase 3: System services                               │
│  - Mount boot floppy (FAT12)                            │
│  - Auto-load LAUNCHER.BIN from floppy                   │
│  - Enter main event loop                                │
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
0x10000-0x16FFF   1000:0000-6FFF    Kernel (28KB)
  └─ 0x11060     1000:1060          API table
0x14000-0x1FFFF   1400:0000+        Heap (malloc pool)
0x20000-0x2FFFF   2000:0000-FFFF    Shell/Launcher segment (fixed)
0x30000-0x3FFFF   3000:0000-FFFF    User app slot 0 (dynamic pool)
0x40000-0x4FFFF   4000:0000-FFFF    User app slot 1
0x50000-0x5FFFF   5000:0000-FFFF    User app slot 2
0x60000-0x6FFFF   6000:0000-FFFF    User app slot 3
0x70000-0x7FFFF   7000:0000-FFFF    User app slot 4
0x80000-0x8FFFF   8000:0000-FFFF    User app slot 5
0x90000-0x9FFFF   9000:0000-FFFF    Scratch buffer (window drag content)
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
6-61      0x0A00      Kernel                  28KB (56 sectors)
62+       0x7A00      FAT12 filesystem        Remaining space
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

```
Loading kernel................................ OK
```

## Kernel

**File**: `kernel/kernel.asm`
**Size**: 28KB (56 sectors)
**Load Address**: 0x1000:0x0000 (linear 0x10000 = 64KB mark)

### Key Subsystems

| Subsystem | Description |
|-----------|-------------|
| System Calls | INT 0x80 for API dispatch, 44 functions |
| Graphics API | 8 functions: pixel, rect, filled_rect, char, string, string_inverted, clear, text_width, draw_icon |
| Memory | malloc/free with first-fit allocation |
| Keyboard | INT 09h handler, scan code translation, 16-byte buffer |
| PS/2 Mouse | INT 0x74 (IRQ12) handler, XOR cursor, title bar drag |
| Events | 32-event circular queue, KEY_PRESS/KEY_RELEASE/MOUSE types |
| Filesystem | FAT12 driver with mount, open, read, close, readdir |
| App Loader | Load and execute .BIN applications from FAT12 |
| Window Manager | Create, destroy, draw, focus, move, close, outline drag (16 max) |
| PC Speaker | PIT Channel 2 tone generation, auto-silence on exit |
| Drawing Context | Window-relative coordinate translation for APIs 0-6 |
| Content Preserve | Save/restore window content during drags (scratch at 0x9000) |
| Desktop Icons | 8 registered icon slots, kernel repaints during window operations |
| Multitasking | Cooperative round-robin scheduler, 6 concurrent user apps |
| Segment Pool | Dynamic segment allocation (0x3000-0x8000) with alloc/free |

### Window Drawing Context

When an app calls `win_begin_draw` (API 31), a drawing context is activated. All subsequent calls to APIs 0-6 have their BX/CX coordinates automatically translated from window-relative to absolute screen coordinates:

```
absolute_x = window_x + 1 (border) + relative_x
absolute_y = window_y + 10 (title bar) + relative_y
```

This allows apps to draw at (0,0) meaning the top-left of the window's content area.

### Text Rendering

Characters are 8x8 pixels but advance by **12 pixels** (8px glyph + 4px gap). To calculate the pixel width of a string, use `gfx_text_width` (API 33) which returns `num_chars * 12`.

## Font System

Two bitmap fonts are included:

**8x8 Font** (`kernel/font8x8.asm`)
- 95 characters (ASCII 32-126)
- 8 bytes per character, 8x8 pixels
- 12px advance (8px char + 4px spacing)
- Used for window titles, app text

**4x6 Font** (`kernel/font4x6.asm`)
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

**Important**: CGA operations must be byte-aligned (4-pixel boundaries) for save/restore operations.

Color palette (Palette 1):
- 00: Background color (blue)
- 01: Cyan
- 10: Magenta
- 11: White

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

## Multi-App Segment Architecture (Build 149+)

```
0x2000:0x0000  Shell/Launcher (fixed, persists always)
0x3000:0x0000  User app slot 0 (dynamic pool)
0x4000:0x0000  User app slot 1
0x5000:0x0000  User app slot 2
0x6000:0x0000  User app slot 3
0x7000:0x0000  User app slot 4
0x8000:0x0000  User app slot 5
0x9000:0x0000  Scratch buffer (window drag content)
```

- Up to 6 concurrent user apps, each in its own 64KB segment
- Segments allocated from pool by `alloc_segment`, freed by `free_segment`
- Shell at 0x2000 survives while user apps run
- Apps return via RETF, segment freed on exit
- Apps use `[ORG 0x0000]` and are loaded at offset 0 in their segment

## BIN File Icon Headers (v3.14.0)

Applications can embed a 16x16 icon in an 80-byte header. See `docs/FEATURES.md` for the full specification.

```
Offset  Content
0x00    EB 4E           JMP short to 0x50 (skip header)
0x02    55 49           "UI" magic
0x04    12 bytes        App display name (null-padded)
0x10    64 bytes        16x16 2bpp CGA icon bitmap
0x50    ...             Code entry point
```

Detection: `byte[0]==0xEB && byte[2]=='U' && byte[3]=='I'`

---

*Document version: 8.0 (2026-02-14) - Updated for v3.16.0 Build 161*
