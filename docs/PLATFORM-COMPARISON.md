# UnoDOS platforms — the asm reference vs. the ports

UnoDOS exists as one **reference implementation** (x86 assembly) plus a family of
**ports** that re-implement the same operating system on very different hardware.
This document compares all of them: what each is built from, how it draws, how it
loads and dispatches apps, and what every port is contractually required to keep
identical. The contract itself lives in [PORT-SPEC.md](PORT-SPEC.md) — *"where
this file and the x86 source disagree, the source wins."*

## Three implementation families

Every target falls into one of three families, and most differences follow from
which family a platform is in:

- **Native x86 asm — the reference** (one codebase, two hardware tiers):
  x86 PC, x86 8088/XT.
- **Native asm per-CPU — bare-metal reimplementations** (each its own assembly,
  sharing no code): Amiga, MacPlus-OS, Genesis, Apple II, C64 (6510),
  SNES (65816), IIGS (65816), SMS (Z80), NES (6502/2A03). *(SMS and NES are the
  two ports built fresh on the 3.1 Contract — see below.)*
- **Portable C core — shared `unodos.c`** (one ~52 KB core + a thin per-platform
  backend): Mac System 7, Mac System 1–6, PS2, Dreamcast.

## All platforms

| Platform | CPU / RAM | Language & boot | Rendering substrate | App model (delivery → dispatch) | Storage | Status |
|---|---|---|---|---|---|---|
| **x86 PC** *(reference)* | 8086–486 / 256–640 KB | x86 asm, bare-metal real mode, BIOS + `INT 0x80` | Direct VGA video segment (CGA/EGA/VGA) | `.BIN` on FAT12 → `app_load`#18 / `app_run`#19, `INT 0x80` ABI; **windowed** | FAT12 floppy + FAT16 HD/CF | ✅ real hw (8088→486) |
| **x86 8088/XT** | 8088 @4.77 / 256–640 KB | *(same source)*, CGA, GLaBIOS | Direct CGA | *(same `INT 0x80` model)* | FAT12 floppy + CF (XT-IDE) | M0–M2+; MartyPC; ⏳ physical XT |
| **Amiga** | 68000 / 512 KB | 68K asm, bare-metal self-booting ADF | Copper / bitplanes (32-col), HW-sprite cursor | 9 `.APP` on FAT12 → fixed **API vector table @ `$77000`** by ordinal; **windowed** | FAT12 (DF1) | M3+; WinUAE; ⏳ A500 |
| **Mac System 7** | 68020+ / MB | **C core**, *hosted* on Mac Toolbox | 8-bit Color QuickDraw | app-free core + **`AppInterface` fn-ptr table**, 11 modules off FAT12 / CODE rsrc; **windowed** | FAT12 floppy | M3; Executor; ⏳ Mac II |
| **Mac System 1–6** | 68000 / MB | **C core**, Toolbox | 1-bit QuickDraw | *(same `AppInterface` model, minus color Theme)* | FAT12 floppy | M3; Executor; ⏳ Mac Plus |
| **MacPlus (OS)** | 68000 (+II 640×480) / MB | 68K asm, bare-metal own boot blocks | 1-bit dither SW renderer, SW cursor | 9 `.APP` on `.Sony` → 16 KB slots, multi-resident; **windowed** | FAT12 (.Sony) | M3; ✅ **real Mac SE** |
| **Sega Genesis** | 68000 / 64 KB | 68K asm, bare-metal **cartridge ROM** | VDP tile-cells, HW-sprite cursor | **built into ROM** (no writable code storage) → in-ROM table | SRAM / tape / Sega CD BRAM *(data)* | M6+; ⚠️ **boots on flashcart** |
| **Apple II** | 6502 @1 MHz / 48–64 KB | 6502 asm, bare-metal Disk II autoload | Hi-res 280×192 1-bit SW | 8 binaries via GCR RWTS → fixed `$6000`; **full-screen, one-at-a-time** | Own GCR mini-FS | M1–M3; py65; ⏳ AppleWin |
| **Sony PS2** | R5900 / 32 MB | **C core**, PS2SDK bare-metal | SW 640×448×32 fb → GS each vsync | `AppInterface`, 11 `.uno` from `mc0:`; **windowed** | Memory card (libmc) | M0–M3; PCSX2 @60 fps |
| **Sega Dreamcast** | SH-4 / 16 MB | **C core**, KallistiOS | SW 640×480×32 fb → PVR RGB565 each vblank | `AppInterface`, `.uno` from `/cd` (ISO9660); **windowed** | VMU | at parity; Flycast @60 fps |
| **Super Nintendo** | 65816 @3.58 / 128 KB | 65816 asm, bare-metal LoROM, shadow+DMA | WRAM tilemap shadow → VRAM DMA (NMI) | **built into ROM** → in-ROM table | Battery SRAM mini-FS | M0–M3; Mesen2 |
| **Apple IIGS** | 65C816 @2.8 / 256 KB–8 MB | 65816 asm, bare-metal ProDOS/SmartPort boot | 4 bpp Super Hi-Res SW | 8 `.APP` from FAT12/SmartPort → bank-0 slots, **JMP vectors**, multi-resident; **windowed** | FAT12 over SmartPort | M0–M3; py65816 |
| **Commodore 64** | 6510 @1 MHz / 64 KB | 6510 asm, bare-metal PRG (`SYS 2061`) | VIC-II hi-res bitmap, per-cell color | disk-loaded to `$5000` via `$DE00`, **`mkapi.py` addresses**; **full-screen, one-at-a-time** | USV1 byte-heap on `.d64` | M1–M3; py65 |
| **Sega Master System** *(3.1-fresh)* | Z80 @3.58 / 8 KB | Z80 asm, bare-metal cartridge, sjasmplus | VDP Mode-4 tile nametable, HW-sprite cursor | **built into ROM** → in-ROM table; **windowed** | *(none — cart ROM)* | M1–M3 + game + audio; BlastEm |
| **Nintendo NES** *(3.1-fresh)* | 6502/2A03 @1.79 / **2 KB** | 6502 asm, bare-metal iNES NROM, dasm | PPU tile nametable, patterns in CHR-ROM | **built into ROM** → directional launcher; **`minimal`: full-screen, one-at-a-time** | *(none yet)* | M1 launcher; Mesen2 |

