# UnoDOS pc64 — metal test findings catalogue

**Status: COLLECTING. Do not fix from this branch — fixes are bundled into a
dedicated session.** This file is the running record of what the debug/stress
build (`pc64-debug-stress`, see [DEBUG.md](DEBUG.md)) turns up on real hardware.
Each machine run appends findings here; the fix session works from this list.

Raw reports are preserved off each stick under
`C:\Users\arin\unodos-metal-reports\<machine>-<date>\`.

## Test matrix

| Machine | CPU | Runs | Notes |
|---------|-----|------|-------|
| Surface Laptop Go | i5-1035G1 | 2 (2026-07-20) | 1536x1024. Stays firmware-**attached**. Both runs' reports saved. |
| X1 Carbon (Gen 8) | i5-10210U | 1 (2026-07-20) | 1920x1080. **Also stays attached** — detach is *blocked* (F6), contrary to expectation. 3 passes clean. |
| Latitude (i7-6600U) | i7-6600U | 1 + 1 retest (2026-07-20) | **DETACHES** (`detach_blocked=0`, no I2C controllers) - the first machine that does, and F8 strands it. Boot log ends at `init:detach`. **Retest on the detach-disabled build produced ZERO telemetry — most likely it never booted the stick** (see below). |
| Lenovo X13 Yoga Gen 3 | i5-10210U | 5 (2026-07-20/21) | 1920x1080. **Clean 3-pass runs, zero crashes** — first machine to run with detach disabled, the one that PROVED F3 is the bottleneck, the perf-iteration mule (P0, Blt, worst-frame runs), and the first WiFi net-test machine (F12). |
| MacBook Pro 2013 13" | — | 1 (2026-07-20) — **FAILS TO BOOT**, see F9 | **the interesting one for F3**: Apple firmware, non-PC GOP setup. If its framebuffer is *not* UC, that is a strong clue about what the PC firmwares are doing and how to fix it |

**Surface run 2 (fixed kernel): 31 passes / 928 s (~15.5 min), ZERO crashes,
ZERO hangs.** `surface-2026-07-20-run2/`. A pass is ~30 s at the default
cadence. This is a clean stability result, and it validates the F1/F2 fixes on
metal: no self-crash across 31 passes (F1) and no black-screen (F2). Telemetry
wrote 31 `PF` snapshots + `BOOTENV.TXT` and survived the operator's power-off.
**Caveat:** because nothing crashed, F2's *attached-reset* fix is still only
QEMU-verified — no real fault occurred on metal to exercise reset-and-reboot.

**X1 Carbon run 1 (bounded `passes=3`): completed all 3 passes in 135 s, ZERO
crashes, ZERO hangs.** `x1carbon-2026-07-20/`. The bounded-run machinery worked
as designed (3 `PF` snapshots, then the driver went idle and handed back the
desktop). Two machines are now clean under stress; the substantive findings are
environmental, not crash bugs.

**X13 Yoga Gen 3 run 1: all 3 passes, 121 s, ZERO crashes, ZERO hangs.**
`x13yoga-2026-07-20/`. First run on the detach-disabled build and everything
worked end to end: `detached=0 volumes=5`, `shell: main loop entered … hud_len=38`,
`stress: ARMED (passes=3)`, three passes, then `driver IDLE, desktop is yours`.
This validates the F8 workaround, the bounded-run machinery, the desktop
hand-back, and the boot log — and it produced the perf split that settles F3.

**Latitude retest (detach-disabled build): ZERO telemetry.**
`latitude-2026-07-20-retest/`. The stick (Cruzer) carries build
`debug-local-20260721-0042` and `passes=3`, but `CRASH\` holds only the shipped
README. This is *not* explained by F8: the bootenv/bootlog writes run **before**
the `#ifndef UNO_NO_DETACH try_detach()` block, so compiling detach out cannot
change whether they happen — and the earlier detach-*enabled* run on this same
machine did write a log. A Latitude that reached the end of init would therefore
have written one again. Most likely it **never booted the stick** (it has Windows
and probably an installed UnoDOS whose boot entry outranks the USB). Unresolved
pending one observation: did the amber splash banner appear?

**X13 Yoga run 2 — P0 optimizations measured (2026-07-20).**
`x13yoga-2026-07-20-P0/`. Same machine, same 3-pass workload, build
`debug-local-20260721-0153`. `present_avg` fell **2.2-3.4x** (238576 -> 107825,
98653 -> 29160, 262518 -> 121732 us), fps 5->7 / 16->24 / 4->8, and the whole run
went **121.3 s -> 70.4 s (1.72x)**. `render_avg` unchanged, as intended. Zero
crashes. This confirms the F3 model end to end: present WAS the frame, and
cutting VRAM bytes cut frame time proportionally. The framebuffer is still
uncached (`vram 26269 KB/s`) — P0 does not change the memory type, so the
remaining ~100x is still on the table via P3.

