# UnoDOS `gba` code audit — performance, security, bugs

Scope: the **gba** port (Nintendo Game Boy Advance, ARM7TDMI, `arm-none-eabi`
GNU as), the default `build.sh` target. Sources: `kernel.s`, `apps.inc.s`,
`dostris.inc.s`, the linker script `link.ld`, and the `mkdata.py`-generated
`build/gfx.s` + `build/gfxequ.inc` tables. This is a MINIMAL UnoDOS instance:
one full-screen app at a time, directional nav, drawn pixel-by-pixel into a flat
Mode 3 framebuffer (240×160, 16bpp BGR555) with a 16-entry IWRAM palette.

Threat model for the security section: bare-metal cartridge, flat IWRAM/VRAM,
**no memory protection** — any out-of-bounds store into IWRAM ($0300xxxx, holding
state, palette and the game board) or VRAM is a direct corruption primitive.
This port has **no untrusted external input at all**: the only input is the
button pad (read via `REG_KEYINPUT`, inverted and masked to 10 bits), and there
is **no SRAM / save / disk** — the Files app itself notes "no disk - cart ROM"
(`apps.inc.s:515`). So the Security section is short by construction: no
attacker-reachable data path exists; only latent defensive items are listed.

Line numbers are exact against the current tree (asm: label + line).

---

## 1. Performance

### P1 — `frect` fills the framebuffer one 16-bit pixel at a time
`frect` (`kernel.s:240`) is the sole area-fill primitive and stores a single
`strh` per pixel in `fr_col` (`kernel.s:256-259`). `clear_screen` (`kernel.s:320`)
issues a full-screen `frect` of 240×160 = **38 400 halfword stores** on *every*
`full_redraw` (`kernel.s:655`) — and `full_redraw` runs on each app switch, each
theme cycle, and in Dostris on **every piece lock and every line clear**
(`dostris.inc.s:220`). **Fix:** since two BGR555 pixels pack into one 32-bit word,
build a `colour|colour<<16` word once and store with `str` (halving the store
count), or `stm`-burst runs of 8; for the full-screen clear, a fixed-colour DMA
(REG_DMA3) fills VRAM far faster than the CPU loop. This is the port's biggest
per-redraw cost.

### P2 — Dostris redraws all 120 board cells including empty ones, on top of a full clear
`dostris_draw` (`dostris.inc.s:488`) walks the whole board (`dd_row`/`dd_col`
514-536) and calls `dcell` (`dostris.inc.s:474`, an 8×8 = 64-store `frect`) for
**every** cell — including cells whose value is 0 (empty), which `draw_chrome`'s
preceding `clear_screen` already painted the desktop colour. Early game that is
~120 × 64 redundant stores per redraw. **Fix:** skip `dcell` when the cell value
is 0 (`cmp r6,#0; beq` before the plot in `dd_col`); only settled blocks and the
walls/floor need drawing over the cleared desktop.

### P3 — `frect` recomputes the row base address every row instead of striding
Inside `frect`, `fr_row` (`kernel.s:250-254`) reloads `=FB` and recomputes
`y*240 + x` (via `lsl #8` − `lsl #4`) for each scanline. **Fix:** compute the
start address once and add the row stride (`240*2`) each row (the pattern `pchar`
already effectively uses at `kernel.s:214`). Micro-opt, but every filled cell
pays it.

*(Already good: `pchar` (`kernel.s:184`) hoists the fg/bg palette lookups out of
the per-pixel loop; `render_partials` keeps per-frame work tiny by drawing only
the changed piece (`draw_piece_partial`, `dostris.inc.s:631`), the clock digits,
or the music status in vblank, and gates the expensive `full_redraw` behind
`v_dirty`; the piece move/rotate paths erase-old/draw-new rather than repainting
the board.)*

---

## 2. Security — memory corruption reachability

**No untrusted-input surface exists.** The GBA port takes no network, USB,
filesystem, or save input, and loads no external code — everything (font, icons,
piece tables, tune, palettes) is assembled into the ROM by `mkdata.py`. The only
runtime input is the button pad: `read_keys` (`kernel.s:160`) reads
`REG_KEYINPUT`, inverts it, `and`s with `0x03FF`, and derives edges — pure masked
bit math with no indexing or copying driven by it. The AUTOTEST pad script
(`apps.inc.s:419`) is in-ROM and only compiled into test builds. There is
therefore no reachable remote/device memory-corruption primitive to rank.

The remaining items are **latent/defensive only** — not reachable with the
current fixed data set, but worth hardening because the draw primitives take
caller coordinates on faith:

### S1 — LOW (latent) — drawing primitives do no framebuffer clipping
`frect` (`kernel.s:240`), `pchar` (`kernel.s:184`) and `picon` (`kernel.s:268`)
compute `FB + (y*240 + x)*2` and store without bounding `x < 240` / `y < 160`.
All current call sites pass in-range constants or values derived from the
bounded board (see B-section for why Dostris coords stay in `[0,BW)×[0,BH)`), so
nothing overflows today. But there is no defence-in-depth: a future off-screen
coordinate would write past the framebuffer into adjacent VRAM. **Fix:** clip
`x/y/w/h` to the screen in `frect`/`pchar`/`picon` (or add debug asserts).

### S2 — LOW (latent) — `pchar` indexes the font with an unchecked `ascii-32`
`pchar` (`kernel.s:186`) does `sub r2,#32` then `font_data + (idx<<3)`; the font
table holds exactly 95 glyphs (chars 32-126, `mkdata.py:25`). A control char
(<32) yields a negative index and a char >126 reads past the table — an OOB
*read* of a garbled glyph. Not reachable (all strings are fixed printable ASCII
≤ `'z'`), so latent. **Fix:** clamp/skip `ascii < 32 || ascii > 126` before the
lookup if dynamic text is ever added.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `two_digits` mis-renders values > 99
`two_digits` (`kernel.s:337`) subtracts 10 in a loop and increments the tens
char; for an input ≥ 100 the tens char overflows past `'9'` (e.g. 100 → `':' '0'`).
The only value that can exceed 99 is `g_lines` in `dostris_draw_score`
(`dostris.inc.s:575`, `two_digits` at 586) after 100+ cleared lines; the clock
(`< 60`/`< 24`) and music note (`≤ 31`) and theme index (`< 4`) all stay in range.
Cosmetic, no memory effect. **Fix:** render `g_lines` with a 3-digit helper, or
cap the displayed value.

### B2 — LOW (latent) — no clipping means correctness depends entirely on the collision invariant
Dostris rendering safety rests on `piece_collide` (`dostris.inc.s:23`) rejecting
any pose whose occupied cells fall outside `[0,BW)×[0,BH)` — it treats
out-of-range coordinates as unsigned, so a negative `bx`/`by` wraps to a large
value and trips `cmp …,#BW/#BH; bhs pc_yes` (blocking the move), and
`dostris_lock` (`dostris.inc.s:259`) only writes cells of an already-validated
resting pose, so `g_board` indices stay in `[0,120)`. This is correct today, but
it is an *implicit* invariant with no bound check at the render/lock sites
(`draw_piece_cells:600`, `dl_loop:279`): a future change to spawn position,
board size, or the collision test could silently turn a piece plot into an OOB
`g_board`/VRAM write. **Fix:** add an explicit `bx<BW && by<BH` guard at the lock
and cell-plot sites (cheap insurance), independent of P/S clipping.

*Checked and clean:* boot state init (`kernel.s:120-126` zeroes 200 words =
IWRAM `VARS` through `g_board`, so the board and all state start cleared — no
uninitialised-read analog to the SNES port); `dostris_init` re-clears the board
and reseeds (`dostris.inc.s:364`); `rng` mod-7 (`dostris_spawn:310`, bounded
subtraction on a byte); grid navigation (`sel_up/down/left/right`,
`kernel.s:520-559`, wrap/clamp keep `v_sel` in `[0,NICONS)`); `draw_app` dispatch
(`apps.inc.s:55`, unmatched app ids fall to `app_generic`, which indexes
`icon_lbl` with the in-range `v_app`); all generated-table indices — `piece_masks`
(28 hwords, index `type*4+rot` ∈ [0,28)), `piece_tiles` (7 bytes, `type` ∈ [0,7)),
`icon_data` (11×256, `idx` ∈ [0,11)), `theme_pals` (4×16, `v_theme` ∈ [0,4)),
`music_song` (`MUSIC_COUNT`, `m_idx` wrapped) — are all within the sizes
`mkdata.py` emits; `clk_str`/`numstr` writes (≤ 9 / ≤ 3 bytes into their
reserved IWRAM slots); `collapse_row`/`row_full` (`dostris.inc.s:437`, `414`,
strictly `BW`-bounded copies).

---

## Suggested order of work
1. **P1 + P2** — word-packed / DMA fills and skipping empty Dostris cells; the
   two together remove most per-redraw cost.
2. **B1** — 3-digit line counter (trivial display fix).
3. **S1 + S2 + B2** — add clipping to `frect`/`pchar`/`picon` and an explicit
   in-bounds guard at the Dostris lock/plot sites; defence-in-depth that also
   satisfies S1/S2.
4. **P3** — stride `frect` instead of recomputing per row.

Build/test note: the port assembles/links via WSL (`arm-none-eabi-as` +
`-ld` + `objcopy`, `build.sh`) and runs under mGBA; that toolchain is not
installed in this audit environment, so the above is proposed, not
compiled/applied.
