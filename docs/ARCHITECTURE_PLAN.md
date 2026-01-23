# UnoDOS Architecture & Prioritization Analysis

## Executive Summary

The "live memory display" feature is **architecturally premature**. Before implementing demo features, we need to establish the **foundation layer** that enables a sustainable app ecosystem. This analysis recommends a different prioritization focused on building reusable architectural components first.

---

# PART 1: ARCHITECTURAL ANALYSIS

## Current State Assessment

**What we have (v3.2.3):**
- Three-stage boot (boot → stage2 → kernel)
- CGA 320x200 graphics mode working
- Basic drawing primitives (draw_char, plot_pixel_white)
- Font rendering (8x8 bitmap font)
- Static welcome screen

**What we're missing:**
- Input handling (keyboard/mouse)
- Memory management
- Event system
- Application loading mechanism
- System call API
- Window management

## Architectural Dependencies

```
┌─────────────────────────────────────────┐
│         Applications Layer              │
│  (Clock, Text Editor, Calculator, etc)  │
└────────────────┬────────────────────────┘
                 │ depends on
┌────────────────▼────────────────────────┐
│      Window Manager & App Loader        │
│  (Window drawing, event routing, FAT12) │
└────────────────┬────────────────────────┘
                 │ depends on
┌────────────────▼────────────────────────┐
│         Foundation Layer                │
│  Graphics API | Keyboard | Memory       │
│  Event System | System Calls            │
└─────────────────────────────────────────┘
                 │ depends on
┌────────────────▼────────────────────────┐
│           Bootloader (Done ✓)           │
└─────────────────────────────────────────┘
```

## The Problem with "Live Memory Display" Now

**Issues:**
1. **Architectural distraction**: Doesn't build toward reusable components
2. **Blocking design**: Uses CPU in tight delay loop, prevents other features
3. **Monolithic kernel**: Hardcodes app logic in kernel (violates separation)
4. **No reuse**: Code won't help with future features
5. **Performance**: Wastes CPU cycles when OS should be event-driven

**Better approach:**
- Implement as a **loadable application** after foundation layer exists
- Use **timer service** (Phase 4) instead of blocking loop
- Demonstrate **app development API** rather than kernel feature

---

# PART 2: SYSTEM CALL ARCHITECTURE (CRITICAL DECISION)

Based on analysis in [SYSCALL.md](SYSCALL.md) and the nature of UnoDOS as a **graphical OS on 8086**, the system call architecture is the most critical architectural decision that will be nearly impossible to change later.

## The Performance Reality

**Key Constraint:** In a graphical OS, system calls happen **hundreds of times per frame**
- Drawing lines, pixels, rectangles
- Checking mouse/keyboard events
- Blitting sprite data
- Window clipping calculations

**On a 4.77MHz 8088:**
- INT instruction: ~50-70 cycles
- CALL FAR: ~28-35 cycles
- Difference: **~40 cycles saved per call**

**Impact:** At 60 FPS with 100 graphics calls per frame:
- INT approach: 420,000 cycles/second wasted = **~9% of CPU at 4.77MHz**
- This causes visible UI stuttering and "mouse lag"

## Recommended Architecture: Hybrid Approach

Following the pattern used by **Windows 1.x/2.x, GEOS, and early Mac OS**, use BOTH mechanisms:

### Phase 1: Discovery (INT 0x80)

**One-time setup when app loads:**

```asm
; App initialization
mov ax, 0x0000              ; Function: Get API table
int 0x80                    ; Call OS
; Returns: ES:BX = pointer to kernel jump table
mov [kernel_table_seg], es
mov [kernel_table_off], bx
```

**Kernel handler:**
```asm
int_80_handler:
    cmp ax, 0x0000
    jne .other_functions

    ; Return pointer to jump table
    mov bx, cs
    mov es, bx
    mov bx, kernel_api_table
    iret
```

### Phase 2: Execution (Far Call Table)

**All subsequent calls use fast CALL FAR:**

```asm
; App code - draw a pixel
mov ax, 100                 ; X coordinate
mov bx, 50                  ; Y coordinate
mov cl, 3                   ; Color (white)
call far [kernel_table_seg:gfx_draw_pixel_offset]
```

**Kernel jump table structure:**
```asm
kernel_api_table:
    ; Graphics API (frequent calls - optimize for speed)
    dw gfx_draw_pixel          ; Offset 0
    dw gfx_draw_line           ; Offset 4
    dw gfx_draw_rect           ; Offset 8
    dw gfx_draw_char           ; Offset 12
    dw gfx_draw_string         ; Offset 16
    dw gfx_clear_area          ; Offset 20
    dw gfx_blit_sprite         ; Offset 24

    ; Memory management (less frequent)
    dw mem_alloc               ; Offset 28
    dw mem_free                ; Offset 32

    ; Event system
    dw event_get               ; Offset 36
    dw event_wait              ; Offset 40

    ; File I/O (very infrequent)
    dw file_open               ; Offset 44
    dw file_read               ; Offset 48
    dw file_write              ; Offset 52
    dw file_close              ; Offset 56
```