Also the first metal test of the new boot marker: `CRASH\BOOTS.TXT` reads
`boot 1  2026-07-20 21:59:04  build debug-local-20260721-0153`.

**X13 Yoga run 3 — Blt present path (2026-07-20).** `x13yoga-2026-07-20-BLT/`.
`blt bench: 108018 KB/s (direct 26665 KB/s) -> BLT IS FASTER`, so
`choose_present_path()` switched. Cumulative against the original baseline:

| pass | baseline | +P0 | +Blt | total |
|------|----------|-----|------|-------|
| 0 | 238 576 us | 107 825 | **55 889** | **4.27x** |
| 1 | 98 653 us | 29 160 | **16 020** | **6.16x** |
| 2 | 262 518 us | 121 732 | **62 957** | **4.17x** |

fps on the present-bound passes: 5 -> 17 and 4 -> 17. Operator confirms 3D
Runner "looked a lot smoother, but it hitched every once in a while".

**X13 Yoga run 4 — worst-frame/hitch tracking (2026-07-21).**
`x13yoga-2026-07-21-WORST/`. Build `debug-local-20260721-0326` (flasher
`20260720-232900`), boot 1 on a fresh flash, clean 3-pass run, zero crashes,
57.4 s. Machine identity cross-checked: `i5-10210U`, `fb_base d0000000`,
`mtrr0 base=c0000000`, `i2c-hid ctrls=0` — unambiguously the Yoga.
Averages reproduce the Blt run to ~1% (present_avg 55695/16135/63013 vs
55889/16020/62957), third consecutive <1%-repeatable run on this machine.
First metal data from the new `worst:` line — and it answers the operator's
"smoother but hitches occasionally" report; see **F11**:

| pass | render_avg | render_max | present_avg | present_max | hitches>100ms | during |
|------|-----------|------------|-------------|-------------|---------------|--------|
| 0 | 2 888 us | 16 443 us | 55 695 us | 83 577 us | 0 | (none) |
| 1 | 75 772 us | **2 340 974 us** | 16 135 us | 84 343 us | **1** | **stress:close** |
| 2 | 3 124 us | **97 577 us** | 63 013 us | 82 031 us | 0 | (none) |

**X13 Yoga run 5 — first WiFi network test (2026-07-21).**
`x13yoga-2026-07-21-NET/`. Build `debug-local-20260721-0521`, the first boot of
the net-test harness on metal, and it worked end to end: UsbIo enumerated 8
firmware USB interfaces (SD reader, Wacom touch, camera, Synaptics, Intel BT),
correctly found no USB ethernet, took the WiFi plan, identified the AX201
(PCI `02f0`, fam 3, gen2), read creds, loaded `IWLAX201.UCO` (1 406 716 bytes),
mapped BAR0 (`hw_rev=00000351 rf_id=0010a100`), passed card-ready/RF-kill/APM —
and stopped at **"no ALIVE notification within 2 s of fw start"** (**F12**).
Stress ran normally afterwards (PF014-016). Also exposed two harness bugs, both
fixed the same day (F13).

## Severity key

- **S1** blocks the machine / data loss  **S2** major (crash, hang, unusable feature)
- **S3** perf / degraded  **S4** cosmetic / behaviour-not-as-expected

---

## Findings

### F1 — stress config parser matched comment text  ·  HARNESS  ·  S2  ·  FIXED
`cfg_has()` did a substring search over the whole `STRESS.CFG`, so the shipped
default's documentation comment `# keys: once fast slow allow-force` silently
enabled **every** key, including `allow-force` — a "safe" stick forced a #PF on
pass 1. This was the actual cause of the Surface black-screen (the forced #PF,
CR003, hit the broken reset path F2).
- Evidence: `surface-2026-07-20/CR003.TXT` log tail — `stress: ARMED (once=1 force=1 speed=1)` from a `mode=continuous` file; `stress: allow-force set - forcing a #PF`.
- **Already fixed** (commit 6010041) because it made the harness self-destruct on every boot — parse only non-comment lines, match delimited tokens. Host- + QEMU-regression-tested (`SAFE=1 tools/dbg_crash_test.py`).

