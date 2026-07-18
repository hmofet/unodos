# UnoDOS `dreamcast` code audit — performance, security, correctness

Scope: the **dreamcast** port — the KallistiOS/SH-4 target built by `./build.sh dc`
(→ `build/unodos-dc.elf`) / `build.sh cdi`. The shipping image is the portable core
`unodos.c` + Mac-compat shim (`mac_compat.c`/`mac_io.c`) over the software
framebuffer `fb.c`, presented and driven by the DC-only platform layer `dc_main.c`,
with the 11 apps loaded from CD as relocatable `.KLF` modules (`dc_modload.c` +
`apps/*.c` over `apps/uno_mod.h`).

Threat model for the security section — **honest and narrow**: bare-metal SH-4 with
**no memory protection** (an OOB write corrupts RAM directly; an OOB read can fault),
but this build has **no network stack** (no BBA/`ax88179` driver is compiled in) and
**no real removable-media parser reachable at runtime** (the FAT12 `.Sony` floppy
device does not exist on a Dreamcast, and the RAM-image fallback is `UNO_AUTOTEST`-only
— see B1). The only live external inputs are the **maple bus** (controller / keyboard
/ mouse) and, in principle, the **VMU** and the **CD** itself. The maple translation
is a fixed, bounded mapping; the VMU File-Manager path is currently unreachable
(B1/B2); the CD carries the OS image and its own app modules. So there is effectively
**no untrusted-input memory-corruption surface** in this build — the security section
is latent/defensive only. `long` is **32-bit** on SH-4 (ILP32), which matters below.

Line numbers are exact against the current tree.

---

## 1. Performance

The DC steady-state per-frame cost is dominated by the present path, not the software
renderer: UnoDOS draws incrementally (event-driven partial repaints, XOR drag
outlines), so most frames only touch a small region of `fb[]` — but the present throws
that away and reprocesses the whole screen, every frame, unconditionally.

### P1 — Full-screen framebuffer→RGB565 conversion every present, no dirty tracking (biggest win)
`dc_main.c:243-250` (`uno_dc_present`). The loop `for (i = 0; i < n; i++) vram[i] =
to565(fb[i])` with `n = FB_W*FB_H = 307200` (line 246-248) converts and copies the
**entire 640×480 framebuffer** to VRAM on every present — regardless of how little of
`fb[]` actually changed. There is no damage/dirty-rect tracking anywhere (confirmed:
`fb.c`/`unodos.c` carry no dirty state). Idle desktop, a blinking caret, a one-tile
Pac-Man step, a rubber-band drag outline — all pay the full ~307k-pixel conversion.
**Fix:** have the fb primitives (or the drawing wrappers in `unodos.c`) record a
min/max dirty row band (or a rect), and in `uno_dc_present` convert/upload only those
rows. This is the single largest per-frame reduction available.

### P2 — `uno_dc_present` is called unconditionally every main-loop iteration
`unodos.c:1610` (`uno_dc_present();` at the tail of the `for(;;)` loop). Even on a
completely idle frame — no event, no tick redraw — the loop still runs the full P1
conversion at 60 Hz (`vid_waitvbl()` at `dc_main.c:247` paces it). Combined with P1
this is *constant* full-screen work whether or not anything moved. **Fix:** set a
global `g_dirty` when any primitive writes `fb[]`, and skip the convert/upload (still
`vid_waitvbl` to pace) when nothing was drawn — or fold this into the P1 dirty-rect so
an empty rect uploads nothing.

### P3 — Focused window ticks and redraws up to twice per frame
`unodos.c:1602-1603`. `tick_all_apps()` (`unodos.c:1002-1007`) dispatches `tick()` to
**every** open window including the topmost; then `post_ticks()` (`unodos.c:984-987`)
posts a tick to the topmost window's task, and the kernel-driven `task_post`
(`unodos.c:974-980`) dispatches `tick()` **again** and unconditionally calls
`draw_window(&gWins[slot])` (line 979). So the focused app's `tick()` runs twice, and
because most app `tick()`s (`clock_tick`, `pacman_tick`, `music_tick`, …) also call
`draw_window` themselves, the topmost window can be fully repainted 2–3× per frame.
Internal `TickCount()` gating makes the *second* game step usually a no-op, but the
redundant `draw_window` still repaints the whole window. **Fix:** drop the topmost
window from `tick_all_apps()` (let `post_ticks` own it), or drop the `draw_window`
from the kernel-driven `task_post` and let the single per-frame present pick up the
already-drawn `fb[]`.

