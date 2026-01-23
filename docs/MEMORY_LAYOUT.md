# UnoDOS Memory Layout Analysis

## Physical Memory Map (Real Mode, 640KB System)

```
Address Range          Size    Usage
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
0x0000:0x0000          1 KB    Interrupt Vector Table (IVT)
0x0000:0x0400          256 B   BIOS Data Area (BDA)
0x0000:0x0500          ~30 KB  FREE / Transient Program Area ⚠️
0x0000:0x7C00          512 B   Boot Sector (loaded by BIOS)
0x0000:0x7E00          ~1 KB   FREE (after boot sector)
0x0800:0x0000          2 KB    Stage2 Bootloader
0x0800:0x0800          ~54 KB  FREE (between stage2 and kernel)
0x1000:0x0000          16 KB   Kernel (current, v3.5.0)
0x1000:0x0500            256 B    ↳ API Table (fixed position)
0x1400:0x0000          ~540 KB Heap (malloc pool)
0x9FFF:0xFFFF          -       End of conventional memory
0xA000:0x0000          64 KB   EGA/VGA graphics memory
0xB000:0x0000          32 KB   Monochrome text memory
0xB800:0x0000          16 KB   CGA text/graphics memory
0xC000:0x0000          256 KB  ROM BIOS extensions
0xF000:0x0000          64 KB   System BIOS ROM
```

---

## Question 1: Why Kernel at 64KB Boundary?

### The 64KB "Boundary"

**Linear address calculation:**
```
Segment:Offset = 0x1000:0x0000
Linear address = (0x1000 × 16) + 0x0000 = 0x10000 = 65536 = 64KB
```

### Why This Location?

**Not for hardware reasons** - The 8086 has no special handling at 64KB:
- No memory protection
- No alignment requirements
- No performance benefits

**Real reasons:**

1. **Clean Segmentation**
   - Single segment (0x1000) covers entire kernel
   - No segment register manipulation needed within kernel
   - DS, ES, CS all set to 0x1000 during kernel execution

2. **Conventional Placement**
   - DOS COM files: 0x0100 (256 bytes after PSP)
   - DOS EXE files: Variable, but typically >64KB
   - Early Unix: Kernel in high memory
   - We chose middle ground: 64KB

3. **Heap Separation**
   - Clear boundary between kernel and heap
   - Heap at 0x1400:0000 (80KB) is 16KB after kernel start
   - Easy to calculate: "Heap starts where kernel ends"

4. **Future Compatibility**
   - 286+ processors: Segment descriptors prefer aligned bases
   - Protected mode transition: Clean GDT entries
   - 64KB is a power of 2, aligns with page boundaries (4KB)

### Could We Use a Different Address?

**YES! Options:**

| Address | Pros | Cons |
|---------|------|------|
| 0x0500:0000 | Uses low memory, more heap | Complex (near BIOS data) |
| 0x0800:0000 | Right after stage2 | Only 54KB to heap, wasteful |
| 0x1000:0000 | **Current choice** | Some memory "wasted" below |
| 0x2000:0000 | More isolation | Less heap (starts at 128KB) |

**Conclusion:** 0x1000:0000 (64KB) is a good compromise.

---

## Question 2: The 30KB "Wasted" Space

### What's Actually There?

```
0x0000:0x0500 to 0x0000:0x7BFF = 30,464 bytes (29.75 KB)
```

This is the **Transient Program Area (TPA)** in DOS terminology.

### Why Not Use It for Kernel Code?

**Segmentation Complexity:**

If we put kernel functions in multiple segments:

```asm
; Keyboard driver at 0x0500
; Event system at 0x1000 (kernel segment)

; To call keyboard driver from kernel:
push ds
mov ax, 0x0050          ; Segment for low memory
mov ds, ax
call far [0x0050:keyboard_handler]  ; Far call required
pop ds
```

**Every cross-segment call adds:**
- 4+ extra instructions
- ~20-30 cycles overhead
- Code complexity

Compare to current (everything in 0x1000):
```asm
call keyboard_handler   ; Near call, ~15 cycles
```

### Could We Use It For Other Things?

**Option A: Disk I/O Buffers** ✓ FEASIBLE
```
0x0000:0x0500  4 KB disk read buffer
0x0000:0x1500  4 KB disk write buffer
```

**Benefits:**
- Keep heap clean for app allocations
- Faster disk I/O (dedicated buffers)
- Easy to access (low segment)

**Option B: Stack Space** ✓ FEASIBLE
```
0x0000:0x7C00  ← Boot sector
0x0000:0x7BFF  ↓ Stack grows down
0x0000:0x5000  Stack bottom (10KB stack)
```

**Benefits:**
- Large stack for deep call chains
- Away from kernel data
- Traditional DOS-style placement

**Option C: Loadable Modules** ⚠️ COMPLEX

Load keyboard/event drivers here, call via far pointers:
```
0x0000:0x0500  Keyboard driver module (2KB)
0x0000:0x0D00  Event system module (1KB)
```

**Drawbacks:**
- Far call overhead on every keystroke
- Complex module loader needed
- Segment register management

**Option D: Leave It Unused** ✓ CURRENT APPROACH

**Why this is actually OK:**
- BIOS uses some of it during POST
- Early boot code uses it temporarily
- Apps could use it if they want
- Simplicity is valuable

