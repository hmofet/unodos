# UnoDOS `vic20` code audit — performance, bugs, security

Scope: the **vic20** world (Commodore VIC-20, MOS 6502 + VIC 6560/6561, dasm) —
`build.sh` default `build/unodos.prg` (a `+8K`-expanded VIC-20, PRG at `$1201`).
Sources: `kernel.s`, `apps.inc`, `dostris.inc`, generated `vic_data.inc`.

Threat model for the security section: bare-metal VIC-20; the PRG **is** the program
(BASIC stub `10 SYS 4109`, `kernel.s:108-115`) — it is not a loader that ingests
untrusted content at runtime. The only runtime inputs are the joystick (`$9111`/
`$9120`, `read_pad` `kernel.s:384`) and, in `AUTOTEST` builds, a ROM `auto_script`.
Direct stores to screen RAM (`$1E00`), colour RAM (`$9600`) and the char set (`$3000`)
with **no memory protection**. Security is therefore short — latent/defensive footguns
only. Line numbers are exact.

Three areas, most-actionable first.

---

## 1. Performance

### P1 — Every piece landing does a full clear + full board repaint (and it can tear)
On a lock, `dostris_update` raises `v_dirty` (`dostris.inc:183-184`); the main loop
runs `full_redraw` (`kernel.s:531`) → `draw_app` → `draw_chrome` (which calls
`clear_screen`, `apps.inc:31-32` / `kernel.s:357`) → `dostris_draw`, repainting walls
+ floor + all `BW*BH`=120 board cells + piece + score (`dostris.inc:368-480`). This
fires on every landing. Unlike the console ports, **the VIC has no display-off**:
writes go straight to the live `$1E00` matrix, so a full board repaint mid-frame can
tear/flicker on top of the CPU cost. The board already has a partial path
(`draw_piece_partial` via `pf_pc`, `dostris.inc:546`). **Fix:** after
`dostris_lock`/`clearlines`, repaint only the board cells (via the existing partial
path) instead of raising `v_dirty`; keep `full_redraw` for launcher↔app switches.

### P2 — `clock_format` runs every wall-second regardless of the visible app
`clock_advance` calls `clock_format` and sets `pf_clk` unconditionally at each second
rollover (`kernel.s:568-570`); the string is only consumed when the Clock app is open
(`render_partials`→`rp_clk` gates on `v_app==1`, `kernel.s:512-517`). **Fix:** gate
the `clock_format`/`pf_clk` on `v_inapp && v_app==1` (as the `ws` port does).

### P3 — `clear_screen` writes 512 bytes for a 506-cell screen
`clear_screen` (`kernel.s:357-365`) clears `$1E00-$1FFF` (512 bytes) on every
`full_redraw`/`draw_chrome`, though the matrix is 22×23 = 506 cells; only a 6-byte
tail is wasted, so this is essentially fine and noted only for completeness.

---

## 2. Security — device-reachable memory corruption

**No untrusted-input surface.** Inputs are the joystick (`read_pad`, `kernel.s:384`)
and the ROM `auto_script` (`apps.inc:373`). Latent / defensive only:

### S1 — LOW (latent) — Dostris board/cell writes rely solely on prior collision validation
`dostris_lock` writes `g_board,y` (`board_off`, `dostris.inc:230-232` / `37-48`) and
`draw_piece_cells` (`dostris.inc:514-545`) writes screen/colour cells using
coordinates only bounds-checked by the preceding `piece_collide` (`49-81`). No
independent clamp in the write loops. Safe today (shared mask); a future
mask/geometry desync would corrupt work RAM (`g_board` at `$0340`) or write outside
the screen matrix. **Fix (defensive):** clamp `g_bx<BW`,`g_by<BH` before `board_off`.

### S2 — LOW (latent) — `copy_charset` over-copies past the char set
`copy_charset` (`kernel.s:160-179`) copies `>(NCHARS*8)+1` = 7 pages (1792 bytes) but
`NCHARS*8` = 1624 (`NCHARS=203`, `vic_data.inc:2`). The extra 168 bytes over-read the
ROM tail after `charset_data` (benign) and land in the unused `$36xx` char-set area
(no clobber of code/screen). Not corrupting today, but the `+1`-page round-up is a
footgun if the char set ever grows to abut live data. **Fix:** copy exactly
`(NCHARS*8+255)/256` pages, or byte-count the tail.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `two_digits` assumes value ≤ 99; `g_lines` is uncapped
`two_digits` (`kernel.s:263-272`) loops `sbc #10`; inputs ≥100 push the tens character
past `'9'`. `g_lines` (zero page `$36`) is incremented per cleared line without a cap
(`dostris.inc:291`) and passed to `two_digits` in `dostris_draw_score`
(`dostris.inc:495-500`). ≥100 lines → non-digit tens glyph. Cosmetic. **Fix:** cap at 99.

### B2 — LOW (latent) — music note-timer underflow on a zero-frame entry
`music_tick` does `dec m_timer; bne` (`apps.inc:280-281`); a `music_song` entry with a
0 frame count underflows `m_timer` to a 256-frame stuck note. `vic_data.inc` currently
uses nonzero frames. **Fix:** clamp the loaded timer to ≥1 in `music_load`
(`apps.inc:266-276`).

*Checked and clean:* the board-cell inner loop in `dostris_draw` has a redundant `clc`
before `sta g_n` (`dostris.inc:437-439`) — a harmless dead flag op, not a bug. Also
clean: `sel_up`/`sel_down` wrap (`kernel.s:450-471`); the music progress bar
(`apps.inc:315-334`, max offset 17 → col ≤19 within the 22-wide row); `clear_screen`
coverage of the 506 live cells (`kernel.s:357-365`); `cellptr` `row_off`-table
addressing (`kernel.s:190-209`); `collapse_row`/`row_full` (`dostris.inc:282-349`);
the `draw_content` `dcptr` +4 advance (`kernel.s:232-261`); and the `theme_bg` table
(`apps.inc:245-246`, 4 entries indexed by `v_theme < NTHEMES`).

---

## Suggested order of work
1. **P1** — board-only repaint on lock (also removes the mid-frame tearing).
2. **P2** — gate `clock_format`/`pf_clk` on the Clock app.
3. **B1, B2** — display cap and zero-frame guard.
4. **S1, S2, P3** — defensive clamps and the char-set copy count.

Building note: the `vic20` target assembles with dasm (`build.sh`) and is verified on a
py65 6502 core (`harness.py`, PNG captures in `build/`). Fixes are proposed, not
applied — audit-only pass.