### P4 — `to565` per-pixel, called through the hot loop
`dc_main.c:57-61` + `:248`. `to565` is `static inline` (fine), but P1's loop still does
per-pixel masks/shifts across the whole screen. Subordinate to P1 — once only dirty
rows are converted this cost drops proportionally; no separate change needed beyond P1.

*(Already good: the drag is an XOR outline — `drag_update`/`xor_outline`,
`unodos.c:1248-1284` — so dragging does **not** trigger `repaint_all`; only mouse-up
does. `repaint_all` (full desktop + all windows) fires only on launch/close/raise/
drop, not per frame. `fb.c` primitives all clip to the screen before writing.)*

---

## 2. Security

**No untrusted-input memory-corruption surface exists in this build.** No network
driver is compiled in; the maple input translation is a fixed bounded mapping; the
FAT12/VMU storage paths that would parse external bytes are unreachable at runtime
(B1/B2). The items below are latent/defensive only.

### S1 — LOW / latent — CD `.KLF` module loader trusts the disc image
`dc_modload.c:194` (`library_open(libname, path)`) hands each `/cd/UNODOS/APPS/APPNN.KLF`
to KOS `elf_load`, which applies the module's own SH relocations and resolves its
undefined symbols against the kernel/`gUnoExports` tables (`dc_modload.c:91-115`). A
modified disc could supply a `.KLF` that relocates and executes arbitrary SH-4 code.
This is inherent to a homebrew load-from-CD design (burning the disc already implies
running arbitrary code) and matches every console port's module story — noted for
completeness, not actionable beyond "the disc is the trust boundary."

### S2 — LOW / latent — VMU File-Manager read path (currently unreachable)
`mac_io.c:193-224` (`FSOpen` DC/VMU backend). If it were wired to the apps (it is not —
see B1), the read path is already bounded: the slurp size is clamped to `VMU_MAX`
(`mac_io.c:206`) and `malloc`'d, and every app read goes through `fat12_read`/`FSRead`
which cap to the caller's buffer. So even a crafted oversized VMU file cannot overflow.
No fix required; listed so the reachability gap (B1) is not mistaken for a latent
overflow.

### S3 — LOW / latent — first-poll controller edge synthesis
`dc_main.c:137` (`edge = now & ~g_prev_cont;` with `g_prev_cont` init 0 at `:53`). On
the very first `poll_controller`, any button already held reads as a fresh press
(spurious Return/Esc/arrow). Not a memory issue and self-correcting after one frame;
defensive-only.

*Checked and clean:* the maple readers (`poll_controller`/`poll_mouse`/`poll_keyboard`,
`dc_main.c:128-212`) only index fixed KOS status structs and clamp the cursor to
`FB_W`/`FB_H`; the keyboard path admits only `27/13/10/8/9` and printable `32..126`
(`dc_main.c:205-211`); `overlay_cursor` (`dc_main.c:226-241`) bounds-checks every pixel;
`to565` masks/shifts are correct (`dc_main.c:57-61`); `p2c`/`c2p` are length-capped
(`mac_io.c:35-50`); `fat_get`/`fat_set` bound the FAT offset before access
(`unodos.c:541,549`); the FAT12 sector reads land in the fixed `gFatSec[512]` and the
RAM device rejects out-of-range LBAs (`unodos.c:443`).

---

## 3. Correctness / robustness bugs

### B1 — MEDIUM — Storage is non-functional on real DC hardware; the VMU backend is never invoked
The module apps reach storage **only** through the KernelApi `fat12_*` callbacks
(`apps/uno_mod.h:28-31`; e.g. `notepad_save`→`fat12_write` `apps/notepad.c:111`,
`paint` `apps/paint.c:433/440`, `tracker` `apps/tracker.c:124-125`, `files`
`apps/files.c:51`). The kernel's `fat12_mount` (`unodos.c:522-534`) first tries
`fat_dev_sony`, which immediately fails on DC — `PBReadSync` returns `-19` for the
`.Sony` refNum `-5` (`mac_io.c:336`) — then falls back to the RAM image **only** under
`#if defined(UNO_AUTOTEST) || defined(UNO_AUTOTEST_FAT12)` (`unodos.c:526-532`).
Interactive `build.sh dc`/`cdi` define neither, so `fat12_mount` **always returns
false** and every save/load silently no-ops.

Meanwhile the VMU File-Manager backend that the README/HANDOFF describe
(`mac_io.c:193-322`, `FSOpen`/`FSRead`/`FSWrite`/`Create` over `/vmu/a1`) is **never
called** by the module build — its only callers are `tools/unodos_orig_dc.c` (a
reference copy that is not compiled into any target). So Files/Notepad/Tracker/Paint
persistence does not work on real hardware, contrary to the documentation. **Fix:**
either route the KernelApi `fat12_*` surface to the VMU backend, or compile the FAT12
RAM fallback into the interactive DC build (drop the `UNO_AUTOTEST` guard at
`unodos.c:526`), so mounted storage actually exists.

