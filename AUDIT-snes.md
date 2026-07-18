# UnoDOS `snes` code audit — performance, security, bugs

Scope: the **snes** port (Super Nintendo, 65C816, LoROM `.sfc`), the default
`build.sh` target. Sources: `kernel.asm` + the `.inc` includes
(`apps.inc`, `games.inc`, `softkbd.inc`, `sram.inc`, `sound.inc`, `theme.inc`,
`tracker.inc`, `paint.inc`, `sched.inc`) and the generated `gen_data.inc` /
`unodef/gen` tables.

Threat model for the security section: bare-metal cartridge console, flat WRAM,
**no memory protection / no guard pages** — every out-of-bounds write into bank 0
($00:0000-$1FFF, the low-8KB WRAM mirror holding DP, stack, VARS and the tilemap
shadow) is a direct corruption primitive. Unlike the pc64 world there is
**essentially no untrusted external input**: the only inputs are the joypad
(read through masked bit tests — not an attack surface) and the battery-backed
**SRAM save**. A flashcart/emulator can load a crafted `.srm`, so the mini-FS in
`sram.inc` is the one place "external data" reaches the kernel; the section
below focuses there and on latent index/bounds footguns.

Line numbers are exact against the current tree (asm: label + line).

---

## 1. Performance

### P1 — Subtractive `div16` makes every on-screen number formatting O(value) — the top cost
`kernel.asm:1666` (`div16`) divides by **repeated subtraction** (`@loop` 1670-1677:
`cmp/sbc/inc DVQ` until the dividend drops below the divisor). `fmt_dec`
(`kernel.asm:1719`) calls it once per decimal digit, and the first call on an
N-digit value spins ~value/10 times: formatting a 5-digit score is ~7 000 loop
iterations, and a 4-digit one still ~1 100 — *per number*. This runs on the hot
path: `pm_hud` (`games.inc:2459`) formats **four** values (score/hi/level/lives)
and is re-issued every Pac-Man step via `pm_redraw_actors` (`games.inc:2667`);
`dostris_panel`/`dt_labelval` (`games.inc:724`, `798`) and the Clock do the same.
At 3.58 MHz (~59 700 cycles/frame) a single 4-5-digit HUD refresh can approach a
whole frame just in `div16`. **Fix:** replace the subtractive divide with a
proper routine — a `/10` reciprocal-multiply for `fmt_dec`, or the classic
"subtract 10000/1000/100/10 with per-place digit counters" — and give `put2dig`
its own `/10` fast path. Biggest single win in the port.

### P2 — Full-scene `repaint_all` on every drag cell-step
`handle_drag` (`kernel.asm:2089`) snaps the dragged window to a cell and, when the
cell changes, calls `repaint_all` (`kernel.asm:2140` → `repaint_all` at `1193`).
`repaint_all` unconditionally `clear_screen`s the whole 32×28 field, redraws the
menu bar + all 11 icons (`draw_desktop`), then every window bottom-up, then the
soft keyboard — i.e. the most expensive paint in the system, re-run for a
one-cell move. There is no damage-rect. **Fix:** during a drag, restore only the
band of cells the window's old/new rectangles cover (union) and redraw the moved
window, instead of `repaint_all`. (Mouse moves within a cell already no-op at
`@nochange`, so only actual cell steps pay this — but each one pays in full.)

### P3 — Whole-board / whole-road game redraws each tick
`dostris_draw` (`games.inc:604`) repaints the entire 10×20 board cell-by-cell
(`@brow`/`@bcol` 623-656, a `gfill`→`fill_cells` per occupied cell) plus the
piece, every gravity step (`dostris_tick:1037` → `redraw_topmost`). `outlast_draw`
(`games.inc:1131`) rebuilds every sky/road/grass row via `ol_fillrow` every
`outlast_tick` (10 frames). Pac-Man already does the right thing with
incremental `pm_redraw_actors`; Dostris/OutLast do not. **Fix:** track the board
delta (only the moved/locked/cleared cells) the way Pac-Man tracks old actor
tiles, rather than repainting the full content each tick.

