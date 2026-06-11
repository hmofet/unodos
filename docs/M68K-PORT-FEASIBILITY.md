# UnoDOS on Motorola 68K — Feasibility Report & Porting Plan

**Date:** 2026-06-11 · **Baseline:** UnoDOS v3.26.0, Build 405 (8086-clean)
**Targets assessed:** Commodore Amiga (OCS/ECS, 68000), Apple Macintosh
Classic-era (128K/512K/Plus/SE/Classic, 68000), Sega Genesis / Mega Drive
(68000 + Z80).

---

## 1. Executive summary

| Target | Feasibility | Fidelity achievable | Relative effort | Recommended? |
|---|---|---|---|---|
| **Amiga (OCS, 68000)** | **High** | Full GUI OS, floppy boot, FAT12 disks readable | 1.0× (baseline) | **Yes — first target** |
| **Mac Classic (68000)** | **High** (Toolbox-based — decision 2026-06-11) | Full GUI in 1-bit mono at 512×342; boots as the startup app, ROM Toolbox drives disk/input | ~0.5× | Yes — second target |
| **Sega Genesis** | Low–Medium | Tech-demo GUI; no filesystem, no keyboard; cartridge "apps" | ~1.3× (reduced scope) | Only as a showpiece |

**The one-sentence verdict:** a 68K port is feasible and would be a
*rewrite against a ported design*, not a translation of code — none of the
current x86 source survives mechanically, but the OS's *architecture*
(API table, event queue, window manager, cooperative task model, FAT12
layout) ports cleanly, and roughly 60–70% of a rewritten core can be shared
across all three targets if it is written once as a portable core with a
small hardware abstraction layer.

---

## 2. Why nothing translates mechanically

The 8088 pass (v3.26.0) was a *same-architecture* effort: 1,153 instruction
sites rewritten within one ISA family, with the BIOS, segmentation, and all
hardware semantics unchanged. A 68K port breaks every one of those
assumptions simultaneously:

1. **ISA**: 16-bit x86 real mode vs. 68000. Different register file
   (AX..DI vs. D0–D7/A0–A7), different flags semantics, no direct analog
   of segment registers. Automated x86→68K assembly translation at this
   codebase's level (segment:offset arithmetic used as a *memory
   allocator*, flag-carrying returns, self-referential `cs:` data) is not
   practical — every line would need human review anyway.
2. **Memory model**: UnoDOS *is built around* segmentation — app slots ARE
   64KB segments (0x3000–0x7000), the heap IS segment 0x8000, task
   isolation IS "your ES is your slot". 68K has a flat address space; the
   slot allocator becomes a flat-memory region allocator and every
   far-pointer convention in the API ABI changes.
3. **Firmware**: the kernel leans on the PC BIOS for video mode set
   (INT 10h), all disk I/O (INT 13h), PS/2 services (INT 15h C2xx), the
   tick counter (BIOS data area 0040:006C), keyboard translation (INT 9
   scancodes). Amiga/Mac/Genesis have no BIOS: Kickstart, the Mac Toolbox
   ROM, and the Genesis's nothing-at-all respectively. A bare-metal port
   re-implements all of it.
4. **Endianness**: 68K is big-endian. On-disk FAT12 structures, the .BIN
   header, window-table words, and every multi-byte API parameter are
   little-endian today. The FS code needs explicit byte-order accessors.
5. **Video**: UnoDOS's graphics core assumes a *packed chunky framebuffer*
   (CGA 2bpp interlaced banks, VGA 8bpp linear). Amiga is *planar*
   bitplanes + blitter/copper; Mac is 1-bit packed mono; Genesis is not a
   framebuffer at all (tile/sprite VDP). The entire graphics layer
   (~6,000 lines, the largest subsystem) is per-platform work.
6. **Apps are x86 assembly**: all 16 apps execute x86 machine code loaded
   from FAT12 .BIN files. They must be rewritten; the .BIN format and
   loader change (68K relocation or PC-relative-only code).

