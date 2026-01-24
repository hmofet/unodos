# FAT12 Hardware Debugging - HP Omnibook 600C

## Summary

During real hardware testing on an HP Omnibook 600C, several critical bugs were discovered and fixed in the FAT12 implementation. This document details the debugging process, the bugs found, and their fixes.

**Final Status:** FAT12 mount, open, read (including multi-cluster), and close all working on real hardware.

---

## Hardware Environment

| Component | Specification |
|-----------|---------------|
| Machine | HP Omnibook 600C |
| CPU | Intel 486DX4-75 MHz |
| RAM | 8 MB |
| Display | Monochrome DSTN LCD (VGA with CGA emulation) |
| Floppy | 3.5" 1.44MB |
| OS | UnoDOS v3.10.1 |

---

## Bugs Fixed

### Bug 1: Stack Cleanup in .found_file (Critical)

**Symptom:** After filename comparison passed (showed "="), system hung.

**Root Cause:** When a file was found, the code jumped to `.found_file` but didn't clean up the DS register that was pushed at line 1608 during the search loop.

**Stack state at bug:**
```
[8.3 filename - 11 bytes]
[sector counter - 2 bytes]
[sector number - 2 bytes]
[DS from search loop - 2 bytes]  ← FORGOT TO POP THIS!
```

**Fix:**
```asm
.found_file:
    ; ... extract file info from SI ...

    ; Restore DS
    push cs
    pop ds

    ; Clean up stack - DS from line 1608 is still on stack!
    add sp, 2                       ; DS from search loop (line 1608)  ← ADDED
    add sp, 2                       ; sector number
    add sp, 2                       ; sector counter
    add sp, 11                      ; 8.3 filename
```

**Commit:** debug08

---

### Bug 2: LBA to CHS Conversion in fat12_read (Critical)

**Symptom:** Mount and open succeeded, but read failed.

**Root Cause:** The LBA to CHS conversion in `fat12_read` was completely broken:
- Used bitmasks instead of proper division
- DH (head) was never calculated
- ES segment wasn't set for INT 13h read
- BX (buffer pointer) was clobbered during the division

**Original (broken) code:**
```asm
; WRONG: Used bitmasks instead of division
mov cl, al                      ; Sector from lower bits (WRONG)
and cl, 0x3F
inc cl
mov ch, ah                      ; Cylinder from upper bits (WRONG)
; DH never set!
; ES never set!
```

**Fixed code:**
```asm
    mov bx, 0x1000
    mov es, bx                      ; ES = 0x1000 (for INT 13h read)
    push bx
    pop ds                          ; DS = 0x1000 (for later access)
    mov bx, bpb_buffer              ; BX = buffer offset

    ; Convert LBA to CHS (proper conversion for floppy)
    push bx                         ; Save buffer pointer
    push ax                         ; Save LBA
    xor dx, dx
    mov bx, 18                      ; Sectors per track (1.44MB floppy)
    div bx                          ; AX = LBA / 18, DX = LBA % 18
    inc dx                          ; DX = sector (1-based)
    mov cl, dl                      ; CL = sector
    xor dx, dx
    mov bx, 2                       ; Number of heads
    div bx                          ; AX = cylinder, DX = head
    mov ch, al                      ; CH = cylinder
    mov dh, dl                      ; DH = head
    pop ax                          ; Restore LBA
    pop bx                          ; Restore buffer pointer
```

**Key fixes:**
1. Set ES=0x1000 before INT 13h read
2. Proper division: `sector = (LBA % 18) + 1`
3. Proper head calculation: `head = (LBA / 18) % 2`
4. Proper cylinder calculation: `cylinder = LBA / 36`
5. Preserve BX around division operations

**Commit:** debug09

---

### Bug 3: Attribute Reading Simplification

**Original:** Used complex ES segment override with push/pop:
```asm
    push es
    push bx
    mov bx, 0x1000
    mov es, bx
    mov al, [es:si + 0x0B]
    mov [cs:.filt_attr], al
    pop bx
    pop es
```

**Simplified:** DS is already 0x1000 in the search loop:
```asm
    mov al, [si + 0x0B]             ; Read attribute byte
```

**Impact:** Cleaner code, fewer bytes, no functional change.

**Commit:** release

---

## Test File Structure

The test file (`TEST.TXT`) was created using `tools/create_multicluster_test.py`:

```
File: TEST.TXT (1024 bytes, 2 clusters)
Cluster 1 (512 bytes): "CLUSTER 1: " + 'A' * 500 + '\n'
Cluster 2 (512 bytes): "CLUSTER 2: " + 'B' * 500 + '\n'

FAT chain: cluster 2 → cluster 3 → EOF (0xFFF)
```

**Verification Output:**
```
C1:A C2:B
```
- C1:A = Character at offset 11 of cluster 1 is 'A' (correct)
- C2:B = Character at offset 512+11 is 'B' (correct, from cluster 2)

---

## Debugging Methodology

### Iterative Debug Builds

Each debug iteration added specific output to narrow down the bug:

