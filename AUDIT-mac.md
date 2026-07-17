# UnoDOS `mac` code audit — performance, bugs, security

Scope: the **hosted classic Mac OS** port (`mac/`) — the two Toolbox applications
`UnoDOS7` (System 7, Color QuickDraw, 68020+) and `UnoDOSClassic` (System 1–6,
1-bit QuickDraw, 68000), built from `unodos.c` + `mac_modload.c` + the eleven
`apps/*.c` modules loaded at runtime as `'Uapp'` code resources (`CMakeLists.txt`,
`app_loader.c`).

**Threat model (hosted, *not* bare-metal).** Unlike the `macplus` port, this one
runs **as an application on top of classic Mac OS**: the ROM Toolbox owns the
screen, Event Manager, QuickDraw, File/Sound/Resource Managers. UnoDOS relies on
the host OS for all I/O and never touches hardware directly. There is still **no
memory protection** on classic Mac OS — an out-of-bounds write corrupts the
application heap / low memory just like on bare metal — but the *reachable*
attack surface is much smaller than pc64:

- **Trusted:** the app's own `'Uapp'` code resources (loaded by `GetResource`,
  part of the signed app bundle), the keyboard/mouse (local user), and the HFS
  boot volume.
- **Untrusted:** the **PC-compatible FAT12 data volume** — boot sector,
  directory entries, FAT, and file contents on a floppy/disk the user inserts
  and mounts via *Files → `V`*. This is the only externally-supplied byte stream
  the code parses. Files opened from it flow into Notepad/Paint/Tracker buffers.
- **No network, no USB, no font parsing, no JS, no HTML** — none of the pc64
  remote-corruption surface exists here.

`long` is **32-bit** on Retro68/m68k (and `int` is 16-bit); several findings
differ from pc64 precisely because of that. Line numbers are exact against the
current tree.

---

## 1. Performance

The renderer draws QuickDraw primitives straight into one full-screen GrafPort;
there is no offscreen GWorld and no damage tracking (a documented limitation).
That is acceptable for discrete events (open/close/raise/drag-commit). The real
waste is in the **per-frame** path — the main event loop and the app tick
handlers — and it is fixable cheaply.

### P1 — Clock redraws its whole window on *every* main-loop iteration
`apps/clock.c:20-24` (`clock_tick`) unconditionally calls `draw_window(w)` — full
chrome (shadow, border, titlebar, pinstripes, `TextWidth`-centred title) **plus**
`clock_draw` — every time it is ticked. It is ticked once per `tick_all_apps()`
(`unodos.c:1500`), and the main loop (`unodos.c:1474-1504`) has **no frame gate**,
so this fires as fast as the loop spins (thousands/sec) while the displayed value
(`now_secs()`) changes only once per second. `app_secondly()` (`unodos.c:1307-1317`)
*already* repaints Clock/SysInfo once a second, so `clock_tick`'s redraw is pure
waste and a flicker source. Every other tick handler is `TickCount`-gated
(`music.c:128`, `tracker.c:132`, `dostris.c:106`, `pacman.c:174`,
`outlast.c:203`) — Clock is the one that is not.
**Fix:** gate `clock_tick` on `now_secs()` changing (or delete its draw and let
`app_secondly` own it).

### P2 — The focused app's `tick()` runs twice per loop iteration
`unodos.c:1500` (`tick_all_apps` — dispatches `tick()` for **all** windows) is
immediately followed by `post_ticks()` (`unodos.c:1501`) → `task_yield()`
(`1502`) → `task_body` (`unodos.c:889` `app_tick_dispatch`), which dispatches the
**topmost** window's `tick()` a second time in the same iteration. So the focused
app pays its tick cost twice every frame. It is currently only *wasteful* (all
tick bodies are `TickCount`-gated so the second call early-returns — see B2), but
it is redundant work on the hot path.
**Fix:** dispatch ticks through exactly one path — either the direct
`tick_all_apps()` or the per-task mailbox, not both.

