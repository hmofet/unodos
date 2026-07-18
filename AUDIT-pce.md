# UnoDOS `pce` code audit — performance, bugs, security

Scope: the **pce** world (NEC PC Engine / TurboGrafx-16, HuC6280 + HuC6270 VDC,
ca65 `--cpu huc6280`) — `build.sh` default `build/unodos.pce`. Sources: `kernel.s`,
`apps.inc`, `dostris.inc`, generated `pce_data.inc`.

Threat model for the security section: bare-metal HuCard boot, HuC6280 with the MPR
banking set up once at reset (`kernel.s:120-139`), 8 KB work RAM, VDC VRAM behind the
`$2000` port window. **No memory protection**; every stray store corrupts work RAM or
mis-programs the VDC directly. **No untrusted external input**: runtime inputs are the
joypad (`JOY $3000`) and, in `AUTOTEST` builds, a ROM `auto_script`. Security is short
by design — latent/defensive footguns only. Line numbers are exact.

Three areas, most-actionable first.

---

## 1. Performance

### P1 — Every piece landing does a full display-off BAT clear + full board repaint
On a lock, `dostris_update` raises `v_dirty` (`dostris.inc:193-194`); the main loop
runs `full_redraw` (`kernel.s:768`), which does `display_off` (`773`) + `clear_bat`
(`774`) + `draw_app` → `dostris_draw`. `clear_bat` pushes **all 1024 BAT entries one
at a time through the single VDC data port** (`kernel.s:292-310`), and `dostris_draw`
then repaints walls + floor + all `BW*BH`=120 board cells + piece + score via the same
per-cell `putcell` path (`dostris.inc:378-500`). This is the heaviest recurring cost,
and it fires on every landing. The board already has a partial path
(`draw_piece_partial` via `pf_pc`, `dostris.inc:537`). **Fix:** repaint only the board
region after `dostris_lock`/`clearlines` instead of raising `v_dirty`; reserve
`full_redraw` for launcher↔app transitions.

### P2 — `clock_format` runs every wall-second regardless of the visible app
`clock_advance` calls `clock_format` and sets `pf_clk` unconditionally at each second
rollover (`apps.inc:178-181`), but the result is only consumed when the Clock app is
open (`render_partials`→`rp_clock` gates on `v_app==1`, `kernel.s:735-742`). So the
format work runs every second in the launcher and every other app. **Fix:** gate the
`clock_format`/`pf_clk` on `v_inapp && v_app==1` (as the `ws` port does).

### P3 — `clear_bat` is per-cell through the VDC port, and clears all 1024 entries
`clear_bat` (`kernel.s:292-310`) writes every BAT entry with a `VDC_DL`/`VDC_DH` port
pair — the slowest possible fill — and covers the full 32×32 even though the launcher/
apps immediately overwrite the live region. Largely subsumed by P1, but if a
standalone clear is still wanted, restrict it to the visible 32×28 or use a tighter
inner loop. Minor relative to P1.

---

## 2. Security — device-reachable memory corruption

**No untrusted-input surface.** Runtime input is the joypad (`read_pad`,
`kernel.s:549`) or the ROM `auto_script` (`apps.inc:398`). Nothing reaches a memory
write from an untrusted source. Latent / defensive only:

### S1 — LOW (latent) — Dostris board/cell writes rely solely on prior collision validation
`dostris_lock` writes `g_board,y` with `y` from `board_off` (`g_by*BW+g_bx`,
`dostris.inc:241-243`, `board_off` at `38-49`) and `draw_piece_cells`
(`dostris.inc:502-535`) writes BAT cells the same way, using coordinates only
bounds-checked by the preceding `piece_collide` (`51-86`). The write loops carry no
independent bound and `board_off` has no clamp. Safe today (shared mask), but any
future desync between `piece_masks`/geometry and `BW/BH/BORG_*` would corrupt
zero-page-adjacent work RAM (`g_board` is at `$0400`) or write bad VDC addresses.
**Fix (defensive):** clamp `g_bx<BW`,`g_by<BH` before `board_off`.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `two_digits` assumes value ≤ 99; `g_lines` is uncapped
`two_digits` (`kernel.s:785-794`) loops `sbc #10`, so inputs ≥100 push the tens
character past `'9'`. `g_lines` (zero page `$36`) is incremented per cleared line with
no cap (`dostris.inc:307`) and passed to `two_digits` in `dostris_draw_score`
(`dostris.inc:486-491`). ≥100 lines → non-digit tens glyph. Cosmetic. **Fix:** cap the
displayed value at 99.

### B2 — LOW (latent) — music note-timer underflow on a zero-frame entry
`music_tick` does `dec m_timer; bne` (`apps.inc:312-313`); a `music_song` entry with a
0 frame count underflows `m_timer` to a 256-frame stuck note. `pce_data.inc` currently
uses nonzero frames. **Fix:** clamp the loaded timer to ≥1 in `music_load`
(`apps.inc:285-308`).

*Checked and clean:* the launcher grid row arithmetic is duplicated — inline in
`draw_launcher` (`kernel.s:508-513`, `grp*7+3` for the icon) and in `label_pos`
(`kernel.s:426-449`, `grp*7+5` for the label). They agree (icon row +2 = label row),
so `draw_highlight` (`kernel.s:534-544`) repaints exactly on the launcher's label
rows — verified consistent, though the duplicated math is fragile and worth
centralizing. Also clean: `sel_left/right/up/down` wrap (`kernel.s:640-681`);
`clear_bat` count (4×256 = 1024, `kernel.s:300-309`); the 16-bit word counter in
`load_tiles` (`kernel.s:253-289`, copies exactly `NTILES*32` bytes); the music bar
(`apps.inc:344-359`, `min(m_idx,16)+1 ≤ 17` cells from col 6 within the 32-wide BAT);
`clk_str` sizing (`apps.inc:185-204`); `collapse_row`/`row_full` (`dostris.inc:297-374`);
and `vdc_cell` address math (`kernel.s:313-343`, `row*32+col` within the 1024-entry BAT).

---

## Suggested order of work
1. **P1** — board-only repaint on lock.
2. **P2** — gate `clock_format`/`pf_clk` on the Clock app.
3. **P3** — trim/optimize `clear_bat`.
4. **B1, B2** — display cap and zero-frame guard.
5. **S1** — defensive index clamp in `board_off`.

Building note: the `pce` target assembles with ca65/ld65 (`build.sh`) and is verified
headlessly (`harness.py`, PNG captures in `build/`). Fixes are proposed, not applied —
audit-only pass.
