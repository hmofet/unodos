# UnoDOS cross-port PERFORMANCE-fix pass — handoff / status

Continuation doc for the audit "option b" (fix the P# findings, per port). The
authoritative work-list is every **`AUDIT-<port>.md` §1** plus **`AUDIT-INDEX.md`
cross-cutting pattern #1** (no damage-rect → full-scene repaint). Each fixed
finding carries a **✅ FIXED (`<commit>`)** note in its audit.

**Hard rule (unchanged):** "it assembles" is NOT enough. Every fix is a redraw
REFACTOR → **render-verify** each one: build the `AUTOTEST` variant, render, and
pixel-diff against a baseline built from the pre-change source (git-stash the edit
→ build → render → pop → build → render → `cmp`). A correct refactor is
**byte-identical**. Apply per port, verify, **commit per port**, push (pull first —
other agents share this branch).

Repo: `github.com/hmofet/unodos`, branch `pc64-usb-flasher`.

## Verify infrastructure (all working on this box = amanuensis)

- **retro-shot** (`tools/retro-shot/`, see its README) — headless libretro render
  → PPM/PNG, no display/root. Cores at `~/uno-emu/cores` in WSL
  (`genesis_plus_gx` → genesis/sms/gg, `snes9x` → snes, `gambatte` → gb); frontend
  at `~/uno-emu/retro_shot`; `ppm2png.py` copied alongside it. Drive via
  `tools/retro-shot/shot.sh <core> <rom> <out.png> [frames]`.
- **dreamcast** — Flycast. This box has it at `C:\Users\arin\dc-tools\flycast`
  (Windows, HLE BIOS): it boots the DC **`.elf` directly** (`build/unodos-dc.elf`);
  the genisoimage `.iso` fallback does NOT boot in it. (The repo's
  `dreamcast/tools/emu_run.sh` drives Flycast under Xvfb+llvmpipe on Linux — the
  flycast *libretro* core SIGILLs, don't use it.)
- **ps2** — PCSX2 at `C:\Users\arin\ps2-tools\pcsx2`. Use the **windowed** runner
  (`ps2/tools/run_pcsx2_windowed.ps1`; a scratch copy is in the session dir):
  fullscreen-exclusive reads black over RDP.
- **mac** — Retro68 at `/opt/Retro68`/`/opt/Retro68-build`; `mac/shots/runshot.sh`
  runs Executor under WSLg. (mac P1 has no visible delta → build + boot is enough.)

**Toolchains** (present): WSL Ubuntu 24.04 (12 cores/31 GB). Cross-asm/link runs
on **Windows** (`vasmm68k_mot.exe`, `sjasmplus.exe`, `snes-tools/{ca65,ld65}.exe`,
`/usr/local/ps2dev`), rendering runs in **WSL** — both see the `/mnt/c` repo.
**KOS** built at `/opt/toolchains/dc` (`source .../kos/environ.sh` before `dc`
builds). **Retro68** built. `dasm`/`cc65`/`arm-none-eabi` present.

> **RDP capture caveat** (`~/.claude/CLAUDE.md`): this box is driven over RDP; when
> the session **disconnects**, `CopyFromScreen` fails ("handle invalid") and GPU
> emulators (PCSX2, Flycast) can't be screendumped. retro-shot is unaffected
> (headless). For DC/PS2, verify by build + crash-free boot + a byte-identical
> argument (or reconnect RDP for the windowed capture).

## DONE — this pass (all render-verified byte-identical unless noted, one commit/port)

The **drag / damage-rect** refactors — the AUDIT-INDEX pattern #1 headline. Each
adds a clip window (default = whole screen, an exact no-op) gating the cell-write
primitives, then clips the full repaint to `union(old_rect, new_rect)`:

| Port | Finding | Commit | Verify |
|------|---------|--------|--------|
| genesis | §1 P1 drag | `facd5f8` | byte-identical (`build.sh drag`, retro-shot) |
| sms | §1 P1 drag | `6a5396a` | byte-identical (`build.sh test` drag) |
| snes | §1 P2 drag | `a4bc3aa` | byte-identical (`build.sh drag`, snes9x) |
| sms | §1 P2 app-content ticks | `f4aebe8` | byte-identical (dostris + music autotests) |
| mac | §1 P1 clock_tick gate | `9cd96e3` | builds (Retro68); no visible delta |
| dreamcast | §1 P1 dirty-row-band present (subsumes P2) | `b040917` | host algorithm test byte-identical + Flycast boot |
| ps2 | §1 P1 no per-frame draw_window | `810914a` | builds + executes on PCSX2 (no EE exceptions) |

Mechanics worth reusing:
- **Clip window** (genesis/sms/snes): `v_clip_*` cells + `clip_reset`; `fill_cells`
  clamps its rect, `draw_str` does row-skip + left/right clip, single-cell writers
  (`draw_icon_cell`/`put_cell`/`draw_char`) test the point. `handle_drag`/`drag_move`
  sets the clip to `union(old,new)` before the repaint, resets after.
- **sms P2** `dirty_proc(proc)` clips a content-only redraw to that app's window
  (redraw_all still composites bottom-up within the clip → z-order stays correct).
- **dreamcast** the dirty band is derived by **comparing fb[] to the previous
  frame** (byte-identical by construction, no per-primitive instrumentation needed —
  fb[] is written from both `fb.c` and the `mac_compat` shim); cursor old/new rows
  are folded in since the cursor overlays VRAM, not fb[].
- Bugs the verify caught that a build would not: genesis `.move` d3 loop-counter
  clobber; snes 8-bit-mode `clip_reset` + S2-scratch collision with `draw_window`.

## DONE — follow-up batch (contained double-tick / redraw fixes, pushed)

| Port | Finding | Commit | Verify |
|------|---------|--------|--------|
| dreamcast | §1 P3 draw (topmost repainted every frame) | `2133f6a` | builds (KOS) + boots (Flycast); byte-identical (same as ps2 P1) |
| mac | §1 P2 focused tick runs twice | `e2f93bb` | builds (Retro68); TickCount-gated → byte-identical |
| ps2 + dreamcast | double-tick (`tick_all_apps` skip topmost) | `9ce7f50` | both build; byte-identical |

**Deferred — genuinely low-value / risky (each deprioritised in its own audit):**
- **ps2 §1 P2** — full GS texture re-upload every frame. The audit calls the
  upload hardware-cheap (~66 MB/s vs >1 GB/s); a safe skip is blocked by gsKit's
  `TexManager` per-frame eviction (skipping `gsKit_texture_upload` can leave the
  texture evicted → garbage). Not worth the risk for a sub-1% win.
- **snes §1 P4** — NMI DMAs the full 2 KB tilemap. DMA is a few µs of hardware
  time; "secondary to P1–P3" per the audit. A dirty-row-span hint is possible
  (the clip bounds already know the damaged rows) if it's ever wanted.

## DONE — this session (per-app incremental redraws + a real bug, all pushed)

| Port | Finding | Commit | Verify |
|------|---------|--------|--------|
| snes | **gravity bug** — Dostris was frozen: `dt_interval` clobbers the elapsed-time scratch in `S0` (the div16→hardware-divider rewrite leaves the remainder there), so the throttle never fired | `65cf383` | retro-shot: piece now descends/locks/stacks (frozen before) |
| snes | §1 P3 — Dostris incremental piece redraw (erase old row / draw new, mirrors `pm_redraw_actors`); lock still full-repaints | `57597ec` | byte-identical (`build.sh dostris`, snes9x, frames 400–6000) |
| c64 | §1 P1 — incremental **Files** nav (repaint 2 rows, not the whole bitmap) | `55f7bdd` | byte-identical (`harness.py`, spaced keys) |
| c64 | §1 P1 — incremental **Theme** nav | `e0f8ce2` | byte-identical (`harness.py`) |
| c64 | §1 P1 — incremental **Tracker** cursor (2-cell) | `1c923e5` | byte-identical (`harness.py`) |
| genesis | §1 P2 — cull non-damaged windows from the clipped drag repaint | `f845166` | byte-identical (`build.sh drag`/`test`, genesis_plus_gx) |
| genesis | §1 P3/P4 (raise path) — raise_window → `redraw_topmost`, not `repaint_all` | `24b0683` | byte-identical (`build.sh raise`, genesis_plus_gx) |
| **amiga** | **deterministic headless renderer** `uae/render.py` (Unicorn 68000; unblocks all amiga byte-verify — WinUAE can't) | `7831ae7` | two runs of one build are byte-identical |
| amiga | §1 P1 (raise+create) — raise_window/win_create → `redraw_top2`, not `repaint_all` | `41c05e6` | byte-identical (`render.py`, `build.sh test` + `test RAISE`) |

**The amiga render path is the reusable unlock here.** `uae/render.py` is a
frame-deterministic Unicorn-68000 harness (hunk-loads `build/UnoDOS68K_test`,
fakes just enough hardware — Supervisor `jmp (a5)` stub, free-running beam for the
splash's `splash_wait_frames`, TBE-ready serial, `ticks` auto-advanced on read so
`fdd_delay` returns; the cursor it also drives is a sprite, not in the bitplanes —
halts at a new `UNODOS68K: ATDONE` serial marker before `main_loop`, reads the 5
bitplanes at `$60000` + copper palette at `$76000` → PNG). Any amiga draw refactor
is now verified by rendering pre/post `AUTOTEST` builds and `cmp`-ing, same
discipline as the retro-shot ports. **This retires the "amiga is unverifiable"
blocker from the prior session.**

The **redraw-top-N pattern** (amiga `redraw_top2`, genesis raise `redraw_topmost`):
a raise/create makes a window topmost, so — when a port has no active-title
restyle, or restyles only the previously-topmost — the sole visible deltas are
that window on top (+ the old topmost going inactive). Redraw just those 1–2
windows instead of the full clear+desktop+all-windows `repaint_all`; nothing is
newly exposed, so it's byte-identical. **close still needs the full repaint** (it
must reveal occluded content — that one wants a clipped damage-rect; on amiga the
windows sit at arbitrary pixel-x so it means pixel-column clipping of
`fill_rect`/`draw_char`/`draw_icon16`).

**New autotest scaffolding added** (needed for the above): snes `build.sh dostris`
(`AUTOTEST_DOSTRIS`); amiga `build.sh test RAISE` (`AUTOTEST_RAISE`) + the
`UNODOS68K: ATDONE` marker; genesis `build.sh raise` (`AUTOTEST_RAISE`); c64 nav
scripts drive `harness.py` directly with `wait`s between keys (see below).

### Verification lessons this session (READ before touching a shared draw path)
- **`harness.py` key timing (c64):** the kernel edge-detects keys; a *slow* full
  redraw overruns the harness's short key-release window (`KEY_INSTRS//4`), so a
  following identical key isn't seen as a new edge and is **dropped** — a fast
  (incremental) build catches it, and the two builds then reach different states.
  Put a `wait 3` **between every key** so both builds register each edge; then the
  renders compare byte-for-byte.
- **c64 renders under Windows Python** (py65 lives there), not WSL:
  `python3 c64/harness.py <prg> <artdir> [--storage x.sav] < script`.
- **genesis (and likely sms/other 68k) main loop is cycle-timing-sensitive:** a
  live game's state at frame N depends on per-frame *render cost*, so **any**
  `draw_window`/draw-primitive edit — even inserting NOPs — shifts the **dostris**
  autotest frame. The already-shipped P1 `facd5f8` perturbs it too. ⇒ Verify
  draw-path changes on the **settled `drag`/`test` scenes**, never against a live
  game. (snes/c64 are frame-deterministic — a faster redraw stays byte-identical —
  which is why snes P3 verified cleanly.)
- **amiga can't be byte-verified here:** the only renderer is WinUAE (GUI, AROS
  ROM). Two runs of the *same* build produce different captures (24 s wall-clock
  boot lands at different guest states; det test: `det_1` was still blank). No
  frame-deterministic headless 68k renderer exists for amiga (unlike the snes
  Unicorn harness / c64 py65 harness). **amiga §1 P1 is blocked on verification**,
  not on the fix. A frame-deterministic ADF-level renderer (or a host algorithm
  test à la the dreamcast P1 commit) would unblock it.

## DONE — earlier sessions (per the prior handoff, already pushed)
- `pf_bd` board-only Dostris-lock repaint on **vic20 / pce / gg / gb** (gb under
  `lcd_off`); **gba** `frect` two-px-per-`str`; **snes** `div16` via the CPU
  hardware divider ($4204/6→$4214/6). The retro-shot rig. Security + low-severity
  correctness fixes.

## REMAINING — not done (prioritised)

**Highest value — the P1 full-scene-repaint damage-rects still open:**
- **rpi / pinephone / ppcmac** — each `AUDIT §1 P1`: "full-screen clear + repaint
  on every dirty event." These are bare-metal **AArch64 / PPC asm**; same
  damage-rect shape as genesis/sms but in the asm redraw path. Biggest remaining
  wins. (Also each has P2 busy-spin `wait_vblank`, P3 per-word `frect`, P4 static
  wall/floor repaint.) *(pinephone/ppcmac share the core the user is doing rpi
  with — coordinate / mirror rpi.)*
- **amiga** §1 P1 — ✅ **raise+create done** (`41c05e6`, verified via the new
  `uae/render.py`). **close remains**: it must reveal what the closed window
  occluded, so it needs a clipped damage-rect repaint (add a clip window to
  `fill_rect`/`draw_char`/`draw_icon16`; windows are at arbitrary pixel-x so the
  text/icon clip is pixel-column, not byte — the fiddly part). Now verifiable with
  `render.py` — build an `AUTOTEST` close scene and `cmp`.
- **c64** §1 P1 — ✅ **done for Files/Theme/Tracker** this session (`55f7bdd`,
  `e0f8ce2`, `1c923e5`). **Notepad deferred**: append-only editor, no cursor-nav /
  highlight — every keystroke changes `note_len` and can tail-scroll the whole
  view, so there's no "two rows changed" case; a byte-identical incremental would
  have to replicate the wrap/scroll layout for low, per-keystroke value.

**Tails on the ports already touched this pass** (dreamcast P3, mac P2, the
ps2/dc double-tick, **snes P3**, **c64 P1 (3 apps)** and **genesis P2** are ✅ done
above; ps2 P2 + snes P4 ⏸ deferred low-value above):
- **snes** P3 OutLast — the road/HUD still rebuilds every `outlast_tick`; same
  delta-tracking idea as the Dostris fix (`57597ec`), unverified for OutLast yet.
- **genesis** P3/P4 — ✅ **raise path done** (`24b0683`: `redraw_topmost`, no
  `clear_screen`/no 11-icon `draw_desktop`). Only the **close** path still
  `repaint_all`s; a clipped close (genesis already has the drag clip window from
  P1) would finish P3/P4 — verify on a static-window close scene (a live game
  scene is cycle-fragile; see the lesson above).
- **sms** P3 (full redraw display-ON → raster tear; blank R1) + P4 (redundant
  `clear_names`). sms defers its repaint via a `v_dirty` flag (raise_window just
  sets it — already lighter than genesis), so the genesis raise trick doesn't
  port directly; these are the display-blank / redundant-clear micro-wins.
- **tile-port clock micro** (gb/gg/…): `clock_advance` already reformats only on a
  second-change and sets `pf_clk`; the residual is `app_clock` calling
  `clock_format` again on every clock-window draw. Removing it needs `clk_str` to
  be initialised before the first draw — check the boot path first. Tiny.
- **genesis** P2 (don't repaint every window's *content* on a chrome-only rebuild),
  P3 (skip the redundant `clear_screen`), P4 (stop redrawing the 11 static icons).
  Lower value — these are one-shot `repaint_all` events (raise/close), not per-frame.
- **sms** P3 (full redraw runs display-ON → raster tear; blank R1), P4 (redundant
  `clear_names` — subsumed once genesis-style content damage lands).
- **ps2** P3 (`oval_span` long-multiplies per pixel — low value).
- **mac** P3 (busy loop, no idle sleep), P4–P6 (chrome-every-frame, no damage
  tracking, `TextWidth` re-measure).

**How to tackle the per-app incremental redraws (snes P3, c64 P1, etc.):** copy
the incremental model the same port's Pac-Man already uses (`pm_redraw_actors` /
old-vs-new tile tracking) — draw only the cells that changed since last frame,
not the whole board/list. Byte-identical because the final cells are unchanged;
verify with that port's game/nav autotest (frame-driven, so fixed and baseline
reach the same state at frame N — no live-clock timing skew to dodge).

**Per-port lock/redraw + micro items** (mostly P2–P4, lower value): see each
`AUDIT-<port>.md §1` — c64 P2/P3/P4, gb P2/P3, gba P2/P3, gg P2, nes P1/P2, pce
P2/P3, vic20, and the `clock_format`-every-wall-second micro on the tile ports.

**On hold:** **iigs P1** — skip per the user.

## Models to copy
- Damage-rect / clip: the genesis+sms+snes work above is now the in-repo
  reference; prior models are **amiga** (XOR outline), **macplus** (damage-min),
  **x86** (full damage-rect).
- Dirty-band present without per-primitive flags: the **dreamcast** compare-to-prev
  approach.
