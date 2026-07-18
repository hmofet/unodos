# UnoDOS `c64` code audit — performance, bugs, security

Scope: the **c64** world (Commodore 64 / 6510), the `build.sh` target — the
`org $0801` kernel (`kernel.s` + `sys.inc` + `fs.i` + generated `build/tables.s`
/ `build/font8.s`) plus the ten disk-loaded apps assembled at `$5000`
(`sysinfo.s` `clock.s` `files.s` `theme.s` `dostris.s` `music.s` `pacman.s`
`tracker.s` `paint.s` `outlast.s`). Threat model for the security section: a
6510 with **no memory protection** — every out-of-range indirect store is a
direct corruption primitive, and page-wrapped pointer math writes anywhere in
64 KB. **There is almost no untrusted-input surface**: no network, the keyboard
is read straight off the CIA matrix, and the apps ship on the project-authored
`.d64` (trusted). The **one structural-parse surface** is the persisted USV1
mini-FS store (`fs.i`, region `$C000-$CFFF`) — today only the trusted py65
harness writes it (a `--storage` sidecar), but the README states that on real
hardware this region is backed by a **1541 disk file via an IEC driver** (the
remaining M2 work), at which point the store becomes attacker/​user-swappable
media. `harness.py` and the `mk*.py` generators are host build/test tools,
outside the shipped-OS threat model.

Three areas, most-actionable first. Line numbers are exact against the current tree.

---

## 1. Performance

The C64 renderer has two coordinate systems (pixel-rows × byte-columns for the
bitmap, 8×8 cells for colour) and no damage tracking: most apps repaint the
whole screen on every event. On a 1 MHz 6510 the dominant costs are the
full-screen wipe and the per-keystroke full redraws.

### P1 — Full-screen wipe (`app_clear`) on every redraw, even cursor-only moves

> **✅ FIXED — Files (`55f7bdd`), Theme (`e0f8ce2`), Tracker (`1c923e5`)** — each cursor move now repaints only the two affected rows/cells instead of `app_clear` + full relist. The per-row/per-cell body is factored into a shared `*_row_draw`/`tk_cell_draw` that always fills the highlight band with the pattern (`$00` plain / `$FF` selected) so the full redraw stays byte-identical while a nav key can erase the old highlight (`files_nav_redraw`/`theme_nav_redraw`/`tk_nav_redraw`). Render-verified BYTE-IDENTICAL via `harness.py` (py65 → VIC bitmap → PNG), waits between keys so both builds register each edge. **Notepad deferred** (see below): it has no cursor-nav / highlight — it is an append-only editor whose every keystroke changes `note_len` and can tail-scroll the whole visible region, so there is no "two rows changed" case; a byte-identical incremental would have to replicate the wrap/scroll layout for low, per-keystroke value.

`kernel.s:942-953` (`app_clear`) calls `clear_bitmap` (`kernel.s:678-694`, which
stores **8192 bytes** one at a time through `(zpDst),y` — it clears the full
`$2000` page though only 8000 bytes are live) and then a whole-screen
`color_fill` (25×40 = **1000 cells**). Every full-screen app calls `app_clear`
at the top of its redraw, and several redraw fully on *navigation* keys that
changed nothing but a highlight: `files_draw` (`files.s:187`, reached from
`files_next`/`files_prev` at `files.s:132,145`), `theme_draw` (`theme.s:86`),
`tracker.tk_draw` (`tracker.s:239`), and `notepad_draw` (`files.s:483`). So a
single CRSR press pays ~9 200 stores to rebuild an identical background.
**Fix:** the incremental model Pac-Man (`pacman.s`, redraws only the vacated /
entered tiles) and Dostris (`dostris.s`, erase/draw the moving piece) already
use — for Files/Theme/Tracker, re-invert only the two affected rows/cells
instead of `app_clear` + full relist. Biggest single win.

### P2 — Launcher repaints all 10 icons on every selection move
`kernel.s:561-585` (`draw_icons`) redraws **NICONS = 10** icons — each a
`color_fill`, a `win_outline` (four `fill_rows`), a full-cell `fill_rows`, a
label `color_fill` and a `draw_string` — and `handle_key` calls it on every
LEFT/RIGHT at the launcher (`hk_redraw`, `kernel.s:269-270`). Only the
previously- and newly-selected icons changed. **Fix:** repaint just those two
icons (redraw old with `COL_ICON`, new with `COL_ICON_SEL`).

### P3 — Notepad scans the entire buffer twice per keystroke
`files.s:542-573` (`np1_loop`, count lines) and `files.s:604-649` (`np2_loop`,
render) each walk **all `note_len` bytes** (up to 2048) on every insert /
backspace / nav key, i.e. ~2·`note_len` iterations per keystroke. The line
count and tail-scroll offset only change near the edit point. **Fix:** cache
`zpNTot`/`zpNFCol` and rescan only from the last line break, or only when the
buffer length changes.