### B2 — LOW — VMU `FSOpen` opens an existing file read-only, so overwrite-save appends instead of truncating (latent)
`mac_io.c:203-218`. `FSOpen` decides read-vs-write purely by whether `fopen(path,"rb")`
succeeds: an existing file becomes a **read** handle (`writing=0`, `len=filesize`,
`pos=0`). A subsequent `FSWrite` then appends at `v->len` (`mac_io.c:268-269`) rather
than truncating, so re-saving an existing name grows the file with stale tail bytes
instead of overwriting it. Unreachable today (B1), but a real bug if the VMU path is
wired up. **Fix:** treat `Create` as marking the name write/truncate, or reset
`len=0`/`pos=0` and `writing=1` on the first `FSWrite` to a handle.

### B3 — LOW — 32-bit integer overflow in `oval_span` for large ovals (latent)
`mac_compat.c:196-203`. `nx = (2*xx-cx2)*b` reaches `(w-1)*(h-1)`; `nx*nx` and
`rr = ((long)a*b); rr*=rr` (line 197) therefore overflow signed 32-bit `long` once the
oval exceeds ~46340 px² in `(w-1)(h-1)` (e.g. anything approaching full-screen). All
current callers — desktop icon glyphs, the clock face, Paint tool glyphs
(`unodos.c:1109`, `apps/paint.c` toolglyphs) — draw only small ovals, so the overflow
is not hit; Paint's canvas ovals use its own `pt_oval_shape` (`apps/paint.c:165-188`).
Latent UB. **Fix:** compute the ellipse test in `long long`, or scale down before
squaring.

### B4 — LOW — `TickCount()` is a call-counter, not a wall clock
`mac_compat.c:230` (`long TickCount(void){ return ++gTicks; }`). Every timing decision
— `now_secs()` uptime (`unodos.c:191`), music/tracker tempo, game step rates,
double-click window — is measured in *calls to TickCount*, not real 60 Hz ticks. The
comment calls this a "deterministic call-clock," which is correct and desirable for the
host screenshot harness, but on interactive DC it means uptime/tempo/animation speed
track loop-call frequency rather than seconds. It happens to stay roughly proportional
to frames (present is `vid_waitvbl`-paced), but it is not wall-clock accurate. **Fix
(DC-specific, optional):** back `TickCount` with a KOS timer (`timer_ms_gettime` → 60ths)
in the DC build so `/60` yields real seconds.

*Checked and clean:* `apps/notepad.c` insert/backspace (guarded `memmove`, insert
gated by `gNLen < NBUF-1`, `:162-169`); `apps/paint.c` flood fill (explicit `PT_STK`
stack-depth guard `:206-221`, and every write goes through `pt_set_px`'s bounds check
`:118-123`); tracker cells loaded from storage stay display-safe (note index is
`(n%12)*2+1 ≤ 23` into `kTkNoteNames[25]`, `apps/tracker.c:60-72`); pacman/dostris/
outlast board indexing (all masked or bounds-tested — `dt_fits` `apps/dostris.c:39-45`,
`pm_walkable` `apps/pacman.c:58-62`, `ol_vrect` clamps x to `[0,OL_W]` `apps/outlast.c:65-74`);
`fmt_u`'s `char tmp[12]` (`unodos.c:195`) is adequate here — `long` is 32-bit on SH-4,
so at most 10 digits; `put2`/`clock_draw` buffers fit (`apps/clock.c:8-16`); `p2c`/`c2p`
length-capped; `fat_free_slot`/`fat_find`/`fat12_read` offset-bounded; the maple/cursor
paths (see §2).

---

## Suggested order of work
1. **B1** — wire storage so it actually mounts/persists on hardware (the port's headline
   claim currently does not hold); decide FAT-RAM vs VMU backend and connect it.
2. **P1 + P2** — dirty-rect the fb→565 present and gate it on a dirty flag: the single
   biggest interactive-framerate win, turning constant full-screen conversion into
   "touch a few rows."
3. **P3** — remove the double tick/redraw of the focused window.
4. **B2** — fix VMU overwrite-vs-append once B1 makes the path live.
5. **B3, B4, S3** — latent overflow, real-clock `TickCount`, first-poll edge (hardening).

Building/testing note: the DC target needs KallistiOS (`sh-elf-gcc` + libkos) and a
Dreamcast/emulator (Flycast) to build and run — neither is on this Windows dev box, so
none of the above was compiled or run here. Findings are from source inspection against
the current tree; fixes are proposed, not applied.
