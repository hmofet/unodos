# UnoDOS/Genesis — port plan (approved 2026-06-11; M0-M2 + M4/M4.5 shipped)

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

## Milestone 0 — boot PoC (DONE, verified in BlastEm 2026-06-11)

Boot PoC: vector table + Sega header, TMSS handshake, Z80 quiesce, VDP
init, the shared UnoDOS 8×8 font as 4bpp tiles, UnoDOS palette in CRAM,
splash + PS/2 wiring banner. Superseded by `genesis/kernel.asm` (M1);
the original `genesis/boot.asm` lives in git history. Bring-up trap
for the record: the M0 vector table was off by two entries (22 err
vectors are needed between the reset PC and the spurious-interrupt
vector) — harmless until M1 enabled interrupts, then the first vblank
jumped into the error loop.

## Milestone 1 — desktop, input, Notepad + Music (DONE 2026-06-11)

`genesis/kernel.asm` + `softkbd.i` + `ps2.i` + `apps.i`, 64 KB ROM,
verified in BlastEm 0.6.2 (`genesis/README.md` has the build/test
matrix and the verified-behavior list):

- **Kernel core on VDP** (was roadmap item 2): cell-based drawing
  primitives on plane A (40×28 cells, H40), 4 palette lines = the 4 UI
  attribute schemes from the theme colors, vblank tick + event queue +
  press-time click latch per PORT-SPEC §3/§6, hardware-sprite cursor,
  desktop with the real x86 app icons, window manager with drag /
  raise / close / z-order (windows snap to the 8 px cell grid).
- **Pad-as-mouse** (roadmap item 3, reshaped per direction): d-pad
  moves the cursor with held-time acceleration (Z = turbo), A = click,
  B = soft keyboard, C/Start/X/Y = Enter/Esc/Backspace/Space. Works on
  3-button pads (no X/Y/Z extras).
- **Soft keyboard**: kernel overlay on the bottom 6 cell rows — full
  QWERTY + sticky Shift + F1 + arrows + Esc, hover highlight, posts
  through the shared event queue with the Amiga-port raw codes.
- **PS/2 drivers wired** (real-hardware-only): port 2 keyboard on the
  EXT level-2 interrupt (11-bit frame assembler + scancode set 2
  decode incl. shift/break/E0), port 1 mouse via host-inhibit with a
  per-vblank receive window + boot-time `$F4` probe (no ACK → pad
  mode). The protocol engines are injectable pure routines; the
  AUTOTEST_PS2 build verifies the full decode path in the emulator.
- **Apps**: SysInfo, Clock, Notepad (2 KB buffer, caret, line nav with
  goal column, vertical scroll clamp, status bar), Music (PSG ch-0
  square-wave sequencer, the shared Canon in D arrangement at 60 Hz,
  staff view with live note highlight).

