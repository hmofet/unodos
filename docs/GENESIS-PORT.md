# UnoDOS/Genesis — port plan (DRAFT, awaiting approval)

Target: Sega Mega Drive / Genesis — 68000 @ 7.67 MHz (same CPU family as
the Amiga/Mac ports), VDP tile/sprite graphics, 64 KB work RAM, Z80 +
PSG/FM audio, two DE-9 control ports.

## Input: PS/2 keyboard + mouse on the two control ports

The Genesis has no keyboard, so per the plan we read **PS/2 devices
directly through the gameports**, bit-banged by the 68000. The I/O
controller exposes every port pin (D0–D3, TL, TR, TH) as programmable
I/O via the `$A10003/05` data and `$A10009/0B` control registers, and
**port 2's TH line can raise the level-2 EXT interrupt** — which is the
basis of the community-precedent wiring (HardWareMan on SpritesMind
attached a standard PS/2 keyboard "to MD via joystick port #2 (using
#EXT interrupt!)"; Sik's documentation of the official Sega/XBAND
keyboard protocol confirms port 2 + TH/TR handshaking as the keyboard
port convention).

Pin assignment (both ports wired identically; passive adapter — PS/2
devices have internal pull-ups, the MD inputs are pulled up too):

| DE-9 pin | MD signal | PS/2 signal | Notes |
|---|---|---|---|
| 1 | D0 ("Up") | **DATA** | bidirectional (host commands need OC emulation: drive low / float high via the CTRL register) |
| 5 | +5V | +5V | the port supplies power |
| 7 | TH | **CLOCK** | port 2: EXT/HL interrupt on transition → interrupt-driven keyboard; port 1: polled |
| 8 | GND | GND | |

- **Port 2 = keyboard**: CLK on TH gives an interrupt per PS/2 clock
  edge; the level-2 handler shifts in the 11-bit frames (start, 8 data,
  parity, stop) and feeds scancodes to the UnoDOS event queue with the
  PORT-SPEC focus stamp.
- **Port 1 = mouse**: TH on port 1 cannot interrupt, so the driver uses
  the PS/2 **host-inhibit** feature: hold CLK low except during a
  per-frame (vblank) window, then busy-clock pending bytes in. Stream
  mode at 40–100 samples/s decimates cleanly to a 50/60 Hz desktop.
- Fallback: the same driver structure can speak Sik's documented Sega
  keyboard protocol (4-bit bus) later, for XBAND/MegaNet-style hardware.

## Milestone 0 — boot PoC (DONE, pending emulator verification)

`genesis/boot.asm` (~32 KB ROM, vasm `-Fbin`, the same assembler as the
Amiga port): valid vector table + Sega header, TMSS handshake, Z80
quiesce, VDP init (H32, plane A at `$C000`), the shared UnoDOS 8×8 font
converted to 4bpp tiles by `genesis/mkfont.py`, UnoDOS palette in CRAM,
and the splash: "UnoDOS 3 / for Sega Genesis - Mega Drive" plus the
PS/2 wiring banner. Structurally validated (header layout, reset
vectors); needs an emulator run for visual sign-off.

## Proposed roadmap (for approval)

1. **M0 verification + harness** — pick an emulator (BlastEm: accurate,
   scriptable, headless-friendly; needs a ~2 MB download) and stand up
   the screenshot-verification loop used by the other ports.
2. **M1 — kernel core on VDP**: tile-based implementations of the
   PORT-SPEC drawing primitives (fill/char/string as name-table +
   tile writes; the desktop is naturally tile-friendly), vblank tick,
   event queue, splash → desktop with icons. Mouse cursor as a VDP
   sprite (hardware sprite, like the Amiga).
3. **M2 — input**: PS/2 keyboard driver on port 2 (EXT interrupt,
   scancode set 2 → UnoDOS keymap) and PS/2 mouse on port 1
   (inhibit/poll); 3/6-button pad as a navigation fallback (d-pad =
   arrows, A/B/C = Enter/Esc/menu) so the port is usable on stock
   hardware. Emulator story: BlastEm gamepad for the fallback path;
   PS/2 verification via a small protocol-injection test build, real
   hardware for the full loop.
4. **M3 — the app set**: SysInfo, Clock, Files (ROM-disk first),
   Notepad, Theme (CRAM palettes — 61 colors on screen, 512 colors,
   shadow/highlight as a stretch), the WM (windows as tile regions;
   plane B for the desktop, plane A for the focused window is the
   natural z-clip analogue of the single-topmost rule).
5. **M4 — games + sound**: Dostris/OutLast/Pac-Man (tile rendering
   fits them perfectly), PSG square-wave sequencer for the shared game
   songs (FM later), Tracker over PSG's 3 tones + noise.
6. **M5 — storage**: SRAM-backed save slots first (battery-backed
   cartridge RAM, `$200000` region, trivially supported by emulators
   and flashcarts), then optionally the Mega-CD/flashcart filesystem
   question. The portable FAT12 core from the Amiga is reusable if a
   storage device with sector access exists; otherwise SRAM is the
   honest Genesis-native answer.
7. **M6 — scheduler**: port the milestone-3 cooperative scheduler
   (it's plain 68000; the Amiga `scheduler.i` moves almost verbatim —
   only the stack arena and tick source change).

Notes/risks: VDP text is tile-quantized (windows snap to 8px), work RAM
is 64 KB total (the app model fits, but Notepad/Tracker buffers need
budgeting), and PS/2-over-gameport ultimately needs real-hardware
validation (emulators don't model PS/2 devices on the ports — the
driver will be structured against an injectable bit-stream interface so
the protocol logic is testable in CI regardless).

## Sources

- SpritesMind: "Megadrive/Genesis clone with a keyboard?" — HardWareMan
  on PS/2 keyboard via port 2 + EXT interrupt
  (gendev.spritesmind.net/forum/viewtopic.php?t=525)
- SpritesMind: "XBAND, Mega Net 2, and Mega Drive Keyboards" — Sik's
  notes on the official keyboard protocol (port 2, TH/TR handshake,
  4-bit bus) (gendev.spritesmind.net/forum/viewtopic.php?t=2556)
- ConsoleMods wiki: Genesis connector pinouts (DE-9: 5 = +5V, 8 = GND,
  7 = TH/select) (consolemods.org/wiki/Genesis:Connector_Pinouts)