### Modern OS Comparison

**DOS (MS-DOS 6.22):**
- IO.SYS at 0x0070
- MSDOS.SYS at ~0x1000
- Uses TPA for driver loading

**Linux (early versions):**
- Kernel at 0x10000 (64KB) or 0x100000 (1MB)
- Everything below used for boot and setup

**CP/M:**
- BIOS at top of memory
- TPA from 0x0100 to BDOS
- Apps load in TPA

**UnoDOS:**
- Similar to Linux approach
- Clean separation of boot/kernel/heap
- Simplicity over maximum memory utilization

---

## Kernel Size Expansion Options

### Current Limitation

```
Kernel: 16KB (0x1000:0000 to 0x1000:0x3FFF)
  Code: ~5KB actual
  Padding: ~11KB to maintain 16KB boundary
```

**Problem:** Need ~1200 bytes more for keyboard + events

### Option 1: Expand to 24KB (RECOMMENDED)

**Changes:**
```asm
; kernel/kernel.asm (line 351):
times 24576 - ($ - $$) db 0    ; Was: 16384

; boot/stage2.asm (kernel load loop):
mov cx, 48                      ; Was: 32 (sectors to load)
```

**New layout:**
```
0x1000:0x0000  Kernel (24KB)
0x1600:0x0000  Heap starts here (was 0x1400)
```

**Impact:**
- Heap: 540KB → 532KB (loses 8KB)
- Kernel space: 16KB → 24KB (gains 8KB)
- Headroom: ~7KB for future features

### Option 2: Expand to 32KB

**New layout:**
```
0x1000:0x0000  Kernel (32KB)
0x1800:0x0000  Heap starts here
```

**Impact:**
- Heap: 540KB → 524KB (loses 16KB)
- Kernel space: 16KB → 32KB (gains 16KB)
- Headroom: ~25KB for future features

### Option 3: Multi-Segment Kernel (NOT RECOMMENDED)

Put keyboard driver in low memory:
```
0x0000:0x0500  Keyboard driver (far calls required)
0x1000:0x0000  Main kernel
```

**Why not:**
- Every keyboard interrupt: far call overhead
- Complex segment management
- Not worth the 8KB heap savings

---

## Padding Explained

### Two Types of Padding

**1. Padding to API Table (offset 0x500)**

```asm
; Line 305 in kernel.asm
times 0x0500 - ($ - $$) db 0
kernel_api_table:
    dw 0x4B41    ; Magic: 'KA'
```

**Purpose:**
- API table at FIXED, PREDICTABLE location
- Apps discover via INT 0x80, then use offset 0x500
- Changing this breaks all applications
- This padding is REQUIRED

**2. Padding to Kernel Size (16KB)**

```asm
; Line 351 in kernel.asm
times 16384 - ($ - $$) db 0
```

**Purpose:**
- Makes kernel binary exactly 16KB
- Stage2 knows how many sectors to load (32 sectors)
- Clean heap start boundary
- This padding is OPTIONAL (could use variable size)

### Why Pad to 16KB Instead of Variable Size?

**Advantages of fixed size:**
- Bootloader is simpler (load exactly N sectors)
- Heap starts at predictable address
- Disk image is predictable
- Testing is easier (consistent layout)

**Disadvantages:**
- Wastes floppy space if kernel is small
- Creates artificial limit

**Could we use variable size?**

YES! We'd need:
```asm
; kernel.asm: Store actual size in header
kernel_size_sectors: dw 32  ; Update this when kernel grows

; stage2.asm: Read size from kernel header
mov cx, [kernel_size_sectors]  ; Dynamic sector count
```

**Is it worth it?** NO:
- Adds complexity
- Saves nothing (floppy has 360KB, kernel is <24KB)
- Fixed size is simpler

---

## Recommendations

### Immediate Action: Expand to 24KB

**Reasons:**
1. Simple: 2 line changes
2. Conservative: Leaves 532KB heap (plenty)
3. Sufficient: ~7KB headroom for keyboard + events + future
4. Standard: Similar to other small OSes

### Future Considerations

**When kernel exceeds 24KB:**
- Expand to 32KB (one-time change)
- Consider loadable modules for non-essential features
- Move test code to separate test program

**When heap becomes tight (<256KB):**
- Unlikely with current architecture
- Apps would need to be VERY large
- Could implement disk-based virtual memory

### Memory Utilization Summary

| Region | Size | Usage | Efficiency |
|--------|------|-------|------------|
| 0x0500-0x7C00 | 30KB | Unused | ⚠️ Could optimize |
| 0x1000-0x1400 | 16KB | Kernel | ✓ Reasonable |
| 0x1400-0x9FFF | 540KB | Heap | ✓ Excellent |

**Overall:** 92% memory utilization (540KB / 590KB available)

---

## Conclusion

**Question 1: Why 64KB boundary?**
- Not a hardware requirement on 8086
- Clean segmentation (single 0x1000 segment)
- Conventional practice
- Future compatibility

**Question 2: Why not use the 30KB at 0x0500?**
- Could use for buffers/stack (worth considering!)
- Not worth it for code (far call overhead)
- Simplicity has value
- 92% memory utilization is excellent

**Recommended Action:**
Expand kernel to 24KB, keep architecture simple.

*Document created: 2026-01-23*
*UnoDOS v3.5.0*
