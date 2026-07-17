# UnoDOS `iigs` code audit — performance, security, bugs

Scope: the **iigs** world (Apple IIGS, native 65C816, Super Hi-Res 320×200×4bpp,
ProDOS/SmartPort block storage, ADB mouse+keyboard, Ensoniq DOC audio). The
default `build.sh` target: `boot.s` → `kernel.s` @ `$00:2000`, plus 8 disk-loaded
`.APP` binaries assembled at fixed bank-0 slots (`sys.inc` `SLOT_*`) and read at
runtime off a FAT12 volume on the 800 KB ProDOS image.

Threat model. This is bare-metal 65816 in native mode with **DBR=$00, no memory
protection, no guard pages** — every out-of-bounds write is a direct corruption
primitive, and the SHR framebuffer lives in bank `$E1` reached through hand-built
24-bit pointers, so a bad row/bank computation corrupts video RAM or wraps within
bank `$E1`. The only external inputs are (a) the **ProDOS/SmartPort disk image**
(`.po`) — boot blocks, the FAT12 volume, the `.APP` binaries, and the `.TXT`
documents — and (b) the **ADB keyboard/mouse** via the firmware/harness soft-switch
page. Both are **project-authored / trusted**: the disk is produced by `mkdsk.py`
+ `mkfs.py` from this repo, and there is no network, no USB-descriptor parsing, no
remote surface. So the security section is mostly *"no untrusted-input surface"*;
what remains is **structural FAT12 parsing** that would only bite against a
maliciously crafted or corrupt disk (latent), plus one **UI-reachable** OOB write
(a window-drag clamp bug, §3 B1) that needs no untrusted data at all.

Line numbers are exact against the current tree. Nothing here was assembled: cc65
(`ca65`/`ld65`) lives at `C:\Users\arin\snes-tools\bin` but was not run; the fixes
are proposed, not applied.

---

## 1. Performance

### Root cause — the whole screen is cleared and repainted on trivial changes

There is **no damage/dirty-rect tracking and no cached background**. Every path
that changes anything on the desktop calls `repaint_all` (`kernel.s:1329`), which
unconditionally:

1. `clear_screen` → `fill_screen` — writes **all 32 000 pixel bytes** as 16 000
   16-bit stores (`kernel.s:369-388`, hot loop `383-387`),
2. `draw_desktop` — repaints the menu bar, the version string, and **all 11 icons**
   (`draw_str` each, `kernel.s:757-793`),
3. then redraws every open window (`kernel.s:1333-1339`).

At 2.8 MHz the `fill_screen` clear alone is ~16 000 × ~12 cyc ≈ **68 ms** — before a
single window is drawn. `repaint_all` fires from `raise_window`, `close_window`,
`win_create`, `apply_theme`, and — the interactive one — **`handle_drag`**.

### Fixes, highest impact first

**P1 — Window drag repaints the entire screen every cell step.**
`handle_drag` calls `repaint_all` (`kernel.s:2059`) whenever the dragged window
crosses an 8-px cell boundary. Each such frame re-runs the full `fill_screen` wipe
+ desktop + every window, even though only one window moved by one cell. This is the
drag lag. **Fix:** damage-rect the move — restore the union of the old and new
window rectangles from a cached desktop background (see P2) and repaint only the
moved window, instead of `repaint_all`. Even the coarse "repaint only the band of
rows the two rectangles span" is a large win.

**P2 — No cached desktop background.**
The desktop (solid `clear_screen` fill + menu bar + version + 11 icons) is identical
frame to frame yet is recomputed on every `repaint_all`. **Fix:** render the static
desktop once into a bank-0 (or bank-`$E1` off-screen) buffer and blit the needed
rows; invalidate only in `apply_theme`/resolution change. Removes most of P1's cost.

