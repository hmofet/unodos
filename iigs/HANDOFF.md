# Apple IIGS port — implementation handoff

## M2 — SHIPPED (2026-06-15, build 414)

FAT12 storage over the SmartPort/ProDOS block driver, with Files + Notepad
and real persistence.  `tests/m2.py` → `M2 PASS` (`shots/m2_files.png`,
`m2_notepad.png`, `m2_persist.png`).

- **blk_io (`fs.i`):** calls the slot firmware's ProDOS block driver (entry
  + unit stashed by `boot.s` at `$0300-$0302`) in **6502 emulation mode** —
  `sec/xce` around the call, capture the result carry, `clc/xce` back to
  native.  Works against real firmware and the harness WDM stub identically.
  Read **and** write (cmd 1/2).
- **FAT12 (`fs.i`), root-dir scope:** `fat_mount` caches the whole 3-sector
  FAT into FATBUF (12-bit entries never straddle a sector); `fat_list_root`,
  `fat_read_file` (single + multi-cluster, verified past the 1 KB cluster
  boundary with CHAIN.TXT), and a full write path — `fat_alloc_chain`,
  `fat_free_chain`, `fat_set_entry`, `fat_flush` (both FAT copies), and
  `fat_save_file` (overwrite-by-name or first free dir slot).  Geometry is
  fixed and synced with `mkfs.py` (PORT-SPEC §6 rule 8): 512 B sectors,
  1 KB clusters, 1 reserved, 2×3 FAT, 112 root entries.  Little-endian
  on-disk fields read natively (no swap, unlike the 68K port).
- **Apps (`apps.i`):** Files lists the root dir and opens the selected file
  into Notepad; Notepad is an append editor over a 4 KB bank-0 buffer
  (printable + CR append, backspace, **Ctrl-S → `fat_save_file`**).
- **`mkfs.py`** (reused from macplus, IIGS volume label) appends the FAT12
  volume at disk block 256; **`mkdsk.py`** reserves it.  The harness ProDOS
  driver already does WRITE + `--writeback`; persistence is verified by
  reboot-and-reread.

**THE bug banked:** `blk_io` first used `F0/F1` as scratch, but the FAT
walkers keep the *current cluster* in `F0` across `blk_io` calls — so
multi-cluster reads/saves corrupted the chain and looped (single-cluster
ops escaped it). `blk_io` now uses private `BLK0/BLK1`. Audit every helper
that holds state across a `blk_io`/`jsr` for scratch aliasing (the apple2
`fs.i` zero-page collision class, restated on the 65816).

**M2 remaining (deferred):** disk-loaded apps (the macplus `diskapp.i` ABI)
— the storage primitives are all in place to add it; not needed for the
Files/Notepad surface.

## M1 — SHIPPED (2026-06-15, build 412)

The SNES M1 surface re-expressed on SHR: a colour desktop (menu bar, icon
grid, version line), the full PORT-SPEC §2 window manager (16-slot table,
z-order raise-on-click, title-bar drag, close box) on an 8×8 **cell grid**
(40×25), a polled **ADB mouse** driving a **save-under software cursor**
(no hardware sprite on SHR), the `$C000/$C010` keyboard into the event
queue, and **SysInfo + Clock** (live `HH:MM:SS`).  `tests/m1.py` → `M1
PASS` (`shots/m1_desktop.png`, `m1_sysinfo.png`, `m1_clock.png`).

**Key architectural decision (corrected from a first cut):** kernel-normal
**DBR=$00**, so all state (`VARS` at `$00:1000`) and tables sit in *fast*
bank-0 RAM; the SHR framebuffer in bank `$E1` is reached only through the
24-bit pointers `GP`/`mtmp` (`[GP],y` stores, bank byte preset to `$E1`)
and long-indexed `f:SHRPIXL,x` stores.  The first cut left DBR=$E1 and put
all WM state in slow Mega-II RAM — self-consistent but a real hardware
speed regression; the pointer approach keeps the WM fast.