### P3 — Busy main loop: `GetNextEvent` + no idle sleep = 100% CPU
`unodos.c:1475` uses `GetNextEvent` (which does **not** yield the CPU) and the
loop body (`1474-1504`) never sleeps, so UnoDOS spins a full core even when
nothing is animating and no window is open. On cooperative classic Mac OS this
starves everything else (DAs, background apps, the Finder).
**Fix:** use `WaitNextEvent` with a sleep quantum, dropping the sleep to ~1 tick
only while a window is actively animating (game/tracker/music playing or a drag
in progress).

### P4 — Animating windows repaint full chrome every frame
Game/tracker/music tick handlers redraw via `draw_window(w)` (e.g.
`dostris.c:92`, `pacman.c:175`, `music.c:133-134`), and `draw_window`
(`unodos.c:260-331`) re-renders the drop shadow, border, titlebar fill,
pinstripes, close box, and re-runs `strlen`+`TextWidth` to re-centre the title
(`unodos.c:320-328`) — none of which changed since last frame. For a ~15–30 fps
game only the content rect changes.
**Fix:** split window chrome from content; have per-frame app redraws call a
content-only repaint and leave chrome alone until a WM event (raise/drag/close).

### P5 — No damage tracking; `repaint_all` is full-scene
`repaint_all` (`unodos.c:335-341`) repaints the whole desktop (background + all
`NICONS` icons via `draw_desktop`) and every window on every raise, close, theme
change, drag-commit (`drag_update` mouse-up, `unodos.c:1255`) and topmost-swap.
These are discrete, so lower priority than P1–P4, but a cached desktop background
+ a damage list would make window operations instant. (The drag itself is already
cheap — it moves an XOR gray outline, `unodos.c:1221-1257`, not a live repaint —
so dragging is *not* the problem here, unlike pc64.)

### P6 — `text_at_max` re-measures with `TextWidth` on every shrink, every redraw
`unodos.c:179-182` loops `TextWidth(...) > maxw` decrementing `len` one char at a
time; the Files listing (`apps/files.c:38`) calls it per row, and the list
redraws in full on every arrow key (`files.c:54-56`). O(name-length) `TextWidth`
calls per row per keypress. Minor; cache the measured width or binary-search the
fit.

*(Already good: the rubber-band drag is an XOR outline with a move-delta early-out
`unodos.c:1245-1250`; all non-Clock tick handlers are `TickCount`-gated; only the
topmost animating window drives redraws.)*

---

## 2. Security — reachable memory corruption

**There is no untrusted-input memory-corruption surface reachable in this port.**
The one externally-supplied byte stream is the **FAT12 data volume**, and its
parser is genuinely hardened (and cross-checked by `mac/test_fat12.py`). Concretely:

- `fat12_read` (`unodos.c:683-707`) clamps the directory-supplied file size to the
  caller's `max` (`694`) and every `memcpy(buf+got, …, take)` obeys
  `take = min(512, size-got)` with `got ≤ size ≤ max`, so it cannot overrun the
  caller buffer (`gNBuf[4096]`, `gPtCanvas[PT_W*PT_H]`, `gTkPat[256]`), which all
  pass their true capacity as `max`.
- A malformed 32-bit size read as signed `long` (0x8000_0000…) goes negative and
  the copy loop simply does nothing (`while got<size` false) — no over-read.
- Cluster walks are bounded (`cl>=2 && cl<0xFF8`) and terminate even on a cyclic
  FAT because `got` increases monotonically toward `size` (`unodos.c:695-705`).
- `fat_get`/`fat_set` bound every FAT offset against the cached FAT size
  (`unodos.c:551`, `559`); reads past the cache return EOC.
- `fat_83_to_name` (`unodos.c:618-627`) expands an 11-byte on-disk name to at
  most `8 + '.' + 3 = 12` chars + NUL and writes into `gFatNames[x][13]` — fits.
- `fat12_list` caps the listing at `FAT_LIST_MAX` **before** each write
  (`unodos.c:674`); `fat_find`/`fat_free_slot` keep the entry offset in
  `[0,480]`, so `gFatSec+off+28` stays inside the 512-byte sector buffer.
- The RAM block device rejects out-of-range LBAs (`fat_dev_ram`, `unodos.c:453`);
  the `.Sony` path only ever reads the attacker's *own* physical disk.
