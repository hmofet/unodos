# UnoDOS Development Session Summary
**Last Updated:** 2026-01-28
**Current Version:** v3.12.0
**Current Build:** 053
**Status:** PS/2 Mouse Driver COMPLETE

---

## Latest Session (2026-01-28) - PS/2 Mouse Driver

### Features Implemented

1. **PS/2 Mouse Driver (Foundation 1.7)** - Full PS/2 mouse support
   - INT 0x74 (IRQ12) interrupt handler
   - 8042 keyboard controller interface (ports 0x60/0x64)
   - 3-byte packet protocol parsing with sync bit detection
   - Automatic mouse detection at boot
   - Position tracking clamped to screen (0-319 X, 0-199 Y)
   - Button state tracking (left, right, middle)
   - Posts EVENT_MOUSE (type 4) to event queue

2. **New APIs (27-29)**
   - `mouse_get_state` (27) - Returns BX=X, CX=Y, DL=buttons, DH=enabled
   - `mouse_set_position` (28) - Sets cursor position
   - `mouse_is_enabled` (29) - Checks if mouse available

3. **MOUSE.BIN Test Application** (578 bytes)
   - Window-based UI with mouse cursor tracking
   - Displays '+' cursor that follows mouse position
   - Shows '*' when button pressed
   - Displays X,Y coordinates
   - Gracefully handles "no mouse detected"
   - ESC to exit

### Technical Details

**Mouse Packet Format:**
```
Byte 0: YO XO YS XS 1 M R L
  - YO/XO: Y/X overflow (ignored if set)
  - YS/XS: Y/X sign bits (for negative movement)
  - Bit 3: Always 1 (sync bit)
  - M/R/L: Middle/Right/Left buttons

Byte 1: X movement delta (8-bit, sign-extended with XS)
Byte 2: Y movement delta (8-bit, sign-extended with YS)
```

**IRQ12 Handling:**
- Send EOI to both slave PIC (0xA0) and master PIC (0x20)
- Check AUXB bit (0x20) in status to distinguish mouse from keyboard

---

## Previous Session (2026-01-28) - Dynamic Discovery & Browser

### Features Implemented

1. **fs_readdir API (API Index 26)** - Kernel directory iteration
2. **Dynamic App Discovery** - Launcher scans for .BIN files
3. **File Browser App** - Shows all files with sizes

### Bug Fixes (Builds 042-052)

| Build | Issue | Fix |
|-------|-------|-----|
| 051 | Browser ESC doesn't work | Added STI, use JC pattern |
| 052 | Cleanup | Removed debug code |

---

## Testing Procedure

```powershell
git pull
.\tools\boot.ps1           # Write UnoDOS to floppy A:
# Swap to blank floppy
.\tools\launcher.ps1       # Write launcher floppy
```

### Test Steps
1. Boot from UnoDOS floppy, verify "Build: 053"
2. Press 'L' to load launcher
3. Swap to launcher floppy when prompted
4. Launcher shows: CLOCK, TEST, BROWSER, MOUSE (dynamically discovered)
5. Select MOUSE, press Enter
6. If PS/2 mouse connected: move mouse, cursor follows
7. Click button: cursor changes to '*'
8. Press ESC → returns to launcher
9. Without mouse: shows "No mouse detected"

---

## Files on Launcher Floppy

| File | Size | Description |
|------|------|-------------|
| LAUNCHER.BIN | 1061 bytes | Desktop launcher |
| CLOCK.BIN | 249 bytes | Real-time clock display |
| TEST.BIN | 112 bytes | Hello test app |
| BROWSER.BIN | 564 bytes | File browser |
| MOUSE.BIN | 578 bytes | Mouse test app (NEW) |

---

## API Table (v3.12.0 Build 053)

| Index | Function | Description |
|-------|----------|-------------|
| 0-5 | gfx_* | Graphics API |
| 6-7 | mem_* | Memory allocation |
| 8-9 | event_* | Event system |
| 10-11 | kbd_* | Keyboard input |
| 12-16 | fs_* | Filesystem |
| 17-18 | app_* | App loader |
| 19-24 | win_* | Window manager |
| 25 | register_shell | Shell registration |
| 26 | fs_readdir | Directory iteration |
| **27** | **mouse_get_state** | **Get mouse X/Y/buttons (NEW)** |
| **28** | **mouse_set_position** | **Set mouse position (NEW)** |
| **29** | **mouse_is_enabled** | **Check mouse available (NEW)** |

---

## Memory Map

```
0x0000 - 0x0FFF  : BIOS/IVT
0x1000 - 0x13FF  : Kernel code and data
  +-- 0x1000:0x0B00 : API table
0x1400 - 0x1FFF  : Heap (kernel allocations)
0x2000 - 0x2FFF  : Shell/Launcher segment
0x3000 - 0x3FFF  : User app segment
```

---

*PS/2 Mouse Driver: COMPLETE*
*Ready for next feature*