### P4 — `fill_rows` / `dither_rect` recompute the column byte-offset every row
`kernel.s:744-761` recomputes `zpFX*8` (a 3× `asl`/`rol` 16-bit shift) **inside**
the `fr_row` per-row loop (`kernel.s:735`), although it is invariant across rows;
`dither_rect` does the same at `kernel.s:798-814` (loop at `kernel.s:789`). The
boot desktop dither is 192 rows tall, so that is 192 redundant shift sequences
per `dither_rect`, plus the cost on every window/separator fill. **Fix:** compute
`zpFX*8` once into a ZP word before the loop and add `rowbase` to it each row.

*(Already good: Pac-Man and Dostris redraw incrementally; OutLast redraws the 20
road bands via `color_fill` each tick with no `app_clear` (`outlast.s:138`);
Clock repaints only when the CIA second changes (`clock.s:38-45`); glyphs are
byte-for-byte from the shared font, never re-rasterized.)*

---

## 2. Security — memory-corruption reachability

No network stack, no device-descriptor parsing, no remote surface. Keyboard
input is a hardware matrix scan. The apps are trusted (`.d64`). The only data
parsed structurally is the **USV1 mini-FS store**, and the only field an app
copies by is a file's stored **size**.

### S1 — MEDIUM (reachable via app interplay) — Paint's load overflows the 512-byte canvas
`paint.s:374-390` (`pt_load`) calls `fs_read` for `PAINT.UNO` with the
destination fixed at `CANVAS` = `APP_BSS` = `$4C00`, **512 bytes** (`PT_W*PT_H`).
`fs_read` (`fs.i:221-241`) copies the file's *stored size* — it has **no
destination clamp**. Paint always writes `PAINT.UNO` as exactly 512
(`pt_save`, `paint.s:358-373`), but the size is not enforced on the way back in,
and the same name is writable by **Notepad**: open `PAINT.UNO` from Files
(`notepad_load`, `files.s:337`), type past 512 bytes (Notepad caps at
`NOTE_MAXLEN` = 2048), press F1 (`fs_save`). Now `PAINT.UNO` is up to 2048 bytes;
launching Paint and pressing **L** copies all of it into the 512-byte canvas —
an OOB write of up to ~1.5 KB starting at `$4C00`, running past the 1 KB
`APP_BSS` slack into **`$5000`, the Paint code that is currently executing**.
Reachable with no external input, purely through the shipped apps.
**Fix:** give `fs_read` a max-length argument (or a `pt_load`-side check) and
clamp the copy to 512; reject / truncate a `PAINT.UNO` whose stored size ≠ 512.

### S2 — LOW / latent (structural parse) — USV1 store header + entries trusted with no validation
`fs_init` (`fs.i:64-125`) validates **only** the 4-byte `"USV1"` magic; it then
trusts `FSC_COUNT`, `FSC_USED` and every entry's `size`/`start` verbatim. A
corrupted or crafted store therefore yields several latent primitives:
- **Unbounded copy:** `fs_read` (`fs.i:221-241`) copies `size` bytes from
  `FSHEAP + start` to the caller's buffer with no bound on either — an oversized
  `size`, or a `start` near the top of the heap, walks off the region and
  scribbles wherever the destination leads (the S1 instance, generalized; also
  `notepad_load` → `NOTEBUF`).
- **Count-driven directory walk:** `fs_find` (`fs.i:164-172`), the Files listing
  (`files.s:243-246`) and `fs_delete` (`fs.i:347`) all loop to `FSC_COUNT`
  without clamping to `FS_MAXFILES` (15). A `count > 15` runs `fs_entry_ptr`
  (`fs.i:130-141`, `idx*16`) past the 240-byte directory into the heap and, at
  `count > ~24`, drives `draw_char`/`fill_rows` at cell rows whose `row*8`
  **wraps mod 256** (`asl` on an 8-bit row, e.g. `blit_cell` `kernel.s:872-874`),
  producing a wild `rowbase[]` index → the "runaway fill sprays garbage"
  failure the HANDOFF documents.

Today the store is written only by trusted code, so this is **latent** — but it
is the port's sole structural-parse surface and **becomes a real
disk/media-parsing bug the moment the IEC/1541 driver replaces the harness
sidecar** (the documented M2 real-hardware follow-up). **Fix, when that driver
lands (cheap to add now):** in `fs_init` reject/clamp `count > FS_MAXFILES` and
`heap_used > HEAPSIZE`; validate each entry's `start + size ≤ heap_used`; give
`fs_read` a destination-max.