- Loaded app code comes from `GetResource('Uapp', …)` (`mac_modload.c:44-65`) —
  a trusted resource in the app bundle, null-checked before call.
- The cooperative scheduler never preempts, and all FAT work runs to completion
  without `task_yield`, so the shared `gFatSec[512]`/`gFatCache` buffers are never
  re-entered mid-operation.

The remaining items are **LOW / latent / defensive only** — no corruption today:

### S1 — LOW (robustness) — boot-sector 16-bit fields stored in signed `short`
`unodos.c:508-510`: `gFatRootEnts`, `gFatTotSec`, `gFatFatSz` are read with the
unsigned `fat_rd16` but stored in signed `short`. A field ≥ 0x8000 becomes
negative. It is *contained* — a negative `gFatFatSz` makes `NewPtr` fail and mount
returns false (`unodos.c:516-517`); a negative `gFatRootEnts` makes the root-scan
loop bound negative so listings come up empty — but it is wrong-behaviour, not a
clean rejection, and a **legal** large FAT12 (32768–65535 sectors, i.e. 16–32 MB
with big clusters) drives `gFatTotSec` negative and corrupts `fat_alloc`'s free-
cluster count (`unodos.c:573-574`) on the *write* path. No OOB. **Fix:** hold these
as `unsigned short`/`long` and validate geometry against the device size.

### S2 — LOW (defensive) — `gFatNames[x][13]` has zero margin
`unodos.c:423` sizes the name array at exactly 13, and `fat_83_to_name` fills all
13 on a worst-case name (see above). Correct today, but any later shrink of the
field, or adding a suffix, overflows into `gFatSizes`. **Fix:** size to 16 and/or
bound the writer explicitly.

### S3 — LOW (latent) — Theme dereferences the mono NULL palette
`apps/theme.c:22-71` reads/writes `kPalette` (= `gK->palette`), which is `NULL`
on the mono build (`app_loader.c:50`). Not reachable — Theme is color-only
(`NICONS = 10` on mono, `unodos.c:83`, so its icon/keyboard index is unreachable)
— but a NULL-deref if the module were ever dispatched on mono. **Fix:** guard, or
don't ship the Theme resource in the mono variant.

---

## 3. Correctness / robustness bugs

### B1 — MEDIUM — unsigned disk geometry truncated to signed `short`
Same root as S1, stated as a correctness defect: `unodos.c:508-510` (and the
derived `gFatRootStart`/`gFatDataStart` `short` arithmetic at `512-513`, and
`fat_alloc`'s cluster-count at `573-574`) all use signed 16-bit for values the
FAT12 spec defines as unsigned 16-bit. Breaks mount/allocation on legal large
FAT12 volumes; harmless on the 1.44 MB (2880-sector) disks the port targets today.
**Fix:** unsigned types + a geometry sanity check.

### B2 — LOW — double tick dispatch can double-advance a future ungated handler
The focused window's `tick()` is dispatched twice per loop iteration (P2:
`unodos.c:1500` and via `unodos.c:1501-1502` → `task_body` `unodos.c:889`). Every
current tick body is `TickCount`-gated so the second call early-returns, and
`clock_tick` only draws (no state) — so no logic is double-stepped **today**. It
is a latent trap: any tick handler added without a `TickCount` gate will advance
twice as fast on the focused window as in the background. **Fix:** single dispatch
path (see P2).

### B3 — LOW (display) — `put2` renders only two digits; hours/values > 99 wrap
`unodos.c:211` (`put2`) computes `(v/10)%10` and `v%10`, so any argument ≥ 100
shows only the low two digits. The Clock uses it for the hours field
(`apps/clock.c:10`), so uptime rolls over visually at 100 h. No buffer issue (it
always writes exactly 3 bytes). **Fix:** use `fmt_u` for the hours field, or
document the 99 h cap.

### B4 — LOW (latent) — no per-window clip region on app draws
`draw_window` (`unodos.c:330`) calls `draw_app_content` without setting a clip
rect to the window bounds; apps draw into the shared full-screen port and rely on
their own coordinate math to stay inside their frame (e.g. `outlast.c:65-74`
`ol_vrect` clamps X to `[0,OL_W]` but not Y). A coordinate bug in any app scribbles
over neighbouring windows / the desktop instead of being clipped. Visual only (no
memory corruption — QuickDraw clips to the port), but it defeats the topmost-only
repaint model. **Fix:** `ClipRect` to `w->bounds` around `draw_app_content`.

