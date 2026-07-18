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
  wall/floor repaint.)
- **c64** §1 P1 (full-screen `app_clear` on every redraw, even cursor moves) and
  **amiga** §1 P1 (`repaint_all` full-scene on raise/close/create). 6502 / 68k.

**Tails on the ports already touched this pass:**
- **genesis** P2 (don't repaint every window's *content* on a chrome-only rebuild),
  P3 (skip the redundant `clear_screen`), P4 (stop redrawing the 11 static icons).
- **sms** P3 (full redraw runs display-ON → raster tear; blank R1 or shrink to a
  vblank-sized damage list), P4 (redundant `clear_names`).
- **snes** P3 (whole-board/road game redraws each tick — track the board delta like
  Pac-Man does), P4 (NMI DMAs the full 2 KB tilemap for any 1-cell change → DMA
  only the dirty row span).
- **ps2** P2 (full framebuffer DMA'd to the GS every frame — damage-rect the GS
  upload), P3 (`oval_span` long-multiplies per pixel).
- **dreamcast** P3 (focused window ticks + redraws 2–3× per frame — drop the
  double-tick).
- **mac** P2 (focused `tick()` runs twice/iteration), P3 (busy loop, no idle
  sleep), P4–P6 (chrome-every-frame, no damage tracking, `TextWidth` re-measure).

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