### F2 — post-crash reset black-screens attached machines  ·  HARNESS  ·  S2  ·  FIXED
`trap_reset()` used `uno_native_reset()` (CF9 + i8042 pulse) unconditionally.
The Surface stays firmware-attached and ignores both, so after capturing CR003
it `hlt`'d to a black screen with the firmware OSK still drawn — exactly what
the operator saw. A real crash on any attached machine would have been
uncollectable-looking (machine appears hung, not rebooted).
- Evidence: CR003 captured with `detached: 0`, then no reboot; operator saw black screen + OSK.
- **Already fixed** (commit 6010041): use the firmware `ResetSystem` runtime service while attached; native CF9 only once detached / as fallback. (QEMU's CF9 works, so it never surfaced there.)

### F3 — framebuffer mapped UNCACHED (ALL THREE machines)  ·  OS / PERF  ·  S3 (S2 for UX)  ·  OPEN
The GOP framebuffer is covered by a variable MTRR of type **UC**, so every VRAM
write is uncached — measured **~14 MB/s vs ~1.8 GB/s to RAM (≈1:130000)**. This
is the review's predicted "slideshow UI". Present is `linear` (direct CPU
writes), so the whole UI pays the UC penalty on every changed pixel; the
existing dirty-row/shadow tracking limits but does not remove it.
- **NOT machine-specific — reproduced on BOTH laptops, three data points:**
  - Surface run 1: `vram 14062 KB/s`, ratio 1:130062, via `mtrr6` (coarse, >4 GB)
  - Surface run 2: `vram 14125 KB/s`, ratio 1:126998, same MTRR
  - **X1 Carbon: `vram 24937 KB/s ram 3001222540 KB/s ratio 1:120352`**, via a
    *different* MTRR — `mtrr0 base=80000000 mask=7f80000800 type=0`, i.e. the
    32-bit MMIO hole (0x80000000-0xFFFFFFFF) with `fb_base c0000000` inside it.
  - **X13 Yoga Gen 3: `vram 27236 KB/s ram 3185483826 KB/s ratio 1:116958`**,
    `mtrr0 base=c0000000 mask=7fc0000800 type=0`, `fb_base d0000000`. Fourth
    machine, fourth MTRR layout.
  - **Latitude (i7-6600U): `vram 43320 KB/s ram 4648625700 KB/s ratio
    1:107308`**, `mtrr0 base=80000000 mask=7f80000800 type=0`, `fb_base
    a0000000`. Third machine, third firmware.
  Different firmware, different MTRR, same outcome: the framebuffer is UC and
  the UC range also covers device MMIO, so the same "cannot simply flip it"
  constraint applies on both. This is a general pc64 problem, not a Surface
  quirk — which raises its priority for the fix session.
- Evidence (`surface-2026-07-20/BOOTENV.TXT`):
  - `gop: mode 1536x1024 stride 1536 pixfmt 1 fb_base 4000000000 present=linear`
  - `mtrr6 covers fb: base=4000000000 mask=4000000800 type=0 (UC - SLOW PRESENT!)`
  - `fb bench: vram 14062 KB/s ram 1828932096 KB/s ratio 1:130062  << UNCACHED-CLASS VRAM`
- **What it costs, in frames.** A full-screen repaint writes `w*h*4` to VRAM:
  - X1: 1920x1080x4 = 8100 KB / 24937 KB/s = **0.32 s → ~3 fps**
  - Surface: 1536x1024x4 = 6144 KB / 14125 KB/s = **0.43 s → ~2.3 fps**
  This is a hard ceiling nothing above the present path can beat, and it matches
  the reported symptoms exactly. Two consequences worth carrying into the fix:
  - **Fullscreen apps are pinned to it.** `UI.full` forces `g_dirty` every frame,
    so Runner3D repaints the whole screen every frame → ~3 fps no matter how
    fast the 3D pipeline itself is. "3D Runner is extremely slow" is this.
  - **Low-res mode does not help.** `uno_pc64_lowres()` drops the desktop to
    1/4 x 1/4 (~1/16 the pixels to *render*), but present scales up and still
    writes the **full panel area** to VRAM. It cuts render cost and leaves the
    actual bottleneck untouched — so the existing optimisation is aimed at the
    wrong half of the frame.
  - Ordinary desktop use stays tolerable only because dirty-row tracking keeps
    most frames far below full-screen.
