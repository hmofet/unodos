# Sony PS2 (FreeMcBoot) port — implementation handoff (for Claude Sonnet)

Audience: the engineer building the UnoDOS/PS2 port, milestone by
milestone (M0–M3). Authoritative direction:
[../docs/PORTS-PLAN.md](../docs/PORTS-PLAN.md) §4; platform contract:
[../docs/PORT-SPEC.md](../docs/PORT-SPEC.md). House method per
milestone: code + scripted/AUTOTEST regression + screenshots + commit
+ README; update this file + PORTS-PLAN at each close.

**The big picture:** UnoDOS/PS2 is an ELF launched by FreeMcBoot with
full hardware access — "firmware-hosted bare-metal," the richest
target in the family. The strategy is **port the C core**:
[../mac/unodos.c](../mac/unodos.c) already contains the complete
UnoDOS — all 11 apps, the window manager, the event model, the
cooperative scheduler, and a device-abstracted FAT12 core — written
against classic Mac Toolbox calls. The PS2 work is a *platform layer
swap*, not a rewrite. Most of the porting risk lives in §2's design
decision and §3's rig; the apps come along nearly free.

---

## M0 STATUS (2026-06-14)

**Done + committed:** the software-framebuffer platform layer (`fb.c`/`fb.h`
— 640×448×32 + fill/frame/invert/text/scaled-text over the 4-colour gamut),
the shared font as a C array (`mkfont_c.py` → `build/font_data.h`), the
hello-GS splash (`uno_splash.c`), and the host shim (`host_main.c` +
`tools/ppm2png.py`). The **§2 design is confirmed** (software FB, GS as a
blitter). The splash is **rendered + screenshotted on the PC** via
`./build.sh host` (WSL gcc) — `shots/m0_splash.png`.

**Toolchain installed, EE ELF builds:** prebuilt ps2dev v2.0.0 under WSL at
`~/ps2dev/ps2dev` (Docker was unavailable on this machine; the prebuilt
release is the recipe — see README). `./build.sh ee` links a real MIPS R5900
ELF (`build/unodos-ps2.elf`, gsKit/libpad). The §1 audit is effectively done
(see §1 note below).

**Blocked:** running the ELF. PCSX2 needs a **4 MB PS2 BIOS**; the BIOS folder
provided held only **512 KB PS1 BIOSes** (PS1 ≠ PS2). So `main.c`'s GS/pad
runtime is unverified — needs a PS2 BIOS dump (e.g. scph39001 ≈ 4 MB) + PCSX2,
or FMCB hardware. The §3 PCSX2 rig recipe is therefore still unverified.

**Next: M1** — bring `unodos.c` up on the host shim first (no PS2 hardware
needed). §1 audit result: the drawing surface is ~25 QuickDraw calls
(SetRect/RGBForeColor/PaintRect/FrameRect/MoveTo/LineTo/DrawText/InvertRect/
PaintOval/InsetRect/PtInRect/…), plus TickCount, GetNextEvent/GetMouse/
StillDown, and File/Sound (defer to M2/M3). Plan: a small Mac-compat shim
(types + those calls) over `fb.*`, so `unodos.c` compiles nearly verbatim.

---

## 1. What `mac/unodos.c` actually is (read before planning)

One ~3600-line C file. It is **not** a formally separated platform
layer, but the Toolbox surface is thin and mostly funneled:

- **Drawing:** funneled through small wrappers near the top —
  `uno_fill`, `uno_box`, `uno_invert`, `text_at`, `text_at_max`,
  `desktop_bg` (`unodos.c:155–237`) — plus scattered direct
  QuickDraw in app code (Paint's pixel ops, games' `fill_rgb`,
  title-bar drawing in `draw_window`). Rect type is QuickDraw's.
- **Time:** `now_secs` over `TickCount()` (`unodos.c:239`); games
  and the scheduler use tick deltas.
