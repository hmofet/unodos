# UnoDOS/C64 — milestone 1 handoff

This is the working note for the next person (or the next session) on the C64
port. M1 is **done and harness-verified** — a colour hi-res desktop, window
manager, SysInfo, a real CIA Time-of-Day Clock, keyboard-matrix nav, PAL/NTSC
detection and a SID blip, packaged as a `.prg` + bootable `.d64` and driven by
a ROM-free py65 harness that renders the VIC bitmap to colour PNG. Below: how it
fits together, the traps that cost time, and what M2/M3 need.

## Files

| File | What |
|---|---|
| `kernel.s` | the whole M1 kernel (dasm, 6510, `org $0801`) |
| `mktables.py` | generates `build/tables.s` — VIC bitmap row-address + screen-row tables |
| `mkfont.py` | copies the shared `kernel/font8x8.asm` glyphs verbatim to `build/font8.s` |
| `mkprg.py` | prepends the `$0801` load address → `.prg`, packs a real 1541 `.d64` |
| `build.sh` | `mktables` + `mkfont` → `dasm` → `mkprg` (`test` = `-DAUTOTEST=1`) |
| `harness.py` | py65 6510 + VIC raster / CIA matrix+TOD / SID models + bitmap→PNG |
| `tests/m1.script`, `tests/m1_ntsc.script` | the M1 regressions |

## Architecture (why it's shaped this way)

- **6510 ≈ 6502 + banking.** Reuses the Apple II port's mental model
  (poll-and-dispatch main loop, z-order window manager, `dc.b "UDx1"` + vars
  pointer discovery header for the harness) but is otherwise its own kernel —
  the renderer, input and clock are all C64 hardware, not Apple II analogues.
  **`$00`/`$01` are the CPU port (banking) — never used as scratch.** ZP scratch
  starts at `$02`.
- **VIC bank 1, hi-res bitmap, colour from screen RAM.** Bitmap `$6000`, screen
  RAM `$4000`, `$D018=$08`, `$D011=$3B`. Two colours per 8×8 cell come from
  screen RAM (`fg<<4 | bg`); colour RAM at `$D800` is unused in this mode. The
  renderer therefore works in **two coordinate systems at once**: pixel-rows ×
  byte-columns for the bitmap *shapes* (`fill_rows`, `dither_rect`, `win_outline`
  borders, `draw_char` glyphs), and 8×8 *cells* for colour (`color_fill`,
  `draw_char`'s colour stamp). They are kept on **separate ZP params**
  (`zpFX/zpFY/zpFW/zpFH/zpFPat` pixels vs `zpCX/zpCY/zpCW/zpCH/zpFCol` cells) so
  a caller never confuses pixel-rows with cell-rows.
- **No KERNAL.** `SEI` at boot, poll forever; the reset/IRQ/NMI vectors are
  never used, so the port doesn't depend on a KERNAL revision. It *does* assume
  the standard CIA #1 keyboard matrix wiring and a running TOD.
- **Address tables are generated, not hand-typed.** `rowbase_lo/hi[200]` =
  `$6000 + (y>>3)*320 + (y&7)` and `scr_lo/hi[25]` = `$4000 + cy*40`. The byte
  for pixel *(x,y)* is `rowbase[y] + (x>>3)*8` — adjacent columns are **8 bytes
  apart** (the surprise vs a linear framebuffer), the 8 rows in a cell are
  consecutive, and crossing a cell boundary in y jumps by 320−7. The font is
  bit-7-left, matching the C64 bitmap, so glyph bytes are used unmodified.

## Traps that cost time (read before touching the renderer)

1. **The fill primitives clobber `zpTmp`/`zpTmp2`** (they use them for the
   `col*8` 16-bit multiply). Anything that must survive a `fill_rows` /
   `color_fill` / `dither_rect` call lives in a **dedicated ZP that the
   primitives never touch**: `zpSlot` (icon loop index), `zpWtop`
   (`win_outline` window-top), `zpSel` (icon selected flag), `zpIconX` (icon
   base column). Two separate "saved value in a clobbered slot" bugs (the
   `draw_icons` loop index in `zpTmp2`, the `win_outline` top in `zpTmp`) caused
   a runaway loop that sprayed garbage over all of memory — the symptom was a
   "hang" (a single giant fill from a corrupted width/height) and a blank
   window. If a draw routine seems to hang or scribble, suspect a value held
   across a fill call.
2. **`win_outline` mutates `zpFX`.** Drawing the right edge leaves `zpFX` =
   the window's *right* column. `draw_icon` must use the saved `zpIconX` (not
   `zpFX`) for the label column afterwards, or the label lands off the right
   side of the box.