**P3 — `fill_band` stores one byte at a time in 8-bit mode.**
`fill_band`'s inner column loop drops to `sep #$20` and does `sta [GP],y` one byte
per pixel-pair (`kernel.s:350-357`, store at `353`). This is *the* universal fill
primitive — every window body (`fill_cells`), every game cell (`fillcell`), and
every solid rectangle goes through it. All fill widths are `cx*4`/`ncols*4` bytes,
i.e. **always even**, so a 16-bit `sta [GP]` word store (PB replicated to both
bytes, as `fill_screen` already does at `377-381`) **halves** the store count on the
hottest loop in the renderer.

**P4 — OutLast redraws the entire road canvas every single frame.**
`outlast_tick` calls `redraw_topmost` unconditionally at `outlast.i:65` (the `@found`
path always animates), and `outlast_draw` (`outlast.i:110-189`) fills the full
`OLW*OLH = 34*20 = 680` cells via `fillcell` each time — ~21 800 byte writes/frame.
Every *other* game gates its redraw behind a step timer (Dostris `game_tick`
`dostris.i:391`, Pac-Man `pacman_tick` `pacman.i:185`, Tracker `tracker.i:102`), so
only OutLast pays a full-window repaint at 60 Hz. **Fix:** redraw only when the scene
actually changes (scroll/phase/steer) or drop the cadence to every 2-3 frames.

**P5 — `fillcell` recomputes the row-base multiply per cell.**
`fillcell` (`kernel.s:571-595`) calls `fill_band`, which calls `calc_gp_px`
(`kernel.s:324-341`) doing the `PY*160` multiply **once per cell**. Game canvases
redraw hundreds of cells per frame (OutLast 680, Paint 648, Dostris 180, Pac-Man
143). **Fix:** compute the destination row address once per canvas row and advance
by 4 bytes per column, or add a `fill_row_cells(colour, x, y, n)` primitive.

**P6 — `draw_window` double-paints the window area.**
`draw_window` fills the whole window body with `fill_cells` (`kernel.s:1369`) and the
title bar, then `app_draw_content` (`kernel.s:1416`) repaints content cells over most
of it — two passes over the same area on every `redraw_topmost`. Minor next to
P1-P4; largely subsumed once games stop full-window redraws.

*(Already good: the software cursor is a save-under, not a full repaint
(`draw_cursor`/`erase_cursor`, `kernel.s:2146`/`2239`); the idle main loop does no
`repaint_all` — only cursor save/restore + input polling; `app_ticks` refreshes the
clock only on a whole-second edge (`kernel.s:2131-2139`); Theme recolours via a
single palette-line poke, no pixel repaint (`theme.i:13-34`).)*

---

## 2. Security

**No remotely- or device-reachable untrusted-input surface exists.** There is no
network stack, no USB, no font/image parser fed from outside, and no attacker-
controlled descriptor. Both input channels — the ProDOS disk image and the ADB
keyboard/mouse — are trusted (project-authored media / firmware). The user-facing
free-form inputs are all bounded:

- Notepad text entry is capped: `notepad_key` rejects appends at `NBUFSZ`
  (`apps.i:298-300`) and guards backspace at length 0 (`apps.i:311-312`).
- Loading a document into Notepad clamps the file size to `NBUFSZ` before the read
  (`files_open`, `apps.i:150-157`).
- `fat_list_root` caps the parsed directory at 16 entries into the 256-byte
  `v_dir_list` (`fs.i:235-237`).
- The cursor position is clamped before any SHR write (`draw_cursor`,
  `kernel.s:2148-2157`).

What remains is **structural FAT12 parsing** that trusts on-disk fields. These are
**latent / defensive-only** under the trusted-disk model, but they are the real
corruption primitives if the `.po` is ever swapped or corrupted, so they are listed
by impact.

### S1 — LOW (latent; trusted disk) — `.APP` loader has no destination bound
`ensure_loaded` → `fat_load_named` reads a directory entry's first cluster and
**size** (`kernel.s:991-994`, size low word from `DIRSEC+28`) into `A0`/`F2`, then
`fat_read_file` (`fs.i:125-169`) writes **whole clusters** into the app's fixed
bank-0 slot (`app_load_tab`, 2 KB `SLOT_*` regions) with **no clamp of `F2` against
the slot size**. The 2 KB bound is enforced only at *build* time (`build.sh`
`size=$0800`, so `ld65` errors if an app overflows). A crafted `.APP` whose
directory size field exceeds 2 KB would overrun its slot into the adjacent app
regions and on toward the `$8000` game buffers. **Fix:** pass a max-length to
`fat_read_file` and stop at the slot size; reject an on-disk size `> $0800`.