- **Events:** a classic `GetNextEvent`-style main loop at the bottom
  of the file (keyboard + mouse from the Toolbox Event Manager).
- **Files:** two storage paths — the FAT12 core (device-abstracted
  via `FatBlkFn`: `fat_dev_sony` = real floppy, `fat_dev_ram` = RAM
  disk, `unodos.c:472–501`) used by Files/Notepad/Paint/Tracker
  saves, and some direct File Manager calls (`files_refresh`'s
  `PBGetCatInfo` listing, `notepad_load_pascal`).
- **Sound:** Sound Manager square-wave synth (`music_*`, `tk_*`,
  `gm_*` channel code).
- **Scheduler:** already cooperative and self-contained
  (`sched_init`/`task_spawn`/`task_yield`/mailboxes,
  `unodos.c:1726+`).

**M0 deliverable #1 is the audit:** copy `unodos.c` → `ps2/unodos.c`,
grep for every Toolbox call (`[A-Z][a-z]+[A-Z]\w*\(` catches most
Toolbox names; also the `#include <*.h>` list), and produce the shim
table in this file: each call → keep (funnel through a wrapper) /
replace (PS2 implementation) / delete (Mac-only path, e.g. the
PBGetCatInfo volume listing — on PS2, Files lists the FAT12 volume
only). Funnel the stragglers through the existing `uno_*` wrappers as
you go — that refactor is the real "platform layer extraction" and
should be mechanical.

## 2. Platform design (recommended, decide finally at M0)

**Software framebuffer, GS as a blitter.** Keep ALL UnoDOS drawing in
software against a 640×448×32 framebuffer in EE RAM (~1.1 MB of the
32 MB), implementing `uno_fill`/`uno_box`/`uno_invert`/`text_at` as
plain C pixel loops (the shared 8×8 font exported to a C array by a
small `mkfont_c.py` from `amiga/gen_data.i`, like every other port).
Each vsync, upload the framebuffer to GS VRAM as a textured
fullscreen quad (gsKit makes this ~20 lines; at 60 Hz it is ~66 MB/s
against a >1 GB/s path — trivial).

Why: unodos.c draws *incrementally* (event-driven partial repaints,
XOR drag outlines, invert highlights). gsKit's natural model is
rebuild-the-display-list-per-frame; mapping incremental semantics
onto it per-primitive fights the grain everywhere. The software FB
preserves UnoDOS semantics exactly (`uno_invert` is a real XOR again)
and shrinks gsKit's role to init + texture upload + vsync — which is
also the answer to "gsKit vs raw GIF": with this design the choice is
low-stakes; take gsKit for M0 speed, swap to raw GIF packets later
only if dependency weight ever matters.

Resolution: 640×448 NTSC interlaced (matches the color-Mac 640px UI
metrics — `ICONS_ROW 6` etc. carry over). If interlace flicker on
real hardware annoys, 512×448 with adjusted metrics is the fallback;
decide on metal, not in the emulator.

**Module/IRX plan** (load via `SifLoadModule` from `rom0:` where
available, embed PS2SDK IRX binaries in the ELF otherwise):
`SIO2MAN` + `PADMAN` (pad, M0), `MCMAN` + `MCSERV` (memory card,
M2), `USBD` + `ps2kbd` (USB keyboard, M2), `audsrv` (sound, M3).

## 3. The rig (decide recipe at M0)

PCSX2 boots ELFs directly — no FMCB needed in the emulator — but
**needs a PS2 BIOS dump** (the user owns a PS2; flag this as a
prerequisite when starting M0, like the IIGS ROM note).

