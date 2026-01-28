# UnoDOS Boot Debug Messages Reference

This document explains the debug messages displayed during the UnoDOS boot process. These messages help diagnose boot issues, especially on real hardware where debugging tools are limited.

## Boot Sequence Overview

```
MBR → VBR → Stage2 → Kernel
```

## Debug Output Format

A typical successful boot on USB/HDD displays:

```
M(XX/YY)L[ZZ]Loading stage2...
Loading UnoDOS kernelRAAAABB........ OK
JCCCCCCCCKNNNNNNN VERSIONSTRING
>
```

---

## MBR Debug Messages

| Output | Meaning |
|--------|---------|
| `M` | MBR (Master Boot Record) has started executing |

The MBR prints only `M` before relocating itself and loading the VBR.

---

## VBR Debug Messages

| Output | Meaning |
|--------|---------|
| `(XX/YY)` | Drive geometry: XX = sectors/track (hex), YY = heads (hex) |
| `(00/00)` | Geometry query failed, using defaults (63/16) |
| `L` | LBA (INT 13h extended) read mode being used |
| `[ZZ]` | First byte of loaded stage2 (hex) - should be `55` for signature |
| `Loading stage2...` | VBR is about to load stage2 |

**Geometry Examples:**
- `(3F/10)` = 63 sectors/track, 16 heads (standard HDD)
- `(00/00)` = Query failed (common on USB drives)

**Expected stage2 signature:** `[55]` (first byte of 'US' signature 0x5355)

---

## Stage2 Debug Messages

| Output | Meaning |
|--------|---------|
| `Loading UnoDOS kernel` | Stage2 started, beginning kernel load |
| `R` | Root directory LBA debug marker |
| `AAAA` | Root directory start sector (hex, low 16 bits) |
| `BB` | First byte of root directory sector (hex) |
| `.` (dots) | Each dot = one sector loaded successfully |
| `OK` | Kernel loaded and signature verified |

**Root Directory Debug Example:**
- `R014455` means:
  - Root directory at LBA 0x0144 (324 decimal)
  - First byte of root dir is 0x55 ('U' from "UNODOS" volume label)

**Error Messages:**
| Output | Meaning |
|--------|---------|
| `Disk error!` | Sector read failed |
| `KERNEL.BIN not found!` | Kernel file missing from filesystem |
| `Invalid kernel!` | Kernel signature (0x4B55 "UK") not found |

---

## Pre-Jump Debug (Stage2)

| Output | Meaning |
|--------|---------|
| `J` | About to jump to kernel |
| `CCCCCCCC` | First 4 bytes at kernel entry point (hex) |

**Expected output:** `JB40EB04B`
- `B4 0E` = `mov ah, 0x0E` (BIOS teletype)
- `B0 4B` = `mov al, 'K'` (character 'K')

---

## Kernel Debug Messages

| Output | Meaning |
|--------|---------|
| `K` | Kernel entry point reached |
| `1` | Segment registers configured (DS=ES=0x1000) |
| `2` | INT 0x80 system call handler installed |
| `3` | Keyboard interrupt handler installed |
| `4` | Mouse handler complete (or skipped) |
| `UnoDOS ` | Character-by-character test (Build 071+) |
| Version string | e.g., "UnoDOS v3.13.0" |
| `>` | Waiting for keypress before graphics mode |

---

## Complete Example

Successful USB boot (Build 071):
```
M(00/00)L[55]Loading stage2...
Loading UnoDOS kernelR014455...................................................... OK
JB40EB04BKUnoDOS 1234UnoDOS v3.13.0
>
```

**Breakdown:**
1. `M` - MBR started
2. `(00/00)` - Geometry query failed
3. `L` - Using LBA mode
4. `[55]` - Stage2 signature byte correct
5. `Loading stage2...` - VBR loading stage2
6. `Loading UnoDOS kernel` - Stage2 loading kernel
7. `R014455` - Root dir at LBA 324, first byte 'U'
8. `....OK` - 56 sectors loaded, signature verified
9. `JB40EB04B` - Pre-jump check passed
10. `K1234` - Kernel init steps completed
11. `UnoDOS v3.13.0` - Version string
12. `>` - Awaiting keypress

---

## Troubleshooting

| Symptom | Likely Cause |
|---------|--------------|
| No output at all | BIOS not booting from device |
| Only `M` | MBR can't find bootable partition |
| `M` then `Disk error` | Can't read VBR sector |
| `M(XX/YY)` then hang | Stage2 load failed |
| `[00]` instead of `[55]` | Stage2 not at expected location |
| `R...` then `KERNEL.BIN not found!` | Filesystem issue |
| `OK` but no `J` | Stage2 code issue |
| `J...` but no `K` | Kernel not executing (bad jump) |
| `K` but no `1234` | Kernel crashing during init |
| `K123` but no `4` | Mouse init hanging (8042 issue) |

---

## Build Numbers

Always verify the build number matches what you expect. Build numbers are displayed:
- In text: `Build: XXX` after version string
- In graphics: Below version on blue screen

If the build number doesn't match, you're testing an old image.
