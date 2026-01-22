# UnoDOS 3 Features Roadmap

This document outlines planned features for UnoDOS 3, tailored for the IBM PC XT hardware platform and tested on the HP Omnibook 600C.

## Hardware Target Summary

| Component | Specification |
|-----------|---------------|
| CPU | Intel 8088/8086 (4.77-10 MHz) to 486 |
| RAM | 128 KB minimum, 640 KB typical |
| Display | CGA 320x200 4-color (also works on VGA in CGA mode) |
| Storage | 360KB or 1.44MB floppy disk |
| Input | PC/XT keyboard, optional serial mouse |
| Audio | PC speaker (optional) |

## Current Features (v3.1.x)

### Boot System
- [x] Two-stage boot loader
- [x] Memory detection
- [x] Video adapter detection

### Graphics
- [x] CGA 320x200 4-color mode
- [x] Custom 8x8 bitmap font
- [x] Custom 4x6 small font
- [x] Pixel plotting routines
- [x] Box/border drawing

### User Interface
- [x] Graphical welcome screen
- [x] Real-time clock display (RTC)
- [x] RAM status display
- [x] Character set demonstration

---

## Phase 1: Input & Basic GUI

**Goal**: Enable user interaction and basic window management.

### Keyboard Driver
- [ ] Scan code reading (INT 9h / Port 60h)
- [ ] Key-to-ASCII translation
- [ ] Keyboard buffer management
- [ ] Modifier key tracking (Shift, Ctrl, Alt)
- [ ] Special keys (arrows, function keys, Escape)

### Mouse Driver (Optional)
- [ ] Microsoft serial mouse protocol
- [ ] Mouse cursor rendering
- [ ] Click detection
- [ ] Cursor movement and bounds checking

### Window Manager
- [ ] Window structure (position, size, title, content)
- [ ] Window drawing (title bar, borders, content area)
- [ ] Window stacking (Z-order)
- [ ] Active window highlighting
- [ ] Window moving (keyboard-based initially)
- [ ] Window resizing (fixed sizes or snap-to-grid)

### Basic Widgets
- [ ] Button widget
- [ ] Label widget
- [ ] Text input field (single line)
- [ ] List/menu widget
- [ ] Scrollbar (optional, for longer lists)

---

## Phase 2: File System

**Goal**: Read and write files from the floppy disk.

### FAT12 Support
- [ ] Boot sector parsing (BPB - BIOS Parameter Block)
- [ ] FAT table reading
- [ ] Directory entry parsing
- [ ] File reading (sequential)
- [ ] Filename display (8.3 format)

### File Manager Application
- [ ] Directory listing display
- [ ] File selection
- [ ] File information display (size, date)
- [ ] Navigation between directories

### Advanced File Operations (Later)
- [ ] File writing
- [ ] File creation
- [ ] File deletion
- [ ] Disk formatting

---

## Phase 3: Built-in Applications

**Goal**: Provide useful applications within the 360KB floppy constraint.

### Text Editor
- [ ] Load and display text files
- [ ] Cursor movement
- [ ] Text insertion and deletion
- [ ] Line wrapping
- [ ] Save to disk
- [ ] Simple search function

### Calculator
- [ ] Numeric keypad input
- [ ] Basic operations (+, -, *, /)
- [ ] Memory functions (M+, M-, MR, MC)
- [ ] Display with result history

### Clock Application
- [ ] Large time display
- [ ] Date display (if RTC supports it)
- [ ] Alarm function (PC speaker beep)
- [ ] Stopwatch/timer

### Settings/Control Panel
- [ ] Display settings (if applicable)
- [ ] Date/time setting
- [ ] System information display
- [ ] About UnoDOS screen

### Game: Snake or Similar
- [ ] Simple graphics game
- [ ] Keyboard controls
- [ ] Score display
- [ ] Demonstrates graphics capability

---

## Phase 4: System Services

**Goal**: Provide APIs for applications and system stability.