**Harness M1 additions:** a `wdm #$02` at the top of the main loop is the
deterministic **frame marker** (a 2-byte NOP on real silicon); the harness
steps frame-by-frame (`boot`/`frames`), injects keys (`$C000`), and feeds a
signed-delta **ADB mouse FIFO** (`$C024` data, `$C027` status: bit7 pending,
bit0 button) with `move_to`/`click`.  A text **script runner** (`run_script`:
`boot/wait/key/move/click/shot`) mirrors the apple2 rig.  Clock ticks off
`v_frame` (60 frames/s; a true `$C019` vblank gate is the hardware-pass
refinement — noted, not yet wired).

**Cell model:** windows/icons use 8×8-px cell coordinates; `fill_cells`,
`draw_str`, `draw_char` convert to SHR pixel-byte coords (cx*4 bytes, cy*8
rows) and call `fill_band`/`render_glyph`.  Attributes map to (fg,bg) pairs
(`attr_fg/attr_bg`): NORM white-on-blue, INV blue-on-white (title bars/menu),
ACC cyan-on-blue, KEY white-on-deepblue.

**Next: M2** — `smartport.i` (blk_read/write reusing the boot ProDOS driver
entry), FAT12 (reuse `macplus/mkfs.py` + a 65816 fat12 core), Files +
Notepad, disk-loaded app ABI.  The harness ProDOS driver already does
WRITE + `--writeback`.

## M0 — SHIPPED (2026-06-15, build 411)

`./build.sh` produces `build/unodos_iigs.po` (800 KB ProDOS-order image)
that boots through the firmware block driver to a **Super Hi-Res splash**
(`shots/m0.png`): blue desktop, grey menu bar "UnoDOS 3 / Apple IIGS",
and a framed cyan window with a deep-blue title bar — all in 16-colour
SHR, rendered by a 4bpp text engine that expands the shared 8×8 font.

