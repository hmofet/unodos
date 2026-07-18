# UnoDOS `gb` code audit — performance, bugs, security

Scope: the **gb** world (Nintendo Game Boy / Game Boy Color, Sharp SM83, rgbds) —
`build.sh` default target `build/unodos.gb`. Sources: `kernel.asm`, `apps.inc`,
`dostris.inc`, generated `gb_data.inc` / `build/tiles.bin`.

Threat model for the security section: bare-metal DMG/CGB, one 32 KB ROM, flat 8 KB
WRAM at `$C000`, **no MMU / no memory protection**. Every out-of-bounds store is a
direct WRAM/VRAM corruption primitive. **There is no untrusted external input**: the
only runtime inputs are the joypad (`$FF00`) and, in `AUTOTEST` builds, a ROM-authored
script table. So the Security section is short by design — it records latent/defensive
footguns, not reachable attacks. Line numbers are exact against the current tree.

Three areas, most-actionable first.

---

## 1. Performance

### P1 — Every piece landing triggers a full-screen LCD-off repaint
On each gravity/soft-drop lock, `dostris_update` sets `v_dirty = 1`
(`dostris.inc:234`), which the main loop turns into `full_redraw`
(`kernel.asm:218`). `full_redraw` (`kernel.asm:680`) does `lcd_off`
(`kernel.asm:685`) + `clear_map` (a 1024-byte BG-map clear, `kernel.asm:286-295`) +
`draw_app` → `dostris_draw`, which repaints the two walls, the floor, **all `BW*BH`
= 120 board cells**, the active piece and the score (`dostris.inc:454-559`). This
runs at every landing, i.e. multiple times a second during play. But the board
already has a per-frame partial path (`draw_piece_partial` via `pf_pc`,
`dostris.inc:623`); only the newly-locked cells and any cleared rows actually change
on a lock. **Fix:** after `dostris_lock`/`clearlines`, repaint just the board region
(reuse the vblank partial machinery or a board-only tile blit) instead of raising
`v_dirty`; keep `full_redraw` for launcher↔app switches only.

### P2 — `clock_format` runs every wall-second regardless of the visible app
`clock_advance` (called from the main loop every frame, `kernel.asm:213`) calls
`clock_format` and sets `pf_clk` **unconditionally** each time the second rolls over
(`apps.inc:123-125`). But the formatted string is only consumed when the Clock app is
open (`render_partials`→`rp_clock` gates on `v_app==1`, `kernel.asm:644-651`). So
`two_digits`×3 plus the 9 `clk_str` stores execute every second even in the launcher
or any other app. **Fix:** only `clock_format` + set `pf_clk` when
`v_inapp && v_app==1` (the `ws` port already does exactly this).

### P3 — `clear_map` clears the full 32×32 map though only 20×18 is visible
`clear_map` (`kernel.asm:286-295`) writes all 1024 BG-map entries on every
`full_redraw`; the visible area is 20×18, so ~43% of the writes are to off-screen
cells. Minor next to P1. **Fix:** clear only `col<20 / row<18`, or rely on the
subsequent draw to overwrite the live region.

---

## 2. Security — device-reachable memory corruption

**No untrusted-input surface.** The only runtime inputs are the joypad read in
`read_pad` (`kernel.asm:479`) and, in test builds, the ROM-resident `auto_script`
(`apps.inc:364`). Nothing device- or network-reachable reaches a memory write. The
items below are **latent / defensive only**.

### S1 — LOW (latent) — Dostris board/tile writes rely solely on prior collision validation
`dostris_lock` writes `g_board[by*BW+bx]` via `board_off` (`dostris.inc:293-296`,
`board_off` at `47-62`) and `draw_piece_cells` writes map cells (`dostris.inc:587-621`)
using coordinates that were only bounds-checked by the preceding `piece_collide`
(`75-114`). The write loops themselves have **no independent bound**. This is safe
today because lock/draw reuse the same mask `piece_collide` validated, but a future
desync between `piece_masks`/`piece_tiles` (`gb_data.inc:13-22`) and the
`BW/BH/BORG_*` constants would turn these into direct WRAM (`board_off`) or VRAM
(`xy_to_hl`) out-of-bounds writes. **Fix (defensive):** clamp `bx<BW`,`by<BH` in
`board_off` and clamp `col/row` to the map in `xy_to_hl` (`kernel.asm:297-314`).

---

## 3. Correctness / robustness bugs

### B1 — LOW — `two_digits` assumes value ≤ 99; `g_lines` is uncapped
`two_digits` (`kernel.asm:372-382`) subtracts 10 in a loop, so for inputs ≥100 the
tens character advances past `'9'`. `g_lines` is a 1-byte WRAM counter with no cap
(`dostris.inc:368` increments it per cleared line) and is fed straight to `two_digits`
in `dostris_draw_score` (`dostris.inc:570-572`). At ≥100 cleared lines the tens digit
renders as a non-digit glyph (and the byte wraps mod 256). Cosmetic, but reachable.
**Fix:** cap the displayed value at 99, or render three digits.

### B2 — LOW (latent) — music note-timer underflow on a zero-frame entry
`music_tick` does `dec [m_timer]; ret nz` (`apps.inc:263-265`). A `music_song` entry
whose frame count is 0 underflows `m_timer` to a 256-frame stuck note. The current
`gb_data.inc` song uses only 26/52-frame notes (`gb_data.inc:24-56`, all nonzero), so
it cannot fire today, but a regenerated table with a 0 would. **Fix:** treat 0 as 1 in
`music_load` (`apps.inc:233-257`), or guarantee nonzero frames in `mkdata.py`.

*Checked and clean:* the unsigned wall check in `piece_collide` (`dostris.inc:87`
`cp BW` / `98` `cp BH`) correctly rejects the `g_px=0 → g_tx=$FF` left-edge case
because `$FF ≥ BW` — no signedness bug (contrast the `ws` port, which uses explicit
signed compares for the same case). Also clean: launcher nav wrap (`sel_up`/`sel_down`
`kernel.asm:564-586`, range `0..NICONS-1`); `clk_str` sizing (`ds 9` holds
`"HH:MM:SS"`+NUL exactly, `apps.inc:129-152`); the music progress bar
(`apps.inc:295-313`, `min(m_idx,16)+1 ≤ 17` tiles from col 2 stays inside the 20-wide
row); the boot WRAM clear (`kernel.asm:169-177`, covers `$C000-$DFFF`);
`collapse_row`/`row_full` indexing (`dostris.inc:356-448`); and the `draw_content`
table walk (`kernel.asm:329-344`).

---

## Suggested order of work
1. **P1** — board-only repaint on lock (the one the eye actually notices).
2. **P2** — gate `clock_format`/`pf_clk` on the Clock app being open.
3. **P3** — trim `clear_map` to the visible region.
4. **B1, B2** — display cap and zero-frame guard.
5. **S1** — defensive index clamps in `board_off`/`xy_to_hl`.

Building note: the `gb` target assembles with `rgbasm`/`rgblink`/`rgbfix`
(`build.sh`) and is verified headlessly (`run.ps1` + PNG captures in `build/`).
The fixes above are proposed, not applied — this was an audit-only pass.
