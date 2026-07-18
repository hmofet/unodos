# UnoDOS `ws` code audit — performance, bugs, security

Scope: the **ws** world (Bandai WonderSwan, NEC V30MZ, 16-bit real-mode nasm) —
`build.sh` default `build/unodos.ws` (64 KB ROM mapped at segment `0xF000`). Sources:
`kernel.asm`, `apps.inc`, `dostris.inc`, generated `build/ws_data.inc` /
`build/ws_equ.inc`. (`min.asm` is a separate boot-smoke ROM, not the OS.)

Threat model for the security section: bare-metal V30MZ, `DS=ES=SS=0` over the 16 KB
internal RAM (`kernel.asm:122-126`), tile map at `0x0800`, tile patterns at `0x2000`,
work vars at `0x0100`, Dostris board at `0x0300`. **No memory protection**; every
stray store corrupts internal RAM or mis-programs an I/O port. **No untrusted external
input**: runtime inputs are the keypad (port `0xB5`, `read_keys` `kernel.asm:369`) and,
in `AUTOTEST` builds, a ROM `auto_script`. Security is short by design. Line numbers
are exact.

Three areas, most-actionable first.

---

## 1. Performance

### P1 — Every piece landing does a full-map chrome redraw + full board repaint
On a lock, `dostris_update` raises `v_dirty` (`dostris.inc:334`); the main loop runs
`full_redraw` (`kernel.asm:538`) → `draw_app` → `draw_chrome` (`apps.inc:27-48`, which
`fill_map`s the entire 32×32 tilemap as 1024 `stosw` plus two `fill_row`s) then
`dostris_draw` (`dostris.inc:377-461`, repainting walls + floor + all `BW*BHT`=150
board cells + piece + score). This fires at every landing. The board already has a
partial path (`draw_piece_partial` via `pf_pc`, `dostris.inc:505`). **Fix:** repaint
only the board region after `dostris_lock`/`clearlines` instead of raising `v_dirty`;
reserve `full_redraw` for launcher↔app switches.

### P2 — already correct: clock work is gated on the Clock app
Unlike the `gb`/`pce`/`vic20` ports, `clock_advance` (`kernel.asm:551-576`) sets
`pf_clk` **only** when `v_inapp && v_app==1` (`kernel.asm:569-574`) and never formats
when the clock isn't visible. This is the pattern the other three ports should adopt;
no change needed here. (Noted so the omission isn't mistaken for a miss.)

### P3 — `fill_map` rewrites the full 32×32 map though only 28×18 is visible
`fill_map` (`kernel.asm:291-297`) `rep stosw`s all 1024 entries on every
`draw_chrome`/`draw_launcher`; the visible window is 28×18 = 504 cells, so ~50% of the
writes are off-screen. Cheap per-write (`stosw`), so minor, but trivially halved by
filling only the visible rectangle. Largely subsumed by P1 for the Dostris path.

---

## 2. Security — device-reachable memory corruption

**No untrusted-input surface.** Runtime input is the keypad (`read_keys`,
`kernel.asm:369`) or the ROM `auto_script` (`apps.inc:302`). Latent / defensive only:

### S1 — LOW (latent) — Dostris board/cell writes rely solely on prior collision validation
`dostris_lock` writes `[g_board+bx]` with `bx = cell_y*BW + cell_x`
(`dostris.inc:198-203`) and `draw_piece_cells` (`dostris.inc:476-502`) writes tilemap
cells the same way, using coordinates only bounds-checked by the preceding
`piece_collide` (`19-58`). The write loops carry no independent bound. Note `g_board`
is only `BW*BHT`=150 bytes at `0x0300` (immediately after the `0x0100-0x02FF` var
block), so an OOB board index would corrupt adjacent work RAM. Safe today because
lock/draw reuse the mask `piece_collide` checked. **Fix (defensive):** clamp
`cell_x<BW`,`cell_y<BHT` before the `g_board` store.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `put2` assumes value ≤ 99; `g_lines` is a word with no cap
`put2` (`kernel.asm:268-281`) does a single `div bx` (bx=10), so it only produces two
digits: for values ≥100 the "tens" tile becomes `value/10 > 9`, i.e. a wrong glyph.
`g_lines` is a word incremented per cleared line with no cap (`dostris.inc:221`) and
passed to `put2` in `dostris_draw_score` (`dostris.inc:471-472`). ≥100 lines →
mis-rendered count. Cosmetic. (`draw_music_status`'s `put2(MUSIC_COUNT)` is fine — 30.)
**Fix:** cap the displayed value at 99, or widen to three digits.

### B2 — LOW (latent) — music note-timer underflow on a zero-frame entry (word-wide)
`music_tick` does `dec word [m_timer]; jnz` (`apps.inc:236`); `music_load`
(`apps.inc:227-229`) loads the frame byte zero-extended into the word `m_timer`. A
song entry with a 0 frame count underflows to a **65535**-frame stuck note — worse
than the 8-bit ports' 256. Generated `music_song` currently uses nonzero frames.
**Fix:** clamp the loaded timer to ≥1 in `music_load`.

*Checked and clean:* `piece_collide` correctly uses **signed** compares
(`dostris.inc:29-41`, `jge`/`jl` with `cmp ax,0`), so the `g_px=0 → g_tx=-1` left-move
is properly rejected — this is the reference the 6502/SM83 ports emulate via unsigned
wrap. Also clean: the boot var clear (`kernel.asm:128-131`, `0x0100-0x02FF`); the
`g_board` (`0x0300`+150) vs VARS/`MAP1`(`0x0800`)/`TILES`(`0x2000`) layout — no overlap;
`sel_up`/`sel_down` wrap (`kernel.asm:449-471`); `mapaddr` addressing
(`kernel.asm:229-240`, `(row*32+col)*2 + MAP1` within the 2 KB map);
`collapse_row`/`row_full` BP-preservation (`dostris.inc:230-277`, the outer
`dostris_clearlines` loop counter survives both calls); `draw_piece_partial`
erase-then-redraw (`dostris.inc:505-533`); and the music wavetable build
(`apps.inc:199-204`, 16 bytes into `WAVE`).

---

## Suggested order of work
1. **P1** — board-only repaint on lock.
2. **P3** — fill only the visible 28×18 in `fill_map`.
3. **B1, B2** — display cap and zero-frame guard (note the word-wide underflow).
4. **S1** — defensive index clamp before the `g_board` store.

(P2 is already handled correctly here.)

Building note: the `ws` target assembles with nasm (`build.sh`, header checksum patched
in Python) and is verified on a Unicorn x86-16 core (`harness.py`, PNG captures in
`build/`). Fixes are proposed, not applied — audit-only pass.