**Harness verdict (the M0 wildcard, resolved): path 1 — a from-scratch
Python 65816 core.** No usable `py65816` exists and MAME/a IIGS ROM are
not available here, so `cpu65816.py` is a new functionally-correct (not
cycle-accurate) 65C816 interpreter: native + emulation modes, M/X width
tracking, the full addressing-mode/opcode set, MVN/MVP, and a **WDM trap**
the harness uses to play firmware. It self-tests (`python cpu65816.py` →
`SELFTEST OK`). `harness.py` is the ROM-free rig built on it — block-0
autoload, the ProDOS block driver (read **and** write, `--writeback` for
M2), the `$C0xx` soft-switch page, and SHR→PNG. This is fully CI-able with
zero ROM dependency, same as `apple2/harness.py`. GSplus/KEGS/MAME remain
the by-hand validation rigs (they need the user's ROM); real hardware is
FloppyEmu in SmartPort mode.

**Boot facts VERIFIED in the harness (§2b confirmed):**
- block 0 → `$00:0800`, sig byte `$01`, enter `$0801` in **emulation
  mode**, `X = slot<<4` (we boot slot 5, the 3.5" SmartPort).
- ProDOS driver entry = `$Cn00 + [$CnFF]`; call ABI `$42`=cmd (1=read,
  2=write), `$43`=unit, `$44/45`=buffer (bank 0), `$46/47`=block; carry
  set = error. `boot.s` drives exactly this, so it runs against real
  firmware and the harness WDM stub identically.
- **NEWVIDEO `$C029` = `$C1`** enables SHR (bit7) — confirmed; the
  harness asserts bit7 before rendering.
- Kernel **load address `$00:2000`**, runs native with 16-bit X/Y +
  8-bit A, **DBR=`$E1`** while drawing; SHR layout exactly as §1 states
  (pixels `$E1:2000`, SCB `$E1:9D00`, palette `$E1:9E00`, 160 B/row,
  high-nibble = left pixel, `$0RGB` little-endian).

**Files:** `cpu65816.py` (CPU), `harness.py` (rig + PNG), `boot.s`
(block-0 stage), `kernel.s` (M0 kernel), `mkdata.py`→`gen_data.inc`
(font + palette), `mkdsk.py` (image packer), `boot.cfg`/`kernel.cfg`
(ld65), `build.sh`, `tests/m0.py` (regression: `M0 PASS`). Toolchain:
cc65 `ca65 --cpu 65816` + `ld65` at `C:\Users\arin\snes-tools\bin`
(shared with the SNES port). **Gotcha banked here:** a `.include` of
generated data BEFORE the first `.segment` lands the data at the CODE
origin `$2000`, so the boot `jmp $2000` hits font bytes — keep emitted
data in an explicit `.segment "RODATA"`. Long branches in the glyph
loop overflow ±127 (4 macro expansions): use `beq +/jmp`.

**Next: M1** — SHR desktop + WM + ADB mouse/keyboard + SysInfo/Clock
(see §3). The renderer (`fill_band`, `draw_char`, `draw_string`,
`set_color`, `set_pos`, `calc_gp`) and `calc_gp`'s y*160 are the M1
primitives; add `$C000/$C010` keyboard polling and the mouse path to
the harness, plus a `wait/shot/key/mouse` script runner over `Harness`.

---

# Original plan (M0–M3)

Audience: the engineer building the UnoDOS/IIGS port, milestone by
milestone (M0–M3). Authoritative direction:
[../docs/PORTS-PLAN.md](../docs/PORTS-PLAN.md) §2; the platform
contract is [../docs/PORT-SPEC.md](../docs/PORT-SPEC.md). Work one
milestone at a time, land each as **code + regression script +
screenshots + commit + README update** (the house method), and update
this file + PORTS-PLAN when a milestone closes. Do not start M(n+1)
in the M(n) commit.

**The big picture:** this port is the macplus story replayed on a new
CPU — firmware bootstraps us, firmware gives us block I/O, firmware
maintains input state we mirror. Read `macplus/README.md` first: the
boot-chain/ROM-assisted/harness architecture there is the template.
The 65816 toolchain this port creates is reused by the SNES port
(`../snes/HANDOFF.md`), so keep the assembler setup clean and
documented.

---

## 1. Envelope (engineering facts)

- **CPU:** 65C816 @ 2.8 MHz, native 16-bit mode after boot (boot
  firmware hands control in 6502 emulation mode — your boot stage does
  `clc / xce`). RAM 256 KB–8 MB; design for the 1 MB floor and detect
  more.
- **Video:** Super Hi-Res 320×200, 16 colors per scanline from 4096
  (4 bits/pixel, 2 px/byte, **160 bytes/row, high nibble = left
  pixel**). Pixel data `$E1:2000–$9CFF` (32,000 bytes), scanline
  control bytes (SCBs) `$E1:9D00`, palettes `$E1:9E00–$9FFF` (16
  palettes × 16 colors × 2 bytes, `$0RGB` little-endian). Enable SHR
  via the NEWVIDEO register `$C029` (set bit 7; commonly `$C1` is
  written — **verify the exact value in the KEGS/GSplus source at
  M0** and record it here).
- **Input:** ADB keyboard + mouse with FIRMWARE support. The keyboard
  arrives through the classic `$C000`/`$C010` latch — same protocol
  the Apple II port polls. The mouse is maintained by the ADB
  microcontroller; deltas/button are readable from the ADB GLU
  (`$C024` mouse data, `$C027` status — exact bit semantics to be
  verified at M1 against the IIGS Hardware Reference / emulator
  source; the fallback is firmware Event Manager mirroring, the
  macplus ROM-assisted pattern).
- **Sound:** Ensoniq DOC, 32 oscillators, 64 KB dedicated sound RAM,
  accessed through the sound GLU (`$C03C` control, `$C03D` data,
  `$C03E/$C03F` address). M3 only.
- **Storage:** 800 KB 3.5" disks (1600 × 512-byte blocks) behind
  **SmartPort/ProDOS block firmware** — a true ".Sony equivalent".
  No GCR hand-rolling on this port; the firmware is the disk driver.
- **Timing:** the VGC provides vblank state (readable at `$C019`) and
  a one-second interrupt; M1 can poll `$C019` for a real 60 Hz tick —
  no soft-clock hacks needed (unlike the Apple II).

## 2. M0 — toolchain + boot PoC + harness decision

**Goal:** an 800 KB disk image that boots in an emulator to an SHR
splash (solid color + a drawn pattern is enough), produced by a
scripted build, plus a *written decision* on the test rig. Everything
else hangs off these three.

### 2a. Toolchain

cc65 suite: `ca65 --cpu 65816` + `ld65` (Windows binaries available;
this also gives the Apple II port a second assembler option). Set up
`build.sh` from day one (mirror `macplus/build.sh`): assemble boot
stage and kernel separately, pack with a new `mkdsk.py`. Keep the
ld65 config minimal (plain binary output segments at fixed origins).

### 2b. Boot contract (verify every byte of this at M0)

ProDOS-style block boot, to confirm in the emulator before building
on it:

1. Firmware scans slots, reads **block 0** of the boot device to
   `$0800`, checks the signature (first byte `$01`), and jumps to
   `$0801` with `X = $n0` (slot × 16) in 6502 emulation mode.
2. Your 512-byte boot stage locates the slot firmware's ProDOS block
   driver: entry = `$Cn00 + the byte at $CnFF`. Call protocol
   (ProDOS 8 device driver): zero page `$42` = command (1 = READ),
   `$43` = unit (`$n0`, bit 7 = drive 2), `$44/$45` = buffer pointer,
   `$46/$47` = block number; JSR entry; carry set = error.
3. Boot stage reads the kernel image from blocks 1..K to a fixed
   load address, switches native mode (`clc/xce`, `rep #$30`),
   enables SHR, and jumps to the kernel entry.
4. `mkdsk.py`: 819,200-byte image; block 0 = boot stage (assert
   signature + size), kernel from block 1, patch a KBLOCKS
   placeholder in the boot stage exactly like `apple2/mkdsk.py`
   patches `ktpatch` (assert unique byte pattern), assert the kernel
   fits its address budget.

Pick the kernel load address at M0 and document it here (suggest:
bank 0 high, e.g. `$00:6000+`, or bank 1 with shadowing — bank 0
keeps the boot-stage copy loop trivial while the image is small;
revisit if the kernel outgrows bank 0's free space).

**SHR writes:** for M1 simplicity write pixels directly to bank `$E1`
(long addressing, `sta $E12000,x` style with the data-bank or
absolute-long forms). It is slower (Mega II side) than shadowed
bank-01 writes — fine for M1; revisit for M3 games if repaints drag.

### 2c. Harness decision (write the verdict into this file)

The plan offers two paths; evaluate in this order, timeboxed:

1. **ROM-free Python harness** (the house pattern, like
   `apple2/harness.py`): needs a Python 65816 core. Investigate
   whether a usable one exists (`py65816` or similar — quality
   unverified; gate on running a known 65816 test suite). The harness
   plays the firmware: block-0 autoload, a fake `$Cn00` ProDOS driver
   entry that traps into Python and serves blocks from the image,
   `$C000/$C010` keyboard, seeded mouse registers, SHR → PNG
   (160 bytes/row, high-nibble-left, palette 0 from `$E1:9E00`).
   If a solid core exists this is the best outcome — same scripting
   (`wait/shot/key/mouse`), no ROM dependency, CI-able.
2. **Scriptable emulator** (the Genesis/BlastEm pattern): MAME's
   `apple2gs` driver has full Lua scripting (screenshots, input
   injection, memory reads) but needs a IIGS ROM image the user must
   supply; GSplus/KEGS need the ROM too and script far less. If path
   1 fails its timebox, MAME+Lua is the recommended rig; keep the
   script-command surface identical (`wait/shot/key/...`) so tests
   read the same.

Either way, **GSplus or KEGS remains the by-hand validation rig**
(the AppleWin role), and real hardware is FloppyEmu in SmartPort mode
(plan §2).

**M0 exit criteria:** `build.sh` produces `unodos_iigs.po`; it boots
to the SHR splash in the chosen emulator; the harness (whichever
path) can boot the same image headlessly and emit a splash PNG; the
decision + verified boot facts are written into this file; commit.

## 3. M1 — SHR desktop + WM + mouse/keyboard + SysInfo/Clock

The macplus M1 surface, in color, with a real pointer from day one.

- **Kernel skeleton:** UDM1 discovery header at the load address
  (`jmp start2`, magic `"UDM1"`, vars pointer — the convention from
  `apple2/HANDOFF.md` §5 / macplus), vars block with documented
  offsets (`frame_ctr`, `mouse_x/y`, `sel_icon`, `zcount`,
  `clock_secs`, `top_win`), BSS clear, AUTOTEST build switch.
- **Palette/theme:** SHR palette 0 entries 0–3 = the UnoDOS UI colors
  (blue `$00A`, cyan `$0AA`, magenta `$A0A`, white `$FFF` in `$0RGB`)
  per PORT-SPEC §1 — entries 4+ free for app/game content later.
- **Renderer:** 4bpp primitives — fill_rect, frame_rect, draw_char /
  draw_string (the shared 8×8 font via a `mkfont.py`-style generator
  from `amiga/gen_data.i`; 2 px/byte means glyph rendering writes 4
  bytes/row; x-coordinates need no cell alignment but byte-aligned
  fast paths are worth it), desktop pattern, menu bar, icons (pull
  icon art from the x86 `.BIN` headers the way `genesis/mkdata.py`
  does).
- **Window manager:** the full PORT-SPEC §2 contract — 16-window
  table, z 0–15 topmost=15, raise/destroy renormalization invariants,
  topmost-only content drawing, title-bar drag with XOR outline,
  close box, drag clamps. Port the macplus `kernel.asm` structure
  (its WM is the proven 68K expression of the same spec).
- **Input:** keyboard from `$C000`/`$C010` (poll per tick, translate
  to the canonical UnoDOS raw codes — arrows `$4C–$4F`, the 68K-family
  convention, so app key handlers stay portable). Mouse per the M0/M1
  verification: ADB GLU registers if they pan out, else firmware
  Event Manager mirroring. Apply PORT-SPEC §3 law: press-time latch +
  sequence counter, edge-only button events, hit-test the latch.
- **Apps:** SysInfo (IIGS detect: `sec / jsr $FE1F`, carry clear ⇒
  IIGS, plus ROM version; show CPU/RAM/machine) and Clock (vblank
  ticks via `$C019` polling; show HH:MM:SS uptime).
- **Harness/tests:** `tests/m1.script` mirroring macplus m1 — boot,
  desktop shot, icon select via keys AND a mouse click path
  (`mouse x y` / `click` script commands), open/drag/close a window,
  clock advance shot.

**M1 exit criteria:** script passes headlessly; screenshots show the
color desktop; by-hand boot in GSplus/KEGS matches; commit + README.

## 4. M2 — storage: FAT12 + Files/Notepad + disk-loaded apps

The macplus M2 shape with SmartPort instead of .Sony:

- **`smartport.i`** (the `macplus/sony.i` analogue): `blk_read(lba,
  dest)` / `blk_write(lba, src)` over the firmware ProDOS driver
  already used at boot (same `$42–$47` protocol; keep a copy of the
  entry address from boot). Volume sector N = disk block
  `FS_START_BLOCK + N`; pick `FS_START_BLOCK` past the kernel image
  (macplus uses 256 ⇒ 128 KB reserved; same number works here) and
  keep it in sync with `mkfs.py`.
- **FAT12:** the on-disk layout is the x86 one (PORT-SPEC §5) — reuse
  `macplus/mkfs.py` (parameterize offsets) to build the volume, and
  write the 65816 FAT12 core mirroring `macplus/fat12.i` /
  `amiga/fat12.i` (CPU-portable logic; all on-disk fields
  little-endian, which the 65816 is natively — simpler than the 68K
  version). Geometry constants defined ONCE (PORT-SPEC §6 rule 8);
  read cluster runs, not single sectors.
- **Files + Notepad:** port behavior from macplus (list root dir,
  open-in-Notepad; caret editing, live Ln/Col status, save =
  real `blk_write` path). Full mouse + keyboard.
- **Disk-loaded apps:** the macplus ksys-table ABI, 65816 flavor
  (`macplus/diskapp.i` is the template): launcher reads an app image
  from the FAT12 volume to a fixed load address, calls entry with a
  draw/key selector and a pointer to a kernel jump table (pass it in
  a register or direct-page slot — document the chosen ABI here);
  app is position-independent. Port `demo_app` as the proof.
- **Tests:** m2 script = mount, list, open, edit, save, close,
  reopen-and-verify, then load + drive the disk app. The harness's
  Python ProDOS driver must support **write** for this (persist with
  a `--writeback` flag, like the Apple II M2 rig).

## 5. M3 — parity: color apps + Ensoniq + scheduler

- **Games/Paint/Theme:** Dostris, OutLast, Pac-Man, Paint from the
  shared logic (`macplus/games.i`, `pacman.i`, `paint.i` for
  structure; the Amiga/Genesis color schemes for palettes — 16-color
  palettes make these straight ports, no 1-bit adaptation). Theme =
  the 8 shared presets (PORT-SPEC §1) rewriting the themed palette
  entries live, plus per-channel RGB editing (4-bit channels).
- **Ensoniq audio:** the richest sound chip in the family. Engine:
  load square(ish) wavetables into DOC RAM, map Music + Tracker
  channels to oscillators (Tracker: 4 channels → 4 oscillators —
  byte-identical pattern format, real polyphony at last; Music: the
  shared Canon in D table). Define the DOC register init from the
  Hardware Reference / KEGS source at implementation time; gate the
  whole engine behind one `snd_*` interface like `macplus/snd.i`.
- **Scheduler:** port `macplus/scheduler.i` semantics (per-window
  tasks, private stacks, one-slot mailboxes, bounded yield-retry).
  The 65816 makes this easy — 16-bit SP, stacks anywhere in bank 0;
  size per-task stacks 2 KB like the 68K ports.
- **Tests/README:** m3 script per app (the genesis/macplus m3
  scripts are the shape); README gets the full parity table.

## 6. Risks & open questions

- **Boot/firmware facts (§2b) are stated from documentation, not yet
  verified in this repo** — M0's first job is proving them in the
  emulator and correcting this file. Same for `$C029` and the mouse
  registers (`$C024/$C027`).
- **Python 65816 core quality** is the M0 wildcard; timebox the
  evaluation and fall back to MAME+Lua without ceremony.
- **ROM dependency:** every real IIGS emulator needs a ROM image the
  user supplies. The ROM-free harness path avoids it for CI; flag to
  the user when the by-hand emulator pass needs the ROM.
- **Bank discipline:** 65816 bugs cluster around DBR/D register state
  at interrupt and across long jumps. Establish the register-state
  convention (what DBR/D/M/X flags mean "kernel normal") in a comment
  block at the top of kernel.s and assert it at entry points.
- **FloppyEmu SmartPort mode** is the real-hardware target (plan §2);
  confirm the user's unit supports IIGS SmartPort 800K when the
  metal pass approaches.