## Benefits of Hybrid Approach

### 1. Performance Where It Matters
- Graphics calls: Fast CALL FAR (~30 cycles)
- I/O calls: Can still use INT if preferred (50-70 cycles, but I/O takes thousands anyway)

### 2. Developer Experience
**Provide a standard library (graphics.lib) for C developers:**

```c
// graphics.h
void gfx_draw_pixel(int x, int y, int color);
void gfx_draw_line(int x1, int y1, int x2, int y2, int color);
// ... etc

// graphics.lib implementation (assembly)
_gfx_draw_pixel:
    ; Standard C calling convention (stack-based parameters)
    push bp
    mov bp, sp
    mov ax, [bp+4]  ; x
    mov bx, [bp+6]  ; y
    mov cl, [bp+8]  ; color
    call far [_kernel_table:0]  ; Call offset 0 of table
    pop bp
    ret
```

**App developer writes normal C:**
```c
#include "graphics.h"

void draw_window() {
    gfx_draw_rect(10, 10, 100, 50, WHITE);
    gfx_draw_string(15, 20, "Hello World");
}
```

### 3. Binary Compatibility
- Apps never hardcode kernel addresses
- Kernel can be relocated in memory
- OS updates don't break apps
- Table offsets are the "ABI" - as long as offsets don't change, apps work

### 4. Future Protected Mode Transition

When moving to 386+ Protected Mode, the hybrid approach enables "thunking":

```
Real Mode App → Calls table → Thunk stub → Mode switch → PM Kernel
```

The app binary **never changes**. Only the thunk stubs need to be rewritten.

## Memory Layout with API Table

```
0x0000:0x0000  IVT (includes INT 0x80 vector)     1 KB
0x0000:0x0500  Free/Temporary                     ~30 KB
0x0000:0x7C00  Boot sector                        512 bytes
0x0800:0x0000  Stage2 loader                      2 KB
0x1000:0x0000  Kernel code + API table            16 KB
   0x1000:0000    Kernel signature + entry
   0x1000:0200    Graphics functions
   0x1000:0400    Memory management
   0x1000:0500    Kernel API table (256 bytes)
   0x1000:0600    Other kernel code
0x1400:0x0000  Heap start (malloc pool)           ~550 KB
0xB800:0x0000  CGA video memory                   16 KB
```

## API Table Format

```asm
; At fixed address 0x1000:0x0500
kernel_api_table:
    dw 0x4B41              ; Magic: 'KA' (Kernel API)
    dw 0x0001              ; Version: 1.0
    dw 60                  ; Number of functions
    dw 0                   ; Reserved

    ; Function pointers (segment already known: CS)
    dw gfx_draw_pixel      ; Function 0
    dw gfx_draw_line       ; Function 1
    dw gfx_draw_rect       ; Function 2
    ; ... etc (60 functions = 120 bytes)
```

---

# PART 3: IMPLEMENTATION ROADMAP

## Phase 1: Foundation Layer (v3.3.0 - v3.4.0)

### 1.1 System Call Infrastructure (FIRST - Everything Depends On This)
**Critical:** Implement the hybrid INT 0x80 + Far Call Table

**Components:**
- INT 0x80 handler (discovery mechanism)
- Kernel API table at fixed address (0x1000:0x0500)
- Table population with stub functions initially
- Test harness to verify table works

**Size:** ~300 bytes
**Time:** 2-3 hours
**Impact:** Enables all future development; must get this right

### 1.2 Graphics API Abstraction
**Wraps existing functions, adds to API table**

Functions:
- gfx_draw_pixel (wraps plot_pixel_white)
- gfx_draw_rect
- gfx_draw_filled_rect
- gfx_draw_char (wraps draw_char)
- gfx_draw_string
- gfx_clear_area

**Size:** ~500 bytes
**Time:** 2-3 hours

### 1.3 Memory Allocator
- malloc/free implementation
- Add to API table at offsets 28, 32

**Size:** ~600 bytes
**Time:** 3-4 hours

### 1.4 Keyboard Driver
- Scan code reading (Port 60h)
- ASCII translation
- Key buffer
- Add kbd_getchar to API table

**Size:** ~800 bytes
**Time:** 4-5 hours