### B5 — LOW — partial mount failure leaves geometry globals set
`fat12_mount_dev` (`unodos.c:500-528`) writes `gFatSecPerClus`/`gFatFatSz`/… as
side effects *before* the `NewPtr`/FAT-read that can fail (`516-523`); on failure
it returns false with `gFatMounted` still false, so the stale globals are gated
off — but a later successful mount relies on overwriting every one of them.
Currently safe; brittle. **Fix:** compute into locals, commit to globals only on
success.

### B6 — LOW (latent) — OutLast reads road edges before the first frame sets them
`apps/outlast.c:212` reads `gOlRoadL`/`gOlRoadR` in `outlast_tick`, but they are
only assigned in `outlast_draw` (`outlast.c:119`). If a tick lands before the
first state-1 draw, it uses the static defaults (`100`/`220`, `outlast.c:38`) —
benign, but a real read-before-write ordering dependency. **Fix:** set the edges
in `ol_new_game`.

*Checked and clean:*
- **`fmt_u` (`unodos.c:203-210`) does NOT reproduce pc64's B1** — Retro68 `long`
  is 32-bit, so the maximum is 10 digits and `char tmp[12]` is sufficient; all
  callers (`gDtScore`, `gPmScore` = `200L<<gPmKills` with `gPmKills≤3`,
  `gOlScore`, `gFatSizes`, `now_secs`) stay within 32 bits.
- Notepad insert/backspace/arrow bounds (`apps/notepad.c:135-176`) — every
  `memmove` is capacity-guarded (`gNLen < NBUF-1`), caret clamped to `[0,gNLen]`,
  load clamps to `NBUF-1`.
- Paint flood fill (`apps/paint.c:191-224`) — explicit `NewPtr` stack capped at
  `PT_STK`, pushes guarded by `n < PT_STK`, max index `2*PT_STK-1` inside the
  `PT_STK*4`-byte block; all pixel writes go through the bounds-checked
  `pt_set_px` (`paint.c:118-123`); drag/spray coords clamped to canvas.
- Dostris/Pac-Man/OutLast/Tracker board & table indexing — piece index masked to
  `[0,6]` (`dostris.c:37`), line-score index `≤4` (`dostris.c:63`), maze access
  bounds-checked in `pm_walkable` (`pacman.c:58-62`), mode-duration index clamped
  (`pacman.c:104`), tracker cells masked (`tracker.c:69,153`), curve/tree tables
  read with fixed loop bounds.
- Files/SysInfo/Music/Theme text buffers (`files.c:18` `line[64]`,
  `sysinfo.c:8` `line[24]`, `outlast.c:80` `hud[48]`, `theme.c:24` `line[48]`) —
  all comfortably larger than their worst-case `fmt_u`/`strcat` content.
- 68000 context switch (`unodos.c:857-867`) and task stack setup
  (`unodos.c:908-920`) — pre-decrement stack fill, 11 saved regs match the
  `movem` frame, killed tasks are gated out of the scheduler and their stacks
  cleanly reused.

---

## Suggested order of work
1. **P1 + P3** — gate `clock_tick` and switch the loop to `WaitNextEvent`; kills
   the 100%-CPU idle spin and the biggest redraw waste for near-zero risk.
2. **P2 / B2** — collapse the double tick dispatch to one path.
3. **B1 / S1** — unsigned FAT12 geometry + a device-size sanity check (the only
   finding that touches untrusted input, even if only latent).
4. **P4 + P5** — chrome/content split and a cached desktop background for the
   remaining redraw cost.
5. **B3, B4, B5, B6, S2, S3, P6** — display, clipping, and latent hardening.

Building/testing note: this port needs the Retro68 m68k cross-toolchain
(`build.sh`, `R68=…`) and runs under the Executor emulator (`run.sh`); neither is
installed on this Windows host, so nothing here was compiled — the findings are
from source review, with the FAT12 core independently cross-checked by
`mac/test_fat12.py`. All fixes are proposed, not applied.