### P4 — NMI DMAs the entire 2 KB tilemap for any single-cell change
The NMI flushes on `v_dirty` (`kernel.asm:2220-2223`) via `FlushTilemap`
(`kernel.asm:603`), which DMAs the full `SCRW_C*32*2` = 2048-byte shadow
(`kernel.asm:622`) even when a redraw touched one cell (a paint pixel, a soft-key
hover, a HUD digit). **Fix:** have the draw primitives record a min/max dirty
tilemap-row span and DMA only that sub-range (VRAM is linear per row), or keep a
dirty-rect the flush honours. Secondary to P1-P3 but a fixed 2 KB/frame tax
whenever anything is dirty.

*(Already good: TTF-style glyph work is unnecessary — text is fixed 8×8 tiles;
the cursor is a hardware OBJ updated in the NMI; Paint flushes a bounded 24
dirty tiles/frame (`paint.inc:184`); Pac-Man redraws incrementally.)*

---

## 2. Security — memory corruption reachability

The joypad path (`pad_to_mouse`, `pad_events`, `pad_game`) only does masked bit
tests on the 16-bit pad word and clamps the cursor to the screen — no reachable
corruption. The **one** external-data surface is the SRAM save (`sram.inc`);
everything below needs a crafted `.srm` (valid `USV1` magic) loaded via an
emulator/flashcart, so this is semi-trusted, not remote. Still real on bare metal.

### S1 — MEDIUM — Unvalidated SRAM file count drives an unbounded draw loop past the tilemap into bank 0
`sram_count` (`sram.inc:108`) returns the raw 16-bit word at SRAM offset 4 with
**no clamp**; `sram_init` (`sram.inc:64`) trusts an existing `USV1` header and
never re-validates the count against `SRD_MAX` (8). `files_draw`
(`apps.inc:268`) loops `@row` from 0 to that count (`apps.inc:286`), and for each
row computes the cell Y as `WY + 2 + row` (`apps.inc:329-334`) and calls
`draw_str`. `draw_str` (`kernel.asm:695`) forms the tilemap index as
`(cy*32 + cx)*2` in a **16-bit** accumulator and `sta TMAP,x` — so a count of,
say, 0x7FFF walks `cy` far past 28, the index wraps within bank 0, and the writes
land in the stack ($1Fxx), direct pages, VARS and the window table: arbitrary
WRAM corruption of the running kernel. `sram_find`/`sram_delete` (`sram.inc:168`,
`240`) similarly trust the count for their loops. **Fix:** clamp `sram_count` to
`SRD_MAX` (and validate/repair the header in `sram_init`); additionally bound the
`files_draw` row loop to the visible window height.

### S2 — LOW — SRAM read trusts the stored data offset (over-read; destination is safe)
`sram_read` (`sram.inc:203`) reads the file's data offset from the directory
(`sram.inc:217-221`) and uses it as the source index into the SRAM window
(`@cp` 224-234) with no range check. A crafted offset reads from the mirrored
SRAM address space. This is **read-only and the destination is bounded** — the
byte count is clamped to the caller's `max` (A2: `NBUF-1` for Notepad,
`TK_PATLEN` for the Tracker), so no OOB *write* occurs; only garbage bytes are
copied into the (correctly sized) `v_npbuf`/`v_tkpat`. **Fix:** validate
`offset` and `offset+size` against the heap bounds (`SRH_OFF`..`SRAM_SIZE`)
before the copy.

*No other untrusted surface exists:* there is no network, no USB, no filesystem
beyond SRAM, no loadable code. Controller input is fully masked. The SPC700
mailbox (`sound.inc`) talks to on-cart silicon over bounded, timeout-guarded
loops. Font/tile/palette blobs are assembled into the ROM, not loaded.

---

## 3. Correctness / robustness bugs

