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
| Latitude | — | in progress | expected to detach (AHCI) — but see F6 before assuming it will |
| Lenovo X13 Yoga Gen 3 | — | pending | convertible/touch — another I2C-HID + firmware-pointer data point for F4/F6 |
| MacBook Pro 2013 13" | — | pending | **the interesting one for F3**: Apple firmware, non-PC GOP setup. If its framebuffer is *not* UC, that is a strong clue about what the PC firmwares are doing and how to fix it |

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

### F3 — framebuffer mapped UNCACHED (both laptops)  ·  OS / PERF  ·  S3 (S2 for UX)  ·  OPEN
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
- **Reproduced on both laptops**: Surface runs 1+2 `ctrls=3 present=0`; **X1
  Carbon `ctrls=2 present=0`**. The I2C-HID probe finds controllers on every
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
- Harness mitigation shipped meanwhile (not a fix for this): telemetry is now
  written **before** the detach attempt, so a stranded boot still leaves its log.

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