- **MEASURED ON METAL — the machine is framebuffer-bound, not CPU-bound.** The
  X13 Yoga's per-pass perf snapshots (the first run to carry them):

  | pass | render_avg | present_avg | fps | present/render |
  |------|-----------|-------------|-----|----------------|
  | 0 | 2 924 us | **238 576 us** | 5 | **82x** |
  | 1 | 75 762 us | 98 653 us | 16 | 1.3x |
  | 2 | 3 186 us | **262 518 us** | 4 | **82x** |

  Rendering the whole scene costs ~3 ms; **pushing it to VRAM costs ~240-260 ms**.
  And that lands right where the bandwidth model predicts: a full-screen write is
  1920x1080x4 = 8100 KB, and 8100 / 27 236 KB/s = **297 ms**. Two independent
  measurements — a synthetic bandwidth bench and real frame timing — agree.
- **The actionable conclusion for the fix session:** optimising the renderer or
  the rasteriser is worth ~nothing here; ~99 % of a frame is the uncached VRAM
  write. Only two things can help: change the framebuffer's memory type, or cut
  the number of bytes pushed to VRAM. Anything else is noise. (Pass 1 is the
  instructive counter-example: when few rows were dirty, present fell to 99 ms
  and fps tripled.)
- Root cause / fix difficulty (investigated, **do not blind-fix**):
  - `mtrr6` is coarse — its physmask (`0x40_0000_0000`) also blankets the >4 GB
    high-MMIO region where the Surface's xHCI/NVMe/I2C register BARs live, which
    **require** strict UC. Flipping `mtrr6` to WC would corrupt device access.
  - Adding a WC MTRR over just the fb does **not** win: on a UC∩WC overlap, UC
    wins (SDM). PAT=WC ∘ MTRR=UC also resolves to UC. So neither can override
    the coarse UC range.
  - A correct fix means reprogramming the MTRR set to cover only the true
    device-MMIO ranges as UC and leave the fb WB/WC — machine-specific, via the
    CR0.CD/WBINVD sequence, with real bricking risk if wrong. Best done as an
    opt-in experiment with the operator at the machine.
  - Cheaper partial mitigations to weigh: present via GOP `Blt()` when the fb is
    UC (firmware blitter may DMA); reduce full-screen repaints further.

### F4 — I2C-HID pointer/keyboard bind on NO tested machine  ·  OS / DRIVER  ·  S2 (feature) / S4 (OSK)  ·  OPEN
Three LPSS I2C controllers are found but **no** I2C-HID device binds, so the
native trackpad and keyboard are unavailable; input falls back to the firmware
Absolute/Simple pointers, and because no native keyboard binds, firmware ConIn
keeps being polled → the firmware on-screen keyboard stays drawn (the OSK icon).
It is also why *neither* laptop detaches (see F6).
- **Reproduced on every machine tested**: Surface runs 1+2 `ctrls=3 present=0`;
  X1 Carbon `ctrls=2 present=0`; Latitude and **X13 Yoga `ctrls=0`** — and the
  Yoga is a touch convertible, so finding *zero* I2C controllers there suggests
  the enumeration itself misses them on some chipsets, not merely that binding
  fails. The I2C-HID probe finds controllers on every
  machine tested and binds a device on none of them. See F6 for the knock-on.
- Evidence (`BOOTENV.TXT`): `i2c-hid: ctrls=3 present=0 addr=0 desc_parsed=0`;
  `pointer: fw_simple=1 fw_abs=3 detach_blocked=0`; `ps2: kbd=0 aux=0`;
  `usb-hid: kbd=0 mouse=0`.