### 1.5 Event System
- Circular event queue
- event_get / event_wait functions
- Add to API table

**Size:** ~400 bytes
**Time:** 2-3 hours

**Phase 1 Total:** ~15-18 hours, ~2.8 KB code

## Phase 2: Standard Library (v3.5.0)

### 2.1 Create graphics.lib
- C-callable wrappers for API table functions
- Stack-based parameter conversion
- Distribute with graphics.h header

**Time:** 3-4 hours

### 2.2 Create unodos.lib
- Initialization (_unodos_init calls INT 0x80)
- Memory wrappers
- Event wrappers

**Time:** 2-3 hours

**Phase 2 Total:** ~5-7 hours

## Phase 3: Core Services (v3.6.0 - v3.7.0)

### 3.1 Window Manager
- Uses graphics API (via table calls)
- Adds window_* functions to API table

**Time:** 6-8 hours

### 3.2 FAT12 + App Loader
- file_* functions added to API table
- Load .BIN apps from disk

**Time:** 6-8 hours

**Phase 3 Total:** ~12-16 hours

## Phase 4: Demo Application (v3.8.0)

### 4.1 Clock Display App
- First third-party app
- Uses graphics.lib
- Demonstrates API usage

**Time:** 2-3 hours

---

# PART 4: SIZE BUDGET ANALYSIS

| Component | Estimated Size | Cumulative |
|-----------|----------------|------------|
| **Current kernel (v3.2.3)** | ~2 KB | 2 KB |
| Graphics API | 500 bytes | 2.5 KB |
| Keyboard driver | 800 bytes | 3.3 KB |
| Memory allocator | 600 bytes | 3.9 KB |
| Event system | 400 bytes | 4.3 KB |
| System call table | 200 bytes | 4.5 KB |
| **Foundation total** | | **4.5 KB** |
| Window manager | 3 KB | 7.5 KB |
| FAT12 driver | 1.5 KB | 9 KB |
| App loader | 400 bytes | 9.4 KB |
| **Core services total** | | **9.4 KB** |
| **Fonts (included)** | 1.5 KB | 10.9 KB |
| **Total kernel+services** | | **~11 KB** |

**Remaining in 16KB kernel:** ~5 KB
**Remaining for apps on 360KB floppy:** ~340 KB

This leaves plenty of room for additional kernel features and many applications.

---

# PART 5: KEY ARCHITECTURAL DECISIONS

## Decision 1: App Binary Format

**Recommendation:** Start with flat .BIN, add header later if needed

**Option A: Flat Binary (.BIN)**
- Pros: Simple, small, fast to load
- Cons: No metadata, fixed load address, no relocations

**Option B: Simple Header Format**
```
Offset | Size | Description
-------|------|------------
0x00   | 2    | Magic number (0x554E = "UN")
0x02   | 2    | Entry point offset
0x04   | 2    | Code size
0x06   | 2    | Data size
0x08   | 2    | Min memory required
0x0A   | ...  | Code+Data
```
- Pros: Flexible, can validate, can specify requirements
- Cons: Slightly more complex loader

## Decision 2: System Call Convention

**Recommendation:** Hybrid approach (INT 0x80 + Far Call Table)

See Part 2 for detailed analysis.

## Decision 3: Event-Driven vs Cooperative Multitasking

**Recommendation:** Single-tasking for v3.x

**Current Plan: Single-tasking, Event-Driven**
- One app runs at a time
- App processes events in loop
- App yields back to OS when done

**Alternative: Cooperative Multitasking**
- Multiple apps in memory
- Each app gets time slice
- Apps yield voluntarily

Rationale:
- Simpler to implement
- Sufficient for target hardware
- Can add multitasking later (v4.x?)

## Decision 4: Memory Layout Strategy

See Part 2 for full memory layout.

---

# PART 6: CRITICAL SUCCESS FACTORS

1. **Table offsets are STABLE** - Once published, never reorder
2. **Version field allows detection** - Apps can check API version
3. **Reserved slots** - Leave gaps for future expansion
4. **Register conventions documented** - AX, BX, CX, DX usage per function
5. **Error handling** - Functions set CF on error, return code in AX

---

# PART 7: REMAINING OPEN QUESTIONS

The system call architecture is now decided (Hybrid approach). Remaining decisions to be made during implementation:

1. **App binary format:** Flat .BIN or header format? (Start with flat .BIN)
2. **Memory allocator:** First-fit, best-fit, or fixed blocks? (Decide when implementing)
3. **Window manager scope:** Minimal, basic, or full? (Decide when implementing)

---

*Document created: 2026-01-23*
*Status: Approved - Ready for implementation*
*Next step: Foundation 1.1 - System Call Infrastructure*
