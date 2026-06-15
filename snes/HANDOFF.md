# SNES port — implementation handoff (for Claude Sonnet)

Audience: the engineer building the UnoDOS/SNES port, milestone by
milestone (M0–M3). Authoritative direction:
[../docs/PORTS-PLAN.md](../docs/PORTS-PLAN.md) §3; platform contract:
[../docs/PORT-SPEC.md](../docs/PORT-SPEC.md). House method per
milestone: code + scripted emulator regression + screenshots + commit
+ README; update this file + PORTS-PLAN at each close.

**The big picture:** this is the Genesis port's twin, re-expressed in
65816 — cell-based desktop on background tiles, hardware-sprite
cursor, pad-as-pointer + soft keyboard, SRAM mini-FS. Read
`genesis/README.md` end to end before starting; then treat these as
the direct templates:

| Genesis file | Becomes |
|---|---|
| `genesis/kernel.asm` | kernel structure: cell renderer, WM, events, vblank ISR, pad pointer |
| `genesis/softkbd.i` | soft keyboard (layout + hit-test + sticky shift, near-verbatim logic) |
| `genesis/sram.i` | USV1 mini-FS + Files app |
| `genesis/scheduler.i` | cooperative scheduler |
| `genesis/games.i`, `pacman.i`, `paint.i`, `tracker.i`, `theme.i` | the M2/M3 apps |
| `genesis/mkdata.py` | `mkdata.py` here (new tile/palette formats, same role) |
| `genesis/build.sh` | `build.sh` (AUTOTEST variants per feature) |

The 65816 toolchain comes standing from the IIGS port
(`../iigs/HANDOFF.md` §2a — ca65/ld65). If the IIGS port hasn't
reached M0 yet, do its §2a toolchain step first; it is shared.

---

## 1. Envelope (engineering facts)

- **CPU:** 65C816 @ 3.58 MHz (slower clock than Genesis's 68000 but
  comparable throughput at cell granularity). **WRAM:** 128 KB at
  `$7E:0000–$7F:FFFF` (first 8 KB mirrored into every bank at
  `$0000–$1FFF`).
- **PPU:** tiles/sprites only — no bitmap mode (same as Genesis).
  Use **Mode 1** (`$2105`): BG1 + BG2 are 16-color tile layers, BG3
  4-color. BG1 = the desktop/window plane (the Genesis "plane A");
  BG2/BG3 free (status overlays, game layers). 256×224 visible ⇒
  **32×28 cells** of 8 px (Genesis is 40×28 — window metrics shrink;
  document as a deviation the way macplus documents 512×342).
- **VRAM 64 KB, CGRAM 256 colors (BGR555), OAM 128 sprites** — all
  **inaccessible outside vblank/forced blank**. This is the one big
  architectural difference from Genesis (which writes its VDP from
  the main loop): see §2's shadow+DMA rule.
- **Audio:** SPC700 coprocessor with its own 64 KB RAM + 8-voice DSP.
  Nothing plays until a driver program is **uploaded** to it through
  the 4 mailbox ports `$2140–$2143`. M3's centerpiece.
- **Input:** controllers via auto-joypad read (`$4200` bit 0; results
  in `$4218/$4219` during vblank); the **SNES Mouse** is widely
  emulated and slots into the same read (identifies itself in the
  extended serial bits — consult fullsnes at M1; Mesen2 emulates it).
- **Storage:** battery SRAM on cartridge — LoROM maps it at
  `$70:0000` (declare 8 KB in the header); emulators/flashcarts
  persist it as `.srm`. **Byte-addressable** — no Genesis odd-lane
  `*2` dance.
- **Boot:** LoROM cartridge. Header at `$00:FFC0` (title, map mode,
  SRAM size byte `$FFD8` = 3 for 8 KB), vectors at `$FFE0+`; reset
  enters 6502 emulation mode (`clc/xce` to native, like the IIGS).