Scripted-input automation in PCSX2 is impractical; use the **Genesis
AUTOTEST pattern** instead: `build.sh <feature>` variants compile in
a self-driving event script (synthetic key/click/tick events posted
into the normal queue — unodos.c's event model makes this easy), and
the rig only has to boot + screenshot. Rig = a PowerShell wrapper
(the `macplus/snapscreen.ps1` / `genesis/snapretry.ps1` pattern) that
launches `pcsx2-qt.exe` with the ELF in batch/nogui mode, waits,
triggers PCSX2's screenshot hotkey (or window-captures), and collects
PNGs. Verify the exact CLI flags + screenshot path at M0 and write
the recipe here. A `shots/`-diff against committed goldens is the
regression check.

Bonus: because the platform layer is thin C, consider a `build.sh
host` target compiling unodos.c + a trivial SDL/Win32 shim on the PC
for instant iteration on app logic — optional, but the C core makes
it nearly free and it would be the family's fastest inner loop.

## 4. M0 — toolchain + hello-GS + decisions

- **Toolchain:** PS2SDK via the ps2dev Docker image (Docker already
  proven on this machine) — `docker run --rm -v "$PWD:/src" -w /src
  ghcr.io/ps2dev/ps2dev sh -c make` — or the ps2dev Windows release
  binaries; decide by which runs cleanly first, record here.
  Makefile from the PS2SDK samples shape (ee-gcc, link as ELF).
- **Hello-GS:** init GS via gsKit (640×448), software FB cleared to
  UnoDOS blue, the shared font's "UnoDOS 3" splash text, FB→GS
  upload loop on vsync.
- **Pad proven:** SIO2MAN+PADMAN loaded, DualShock 2 read, a visible
  cursor moved by d-pad/stick — proves the IRX + input path.
- **Decisions recorded:** §2 design confirmed, §3 rig recipe
  verified (boots in PCSX2, screenshot lands), Docker-vs-binaries.
- Commit: `ps2/` with Makefile/build.sh, main.c (splash + pad),
  README stub.

## 5. M1 — the desktop arrives (C core + shims)

Bring over `unodos.c` and make the M1 surface live: desktop + icons +
window manager + SysInfo + Clock, driven by pad-as-pointer + soft
keyboard.

- **Shims:** `uno_*` draw wrappers → software FB; `now_secs`/ticks →
  vsync counter (60 Hz NTSC; PORT-SPEC §3 says expose the rate);
  event loop → a PS2 main loop that polls pad state per vsync and
  posts events through unodos.c's existing queue (press-time latch +
  sequence counter + edge-only button posting are already in the C
  core's event model — keep them intact, PORT-SPEC §6).
- **Pad-as-pointer:** d-pad accelerating cursor *plus* the left
  analog stick (DualShock 2 — better than any other console port's
  pointer); A-equivalent (Cross) = click/drag, Circle = Enter,
  Triangle = soft keyboard, Start = Esc, shoulder = turbo. Mirror
  the Genesis button-role table in the README.
- **Soft keyboard:** new C code modeled on `genesis/softkbd.i`
  (QWERTY layout, sticky shift, posts through the event queue).
  This is the largest piece of genuinely new M1 code.
- **SysInfo:** EE clock/RAM/region + which IRX modules loaded;
  **Clock:** uptime from the tick counter.
- **Stub out** Mac-only paths behind the M2/M3 milestones (storage,
  sound) so the file compiles clean; keep the full app set compiled
  in but their storage/sound calls no-op'd until their milestone.
- **Tests:** AUTOTEST build opens SysInfo + Clock, drives icon
  selection + a window drag + close via synthetic events;
  screenshots: desktop, drag, soft keyboard open, clock advanced.

## 6. M2 — memory-card storage + USB keyboard + Files/Notepad

- **Storage:** keep the FAT12 core; add `fat_dev_mc` (a third
  `FatBlkFn`): a 1.44 MB `UNODOS.IMG` file on the memory card via
  libmc (`mcInit`/`mcOpen`/`mcRead`/`mcWrite` + `mcSync` waits),
  512-byte sectors at offset LBA×512, created+formatted on first run
  (reuse `fat_ram_format`'s logic / `tools/dump_fat12.py` to verify
  the image from the PC). Rationale: every app save path already
  speaks `fat12_read`/`fat12_write` — this is the minimal-churn
  route and keeps the volume PC-inspectable. Drop the Mac
  PBGetCatInfo listing path; Files lists the FAT12 volume (it
  already does for the RAM-disk case).
- **USB keyboard:** USBD + ps2kbd IRX; translate to the same event
  queue (soft keyboard remains the always-works path). USB **mouse**
  is stretch — survey what PS2SDK actually provides at M2 time; if
  nothing solid exists, document it as backlog rather than fighting
  it (the pad pointer is first-class anyway).
- **Files/Notepad:** already in the C core — this milestone
  validates them against `fat_dev_mc` (open/edit/save/reopen
  persistence test across a rig restart, the macplus m2 shape).
- **Card-loaded apps:** the macplus model in C — a demo app linked
  at a fixed `APP_LOAD` address (no PIC contortions), stored in the
  FAT12 volume, loaded + called with `(op, window*, ksys*)` where
  ksys is a struct of function pointers (the `macplus/diskapp.i`
  ABI, C flavor). Keep the ksys struct small and versioned.
- **Tests:** persistence AUTOTEST (PCSX2 persists virtual MC
  images), Files/Notepad screenshots, demo-app load + key drive.

## 7. M3 — parity: sound, Theme, scheduler decision

- **Sound:** audsrv; a square-wave PCM synth in C (generate from the
  shared note tables — `mac/unodos.c`'s Sound Manager synth shows
  the arrangement; Music = Canon in D, Tracker = 4-channel
  byte-identical pattern format, now genuinely polyphonic by mixing
  voices into the PCM stream). Game event sounds via the same mixer.
- **Theme:** the 8 shared presets + custom editing with **true
  32-bit color** (per-channel 8-bit editing — the family's richest
  gamut; UI chrome still routes through the 4 themed slots so
  presets restyle identically, PORT-SPEC §1).
- **Scheduler:** the C core's cooperative scheduler comes along for
  free. The plan asks "cooperative vs real EE threads — authenticity
  or capability?" **Recommendation: keep cooperative** — identical
  semantics with every other port, no new failure modes; note the
  EE-threads option in the README as deliberately unused. Revisit
  only if some app actually needs it.
- **Full-parity validation:** all 11 apps screenshotted via
  AUTOTEST variants (the Genesis `build.sh <feature>` list is the
  model); games run their scripted steps (synthetic events).
- **Real hardware:** FMCB memory card; document the install path
  (`BOOT.ELF` naming/placement for FMCB's launcher, or uLaunchELF
  from USB stick) in the README. PCSX2-vs-metal differences to
  watch: interlace flicker (§2 resolution fallback), MC timing
  (mcSync waits), pad pressure-sensitivity quirks.

## 8. Risks & open questions

- **PCSX2 BIOS prerequisite** — surface to the user at M0 start.
- **PCSX2 CLI/screenshot automation** is assumed, not yet verified
  on this machine — M0 proves the recipe before anything depends on
  it.
- **IRX availability/versions** (especially ps2kbd, anything mouse) —
  audit at M0/M2; embed known-good PS2SDK builds in the ELF rather
  than depending on `mc0:`/`rom0:` variants.
- **unodos.c divergence:** ps2/unodos.c starts as a copy of
  mac/unodos.c, and the two will drift. Accept it (macplus and mac
  already coexist as separate expressions); backport only bug fixes
  that affect shared logic, and say so in commit messages.
- **USB mouse** support quality in PS2SDK is unknown — treat as
  stretch, never a milestone blocker.
- **Plan open question:** does the user have a USB keyboard for the
  PS2, and is the memory card or a USB stick the primary storage
  story? (Plan §4/open questions — MC is assumed primary here; ask
  when M2 starts.)