### S2 — LOW (latent; trusted disk) — cluster-chain walk trusts 12-bit link values
`fat_read_file`'s loop only tests `cmp #$0FF8` for EOF (`fs.i:128-129`); it accepts
any link `2..$0FF7` as a valid data cluster. `fat_next_cluster` indexes the cached
FAT with `offset = cluster*3/2` (`fs.i:96-101`, `lda FATBUF,x` at `101`), but
`FATBUF` is only `SPF*BPS = 1536` bytes, so a link `≥ 1024` reads **past `FATBUF`
into `SECBUF`/`DTBOARD`/`NBUF`** (an OOB *read*), and the same unvalidated cluster
drives the data-sector LBA `((cluster-2)*SPC)+DATA_START` (`fs.i:134-139`) — an
out-of-volume block read, and for `cluster < 2` a wildly negative LBA. The real
volume has only **665 data clusters** (`clusters = (1344-14)/2`, `mkfs.py:31`;
matched by `fat_alloc`'s `cmp #667` at `fs.i:338`). **Fix:** in the chain walk (and
in `fat_next_cluster`) reject `cluster < 2` or `cluster ≥ 667` and treat it as EOF.

*(Read-only and reachable only via a malformed disk; no write primitive here beyond
S1's. Under the trusted-disk threat model both S1 and S2 are hardening, not live
holes.)*

---

## 3. Correctness / robustness bugs

### B1 — MEDIUM — window-drag Y clamp ignores window height → OOB write past the SHR framebuffer
`handle_drag` clamps the dragged window's X to `SCRW_C - WW`
(`kernel.s:2025-2030`, `lda #SCRW_C / sbc v_wintab+WW,x`) so the window's *right
edge* stays on screen — but the Y clamp uses the **constant** `SCRH_C-1` (=24) and
**never subtracts `WH`** (`kernel.s:2043-2046`):

```
@ymin:  lda #(SCRH_C-1)     ; 24  — should be SCRH_C - WH
        cmp A1
        bcs @yok
        sta A1
```

So any window can be dragged to `WY = 24`. Every app window has `WH ≥ 9`
(`app_def_tab`, `kernel.s:2417-2427`), so `draw_window`/`fill_cells` then fills from
pixel row `PY = 24*8 = 192` downward for `WH*8` rows (up to `176`), i.e. rows
`192..367`. The SHR pixel region is rows `0..199` (offset `0..31999`); rows `≥ 200`
write into the SCB array (`$E1:9D00`), the palette (`$E1:9E00`), and then **wrap
within bank `$E1`** as `GP` (16-bit, bank byte fixed `$E1`) overflows `$FFFF`. The
trigger is an ordinary mouse drag to the bottom of the screen — **no untrusted data
required** — and the effect is corrupted scanline-control/palette bytes (visible
video corruption, possibly a stuck video mode) plus wrapped writes into bank-`$E1`
low RAM. `fill_band`/`fillcell`/`render_glyph` do no screen-bounds clamp of their
own, so this drag clamp is the sole guard and it is wrong. It escaped the
regression suites because `tests/m1` never drags a tall window to the extreme
bottom. **Fix:** mirror the X path — `lda #SCRH_C / sec / sbc v_wintab+WH,x` for the
maximum `WY` (keeping the `MENUBAR_C` minimum), so the window's *bottom* edge stays
on screen.

### B2 — LOW — `fat_read_file` always writes the full final cluster (no partial-tail bound)
`fat_read_file` writes `SPC` whole sectors per cluster regardless of `F2`
(`fs.i:142-164`): the remaining-bytes decrement saturates to 0 (`fs.i:154-160`) but
the inner `@sec` loop still writes the rest of the cluster. For a file whose length
is not a multiple of 1 KB, up to 1023 bytes past the logical EOF are written into the
destination. It is **safe today** — `NBUF` is 4 KB (a 4-cluster max) and the app
slots are build-capped at 2 KB — but any future destination sized to the exact byte
length would be overrun. Overlaps S1's root cause. **Fix:** stop writing within a
cluster once `F2` reaches 0, or document that destinations must be cluster-rounded.