What *does* port — and is genuinely valuable:

- The **system-call surface** (105 documented APIs: windows, drawing,
  widgets, events, FS, tasks) — `docs/API_REFERENCE.md` is effectively the
  port's specification.
- The **window manager model** (16-slot table, z-order 0–15 with topmost
  cache, dirty-rect repaint, single-topmost clipping) and its hard-won
  invariants from the 2026-06 audit (z renormalization, focus-stamped key
  routing, press-time click latching, cursor protect bracket, deferred
  ISR work). These bugs were paid for once; the port inherits the fixes
  as *design rules*.
- The **event model** (32-entry queue, tombstones, edge-only mouse events,
  focus routing) and the **cooperative scheduler** semantics.
- **FAT12 logic** (mount/open/read/write/create, cluster-run batching) —
  algorithmically identical, only byte-order and the sector driver change.
- The widget toolkit's look/metrics, the 8×8/4×6 fonts (bit-for-bit), the
  desktop/launcher UX, and all app designs.

---

## 3. Per-target assessment

### 3.1 Amiga (OCS/ECS, 68000 @ 7.14 MHz) — HIGH feasibility

The Amiga is the natural first 68K home for UnoDOS: a flat-memory 68000
with chipset documentation that rivals the PC's, a hardware-bootable
floppy, and graphics hardware that *helps* rather than fights.

- **Boot**: trackdisk boot block (1KB at track 0) → custom loader, exactly
  analogous to boot.asm → stage2 → kernel. No OS needed; take over the
  machine (standard demo/game practice). Kickstart can be ignored after
  boot.
- **Video**: 320×200 NTSC (320×256 PAL) playfield. UnoDOS's default CGA
  mode is 320×200×4 — identical logical resolution, 2 bitplanes give
  exactly 4 colors *with a real palette* (better than CGA's fixed cyan/
  magenta/white). Cost: chunky→planar — the pixel-plot/fill/blit/text
  layer must think in bitplanes; the **blitter** accelerates fills, rect
  copies, and (with some care) font rendering well beyond what an 8088
  CGA ever could. The audit's `cga_row_table`/row-blit ideas map directly
  to per-bitplane row addressing.
- **Input**: mouse is two hardware quadrature counters (JOY0DAT) — *far
  simpler than PS/2*; keyboard arrives as a serial byte stream on CIA-A
  with one interrupt per key — maps 1:1 onto the existing INT 9 →
  focus-stamped event design.
- **Timer**: CIA timers or the vertical-blank interrupt (50/60 Hz) replace
  the 18.2 Hz tick; `boot_ticks`-relative API 63 semantics carry over.