- Open question for the fix session: why the I2C-HID probe finds controllers but
  no device (address discovery? the Surface's HID-over-I2C descriptor path?).
  Cross-ref the LPSS-clock / 64-bit-BAR history in the pc64 trackpad notes.

### F5 — ~~open-window count creeps up over a long run~~  ·  WITHDRAWN (my error)
Originally filed from run 2's *first and last* snapshots (9 → 12) as a monotonic
creep toward unoui's 24-window cap. **Reading the full 31-snapshot series
refutes it**: the count oscillates in a bounded band and never trends upward —
`9 13 14 15 12 12 11 14 13 13 13 12 13 11 9 14 15 11 14 14 12 10 12 14 12 15 13
11 12 11 12`. The open and close phases are balanced. No action needed; kept
here so the fix session doesn't chase it. Lesson: don't infer a trend from two
endpoints when the whole series is sitting right there.

### F6 — neither laptop detaches, so the whole native stack is untested on metal  ·  OS / COVERAGE  ·  S2  ·  OPEN
The X1 Carbon was expected to detach (ExitBootServices → our own drivers). It
does not: `detached: 0` with **`detach_blocked=1`**. The detach gate's
"would this strand the pointer?" guard fires — the machine has LPSS I2C
controllers (`ctrls=2`) but no I2C-HID pointer bound (F4), and no native USB-HID
pointer, so detaching would kill the only working pointer (firmware
`fw_simple=2 fw_abs=1`). The guard is behaving *correctly*; F4 is the root cause.
- **Consequence, and the reason this is S2:** on both machines tested, pc64 runs
  entirely on firmware services. The M3 decoupling work — our own IDT, the LAPIC
  watchdog, native AHCI/NVMe storage, native PS/2 input, CF9 reset — has
  therefore **never executed on real hardware**. Every metal result so far
  describes only the attached path. (`ps2: kbd=0 aux=0` on the X1 is not
  evidence of a missing i8042: `uno_ps2_init()` only runs post-detach, so those
  fields are simply uninitialised.)
- Fix F4 and the X1 should detach, which would open up the whole untested half.
- Until then, treat "the Latitude will detach" as an assumption to verify, not a
  given.

### F7 — `text_pen()` shifts a negative x: UB in the text renderer  ·  OS / ROBUSTNESS  ·  S2  ·  OPEN
`pc64_font.c:307` — `int pen26 = x << 6;`. Left-shifting a **negative** signed
int is undefined behaviour, so drawing any string at a negative x is UB. With
the debug build's sanitizer this is an immediate `ud2` → crash; in a release
build it is silent UB whose result depends on the compiler.
- **Caught by UBSan, on the first build that happened to pass a negative x.**
  Reports `CR005`/`CR009` (QEMU): `vector 6 (#UD)`, `RIP text_pen.cold+0x0`,
  flagged `ud2: UBSAN TRAP`, with `RBX=RCX=0xffffffd6` = **x = -42**, and the
  raw-stack scan naming `prov_text+0x32` and `uno_main+0xcd4`. This is exactly
  the class the sanitizer was added for, and it validates that investment.
- The *trigger* was the debug overlay (right-aligning a status string wider
  than the desktop) — that caller is clamped now — but the **renderer weakness
  is real and general**: any caller that can produce a negative x hits it.
  Plausible existing callers: the centred `fb_text(cx - fb_text_w(s)/2, ...)`
  used by the splash and dialogs (negative whenever the string is wider than
  the surface), and anything drawing scrolled/partially-offscreen text.
- Fix direction for the fix session: make `text_pen` well-defined and clipping-
  safe for negative x (multiply instead of shift, or clamp/skip glyphs left of
  the surface), and audit `fb_text` callers that can go negative. Cheap fix,
  broad blast radius.

### F8 — detach strands a USB-booted system on a machine with UnoDOS installed  ·  OS  ·  S1  ·  OPEN
Booting the USB debug stick on the **Latitude** produced a correct splash (build
`debug-local-20260720-2332`) and then **zero telemetry**: `CRASH\` held only the
shipped `README.TXT` — no `BOOTLOG.TXT`, no `BOOTENV.TXT`, no `PF` snapshots —
and the stress driver never armed.
- **Mechanism.** The native stack has AHCI / NVMe / SDHCI but **no USB
  mass-storage driver** (`xhci.c` serves HID only). After `ExitBootServices`
  the firmware Block-IO fallback in `blkdev.c` is gone, so a **USB-booted**
  system can no longer reach its own boot volume. `crash_vol()` then finds no
  writable volume → all telemetry silently dies, and `arm()` cannot read
  `STRESS.CFG` → the stress driver disables itself. Both reported symptoms fall
  out of this single cause.
- **Why the gate allowed it.** `uno_fat_native_eligible()` permits detach when a
  UnoDOS `BOOTX64.EFI` sits on a natively-reachable FAT volume. The Latitude has
  UnoDOS **installed to its internal disk**, which satisfies that test — so the
  guard added for a *merely foreign* FAT partition does not cover "an installed
  UnoDOS on the internal disk while running from USB".
- **Severity S1:** the running system loses its boot device. Beyond telemetry,
  anything loading from the boot volume afterwards (`.UNO` modules, fonts, docs)
  is gone. Any user who installs UnoDOS and later boots the USB hits this.
- Fix direction: refuse to detach when the volume we BOOTED FROM would become
  unreachable (compare the boot device path against what the native stack can
  reach), rather than asking only whether *some* eligible volume exists. The
  boot-device identity is already known — `uno_pc64_image_handle()`.
- **CONFIRMED on the Latitude (i7-6600U), 2026-07-20.** It is the first machine
  that actually detaches: `pointer: fw_simple=1 fw_abs=1 detach_blocked=0`, and
  `i2c-hid: ctrls=0` so the F6 pointer-stranding guard has nothing to fire on.
  Its `BOOTLOG.TXT` (captured only because telemetry now runs pre-detach) ends
  at `last_checkpoint: init:detach @ 3516ms` with `volumes=7` — nothing after
  the detach attempt is ever recorded, and the machine produced no HUD and no
  stress cycle. The Surface and X1 escaped this only because F6 blocks their
  detach.
- Harness mitigations shipped (neither is a fix for this):
  1. telemetry is written **before** the detach attempt, so a stranded boot
     still leaves its log — this is the only reason we have the Latitude data;
  2. **the debug build no longer detaches by default** (`-DUNO_NO_DETACH`,
     `UNO_DETACH=1` re-enables). Every test boot is from USB, so with detach on
     the harness is useless on any machine that qualifies.

### F9 — MacBook Pro 2013 boots very slowly then resets to the internal drive  ·  UNTRIAGED  ·  S2  ·  OPEN (flagged, not investigated)
Operator report 2026-07-20: the debug stick (`debug-local-20260721-0042`, the
detach-disabled build) on a 13" MacBook Pro 2013 boots **extremely slowly**,
then resets and the machine boots its internal drive instead. Deferred by the
operator for later investigation; recorded now while the evidence is fresh.
- From the operator's photo, the splash **does** render and reaches the LAST
  loading-bar segment (all four filled: red/green/cyan/white), so `uno_pc64_init`
  got through GOP, driver connect, input, and the storage/ACPI/audio stages.
  Whatever goes wrong is at or after the end of init.
- Not F8: this build has detach compiled out.
- **PRIME SUSPECT — our own watchdog, i.e. a HARNESS bug, not an OS one.**
  `uno_dbg_watchdog_start()` arms a 20 s firmware-timer watchdog at the end of
  init, but the heartbeat it watches is only fed once the *shell's* main loop
  runs. On a machine where everything between those two points is slow (this Mac
  is slow, and F3's uncached framebuffer makes the first paints very expensive),
  the watchdog can time out **before the first heartbeat** and reset the machine
  — which would present exactly as "very slow, then reboots". If so the fix is
  ours: don't arm until the shell's first heartbeat, or give the arm a generous
  one-shot grace period.
  - Test cheaply: boot it once more and see whether `CRASH\HG###.TXT` appears
    (the watchdog stashes and the report lands on the following boot), or build
    with the watchdog disabled and see if the Mac comes up.
- **The stick was read afterwards (devbuntu, direct): build `0042` confirmed on
  it, and `CRASH\` holds ONLY the shipped `README.TXT` — no `BOOTLOG.TXT`, no
  `BOOTENV.TXT`, nothing.** So the MBP wrote *no telemetry at all*.
  - That narrows it usefully. The operator's photo shows the `0042` splash
    banner AND the `fb bench` pattern along the bottom edge (F10) — and the
    bench lives *inside* `uno_dbg_envblock()`, which runs immediately before
    `uno_dbg_write_bootenv()` / `uno_dbg_write_bootlog()`. So execution reached
    the writes and the **writes themselves failed**: `crash_vol()` found no
    writable FAT volume.
  - Detach is compiled out in this build, so unlike F8 this is NOT detach. On
    Apple firmware our native block/FAT layer appears not to enumerate the USB
    stick through Block IO at all.
  - Caveat, stated honestly: the disk cannot prove *which* boot last touched it,
    so "the MBP reached the writes and they failed" is inference from the photo
    plus the empty CRASH dir, not something the disk alone establishes. It is
    consistent with both, and it is the first thing to re-test.
  - **Consequence: the MBP cannot produce ANY telemetry until this is fixed** —
    it is untestable, not merely slow, and the watchdog theory above is now only
    one candidate among two (a reset from the watchdog, or something after the
    failed writes).
- **Cost of leaving it broken:** this was the most informative machine left for
  F3 — Apple firmware, non-PC GOP setup. If its framebuffer is *not* UC, that is
  a direct clue toward fixing the uncached-framebuffer problem on the PCs.

### F10 — ~~the fb bandwidth bench leaves visible garbage on screen~~  ·  HARNESS  ·  S4  ·  FIXED
The boot-env `fb bench` writes a test pattern into the **bottom 64 rows of the
panel** and then calls `uno_pc64_dbg_invalidate()` to force a repaint. When the
presented desktop is letterboxed (`gOutH < gModeH`) those rows lie outside the
presented area and are never repainted, so the pattern stays on screen for the
whole session. Clearly visible as green/blue striping along the bottom edge of
the operator's MacBook photo.
- Also visible in that photo: the splash's `DEBUG / STRESS BUILD <id>` banner is
  drawn at `H/2+40` and **collides with the loading bar** at `H/2+46`; the bar
  paints over the middle of the text. Cosmetic, mine, one-line fix.
- Related harness inefficiency spotted while reading this: `crash_vol()` caches
  its result **only on success**. On a machine where no volume qualifies (F8,
  F9) every single call re-scans every volume with `uno_fat_list_ex` +
  `uno_fat_mkdir` — and `uno_dbg_write_bootlog()` is called every 30 s from the
  heartbeat. Repeated failing scans over firmware Block IO are exactly the kind
  of thing that could make a struggling machine feel "extremely slow". Cache the
  negative result (or bound the retries).
- Neither affects results, but both are noise in operator photos and the bench
  garbage could be mistaken for framebuffer corruption — which is exactly the
  kind of thing we are trying to diagnose, so it should not be self-inflicted.
- **FIXED**: `uno_pc64_dbg_bench_cleanup()` erases the benched band and forces a
  repaint over it. QEMU screenshot confirms a clean bottom edge. (The splash
  banner/loading-bar overlap is still open — cosmetic.)

### F11 — the residual hitches are in RENDER, not present  ·  OS / PERF  ·  S3  ·  OPEN
The question the worst-frame line was added to answer, answered on metal
(Yoga run 4). After P0+Blt, **present is bounded**: `present_max` never
exceeded 84 ms in any pass (the Blt path caps a full-screen push at ~80 ms on
this machine), so present can no longer produce a >100 ms hitch here. The
spikes the operator feels are **render-side** — exactly what QEMU hinted
(133 ms peak render vs 15 ms avg).
- **The one >100 ms hitch: a single `unoui_render_ui()` call took 2.34 s**,
  tagged `during=stress:close` (the driver's close-focused-window +
  focus-next phase). The instrumentation brackets *only* `unoui_render_ui`
  (`pc64_uui.c` timed block), so this is 2.3 s inside widget draw callbacks —
  ~800x a normal full-scene render (~3 ms). Something a close/focus-change
  triggers does massive one-time work inside a draw (lazy re-layout or
  decode on first draw after focus is the prime suspect — pass 1's window
  population includes Browser/Editor windows holding `HUGE.HTM` plus Music,
  Tracker, Paint from earlier phases).
- **Pass 1's `render_avg` of ~75 ms is not noise** — it reproduces across all
  four Yoga runs (75 762 / 75 548 / 75 772 us). The pass-1 window mix makes
  *every* full render cost ~75 ms, i.e. some resident window's draw callback
  is ~25x the whole rest of the scene. Whatever that window is, it is also
  the likely home of the 2.34 s outlier.
- **Runner3D's felt hitches are real but sub-threshold per half:** pass 2
  peaked at `render_max=97 577` us — under the 100 ms hitch bar — but stacked
  on that pass's 63-82 ms present, the worst *frame* is ~180 ms against a
  ~66 ms typical frame. A 3x frame-time spike is clearly visible; the counter
  reads 0 because the threshold is applied per-half. Harness note for the
  next iteration: also track worst render+present *total* (and tag it), so a
  frame like this is counted, not hidden.
- Fix-session direction: the hitch hunt has moved from present to render.
  Next instrumentation step is per-window draw timing (name the window whose
  draw eats 75 ms, and catch the 2.34 s path with a checkpoint inside
  `unoui_render_ui` naming the widget being drawn). Do NOT spend more on the
  present path for hitches — it is bounded by Blt on this machine.

### F12 — AX201 firmware loads but never posts ALIVE  ·  OS / DRIVER  ·  S2  ·  OPEN
The first metal WiFi run (Yoga, AX201 CNVi) gets through every transport
stage — card identified, `IWLAX201.UCO` read and TLV-parsed, BAR0 mapped,
card-ready handshake, no RF-kill, APM up, gen2 context-info load issued — and
then the firmware never raises the ALIVE notification within the 2 s wait.
- Evidence: `x13yoga-2026-07-21-NET/NETLOG.TXT` — `hw_rev=00000351
  rf_id=0010a100`, `FAIL no ALIVE notification within 2 s of fw start`.
- **Suspects, in order:**
  1. **Qu vs QuZ firmware variant.** `IWLAX201.UCO` is one filename but
     upstream ships two AX201 images (`Qu-b0-hr-b0` vs `QuZ-a0-hr-b0`);
     fetch-fw packs one of them. Loading the wrong variant fails exactly like
     this (fw refuses to boot, no error visible to the host). The recorded
     `hw_rev`/`rf_id` are the inputs Linux uses to pick — resolve against the
     iwlwifi table in the fix session, and consider shipping both files with
     selection by `CSR_HW_REV` (read the BAR *before* choosing firmware).
  2. **The polled ALIVE wait.** `wait_alive()` polls the RX ring for the
     notification with legacy-IRQ assumptions; if the card posts it with
     MSI-X-style indexing (or to a queue the poll isn't reading), it would be
     missed even though the firmware booted. A CSR scratch-register check
     (the fw sets `CSR_UCODE_DRV_GP1`/scratch on boot) could distinguish
     "fw booted, we missed the notification" from "fw never booted".
  3. Timeout too short is the least likely (Linux's own wait is ~1 s), but
     free to test.
- The trace did exactly its job: on the first metal attempt, the failing
  stage has a name and two falsifiable hypotheses. Whatever the other laptops
  report (same line or different) will split card-specific from generic.

### F13 — a shipped image carried QEMU telemetry + a creds-shadowing WIFI.CFG  ·  HARNESS  ·  S2  ·  FIXED
Two self-inflicted hazards found via the Yoga's stick:
1. **QEMU dev-run telemetry rode into the shipped image.** The QEMU harness
   scripts booted `vvfat fat:rw:build/esp`, so guest writes (BOOTLOG, PF
   snapshots, boots.txt — the latter with vvfat cluster garbage: BearSSL
   symbol-table and HTML-corpus fragments) landed back in `build/esp`, which
   deploy-to-share then embedded. The Yoga's stick carried lowercase
   `pf005-013.txt` from QEMU next to its own `PF014-016.TXT` — exactly the
   stale-vs-real ambiguity the reflash rule existed to prevent, now baked in
   at the factory.
2. **The shipped placeholder `WIFI.CFG` shadowed the real creds.** The driver
   prefers `WIFI.CFG` over `WIFI.TXT`, so the staged starter
   (`ssid=YourNetwork`) silently won over the flasher-staged `wifi.txt` —
   the Yoga run tried to join "YourNetwork".
- **FIXED (same day):** build.sh's dbg staging now purges `build/esp/CRASH`
  (ships only README) and root `BOOTENV.TXT`, and ships **no** `WIFI.CFG`;
  the QEMU scripts (`nettest.py`, `nettest_stage.py`) run on a scratch copy
  (`build/esp-nettest`) so `build/esp` stays pristine regardless.
- Lesson: anything a dev-loop can write into the artifact tree WILL
  eventually ship; test harnesses must not share a directory with the
  shipped image.

---

## Positive confirmations (not bugs — the harness works on metal)

- **Crash pipeline works on real hardware.** The forced #PF was caught by the
  IDT handler, self-symbolized (`uno_dbg_force+0x7c [rva ...]`), captured with
  full registers + CR2 + checkpoint (`stress:close`) + a populated boot-env
  block, and **written to `\CRASH\CR003.TXT` on the FAT32 volume** — on the
  attached firmware-Block-IO write path. Symbol table + boot-env block are
  legible exactly as designed.
- **Stress driver runs on metal** — opened every app, built the deep directory
  tree (16 levels / 163-char path) and fed the corpus without incident up to the
  forced crash (`PF002` perf snapshot).

## To carry into future runs

- Surface stays **attached** (reset/detach/watchdog all take the attached path);
  X1 (has i8042) and Latitude (AHCI) will likely **detach** → exercise the
  native-IDT + LAPIC-watchdog + CF9-reset paths instead. Compare boot-env blocks.
- Grab `\BOOTENV.TXT` first each run (a re-boot overwrites it), then the whole
  `\CRASH` folder.
- **SUPERSEDED (2026-07-21): telemetry is now machine-scoped** — every file
  lands under `CRASH\<MACHINE>\` (SMBIOS-derived: X13YOGA, X1CARBON, SURFGO,
  LATITUDE, MACBOOK, QEMU), so one stick covers a whole batch without
  collisions and the paragraph below is history (kept for the lesson). The
  `cpu:`/`fb_base` cross-check remains a good habit for the `UNKNOWN` folder.
- ~~**Reflash, or clear the telemetry, BETWEEN MACHINES.**~~ A stick reused across
  machines carries the previous machine's `BOOTLOG.TXT` / `BOOTENV.TXT` / `PF*`,
  and a boot that writes nothing (F8/F9) leaves that stale data untouched — so
  "old data" and "this machine failed to write" look identical. This bit us
  once: a Latitude attempt came back byte-identical to the preceding X13 Yoga
  run (same md5, `i5-10210U` vs the Latitude's `i7-6600U`), which could easily
  have been misread as a Latitude result. A freshly flashed stick is always
  unambiguous because the flasher ships an empty `CRASH\`.
  - Cross-check every report against `cpu:` / `fb_base` in the env block before
    attributing it to a machine.