Port-specific traps for the next milestones: acknowledge the vblank
interrupt with a VDP status read (control-port writes must stay single
`move.l`s so the read can't split one); `lsl.w #7` of a negative cell
row flips the control word's top bits into a CRAM write — the
PROBE_GUARD build traps out-of-range cell coordinates and renders the
caller PC in hex; BlastEm's debugger doesn't engage on piped stdin, so
on-screen hex forensics is the workable equivalent.

## Milestone 2 — the game ports (DONE 2026-06-11)

`genesis/games.i` + `genesis/pacman.i`: Dostris, OutLast and Pac-Man
with the same piece tables / track table / maze / ghost AI / scoring
as the x86 originals (donor: the Amiga ports), rescaled 50→60 Hz.
Rendering is cell-based through `gcol`, a 32-entry map from the
Amiga's extended-palette indexes to (attr|solid-tile) words on palette
lines 2/3 — note the channel swap, Amiga `$0RGB` vs Genesis `$0BGR`.
Pac-Man's actors are hardware sprites (1-4, chained after the cursor),
so the maze never repaints under them — only eaten-dot cells redraw.
Game music is the shared Korobeiniki / Sunset Drive on PSG channel 1
(`gm_*`, mutes when the owning game loses topmost). A game window
topmost flips the pad into game mode: d-pad = arrows with hold-repeat,
A = Space, X = 'n', Y = 'p'. Verified in BlastEm via the dostris /
outlast / pacman AUTOTEST builds.

## Milestones 4 + 4.5 — storage (DONE 2026-06-11)

Full design + tier specs in **docs/GENESIS-STORAGE.md**.

- **M4 SRAM** (`genesis/sram.i`): 8KB battery SRAM (header-declared,
  odd bytes at `$200001`), USV1 mini-filesystem (8 files, save-by-name
  overwrite, delete-compaction), the **Files app** (proc 7), and
  Notepad **F1-save**. Verified in BlastEm (AUTOTEST_SRAM round trip).
  Traps: index SRAM with `(a0,d0.w)` (a stale high word in a .l index
  reaches random buses), and write `$A130F1 = 1` once at boot — the
  per-access on/off dance unmapped SRAM for good under BlastEm.
- **M4.5 tape/WAV** (`genesis/tape.i` + `genesis/mktape.py`): KCS
  1200-baud AFSK; the PSG writes (zero hardware — record the
  headphone jack), a one-comparator adapter on port 2 pin 1 reads.
  Poll-count timebase (no timer needed). The decoder is injectable
  and emulator-verified (AUTOTEST_TAPE); mktape.py encodes/decodes
  WAVs on the PC side (selftest + 2KB WAV round trip pass) so the PC
  is the tape deck. Files: `w`/`r` keys.

## Milestones 3 + 5 + 6 — parity with the Amiga port (DONE 2026-06-12)

- **M3 Theme + Tracker** (`genesis/theme.i`, `genesis/tracker.i`):
  the 8 shared presets restyle the themed entries of all four CRAM
  palette lines (game colors stay fixed), with 3-bit per-channel
  custom editing; the Tracker is the Amiga pattern editor on the PSG
  (3 squares + noise, byte-identical pattern format, SONG.TRK to
  SRAM via s/l, tape via t/y). Desktop grows to 10 icons.
- **M5 Sega CD backup RAM** (`genesis/bram.i`): Mode-1 Sub-CPU boot
  (Kosinski sub-BIOS decompress + ~300-byte SP stub calling the BIOS
  _BURAM traps), LIST/READ/WRITE/DELETE mailbox RPC with Word-RAM
  staging, Files `v` volume toggle, Notepad F1 to the active volume.
  Implementation notes + emulator traps (the BlastEm $400000 bus
  error!) in docs/GENESIS-STORAGE.md tier 3.
- **M6 scheduler** (`genesis/scheduler.i`): the Amiga milestone-3
  cooperative scheduler — per-window tasks, private 2KB stacks at
  $FF4000+, one-slot mailboxes; keys post with bounded yield-retry
  so input bursts survive. All 14 AUTOTEST builds re-verified in
  BlastEm through the task machinery.

## Remaining roadmap

1. **Deferred — SD card over bit-banged SPI** (spec'd in
   docs/GENESIS-STORAGE.md): lands with the real-hardware adapter
   PCB alongside the PS/2 sockets and tape comparator.
2. **Real hardware**: PS/2 wiring validation (keyboard EXT interrupt,
   mouse inhibit/poll timing), tape comparator thresholds, TMSS on a
   model 3, pad feel, PSG balance, and the Sega CD Mode-1 path
   against a real attachment (or Genesis Plus GX / Ares — BlastEm
   has no CD). First new port headed for physical hardware.

Notes/risks: work RAM is 64 KB total (the app model fits; Notepad runs
a 2 KB buffer), and PS/2-over-gameport needs the real-hardware pass —
emulators don't model PS/2 devices on the control ports.

## Sources

- SpritesMind: "Megadrive/Genesis clone with a keyboard?" — HardWareMan
  on PS/2 keyboard via port 2 + EXT interrupt
  (gendev.spritesmind.net/forum/viewtopic.php?t=525)
- SpritesMind: "XBAND, Mega Net 2, and Mega Drive Keyboards" — Sik's
  notes on the official keyboard protocol (port 2, TH/TR handshake,
  4-bit bus) (gendev.spritesmind.net/forum/viewtopic.php?t=2556)
- ConsoleMods wiki: Genesis connector pinouts (DE-9: 5 = +5V, 8 = GND,
  7 = TH/select) (consolemods.org/wiki/Genesis:Connector_Pinouts)