| Build | Output | Purpose |
|-------|--------|---------|
| debug06 | D:T S:T A:20 F:20 | Verify directory entry found |
| debug07 | = or ! | Show cmpsb comparison result |
| debug08 | Open: OK/FAIL | Test .found_file path |
| debug09 | Read: OK/FAIL + content | Test fat12_read |
| debug10 | Full cluster content | Verify multi-cluster |
| debug11 | C1:A C2:B | Compact multi-cluster verify |

### Debug Output Format

For the directory search, we displayed:
- **D:** First character of directory entry
- **S:** First character of search filename
- **A:** Attribute byte (hex)
- **F:** Attribute byte after filtering (hex)
- **=** or **!:** Comparison result (match/mismatch)

This allowed identifying exactly where the failure occurred in the 11-byte filename comparison.

---

## CHS Conversion Formula

For 1.44MB floppy disks:
- Sectors per track: 18
- Heads: 2
- Cylinders: 80

**LBA to CHS:**
```
Sector (1-based):   (LBA % 18) + 1
Head:               (LBA / 18) % 2
Cylinder:           LBA / 36
```

**Example - Reading root directory (LBA 19):**
```
Sector: (19 % 18) + 1 = 1 + 1 = 2
Head:   (19 / 18) % 2 = 1 % 2 = 1
Cyl:    19 / 36 = 0

Result: C=0, H=1, S=2 ✓
```

**Example - Reading data cluster 2 (LBA 33):**
```
Sector: (33 % 18) + 1 = 15 + 1 = 16
Head:   (33 / 18) % 2 = 1 % 2 = 1
Cyl:    33 / 36 = 0

Result: C=0, H=1, S=16 ✓
```

---

## FAT12 Cluster Chain Algorithm

Reading the FAT to follow cluster chains:

```
FAT12 Cluster Chain Algorithm:
1. Calculate FAT byte offset: (cluster × 3) / 2
2. Load FAT sector containing that offset
3. Read 2 bytes (word) from FAT at offset
4. If cluster is even: value = word & 0x0FFF
   If cluster is odd:  value = word >> 4
5. If value >= 0xFF8: End of chain
   Otherwise: value is the next cluster number
```

**Implementation in get_next_cluster():**
```asm
    ; Calculate FAT offset
    mov bx, ax              ; BX = cluster
    shl ax, 1               ; AX = cluster * 2
    add ax, bx              ; AX = cluster * 3
    shr ax, 1               ; AX = (cluster * 3) / 2

    ; Load word from FAT
    mov ax, [fat_cache + offset]

    ; Extract 12-bit value
    test original_cluster, 1
    jnz .odd
    and ax, 0x0FFF          ; Even: lower 12 bits
    jmp .done
.odd:
    shr ax, 4               ; Odd: upper 12 bits
.done:
    ; Check for EOF
    cmp ax, 0xFF8
    jae .end_of_chain
```

---

## Lessons Learned

### 1. Stack Balance is Critical
In complex functions with multiple exit paths (like `fat12_open`), tracking stack balance is essential. Every push must have a corresponding pop or `add sp, N`.

### 2. Division Clobbers Registers
x86 division uses DX:AX as dividend and returns quotient in AX, remainder in DX. Any register you need to preserve must be pushed before division.

### 3. ES Must Be Set for INT 13h
BIOS disk services (INT 13h) expect the buffer address in ES:BX. Forgetting to set ES is a common bug.

### 4. Real Hardware Reveals Bugs
QEMU was more forgiving than real hardware. Bugs that "worked" in emulation failed on the Omnibook.

### 5. Iterative Debug Output Works
Adding targeted debug output at each stage quickly isolated the bugs. The D:T S:T A:20 F:20 format immediately showed that directory parsing was working.

---

## Files Modified

| File | Changes |
|------|---------|
| kernel/kernel.asm | Fixed stack cleanup, LBA→CHS conversion, simplified attribute read |
| docs/FAT12_HARDWARE_DEBUG.md | This document |
| CHANGELOG.md | Updated with bug fixes |

---

## Verification Commands

```bash
# Build release
make clean && make floppy144

# Create test floppy
python3 tools/create_multicluster_test.py build/test-fat12-multi.img

# Test in QEMU
make test-fat12-multi

# Test on hardware
# 1. Write UnoDOS to floppy
# 2. Write test image to second floppy
# 3. Boot, press F, swap floppies, press key
# 4. Should show: Mount: OK, Open: OK, Read: OK, C1:A C2:B
```

---

## Conclusion

Three critical bugs were fixed during hardware testing:

1. **Stack cleanup bug** - Hung after finding file
2. **LBA→CHS conversion bug** - Read failed completely
3. **ES segment bug** - INT 13h wrote to wrong memory

All bugs were found through iterative debugging with targeted output. The final v3.10.1 release successfully mounts FAT12, opens files, and reads multi-cluster files on real hardware.

---

*Document created: 2026-01-24*
*UnoDOS v3.10.1 - Hardware Testing Complete*