### Memory Management
- [ ] Simple memory allocator
- [ ] Free list management
- [ ] Out-of-memory handling

### Timer Services
- [ ] System tick counter
- [ ] Delay functions
- [ ] Timer callbacks (if feasible)

### String/Utility Library
- [ ] String comparison
- [ ] String copying
- [ ] Number-to-string conversion
- [ ] String-to-number parsing

### Error Handling
- [ ] Error code definitions
- [ ] Error message display
- [ ] Graceful failure recovery

---

## Phase 5: Polish & Optimization

**Goal**: Improve user experience and performance.

### Visual Improvements
- [ ] Custom window themes/colors
- [ ] Icons (if space permits)
- [ ] Splash screen improvements
- [ ] Smoother animations

### Performance
- [ ] Optimized pixel routines
- [ ] Dirty rectangle tracking (partial screen updates)
- [ ] Reduced flicker

### Sound (PC Speaker)
- [ ] Beep for alerts
- [ ] Key click feedback
- [ ] Simple melodies/tunes

### Documentation
- [ ] User manual (on disk or in-app help)
- [ ] Keyboard shortcut reference
- [ ] Quick start guide

---

## Hardware-Specific Considerations

### For IBM PC XT (8088)

- **CPU Speed**: 4.77 MHz is slow; minimize complex calculations
- **No Math Coprocessor**: Avoid floating-point; use integer/fixed-point
- **Limited RAM**: 128KB minimum; keep data structures compact
- **Slow Floppy**: Minimize disk access; cache when possible

### For HP Omnibook 600C (486)

- **Much Faster CPU**: Can use longer delays for animations
- **VGA Display**: CGA mode is emulated; full 320x200 visible
- **DSTN Display**: Has ghosting; avoid fast-moving elements
- **1.44MB Floppy**: More space for applications and data

### Display Compatibility

The CGA 320x200 mode works across:
- Original CGA cards
- EGA cards (backward compatible)
- VGA cards (backward compatible)
- Modern VGA implementations (Omnibook)

Color palette (Palette 1):
- Background: Blue (configurable)
- Color 1: Cyan
- Color 2: Magenta
- Color 3: White

---

## Size Budget (360KB Floppy)

| Component | Estimated Size |
|-----------|----------------|
| Boot sector | 512 bytes |
| Stage 2 loader | 8 KB |
| Fonts | 1.5 KB |
| Kernel/drivers | ~10 KB |
| Window manager | ~5 KB |
| Applications | ~30 KB |
| File system overhead | ~10 KB |
| User data area | ~300 KB |
| **Total** | ~360 KB |

This leaves significant room for user files on a 360KB disk, and even more on 1.44MB.

---

## Feature Priority Matrix

| Feature | User Value | Complexity | Priority |
|---------|-----------|------------|----------|
| Keyboard input | High | Medium | **P1** |
| Window manager | High | High | **P1** |
| FAT12 read | High | Medium | **P1** |
| Text editor | High | High | **P2** |
| Mouse support | Medium | Medium | **P2** |
| Calculator | Medium | Low | **P2** |
| File writing | Medium | Medium | **P3** |
| Games | Low | Medium | **P3** |
| Sound | Low | Low | **P3** |

---

## Non-Goals

The following are explicitly out of scope for UnoDOS 3:

- **Networking**: No TCP/IP, no modem support
- **Multitasking**: Single application at a time
- **Protected Mode**: Stays in real mode for XT compatibility
- **High Resolution**: Sticks to CGA modes for compatibility
- **Modern File Systems**: No FAT32, NTFS, ext4, etc.
- **DOS Compatibility**: Not a DOS clone; different API

---

## Version Milestones

| Version | Target Features |
|---------|-----------------|
| 3.2.0 | Keyboard input working |
| 3.3.0 | Basic window manager |
| 3.4.0 | FAT12 read support |
| 3.5.0 | File manager application |
| 4.0.0 | Text editor, multiple apps |

---

*Document version: 1.0 (2026-01-22)*
