# Apple II port — milestone 3 handoff (for Claude Sonnet)

Prerequisite: **M1 and M2 landed** ([HANDOFF.md](HANDOFF.md),
[HANDOFF-M2.md](HANDOFF-M2.md)). Direction:
[../docs/PORTS-PLAN.md](../docs/PORTS-PLAN.md) §1, M3 line.

**M3 scope:** the scaled app roster — **Dostris** and **Pac-Man**
(feasible), **Paint** (hi-res, keyboard/paddle), **Tracker** and
**Music** as *blocking* speaker playback, **OutLast** re-checked at
1 MHz (allowed to stay off the roster — the honest-deviation rule),
**Theme** as hi-res dither schemes, and an honest evaluation of the
**cooperative scheduler** on the 6502. This is the "UnoDOS Lite" test:
parity adapted to the envelope, with deviations documented rather than
faked (the plan's macplus precedent).

M3 is feasibility-gated in two places (OutLast, scheduler). For each:
prototype first, decide, write the decision into the README and
PORTS-PLAN. "We measured, it doesn't fit, here's the number" is a
*valid milestone outcome* for those two items.

Recommended order: Theme (small, exercises the renderer) → Dostris →
Pac-Man → Music → Tracker → Paint → OutLast feasibility → scheduler
evaluation → tests/README/commit.

---

## 1. The performance reality (read before writing any game)

At 1 MHz you have ~17,000 cycles per 60 Hz frame; a full 8 KB hi-res
page repaint (load+store+index per byte) costs several frames by
itself. Consequences, baked into every M3 app:

- **Dirty-cell rendering only.** Games keep a logical grid (Dostris
  board, Pac-Man maze) and repaint only changed 7-px byte-cells.
  Never repaint the whole window per tick. The 68K ports already
  follow this discipline (macplus repaints topmost-only once a second;
  games update deltas) — at 1 MHz it is mandatory, not hygiene.
- **Game speed comes from the M1 soft tick** (calibrated pass
  counter). Define game gravity/AI steps in soft-tick units; the
  harness `wait` then drives deterministic frames (TICK_INSTRS
  calibration from M1 is what makes M3 screenshots reproducible).
- **No sound during gameplay.** The speaker is CPU-blocking (§4);
  games get at most short event beeps (line clear, death), accepted
  as pauses.

## 2. Games

Port logic/tables from the shared sources — same tables, same physics,
same AI, re-expressed in 6502 (the macplus versions are the 1-bit
rendering reference: dither-density ghost identities etc.):

| App | Logic template | 1-bit scheme template |
|---|---|---|
| Dostris | `macplus/games.i` (Dostris section) | macplus (dither-filled pieces) |
| Pac-Man | `macplus/pacman.i` | macplus (dither ghosts, hollow frightened) |
| OutLast | `macplus/games.i` (OutLast) | feasibility-gated, see below |

- **Dostris:** board 10×20 of 7-px cells fits a 70×140-ish window.
  Keyboard: left/right move, `a`/`z` or up rotate (II+ has no
  up-arrow — pick letter keys and print them in the HUD), space =
  hard drop, `p` pause, `n` new. Redraw = settled delta cells +
  active piece cells only; score/lines/level text on change only.
- **Pac-Man:** the maze is the stress case. Render the static maze
  once; per tick erase/redraw only the 4 actors + eaten dot cells
  (byte-aligned actor cells — actors move in 7-px steps; that is the
  documented deviation vs. sprite ports). If actor flicker/cost is
  unacceptable, halve the tick rate before cutting features. The AI
  tables (scatter/chase timers, ghost-house logic) port verbatim from
  `macplus/pacman.i`.
- **OutLast feasibility (decision point):** the road raster is a
  per-scanline effect — on hi-res that means rewriting ~140 rows of
  ~20 bytes per frame ≈ too hot at 1 MHz at full rate. Prototype the
  cheapest honest variant first: half-vertical-resolution road (rows
  doubled), 4–6 Hz update, byte-aligned curve steps. Timebox the
  prototype (a day of agent effort, not a week); if it can't hold
  ~5 fps with input responsive, **drop it from the roster** and record
  the measured number in README + PORTS-PLAN ("like Genesis skips
  FAT12" — precedent for honest omission).

## 3. Paint

The MacPaint-style editor (`macplus/paint.i` is the structure
template) on a hi-res canvas with the platform's true gamut = the four
M1 dither inks (white / 25% / 50% / black):

- Canvas = the window content; backing store = the framebuffer itself
  (1-bit, byte-aligned) — no shadow buffer needed unless flood fill
  wants one (a row-stack flood on the framebuffer is fine at this
  resolution; cap stack depth, it's a 6502).
- Tools: pencil, eraser, line, rect, filled rect, oval if cheap,
  flood fill, ink picker. Keyboard cursor (arrows/IIe or letter keys
  on II+, configurable step), space = apply; **paddle pointer (M2 §4)
  is the natural input if it shipped** — wire it in if present.
- Save/load `PAINT.UNO` to the M2 mini-FS: raw byte-aligned bitmap
  rows of the canvas rect (document the header: width-bytes, height,
  then data). Round-trip test in the m3 script.

## 4. Music + Tracker (blocking speaker playback)

The plan's honest framing: timed square waves **monopolize the CPU** —
playback freezes the desktop, exactly like real Apple II software.
Build both on M2's parameterized beep routine:

- A note = (half-period in cycles, duration in cycles) toggle loop on
  `$C030`. Generate the shared note table (the Paula/PSG-period
  sources in `amiga/mkdata.py` / `genesis/mkdata.py` show the
  conversion shape) into `build/notes7.s` via mkfont.py-style codegen:
  period(cycles) = 1,000,000 / (2 × freq).
- **Music:** Canon in D (the shared arrangement — note/duration table
  from the other ports), staff view drawn before playback starts,
  playback highlights the current note *between* notes (redraw is
  allowed to steal time at note boundaries only). ESC checks `$C000`
  between notes to abort — poll only at boundaries, never inside the
  timed loop.
- **Tracker:** the shared 32-row/4-channel byte-identical pattern
  format (`macplus/tracker.i`, `genesis/tracker.i`) BUT playback is
  single-voice: play the leftmost non-empty cell per row (the x86
  PC-speaker port precedent, already named in macplus's README).
  Editing UI is the macplus grid, keyboard-driven; saves `SONG.UNO`
  to the mini-FS.
- Harness: the M2 toggle-count hook plus recorded toggle step-indices
  lets the m3 script assert a note's period (±instruction jitter).
  One period assertion + screenshots is enough.

## 5. Theme — dither schemes

Port the macplus model (`macplus/theme.i`, the mutable `pat_tab`): the
renderer's fills go through a small pattern table (desktop pattern,
window fill, title fill, accent); Theme rewrites the table live and
repaints. Ship ~6 presets including a full invert (macplus precedent).
If M1's renderer hard-coded patterns, the first task is routing all
fills through `pat_tab` — do that refactor before writing the app.

## 6. Scheduler — evaluate honestly

The plan: "cooperative per-window tasks are feasible on 6502 (software
stacks are the constraint — evaluate honestly at M3)." The 6502 has
ONE 256-byte hardware stack at `$0100`. Options to evaluate:

1. **Stack partitioning:** N tasks × fixed slices of page 1 (e.g.
   kernel 96 B + 4 tasks × 40 B). task_yield = save S, load next S.
   Cheap switch; the risk is slice overflow — JSR depth + interrupts
   (none here) must fit 40 bytes ⇒ enforce shallow call trees in app
   procs, add a canary byte per slice checked at yield.
2. **Stack copy:** one full-depth stack, yield copies the used span
   to a per-task save area. Safe but yield costs ~100s of cycles ×
   depth — measure before choosing.
3. **No scheduler** (documented deviation): keep the M1/M2
   poll-and-dispatch loop with per-app tick procs (the pre-M3 macplus
   shape). Valid fallback if 1/2 prove fragile.

Recommendation: prototype option 1 with the canary, port the
`task_yield`/mailbox surface from `macplus/scheduler.i` (same names,
same one-slot mailbox + bounded yield-retry semantics), and run
Dostris gravity + Clock + Notepad typing concurrently as the proof.
If canaries trip in normal use, fall back to option 3 and write the
finding into README/PORTS-PLAN with the measured depth numbers.

## 7. Tests, README, closeout

- `tests/m3.script`: AUTOTEST opens each new app, screenshots it,
  drives a few keys (Dostris drops, Pac-Man steps, Theme preset
  apply, Tracker row entry, Paint stroke + save/load round trip,
  Music period assertion). Keep `wait` counts deterministic via the
  M1 calibration.
- README: per-app deviation table (7-px actors, blocking audio,
  single-voice tracker, OutLast verdict, scheduler verdict) — the
  macplus README's honest-envelope voice.
- Update `../docs/PORTS-PLAN.md` §1 (M3 → DONE, with the OutLast and
  scheduler verdicts inline), then the real-hardware pass: AppleWin
  full m1+m2+m3 scripts by hand, then FloppyEmu on metal (plan §1,
  "Real-hw validation").