3. **PAL/NTSC detect needs a coherent raster.** The kernel reads `$D011` (bit 8)
   then `$D012` (low byte) a few instructions apart and keeps the max 9-bit
   value; the harness advances the raster ~1 line per 8 MPU steps so those two
   reads see the same line and a sweep covers every line. Decision: bit 8 set
   **and** low byte ≥ `$20` ⇒ PAL (max line 311, low `$37`); NTSC's bit-8-set
   lines (256–262) only ever show low `$00`–`$06`, so it can't false-positive.
4. **The detect loop counter is 16-bit but exits on the low-byte wrap.**
   `inc zpCnt / bne` loops 256× per high-byte step; seed `zpCnt+1 = $F0`
   (→ 4096 samples). Seeding `$00`/`$20` makes it run 64k/57k iterations — slow
   enough in py65 to look hung.
5. **Boot is expensive in the harness.** The full-screen dither plus several
   window redraws cost ~360k py65 steps before `mainloop`. The first `wait` in a
   script must clear that (`TICK_INSTRS=7000`, lead `wait 60`) or screenshots
   catch a half-drawn frame.

## Harness contract

- Renders directly from `$4000` (colour) + `$6000` (bitmap) — it does **not**
  need the vars block to draw. The `UDC1` + vars-pointer header is used only for
  `assert pal`/`ntsc`/state reads (`var(off)`: 0=sel_icon, 1=focus, 4=is_pal,
  6/7=win_state, 8/9=zlist).
- Keyboard: `key NAME` holds the matrix positions for `KEY_INSTRS` steps, then
  releases — the kernel's edge detection (`last_key`) turns that into exactly
  one event. `left`/`up` include left-shift, as a real C64 does.
- TOD: base 12:00:00, advances from CPU steps; hours-read latches, tenths-read
  releases (the kernel reads HR→MIN→SEC→tenths).

## What M2/M3 need

- **M2 — storage + text apps.** The honest C64 path is the **IEC serial bus to a
  1541** (`$DD00` CLK/DATA/ATN bit-banging) or, more tractably first, a `.d64`
  block driver the harness serves (mirror the Apple II RWTS/mini-FS split:
  `fs.i`-style USV1 catalog over 256-byte blocks). Add letter/digit/space decode
  to `scan_keyboard` (the matrix tables are already there) and an `app_mode`
  dispatch in `handle_key` (the Apple II kernel's structure ports directly).
  Files + Notepad reuse the Apple II app logic almost verbatim.
- **M3 — the app roster.** Theme is nearly free here (recolour cells — the C64's
  per-cell colour is the whole point). Dostris/Pac-Man/Paint port from the
  6502 Apple II apps with the cell/pixel split above. Music + Tracker should use
  the **three real SID voices** (a genuine win over the 1-bit ports — no
  blocking square wave). Watch the ~14 KB code budget below `$4000`; if it gets
  tight, move BSS/buffers to `$C000`–`$CFFF` (already reserved) or relocate the
  bitmap to keep code contiguous.
- **Real hardware.** VICE (`x64sc`) first to validate the VIC bank switch,
  matrix decode and TOD on a cycle-accurate machine, then SD2IEC / 1541 Ultimate
  from the `.d64`.