*No untrusted-input surface otherwise:* keyboard is a hardware scan
(`scan_keyboard`, `kernel.s:145`), the app loader takes a 0-9 icon id
(`launch_app`, `kernel.s:286`) that indexes a fixed 10-app registry, and the
renderer's `rowbase[200]`/`scr[25]` tables make `fill_rows`/`color_fill` /
`draw_char` bounds-safe for **in-range** cell/pixel inputs (all trusted callers
stay in range; the boundary cases — `app_clear` at 25 rows, the boot dither at
pixel row 199, a cell at col 39 → last bitmap byte `$7F3F` — are exact, not
over).

---

## 3. Correctness / robustness bugs

### B1 — LOW — Notepad help-line column guard is off by one
`files.s:706-711` (`nd_help`): the status help `msg_note_help` is 20 chars, so
its last cell is `zpCol+19`; to stay within columns 0-39 the guard must skip when
`zpCol+19 ≥ 40`, but it tests `cmp #41` (`files.s:710`), so at `zpCol = 21`
(reachable when a large `Bytes:` count pushes the cursor right) the final char
lands at **cell column 40**. That write stays inside screen/bitmap RAM (it
bleeds into the next row's first cell — screen `$43C0`, bitmap `$7E00`), so it is
cosmetic, not memory-unsafe. **Fix:** `cmp #40`.

### B2 — LOW (defensive; same root as S1) — `fs_read` has no destination-size bound
`fs.i:221-241`. The API copies a file into a caller pointer sized only by
convention. `notepad_load` happens to match (`NOTEBUF` = 2048 = the Notepad cap,
exact fit) and `pt_load` does not (S1). **Fix:** add a `max` argument to
`fs_read` and copy `min(size, max)`; this closes S1 and hardens every future
caller.

### B3 — LOW (latent; same root as S2) — count loops don't clamp to `FS_MAXFILES`
`fs_find` (`fs.i:164-172`), Files listing (`files.s:243-246`), `fs_delete`
(`fs.i:347`). `fs_save` correctly refuses to grow past 15 (`fs.i:266-270`), but
the readers trust whatever `FSC_COUNT` holds. **Fix:** clamp `count` to
`FS_MAXFILES` in `fs_init` (a corrupt store then lists at most 15).

*Checked and clean:* Pac-Man tile access (`pm_walk` bounds-checks `col < 13` and
`row < 13` *before* indexing `PMMAZE`, `pacman.s:128-151`); Dostris piece/board
math (`dostris_fits` checks `BX < 10` / `BY < 20` before `dos_board_idx`, and
every piece rotation occupies local column 0 so the `px` left-edge underflow
maps to `BX = 255` → caught by the `≥10` test, `dostris.s:176-202`); Notepad
insert/backspace (`note_len` capped at 2048 = the exact `NOTEBUF` size,
`files.s:398-445`); Notepad tail-scroll (visible rows bounded to
`APP_VIEW_ROWS` = 22 by the `zpNSkip` clamp, `files.s:574-586`); the keyboard
matrix decode and shift/cursor remap (`kernel.s:145-217`); the CIA TOD read +
BCD→ASCII format (`read_tod` `kernel.s:372`, `ck_fmt_bcd` `clock.s:121`); the
PAL/NTSC raster detector (`kernel.s:413-464`); the SID routines
(`sid_click` `kernel.s:470`, Tracker/Music voice writes with fixed
7-byte-stride bases); Theme/SysInfo/Clock (fixed tables, indices bounded to
`NTHEMES`/note count); `draw_dec16` (16-bit, ≤5 digits, saves/restores X,
`kernel.s:989`); and the `fs_save`/`fs_delete` heap accounting (fits check
allows `used == HEAPSIZE` exactly, writing to the last byte `$CFFF` and no
further, `fs.i:271-342`).

---

## Suggested order of work
1. **S1** (`pt_load` clamp to 512, or `fs_read` `max` arg per **B2**) — the one
   reachable OOB write; a few instructions.
2. **P1 + P2** — the per-keystroke full-screen wipe and the all-icons launcher
   repaint (the felt lag on real hardware).
3. **B3 + the S2 hardening** — clamp `count`/`heap_used`/entry ranges in
   `fs_init`; do this **before** the IEC/1541 store driver lands, since that
   turns S2 from latent into a live disk-parse surface.
4. **P3, P4** — the Notepad double-scan and the per-row offset recompute.
5. **B1** — the cosmetic column off-by-one.

Building/testing note: the c64 target assembles with `dasm` (the
`C:\Users\arin\apple2-tools\dasm.exe` default in `build.sh`) and is regression-
tested by the ROM-free py65 `harness.py`. `dasm` is not confirmed installed in
this environment, so none of the above was assembled or run here — the findings
are from source inspection, and the fixes are proposed, not applied.