## 2. The rendering rule (decide once, at M0)

Genesis law was "all VDP access in main-loop context; ISRs only set
state." The SNES **inverts the transfer half**: VRAM/CGRAM/OAM can
only be written during blank. Adopt this architecture from the first
line of kernel code:

- The main loop (and all app/WM code) renders into a **WRAM shadow**:
  a 32×28 word tilemap shadow + a dirty-row bitmap (or dirty-rect
  list), plus staged CGRAM/OAM shadows.
- The **NMI handler** (vblank) flushes: DMA dirty tilemap rows to
  VRAM, CGRAM/OAM shadows when flagged, reads auto-joypad, updates
  the cursor sprite, latches press-time click state + sequence
  counter (the Genesis vblank ISR's job list, plus the DMA flush).
- Tile *pattern* (character) data is mostly static — font/chrome/icon
  tiles upload once at boot under forced blank. Dynamic patterns
  (Paint's canvas tiles at M3) go through a small per-frame DMA
  budget queue — design the "queue full" behavior explicitly
  (PORT-SPEC §6 rule 10).

PORT-SPEC §6 rule 2 still holds in spirit: game/app *logic* never
runs in the ISR; the NMI only transfers prepared bytes and samples
input.

## 3. M0 — LoROM skeleton boots in Mesen2  ✅ DONE

**Closed.** `build.sh` → `unodos.sfc` boots in Mesen2 to the "UnoDOS 3"
splash and renders the live joypad as `PAD:xxxx`; the shadow+DMA
architecture and the vblank NMI are in place; `mkdata.py` emits 4bpp
planar tiles + a BGR555 palette from the shared font. See
[README.md](README.md) for the full M0 writeup. Decisions that landed and
bind later milestones:

- **Rig = software renderer + PrintWindow + AUTOTEST.** Mesen's CLI does
  NOT autoload Lua, and `PrintWindow` returns black for its GPU surface on
  this headless desktop — so `setup_mesen.ps1` forces
  `Video.UseSoftwareRenderer`, and input is checked by AUTOTEST
  self-injection (the Genesis fallback), not Lua memory asserts. The
  `"UDM1"` magic at `$7E:0100` is in place if Lua asserts become viable.
- **ca65 `.define` foot-gun.** A parameterised `.define CELL(col,row)
  (((row)*COLS+(col))*2)` mis-evaluates to `row*COLS + col*2` (the outer
  grouping is dropped → half row stride). Use inline arithmetic inside a
  real `.macro` instead (`row*64 + col*2`). This bit the M0 splash; don't
  reintroduce it in the M1 cell renderer.
- **Scroll registers are write-twice.** The bulk PPU-register clear leaves
  the BG scroll latches nonzero; `Reset` zeroes `$210D/$210E` explicitly.

The original pre-implementation plan is kept below for context.

**Goal:** `build.sh` → `unodos.sfc` that boots in Mesen2 to a splash
(tiles + palette, "UnoDOS 3" text from the shared font) and reacts to
the joypad (any visible change on button press), plus the scripted
rig proven.

- **ca65 setup:** LoROM ld65 config (bank `$80` org `$8000` is the
  conventional fast-rom view; start 32 KB, grow by banks), header +
  vectors, the canonical register init (forced blank `$2100=$8F`,
  zero the PPU regs, Mode 1, enable NMI + auto-joypad `$4200=$81`).
- **`mkdata.py` (new):** port `genesis/mkdata.py`'s role — shared 8×8
  font, window chrome, cursor sprite, app icons (from the x86 `.BIN`
  headers) — emitting **SNES 4bpp planar tiles** (rows of plane 0/1
  byte pairs in the first 16 bytes, planes 2/3 in the second 16) and
  BGR555 palettes from the UnoDOS RGB values (PORT-SPEC §1: entries
  0–3 of each UI palette = themed colors).
- **Scripted rig (the BlastEm role):** Mesen2 has Lua scripting
  (memory read, input override, screenshots). At M0, establish the
  harness pattern: a Lua script driven by a per-test table (waits,
  synthetic input, screenshot calls, memory asserts against the vars
  block) + a `.ps1`/shell wrapper that launches Mesen2 with the ROM
  and script and collects PNGs (the `genesis/snapretry.ps1` role).
  Verify how headless/CI-quiet Mesen2 can run on this machine and
  write the recipe here. Fallback if Lua input injection
  disappoints: the Genesis AUTOTEST pattern — `build.sh <feature>`
  variants that self-inject synthetic events, with the rig only
  taking screenshots (this is how genesis tests soft-kbd/PS2/games).
- **Vars discovery:** keep the UDM1 convention — magic + vars pointer
  at a fixed WRAM/ROM location the Lua script reads (e.g. magic in
  ROM at a fixed offset, vars base in WRAM at `$7E:0100`), so memory
  asserts don't hardcode addresses.

**Exit:** splash + joypad reaction, scripted screenshot lands, recipe
documented, commit.

## 4. M1 — tile desktop + WM + cursor + pad/mouse + soft keyboard + SysInfo/Clock  ✅ DONE

**Closed.** kernel.asm + softkbd.inc implement the cell renderer (four UI
palette schemes), the window manager (z-order, raise/close, drag, chrome),
pad-as-pointer + the 32-cell soft keyboard, the OAM cursor, and SysInfo +
Clock. Verified in Mesen2 (`build/m1.png`, the F12 framebuffer screenshot):
desktop, two overlapping windows with correct palettes/chrome/z-order, a
live clock, and the full cyan soft keyboard; VRAM also proven byte-correct
by CPU read-back. See [README.md](README.md).

Traps that bit M1 (don't repeat in M2/M3):
- The cell routines are `.a16`/`.i16` — call them with A **and** X/Y 16-bit
  (`rep #$30`), or the 16-bit immediates/ops run in 8-bit mode → garbage.
- Outer loop counters get clobbered by the draw routines they call. Use the
  dedicated `LC0`/`LC1` DP slots for any loop that calls draw/WM code.
- `FlushOAM`/`FlushTilemap` expect 8-bit A; the NMI must `sep #$20` before
  calling them. Both also force 16-bit index for the DMA size register.

**The capture rig (now solved — use it for M2/M3).** On this headless/RDP
host the GPU surface is black through `PrintWindow`. DON'T force Mesen's
software renderer to grab the window — its display blit drops BG palette
bits below ~scanline 160 (cost me a long detour; the VRAM was always
correct). Instead `run_mesen.ps1` triggers **F12 = TakeScreenshot** (Mesen
keycode 101) to dump the accurate PPU framebuffer to
`Documents/Mesen2/Screenshots`, with focus forced via `AttachThreadInput`.
That is the reference render.

The notes below are the original pre-implementation plan.

The Genesis M1 surface on the §2 architecture:

- **Cell renderer:** draw_char/draw_string/fill_cells/window chrome
  as tilemap-shadow writes (Genesis `kernel.asm` cell primitives port
  almost mechanically — A/X/Y in 16-bit mode make the word-per-cell
  model natural). Four UI attribute schemes = four palette lines
  (normal/inverted/accent/softkey), exactly the Genesis model, as
  Mode-1 palettes 0–3 for BG; sprite palettes carry the cursor.
- **Window manager:** PORT-SPEC §2 invariants (z-order topmost=15,
  raise/destroy renormalization, topmost-only content, drag with
  XOR-style outline — on cells, the Genesis drag outline approach),
  windows snap to 8 px cells.
- **Input:** pad-as-pointer verbatim from Genesis (d-pad accelerating
  cursor, A=click/drag, B=soft keyboard, C=Enter, Start=Esc, X/Y/Z
  extras — keep the same button roles, documented in README);
  press-time latch + sequence counter in the NMI (PORT-SPEC §3);
  same 32×3 event queue, `EV_KEY` data = (raw<<8)|ascii with the
  Amiga/Genesis raw codes (arrows `$4C–$4F`, F1 `$50`) so app key
  handlers stay byte-portable across the family.
- **Soft keyboard:** port `genesis/softkbd.i` (layout tables,
  hit-test, sticky shift, posts through the event queue).
- **SNES Mouse:** detect at boot/hotplug from the controller ID bits,
  map deltas to the cursor, button = click; fall back to pad
  silently (the Genesis PS/2-probe pattern). Mesen2 emulates the
  mouse — wire it into the rig.
- **Apps:** SysInfo (CPU/RAM/region/mouse-present) + Clock (NMI tick
  at 60 Hz — a real tick, count to HH:MM:SS).
- **Tests:** m1 script = desktop shot, cursor move + click-raise,
  drag, close, soft-kbd typing into a probe field, mouse-mode shot,
  clock advance.

## 5. M2 — SRAM storage (USV1) + Files/Notepad + games  ✅ DONE

**Storage core** (`sram.inc` + `apps.inc`): USV1 mini-FS on 8 KB LoROM SRAM
at `$70:0000` (byte-addressable, little-endian words via `lda f:$700000,x` —
no Genesis odd-lane dance); init/save/read/find/delete + heap compaction.
Notepad (append editor, F1 → SRAM) + Files (list/open/delete). Header
declares SRAM (type `$02`, size byte 3). Verified in Mesen2 (`build/m2.png`):
save → directory → listing round-trip.

**Games** (`games.inc`), all three verified (`build/dostris.png`,
`outlast.png`, `pacman.png`); seven desktop icons via an icon→proc table,
app-key routing through `app_key`:
- **Dostris** (proc 4) — Genesis piece tables/scoring/physics, cell render.
- **OutLast** (proc 5) — linear-perspective road (divide-free converging
  bands). DEVIATION: no per-row 1/z raster, curve, or oncoming traffic —
  the 65816 has no fast software 16/16 divide. (HDMA on BG scroll/colour, as
  the plan suggested, would restore the true raster — left for a polish pass.)
- **Pac-Man** (proc 6) — the full x86 ghost AI (Blinky/Pinky/Clyde,
  scatter/chase schedule, frightened eat-chain) intact, recast to tile
  coordinates. DEVIATION: actors are CELL-GRID BG tiles (shaped pac/ghost/
  dot/pellet tiles in `mkdata.py`), not pixel-smooth OAM sprites — one tile
  per step, collisions by tile equality, mode/fright timers in steps. The
  28×25 maze + a compact 1-row HUD fill the 30×28 window (the Genesis 8-col
  side panel won't fit the 32-cell screen). The OAM-sprite route the plan
  suggested is viable but the cell-grid keeps the whole game on the existing
  shadow+DMA model — a clean fit, left as the shipped form.

DEVIATION (carried from M2): Notepad is an append editor, not full caret/line
nav — the storage round-trip is what M2 proves.

Traps that bit M2 (don't repeat in M3):
- A ca65 A-width-tracking leak crashed `sram_init` — a label reached at
  runtime in 8-bit A but assembled as 16-bit (a preceding branch's `.a16`
  leaked), so `lda #imm` over-read and the spilled byte ran as BRK. Put an
  explicit `.a8`/`.a16` at any label reached in a different width than the
  fall-through assumes. (Same family as the M1 width traps.)
- WRAM map collision: Pac-Man state first landed at `$0400`, on top of the
  2 KB Notepad buffer `v_npbuf` ($0400–$0BFF) — `notepad_set_demo` seeded
  "OS S" ($534F) into the hi-score. The free WRAM below the tilemap shadow
  is tight: `$0400–$0BFF` npbuf, `$0C00–$0CC7` Dostris board, then only
  `$0CC8–$0FFF` (824 B) free before `TMAP=$1000`. Pac-Man packs scalars +
  ghosts + the 700-byte maze into `$0CC8–$0FE5`. Audit the map before
  adding M3 state — the scheduler's per-task stacks and the SPC upload
  buffer both want bank-0 room.
- Long-proc branch ranges: `pm_step`/`pm_steer` overflow the 65816
  ±127 `bcc/beq` range; convert the cross-block ones to `jmp` (or
  `bCC :+ / jmp / :`). Same fix used across the Dostris long procs.

The original plan notes follow.

- **USV1 port:** `genesis/sram.i` nearly verbatim — same format
  (magic "USV1", count, heap top, 8×16-byte dir entries, compacting
  heap), minus the odd-lane addressing (SRAM here is flat bytes at
  `$70:0000`). Keep fields big-endian only if you want byte-identical
  images with Genesis saves; otherwise note the choice — SRAM is not
  interchange media (the Genesis file says the same).
- **Files + Notepad:** port from Genesis (`sram.i`'s Files app +
  kernel Notepad), including Notepad F1-save via the soft keyboard's
  F1 and the pad's mapped keys.
- **Games:** Dostris, OutLast, Pac-Man from `genesis/games.i` /
  `pacman.i` — same tables/physics/AI re-expressed in 65816. The PPU
  makes these *easier* than Genesis (plan §3): Pac-Man actors as OAM
  sprites, OutLast's road via per-scanline HDMA on BG scroll/color
  (HDMA is the SNES's gift — investigate it for the road raster
  before hand-rolling). Game-mode pad remap (d-pad=arrows with
  hold-repeat, A=action, X=new, Y=pause) ports as-is.
- **Tests:** the Genesis m2/m4 AUTOTEST set — save→wipe→reopen SRAM
  round trip (Mesen2 persists `.srm`; the rig can also memory-assert
  the USV1 magic), per-game scripted steps.

## 6. M3 — SPC700 audio + Tracker/Music + Theme + scheduler  ✅ DONE

**Shipped.** SPC700 audio core (`spc700.py` Python SPC700 assembler + driver
+ square-wave BRR + note-pitch table; `sound.inc` IPL upload + mailbox,
verified by ack — SysInfo "Audio: SPC700 OK"). Music (proc 3, voice 0).
Theme (proc 8, 8 presets + BGR555 editor; palette-shadow `v_pal` $1800 +
NMI `FlushPalette`, since CGRAM is write-only outside vblank). Tracker
(proc 9, 32×4 pattern on voices 0–3, SONG.TRK in SRAM). Paint (proc 10, the
per-pixel **unique-tile canvas**: pixels painted into a planar-tile shadow in
bank $7F, dirty tiles DMA'd by the NMI at ≤24/frame). 11 desktop icons.

DEVIATIONS (M3): SPC driver is square-wave only (no real instruments yet);
the Tracker's 4th channel is a tone not noise; Paint is pencil-only with a
fixed line-3 palette and no tape I/O; the scheduler is tick-based, not
context-switched (the bank-0 verdict below). Backlog: game-music gm_* hooks,
DSP noise/instruments, Paint tools + live colour, the SNES-Mouse probe.

**Scheduler verdict (bank-0 constraint).** Per-task private stacks are
infeasible — the 65816 stack lives in bank 0, whose low 8 KB is full here
(DP×2, VARS, 2 KB Notepad buffer, Dostris+Pac-Man, tilemap shadow,
palette/Tracker/Paint vars, OAM shadow, stack → ~736 B free, short of one
1 KB stack). So `sched.inc`'s `sched_run` schedules COOPERATIVELY BY TICKS:
every app's `*_tick` runs from the main loop each frame. Same behaviour
(Music/Tracker play under any window, the Clock advances, games step, Paint
flushes) without a context switch; the apps are already tick functions.
Parallels the Apple II port's verdict.

Traps that bit M3 (don't repeat): the ca65 width trap returns (a `rep #$20`
in a timeout path re-tracks A as 16-bit, so the next branch-target label's
`lda #imm` assembles 16-bit but runs 8-bit → BRK/black screen — `.a8` at the
label; bit `spc_upload`). `lda f:long,Y` / `sta f:long,Y` are illegal — long
indexed is X-only (bit the Paint dirty ring). `STZ` has no long-addressing
mode — use `LDA #0 / STA f:...,x` for bank-$7F clears. `fmt_dec` clobbers
S0/S1/S5/S6 — carry live draw state in S2/S3/S4/S7 or on the stack.

### original M3 plan notes

- **SPC700 driver (the hardest novel piece in this port):**
  - *Toolchain decision first:* ca65 does NOT assemble SPC700.
    Options: wla-dx (`wla-spc700`), or a tiny Python assembler/
    hand-assembled blob emitted by `mkdata.py` (the driver is small —
    a mailbox loop + DSP register writes). Decide, document here.
  - *Upload:* the IPL ROM handshake on `$2140–$2143` (wait for
    `$AA/$BB`, then the standard block-transfer protocol, then jump).
    Implement once as `spc_upload(blob, addr)`.
  - *Driver contract:* a mailbox protocol (port 0 = command/ack,
    ports 1–3 = args) for: load instrument (square-wave BRR samples
    generated by mkdata.py — BRR is the DSP's mandatory sample
    format), note on/off per voice, volume. Keep it tiny and
    documented; the 65816 side queues, the SPC side owns the DSP.
  - *Test path:* Mesen2 emulates the SPC700 fully; assert via the rig
    that the driver acks (mailbox memory asserts) — audio itself is
    verified by ear in the by-hand pass.
- **Music/Tracker:** shared note tables → DSP voices; Tracker's 4
  channels map to 4 real voices (byte-identical pattern format,
  saves to SRAM via USV1 like Genesis).
- **Theme:** the 8 shared presets + custom RGB editing rewriting the
  themed CGRAM entries (the CRAM model verbatim; BGR555 gives 5-bit
  channels vs Genesis's 3 — the custom editor gets finer steps).
- **Scheduler:** port `genesis/scheduler.i` (per-window tasks,
  private 2 KB stacks — place them in `$7E` WRAM above the kernel
  vars, one-slot mailboxes, bounded yield-retry for key posts).
  65816 native-mode SP is 16-bit bank-0… **note:** the 65816 stack
  must live in bank 0 (`$00:0000–$FFFF` = mirrors of `$7E:0000–$1FFF`
  low 8 KB) — so per-task stacks live in the low-8 KB mirror; budget
  accordingly (e.g. kernel + 6 tasks × 1 KB fits under `$1FFF` with
  room for ZP/DP). This constraint is real; design the memory map
  around it at M3 start.
- **Paint:** the Genesis unique-tile canvas trick ports directly
  (240 unique tiles + byte-per-pixel backing store in `$7F` WRAM);
  the per-frame DMA budget queue from §2 carries the dynamic tiles.

## 7. Risks & open questions

- **Vblank budget:** one vblank holds only a few KB of DMA — the
  dirty-row flush + OAM + CGRAM must fit. Measure at M1 with a
  full-desktop repaint; if it overflows, flush across frames
  (windows already snap to cells, tearing is invisible).
- **Mesen2 automation depth** (headless? input override fidelity?)
  is the M0 wildcard — the Genesis AUTOTEST fallback pattern is the
  safety net; don't fight Lua for weeks.
- **SPC700 toolchain** is the M3 wildcard — decide early in M3,
  keep the driver blob tiny so even hand-assembly is viable.
- **Bank-0 stack constraint** (§6) shapes the scheduler memory map.
- **Real hardware:** which flashcart does the user own, and is there
  a SNES Mouse? (Plan open question — ask when the metal pass
  approaches, not before.)
