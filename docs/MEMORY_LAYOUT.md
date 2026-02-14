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
0x1000:0x0000          28 KB   Kernel (v3.15.0)
  0x1000:0x1100        ~128 B    ↳ API Table (43 functions)
0x1400:0x0000          ~64 KB  Heap (malloc pool)
0x2000:0x0000          64 KB   Shell/Launcher segment (fixed)
0x3000:0x0000          64 KB   User app slot 0 (dynamic pool)
0x4000:0x0000          64 KB   User app slot 1
0x5000:0x0000          64 KB   User app slot 2
0x6000:0x0000          64 KB   User app slot 3
0x7000:0x0000          64 KB   User app slot 4
0x8000:0x0000          64 KB   User app slot 5
0x9000:0x0000          64 KB   Scratch buffer (window drag content)
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
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
0x0000    Signature ("UK") + entry point
0x0002    Kernel code (init, drivers, API stubs)
          - INT 0x80 handler
          - Keyboard driver (INT 09h)
          - PS/2 mouse driver (INT 0x74)
          - Graphics routines (pixel, rect, string, XOR)
          - Memory allocator (malloc/free)
          - Event system
          - FAT12 filesystem driver
          - Application loader with segment pool
          - Window manager (create, destroy, draw, focus, move, drag)
          - Mouse cursor (XOR sprite, hide/show, drag state)
          - Content preservation (save/restore scratch buffer)
          - Segment pool (alloc_segment, free_segment)
0x1100    API table (header + 43 function pointers)
~0x1084   Font data (8x8 + 4x6)
~0x1500   Data section (window_table, app_table, segment_pool, variables)
~0x6FFF   End of 28KB kernel (padded)
```

### API Table Structure (at 0x1000:0x1100)

```
Offset  Size  Content
0x00    2     Magic: 0x4B41 ('KA')
0x02    2     Version: 0x0100 (1.0)
0x04    2     Function count: 43
0x06    2     Reserved
0x08    86    Function pointers (43 × 2 bytes)
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

### Multi-App Segment Architecture (Build 149+)

```
0x2000:0x0000  Shell (Launcher) - fixed, persists always
0x3000:0x0000  User app slot 0 (dynamic pool)
0x4000:0x0000  User app slot 1
0x5000:0x0000  User app slot 2
0x6000:0x0000  User app slot 3
0x7000:0x0000  User app slot 4
0x8000:0x0000  User app slot 5
```

- Shell loads at 0x2000 (fixed), user apps allocated dynamically from pool
- Up to 6 concurrent user apps, each in its own 64KB segment
- `alloc_segment(task_handle)` finds free segment, marks owned
- `free_segment(segment)` returns segment to pool on task exit
- `segment_pool` (6 words) + `segment_owner` (6 bytes) track allocation state
- Apps use `[ORG 0x0000]` and are loaded at offset 0

### Scratch Buffer (0x9000)

Used for OS-managed content preservation during window drags:
- Reserved for future use (formerly used for window drag content save/restore)
- Build 156: Outline drag replaced content save/restore approach
- Size: up to 64KB
- Moved from 0x5000 to 0x9000 in Build 149 to free segment for app pool

---

## Current Memory Usage (Build 159)

| Region | Size | Usage | Notes |
|--------|------|-------|-------|
| 0x0500-0x7C00 | 30KB | Unused (TPA) | Available for future use |
| 0x1000-0x16FF | 28KB | Kernel | In use |
| 0x1400-0x1FFF | 48KB | Heap | Available for malloc |
| 0x2000-0x2FFF | 64KB | Shell segment | Launcher (fixed) |
| 0x3000-0x8FFF | 384KB | User app pool | 6 × 64KB segments |
| 0x9000-0x9FFF | 64KB | Scratch buffer | Reserved (formerly drag content) |

**Overall:** 7 app segments (1 shell + 6 user) using 448KB of the 640KB address space.

---

*Document created: 2026-01-23*
*Last updated: 2026-02-14*
*UnoDOS v3.15.0 Build 159*