### B1 — MEDIUM — Pac-Man state is never initialised at boot (uninitialised-read → OOB draw on non-zero power-on WRAM)
`ClearState` (`kernel.asm:407`) zeroes only VARS ($0200-$03FF, loop bound
`cpx #$0200` at `kernel.asm:416`) plus an explicit `stz v_pm_hi`
(`kernel.asm:418`). Pac-Man's state block was relocated into free WRAM at
`v_pm_state = $0CC8` (`kernel.asm:195`) and its maze/actors up to $0FE5 — **none
of which the boot clear reaches**, and only `pm_new_game` ever initialises them.
Merely *opening* the Pac-Man window runs `pacman_draw` (`games.inc:2592`
`lda v_pm_state`) and `pacman_tick` (`games.inc:2747`) against whatever WRAM
powered up holding. If `v_pm_state` happens non-zero it takes the `@game` path
and draws the maze/ghosts from uninitialised `v_pm_maze`/`v_pm_gh`; `pm_putcell`
→ `fill_cells` then writes the tilemap at `(cy*32+cx)*2` for arbitrary col/row,
i.e. OOB into bank 0 (same wrap as S1). Emulators that zero WRAM hide this;
real hardware does not guarantee a zero power-on state. **Fix:** add
`stz v_pm_state` (and ideally clear the whole $0CC8 block) in `ClearState`/boot,
or call a lightweight `pm_reset` when the window is created. One line closes the
memory-safety hole; the rest is display robustness. (Dostris/OutLast/Music state
live in the *cleared* VARS region, so they are safe — Pac-Man is the sole
offender.)

### B2 — LOW — `div16` divide-by-zero hangs the kernel
`div16` (`kernel.asm:1666`) loops `while dividend >= divisor`; with `divisor == 0`
the compare's carry is always set, so it subtracts 0 forever (`DVQ` wraps) — an
infinite loop with the screen frozen. Every current caller passes a non-zero
constant (10, 12, 60, 3, 7), so it is not currently reachable, but any future
caller feeding a computed divisor would hang. **Fix:** early-out with quotient 0
when `S1 == 0`.

### B3 — LOW — 16-bit score/counter wrap in long sessions
Scores and counters are 16-bit words: `v_pm_score` (Pac-Man, +200<<kills per
ghost, `games.inc:2291`), `v_dt_score` (`games.inc:453`), `v_ol_score`
(`games.inc:1494`). A long enough game overflows 65535 and wraps to a small
value. Purely a display/scoring glitch — `fmt_dec` writes at most 5 digits + NUL
into the 16-byte `v_numbuf`, so there is **no** buffer overrun (unlike pc64's
`fmt_u`, whose input was 64-bit). **Fix:** widen the score accumulators to 32-bit
(two words) with a matching `fmt_dec32` if longer play is desired; otherwise
document the cap.

*Checked and clean:* Notepad insert (`apps.inc:239`, guards `v_np_len` against
`NBUF-1` before writing `v_npbuf`); the caret/wrap draw loop (bounded by visible
rows, `apps.inc:100`); the soft-keyboard tables (`softkbd.inc`, 55 fixed keys,
`key_at`/`drawkey` index-bounded); `sram_save` (`sram.inc:358`, enforces
`SRD_MAX` and `heaptop+len <= SRAM_SIZE` before writing); Paint's planar-tile
writes (`paint.inc:27`, clipped to the 160×96 canvas, dirty-ring dedup'd, 24-tile
DMA budget); the Tracker row-string builder (`tracker.inc:131`, 27 chars into the
40-byte `v_tkbuf`) and its `TK_PATLEN` load/save; Theme's channel edit
(`theme.inc:277`, masked to 5-bit BGR555 fields); the event queue (`ev_post`
drops when full, `kernel.asm:1756`); Dostris/OutLast/Pac-Man board indexing (all
masked or `cmp #DT_COLS/#DT_ROWS/#PM_COLS` bounds-tested before the board read);
`note_pitch`/`piece`/`app_def` table indices (all within the generated table
sizes — `PM_NOTE_COUNT=61` vs max index 42).

---

## Suggested order of work
1. **B1** (`stz v_pm_state` at boot) — one line, closes the only bank-0 OOB write
   reachable without a crafted save.
2. **S1** (clamp `sram_count`, bound the `files_draw` loop) — closes the
   crafted-`.srm` corruption path.
3. **P1** (`div16`/`fmt_dec` fast divide) — the single largest perf win; unblocks
   HUD-heavy games.
4. **P2 + P3** — damage-rect the drag and the Dostris/OutLast board redraws.
5. **P4, S2, B2, B3** — dirty-span DMA, SRAM offset validation, div-by-zero
   guard, 32-bit scores.

Build/test note: the port assembles with cc65 (`ca65 --cpu 65816` + `ld65`,
`build.sh`) and runs under Mesen; that toolchain is not installed in this audit
environment, so the above is proposed, not compiled/applied.