- **Storage**: the floppy controller reads raw MFM tracks via DMA — a
  small MFM encoder/decoder is needed (well-documented; ~500 lines).
  Decision point: keep the UnoDOS 1.44MB→880KB FAT12 layout (PC-readable
  disks, reuse fat12.c logic verbatim) or use ADF-native layout. Keeping
  FAT12 on an 880KB DD disk is straightforward (different geometry
  constants — and Build 405's geometry code is already centralized).
- **Memory**: 512KB chip RAM baseline ≫ 640KB-class budget. App slots
  become 64KB flat regions; same 5-slot pool + shell layout.
- **Risks**: chunky→planar text rendering performance (mitigated by
  blitter + font pre-shifting); MFM floppy writing (Notepad save, MkBoot)
  is fiddlier than reading.

### 3.2 Macintosh Classic-era (68000 @ 8 MHz, 512×342×1) — HIGH
### (Toolbox-based port — DECIDED 2026-06-11)

**Strategy change:** instead of a bare-metal takeover, the Mac port
**leans on the ROM Toolbox deliberately**. The Mac's ROM is not a thin
BIOS — it is a complete, documented operating substrate (QuickDraw,
Event Manager, Device/File Manager, Sound, fonts, mouse/keyboard
drivers), and using it converts the port's three hardest problems
(floppy hardware, input hardware, pixel pipeline) into library calls:

- **Boot/identity**: UnoDOS ships as the **startup application** on its
  own System floppy (the `Finder` of its disk is UnoDOS). The machine
  still "boots into UnoDOS" from the user's point of view; the ROM +
  a minimal System file replace boot.asm/stage2. No INIT tricks needed.
- **Video**: one full-screen QuickDraw window (or direct screenBits
  access after HideMenuBar) at 512×342×1. UnoDOS's HAL maps onto
  QuickDraw primitives: fill = FillRect with a dither Pattern, blit =
  CopyBits, glyphs = our own 8×8 font via CopyBits (keeping UnoDOS's
  look) or direct framebuffer spans — 1-bit chunky is the simplest
  pixel format of any target. UnoDOS draws its OWN window chrome,
  widgets, cursor theme; the Mac Window Manager is *not* used (one
  GrafPort, our WM inside it).
- **Input**: Event Manager (`GetNextEvent`/`WaitNextEvent`) delivers
  keyboard and mouse with no hardware work at all — ADB vs. pre-ADB
  stops mattering. UnoDOS's focus-stamp/press-latch rules apply at
  translation into its own event queue.
- **Storage**: File Manager + HFS replace the FAT12 driver outright
  (PBRead/PBWrite/PBGetFInfo). Apps are 68K code resources/files on the
  same floppy; the .BIN header (name + 2bpp icon at 0x10) is kept inside
  the data fork for cross-platform identity, with the icon dithered to
  1-bit at load. Optional later: mount PC FAT12 floppies via the
  existing portable FAT12 core on the SuperDrive.
- **Timer**: TickCount() (60.15 Hz) → boot-relative ticks, exactly the
  API 63 model.
- **Hardware floor**: Mac Plus / System 6 (4MB ceiling, 1MB typical) —
  512K works if the core stays lean; SE/Classic ideal.
- **Costs of the approach**: ~100–200KB RAM ceded to System; cooperative
  scheduling must pump GetNextEvent (which UnoDOS's cooperative model
  already does naturally at its yield points); absolute purists lose
  "bare-metal" bragging rights. All acceptable.
- **Effort impact**: the bare-metal estimate was 6–8 weeks dominated by
  IWM/GCR floppy work; Toolbox-based, the Mac target shrinks to
  **~3 weeks** (theme pass + HAL-on-Toolbox + packaging), the smallest
  of the three.
- **Toolchain**: Retro68 (m68k gcc + MPW interfaces) or THINK C under
  emulation; Mini vMac / MAME `macplus` for CI.

### 3.3 Sega Genesis / Mega Drive (68000 @ 7.67 MHz + VDP) — LOW–MEDIUM

- **Video — the fundamental mismatch**: the VDP is tile- and sprite-based
  with no CPU-addressable framebuffer. A GUI needs arbitrary pixel
  drawing. The known workaround is a **pseudo-framebuffer**: fill a plane
  with 35,840 bytes of unique 8×8 tiles (320×224 ÷ 64 px × 32 B) inside
  the 64KB VRAM and treat tile memory as a chunky-ish (4bpp packed)
  surface updated via DMA during vblank. It works (homebrew Wolf3D-style
  engines prove it) but redraw bandwidth is capped (~7KB/frame of DMA at
  60 Hz NTSC), so full-screen repaints take multiple frames. Windows,
  text, and the cursor are fine; games like the OutLast ports would be
  rewritten against tiles/sprites instead (and would be *better* for it).
- **Input**: no keyboard exists in the standard ecosystem (the Mega
  Mouse is real but rare). A GUI OS without a keyboard loses Notepad,
  file naming, etc. Controller-as-keyboard (on-screen keyboard) is the
  realistic answer.
- **Storage**: no floppy, no writable FS. "Apps" become objects in the
  cartridge ROM image (the FAT12 layer is replaced by a ROM directory);
  saving requires cartridge SRAM (8–32KB typical). The OS-ness of UnoDOS
  (load programs from disk, save documents) mostly evaporates.
- **Verdict**: feasible as an impressive *demo cartridge* (desktop, mouse
  cursor via sprite — the easiest cursor of all three! — windows, a few
  built-in apps), not as a faithful port of an operating system.

---

## 4. Porting plan

### 4.0 Strategic decision: port the *design* in C, not the code in asm

Rewriting 21K lines of x86 assembly directly into three dialect-specific
68K assembly trees would triple the maintenance surface and re-pay the
audit's bug tax three times. The plan that converges:

> **Restructure UnoDOS as a portable C core + per-platform HAL + small
> per-platform assembly**, with the current x86 tree remaining the
> reference implementation for PC hardware.

- Core in C99 (freestanding, no libc beyond memcpy/memset): window
  manager, event queue, scheduler, widget toolkit, FAT12, desktop/launcher
  logic, API dispatch. Compiled per target.
- HAL (~15 functions): `video_init/setmode`, `plot/hline/fill/blit`,
  `text_blit_glyph`, `input_poll` (feeding the existing event design),
  `timer_ticks`, `disk_read/write_sectors`, `mem_layout`, `enter_app/
  exit_app`.
- Per-platform assembly: boot path, interrupt stubs, the inner pixel/blit
  loops where C is too slow (Amiga blitter setup, Mac 1-bit span code,
  Genesis VRAM DMA queue), and the syscall trap.
- **App ABI**: INT 0x80 becomes `TRAP #0` (the 68K-native analog).
  Register convention: D0=AH equivalent (function number) or D0/D1/D2/D3
  ↔ AX/BX/CX/DX, A0/A1 ↔ SI/DI. Apps are position-independent 68K
  binaries with the same 0x50-byte header (icon + name) — header stays
  little-endian for PC-disk compatibility, loader byte-swaps.

### 4.1 Toolchain & test rigs (all free, all CI-able)

| | Amiga | Mac Classic | Genesis |
|---|---|---|---|
| C compiler | vbcc or m68k-elf-gcc | Retro68 (gcc) or vbcc | SGDK (m68k-elf-gcc) |
| Assembler | vasm | vasm | vasm / SGDK |
| Emulator (CI) | WinUAE / FS-UAE (scriptable) | Mini vMac / MAME `macplus` | BlastEm / MAME `genesis` |
| Image tooling | adfcreate / custom (FAT12 ADF) | dc42 / 1.44 raw | ROM concat |
| Accuracy note | cycle-accurate OCS in WinUAE | Mini vMac is solid for 68000 | BlastEm is cycle-accurate |

The existing `tools/qemu_test.py` pattern (headless boot → scripted
input → screenshot assertions) ports to each emulator; FS-UAE and MAME
both expose scripting/screenshot hooks. Keep the same artifact-based
regression style that caught the Build-404/405 issues.

### 4.2 Phases and milestones

**Phase 0 — Specification freeze (1–2 weeks)**
Extract the port spec from the x86 tree: API semantics (API_REFERENCE.md
is 90% there — finish APIs 91–104), window/event/scheduler invariants
(the audit digest is the de-facto invariants document), FAT12 on-disk
constants, .BIN header, theme metrics, font data. Deliverable:
`docs/PORT-SPEC.md` + fonts exported as C arrays.

**Phase 1 — Portable core on a desktop harness (4–6 weeks)**
Write the C core against the HAL with an SDL2 (or plain Win32) harness
implementing the HAL on the dev machine: instant edit-run loop, the whole
WM/event/widget/FAT12 stack debuggable with real tooling. Port the
existing QEMU regression scripts to the harness. Exit criteria: desktop,
launcher, Notepad-equivalent, Clock running on the harness, FAT12 disk
images mountable, full regression suite green. *This harness is also the
permanent home for core regression tests.*

**Phase 2 — Amiga bring-up (4–6 weeks)**
1. Boot block + track loader + kernel load into chip RAM (1 wk)
2. Video HAL: 2-bitplane 320×200, palette, blitter fill/copy, glyph
   blitter path; cursor as a hardware sprite (free on Amiga) (2 wk)
3. CIA keyboard ISR + quadrature mouse + vblank timer (1 wk)
4. MFM floppy read, then write; FAT12-on-880KB geometry (1–2 wk)
5. Wire HAL to core, run the ported regression scripts under FS-UAE.
Milestone: **self-booting Amiga ADF with desktop + 3 apps.**

**Phase 3 — App ecosystem (3–4 weeks, overlaps Phase 2)**
Rewrite apps in C against the API (most are <1K lines of logic):
launcher (already core), SysInfo, Clock, Notepad, Files, Settings,
Music (Paula audio is a *huge upgrade* — 4-channel PCM vs PC speaker),
Mouse test. Games (Tetris/Pac-Man/OutLast) get per-platform render
backends.

**Phase 4 — Mac Classic target, Toolbox-based (~3 weeks)**
1-bit theme pass on the widget set (1 wk); HAL on QuickDraw/Event
Manager/File Manager + TickCount, UnoDOS as startup application (1 wk);
System-floppy packaging + 512×342 layout QA under Mini vMac (1 wk).
Milestone: **System 6 boot floppy that starts straight into the UnoDOS
desktop on a Mac Plus.**

**Phase 5 — Genesis demo target (6–8 weeks, optional)**
Pseudo-framebuffer plane + DMA queue (2 wk); sprite cursor + pad-driven
pointer + on-screen keyboard (2 wk); ROM-directory app loading + SRAM
"documents" (1 wk); scope-reduced desktop + 3 built-ins (2 wk).
Milestone: **demo cartridge ROM running the desktop at 60 Hz.**

### 4.3 Effort summary

| Workstream | Estimate (focused solo dev) |
|---|---|
| Phase 0 spec | 1–2 weeks |
| Phase 1 portable core + harness | 4–6 weeks |
| Phase 2 Amiga | 4–6 weeks |
| Phase 3 apps | 3–4 weeks |
| Phase 4 Mac (Toolbox-based) | ~3 weeks |
| Phase 5 Genesis (optional) | 6–8 weeks |
| **Amiga-only total** | **~3–4 months** |
| **All three** | **~5–6 months** |

### 4.4 Key design rules to carry over (paid for by the 2026-06 audit)

1. Cursor protect bracket must be atomic with the lock (cursor_protect_begin).
2. No drawing or device I/O in interrupt context — ISRs set dirty flags;
   task context syncs (mouse_cursor_sync pattern).
3. Stamp focus at event-source time, route at delivery time, discard
   stale (key + mouse focus stamps).
4. Latch press coordinates and a press sequence counter in the ISR;
   consumers hit-test the latch, never the live position.
5. Edge-only mouse event posting; motion is pollable state.
6. Z-order renormalization on every focus/destroy; single source of
   truth for topmost.
7. Validate loaded image sizes against the slot + stack frame; verify
   actual bytes read; reap file handles by owner on every kill path.
8. Centralize disk geometry constants once (the quintuplicated-constants
   lesson) and read sector runs, not single sectors.

---

## 5. Recommendation

Port to the **Amiga first**: it is the only target where the full identity
of UnoDOS — boots from a floppy into a windowed GUI on stock 1985-class
hardware — survives intact, and its hardware actively improves the product
(palette, blitter, sprite cursor, Paula audio). Build the portable C core
on a desktop harness first; it de-risks every subsequent target and gives
the x86 tree a regression-tested reference spec. Treat the Mac port as a
fast follow — with the Toolbox-based strategy its only real design work
is the 1-bit theme, since the ROM supplies disk, input, and the pixel
pipeline — and the Genesis as an optional showpiece once the core exists.
