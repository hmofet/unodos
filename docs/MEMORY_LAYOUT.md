# UnoDOS Memory Layout Analysis

## Physical Memory Map (Real Mode, 640KB System)

```
Address Range          Size    Usage
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
0x0000:0x0000          1 KB    Interrupt Vector Table (IVT)
0x0000:0x0400          256 B   BIOS Data Area (BDA)
0x0000:0x0500          ~30 KB  FREE / Transient Program Area
0x0000:0x7C00          512 B   Boot Sector (loaded by BIOS)
0x0000:0x7E00          ~1 KB   FREE (after boot sector)
0x0800:0x0000          2 KB    Stage2 Bootloader
0x1000:0x0000          28 KB   Kernel (v3.13.0)
  0x1000:0x0F80        ~128 B    ↳ API Table (34 functions)
0x1400:0x0000          ~64 KB  Heap (malloc pool)
0x2000:0x0000          64 KB   Shell/Launcher segment
0x3000:0x0000          64 KB   User app segment
0x5000:0x0000          64 KB   Scratch buffer (window drag content)
0x9FFF:0xFFFF          -       End of conventional memory
0xA000:0x0000          64 KB   EGA/VGA graphics memory
0xB000:0x0000          32 KB   Monochrome text memory
0xB800:0x0000          16 KB   CGA text/graphics memory
0xC000:0x0000          256 KB  ROM BIOS extensions
0xF000:0x0000          64 KB   System BIOS ROM
```

---

## Kernel Layout (0x1000:0x0000)

```
Offset    Content
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
0x0000    Signature ("UK") + entry point
0x0002    Kernel code (init, drivers, API stubs)
          - INT 0x80 handler
          - Keyboard driver (INT 09h)
          - PS/2 mouse driver (INT 0x74)
          - Graphics routines (pixel, rect, string, XOR)
          - Memory allocator (malloc/free)
          - Event system
          - FAT12 filesystem driver
          - Application loader
          - Window manager (create, destroy, draw, move, drag)
          - Mouse cursor (XOR sprite, hide/show, drag state)
          - Content preservation (save/restore scratch buffer)
0x0F80    API table (header + 34 function pointers)
~0x0FA4   Font data (8x8 + 4x6)
~0x1500   Data section (window_table, variables, buffers)
~0x6FFF   End of 28KB kernel (padded)
```

### API Table Structure (at 0x1000:0x0F80)

```
Offset  Size  Content
0x00    2     Magic: 0x4B41 ('KA')
0x02    2     Version: 0x0100 (1.0)
0x04    2     Function count: 34
0x06    2     Reserved
0x08    68    Function pointers (34 × 2 bytes)
```

---

## Segment Architecture

### Why Kernel at 64KB Boundary?

```
Segment:Offset = 0x1000:0x0000
Linear address = (0x1000 × 16) + 0x0000 = 0x10000 = 64KB
```

**Reasons:**
1. **Clean Segmentation** - Single segment (0x1000) covers entire kernel
2. **No segment manipulation** - DS, ES, CS all 0x1000 during kernel execution
3. **Clear heap boundary** - Heap at 0x1400:0000 is 16KB after kernel start
4. **Future compatibility** - Aligned for potential protected mode transition

### Dual Segment App Architecture

```
0x2000:0x0000  Shell (Launcher) - persists during app execution
0x3000:0x0000  User app (Clock, Browser, Mouse, etc.)
```

- Shell loads at 0x2000, user apps at 0x3000
- Apps use `[ORG 0x0000]` and are loaded at offset 0
- Launcher survives while user apps run and return via RETF

### Scratch Buffer (0x5000)

Used for OS-managed content preservation during window drags:
- Before moving a window, its CGA pixel content is saved here
- After the move, content is restored at the new position
- Prevents apps from needing to redraw during drags
- Size: up to 64KB (enough for full-screen window content)

---

## Current Memory Usage (Build 135)

| Region | Size | Usage | Efficiency |
|--------|------|-------|------------|
| 0x0500-0x7C00 | 30KB | Unused (TPA) | Could optimize |
| 0x1000-0x16FF | 28KB | Kernel | In use |
| 0x1400-0x1FFF | 48KB | Heap | Available |
| 0x2000-0x2FFF | 64KB | Shell segment | In use when launcher active |
| 0x3000-0x3FFF | 64KB | User app | In use when app running |
| 0x5000-0x5FFF | 64KB | Scratch buffer | In use during window drags |

**Overall:** Good memory utilization with clear segment separation.

---

*Document created: 2026-01-23*
*Last updated: 2026-02-11*
*UnoDOS v3.13.0 Build 135*