The last two rows are the **3.1-fresh** ports — written from scratch against the
Contract (`unodef/`), not migrated. SMS adds a fourth bare-metal asm CPU family
(Z80) reusing `gen/z80/`; NES is the Contract's `minimal`-profile flagship (2 KB)
reusing the 6502 `gen/6502/` world.

Two orthogonal axes cut across the families and explain most of the remaining
variation:

- **Rendering substrate** — *direct* hardware drawing (x86 VGA segment, Amiga
  bitplanes, Genesis/SNES tile cells) vs. a *software framebuffer* blitted each
  frame (PS2→GS, DC→PVR; the substrate [`unoui`](UNOUI.md) / [`uno3d`](UNO3D.md)
  sit on) vs. *hosted* on an existing OS's drawing (Mac Toolbox QuickDraw).
- **Windowing** — *windowed multitasking* (several apps resident) on the roomy
  machines vs. *full-screen, one app at a time* on the RAM-tight 8-bits
  (C64, Apple II), where only one app region fits in memory.

---

## Drill-down 1 — how an app is loaded and dispatched

This is the most illuminating difference, because all three families converged on
the *same goal* — **"the kernel/core holds no app code; apps are separate units
behind a stable interface"** — but realize it three different ways dictated by the
hardware.

### Model A — raw binary + a fixed call gate (the asm ports)

An app is a position-fixed binary blob with *no linker relationship* to the
kernel. It is loaded to a **fixed address region** and reaches the kernel through
a **call gate that never moves**, so the kernel can be rebuilt/shrunk and the
on-disk apps still work:

- **x86** — software interrupt `INT 0x80` (function # in a register).
  `app_load`#18 reads the `.BIN` off FAT12; `app_run`#19 enters it.
- **Amiga** — a fixed **jump table at `$77000`**; the app calls kernel routines
  by ordinal.
- **IIGS** — apps dispatched through their loaded **JMP vectors**; the kernel
  reached by long calls into bank 0.
- **C64** — the **`$DE00` loader port** plus kernel entry addresses that
  `mkapi.py` extracts into `kernel_api.inc`; the app is assembled against those
  fixed addresses.
- **Apple II** — the GCR **RWTS** loads the binary to a fixed `$6000` region.

Because the boundary is a stable ABI (interrupt / jump table / fixed addresses),
this is exactly what let every asm port move apps *out* of the kernel — hence the
dramatic shrink: C64 −56 %, MacPlus −46 %, Amiga −25 %, plus IIGS and Apple II.

### Model B — a function-pointer table (the C ports)

The shared `unodos.c` core used to dispatch apps with a compile-time
`switch(proc)` — every app compiled *into* the core. That was refactored into a
runtime **`AppInterface` table**: each app exports a small struct of function
pointers (init/draw/event/…), the core calls through the table, and apps become
separately loadable modules — `dlopen` on the host, `.uno` modules from
`mc0:/UnoDOS/Apps/` (PS2) or `/cd/UNODOS/APPS/` ISO9660 (DC), or a CODE
resource / FAT12 module (Mac). Same decoupling as Model A; the call gate is a C
function pointer instead of an interrupt or jump table.

### Model C — compiled into ROM (the cartridge consoles)

Genesis and SNES: cartridge ROM *is* the delivery medium and there is no
removable/writable code storage, so apps stay built into the ROM image and
dispatch through an in-ROM table. No load step — but the same app set and the same
logical dispatch.

> It is *one architecture* ("app-free kernel/core + apps behind a stable
> interface") expressed through whatever each machine makes natural.

---

## Drill-down 2 — what a "port" actually has to implement (PORT-SPEC)

[PORT-SPEC.md](PORT-SPEC.md) is the platform-independent contract **extracted
from the x86 source**. A port re-implements its six sections against the target
hardware, in whatever language that hardware wants:

1. **Identity & UX** — boot straight into a windowed desktop, GUI-first, default
   320×200 4-color palette with *exact* RGB (`#0000AA / #00AAAA / #AA00AA /
   #FFFFFF`); palette machines match those RGBs, mono machines map by luminance.
2. **Window manager** — z-order, drag, click-to-raise, and a content-area drawing
   context (the app draws at `(0,0)` = content top-left; the port translates +
   clips), plus title/close chrome.
3. **Events & input** — the event-queue model (`KEY_PRESS / MOUSE / WIN_MOVED /
   WIN_REDRAW`), focused-task-only keys, native input mapped into it.
4. **Tasking & app model** — cooperative scheduling, the app load+dispatch of
   Drill-down 1, per-app regions, and the API the app calls.
5. **Filesystem** — a unified FS API and **shared on-disk formats** (FAT12
   interchange where possible; the mini-FS formats), so *data* files are
   byte-identical across machines.
6. **Design rules** — the hard-won "audit tax" invariants (cursor XOR
   protection, coordinate translation, etc.).

For the **asm ports**, sections 1–4 are written directly in the kernel for that
CPU. For the **C ports**, they live in the shared `unodos.c` core, and a port
supplies only a thin backend (give input as events; put a framebuffer on screen).
The [`unoui`](UNOUI.md) and [`uno3d`](UNO3D.md) libraries are precisely the
*C-side embodiment* of sections 1–3 for those ports — the reusable pieces the asm
kernel provides natively.

---

## What stays identical across all 13 targets

Regardless of language or rendering substrate, three things are invariant — and
they are the working definition of "a port" in UnoDOS:

- The **UX** — GUI-first, boots to the same windowed desktop with the same
  default palette.
- The **app set** — the same ~11 apps + 3 games on every machine.
- The **on-disk data formats** — the Tracker song format is byte-identical
  everywhere, the Theme palette presets, FAT12 interchange.

So a "port" is *same behavior and same formats, re-implemented natively per
machine*. That is the macro-scale version of the same "write once, swap the
platform-specific layer" pattern that `uno3d` (swap the rasteriser backend) and
`unoui` (swap the theme + feed the event stream) apply at the library scale.

## See also

- [PORT-SPEC.md](PORT-SPEC.md) — the platform-independent contract
- [ARCHITECTURE.md](ARCHITECTURE.md) — x86 boot process, memory map, segments
- [FEATURE-MATRIX.md](FEATURE-MATRIX.md) — per-port maturity & feature coverage
- [UNOUI.md](UNOUI.md), [UNO3D.md](UNO3D.md) — the two write-once C libraries