### B3 — LOW — `handle_drag` Y-underflow mis-clamps (asymmetric with X)
Still in `handle_drag`: the X path guards a negative result with `bpl @xpos / lda #0`
(`kernel.s:2022-2024`), but the Y path has no equivalent — if `mouse_y/8 -
v_drag_offy` underflowed, `A1` would wrap to `$FFxx`, sail past the `cmp #MENUBAR_C`
min check (`kernel.s:2038-2042`), and then get clamped to the **bottom** row (24)
instead of the top. It is **latent**: title bars are 1 cell tall, so a title-bar
click always yields `v_drag_offy = 0` (`handle_clicks`, `kernel.s:1957-1960`) and the
subtraction can't go negative. **Fix:** add the same `bpl/lda #0` guard on the Y
result for symmetry and future-proofing.

*Checked and clean:* the window-manager z-order (`raise_window`/`close_window`/
`z_index_of` renormalisation, `kernel.s:1261-1316`); the event ring buffer
(`ev_post`/`ev_get`, power-of-two `EVQ_SIZE=32` masking, full-queue drop,
`kernel.s:1769-1815`); `fill_screen`'s exact 32 000-byte coverage (`NPIX` even,
`kernel.s:383-387`); `clear_state` covering all of `VARS` (highest field
`v_ol_phase = VARS+$48C < $500`, `kernel.s:271-279`); `fmt_dec`/`put2dig`/`div16`
number formatting (`v_numbuf`/`v_clkbuf` are 16 B, max output 6 B; zero handled,
`kernel.s:1694-1762`); the Dostris piece indexing (`piece*16+rot*4 ≤ 108 < 112`,
`dostris.i:76-96`) and its private `DTR0/DTR1` scratch (the banked row-base
aliasing fix); Pac-Man's ghost-struct access via `abs,x` with `X` = struct base
(`pacman.i:264-331`); `blk_io`'s private `BLK0/BLK1` (the banked `F0`-aliasing fix)
and its `phx/plx` around every emulation-mode driver call; the FAT12 write path
(`fat_alloc`/`fat_alloc_chain`/`fat_save_file`, cluster count `ceil(len/1024)` min 1,
free-slot/overwrite scan, both-FAT flush, `fs.i:334-642`); the disk-app dispatch
tables (`app_*_tab` indexed by proc `0..10` / binary id `0..7`, all in range,
`kernel.s:2293-2337`); and the bank-0 buffer map (`DIRSEC`/`FATBUF`/`SECBUF`/
`DTBOARD`/`PAINTBUF`/`TRKPAT`/`NBUF` are non-overlapping, `sys.inc:180-209`).

---

## Suggested order of work
1. **B1** (drag Y clamp) — the one UI-reachable OOB write; a two-instruction fix
   (`lda #SCRH_C / sbc v_wintab+WH,x`) that stops video-RAM corruption.
2. **P1 + P2 + P3** — the drag/UI lag: damage-rect the drag, cache the desktop,
   word-store `fill_band`. Together these take a full-desktop drag from "repaint
   everything every cell step" to "touch a few window rows."
3. **P4 + P5** — cap OutLast's redraw cadence and hoist the per-cell row multiply.
4. **S1 + S2** — bound the `.APP` loader and validate cluster links; hardening that
   matters the moment a disk is swapped or corrupted.
5. **B2, B3, P6** — the remaining latent/robustness cleanups.

Building/testing note: the port assembles with cc65 (`ca65 --cpu 65816` + `ld65`)
and runs headlessly through the ROM-free `cpu65816.py` + `harness.py` rig
(`build.sh`, `tests/*.py`). That toolchain was not invoked here, so none of the
above has been compiled or run — the fixes are proposed, not applied.
