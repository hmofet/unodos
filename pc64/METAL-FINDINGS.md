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
| Surface Laptop Go | i5-1035G1 | 2 (2026-07-20) | stays firmware-**attached** (no i8042; eMMC; i2c-hid didn't bind). Both runs' reports saved. |
| X1 Carbon (Gen 8) | — | pending | expected to **detach** → exercises the native-IDT / LAPIC-watchdog / CF9 paths |
| Latitude | — | pending | expected to detach (AHCI) |

**Surface run 2 (fixed kernel): 31 passes / 928 s (~15.5 min), ZERO crashes,
ZERO hangs.** `surface-2026-07-20-run2/`. A pass is ~30 s at the default
cadence. This is a clean stability result, and it validates the F1/F2 fixes on
metal: no self-crash across 31 passes (F1) and no black-screen (F2). Telemetry
wrote 31 `PF` snapshots + `BOOTENV.TXT` and survived the operator's power-off.
**Caveat:** because nothing crashed, F2's *attached-reset* fix is still only
QEMU-verified — no real fault occurred on metal to exercise reset-and-reboot.

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

### F3 — framebuffer mapped UNCACHED on the Surface  ·  OS / PERF  ·  S3 (S2 for UX)  ·  OPEN
The GOP framebuffer is covered by a variable MTRR of type **UC**, so every VRAM
write is uncached — measured **~14 MB/s vs ~1.8 GB/s to RAM (≈1:130000)**. This
is the review's predicted "slideshow UI". Present is `linear` (direct CPU
writes), so the whole UI pays the UC penalty on every changed pixel; the
existing dirty-row/shadow tracking limits but does not remove it.
- **Reproduced in run 2** with near-identical numbers (`fb bench: vram 14125 KB/s ram 1793855911 KB/s ratio 1:126998`), same `mtrr6 ... type=0`. Two independent data points.
- Evidence (`surface-2026-07-20/BOOTENV.TXT`):
  - `gop: mode 1536x1024 stride 1536 pixfmt 1 fb_base 4000000000 present=linear`
  - `mtrr6 covers fb: base=4000000000 mask=4000000800 type=0 (UC - SLOW PRESENT!)`
  - `fb bench: vram 14062 KB/s ram 1828932096 KB/s ratio 1:130062  << UNCACHED-CLASS VRAM`
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

### F4 — I2C-HID pointer/keyboard don't bind on the Surface  ·  OS / DRIVER  ·  S2 (feature) / S4 (OSK)  ·  OPEN
Three LPSS I2C controllers are found but **no** I2C-HID device binds, so the
native trackpad and keyboard are unavailable; input falls back to the firmware
Absolute/Simple pointers, and because no native keyboard binds, firmware ConIn
keeps being polled → the firmware on-screen keyboard stays drawn (the OSK icon).
This is also why the Surface never detaches.
- **Reproduced in run 2** (identical: `ctrls=3 present=0`).
- Evidence (`BOOTENV.TXT`): `i2c-hid: ctrls=3 present=0 addr=0 desc_parsed=0`;
  `pointer: fw_simple=1 fw_abs=3 detach_blocked=0`; `ps2: kbd=0 aux=0`;
  `usb-hid: kbd=0 mouse=0`.
- Open question for the fix session: why the I2C-HID probe finds controllers but
  no device (address discovery? the Surface's HID-over-I2C descriptor path?).
  Cross-ref the LPSS-clock / 64-bit-BAR history in the pc64 trackpad notes.

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

### F5 — open-window count creeps up over a long run  ·  HARNESS (watch)  ·  S4  ·  OPEN
Across run 2 the driver's open-window count drifted 9 → 12 over 31 passes: the
smoke phase opens more than the close phase closes. Harmless at this rate, but
unoui has a 24-window cap, so a long enough run would start hitting `ui_add`
failures and the run would silently change character. Not an OS bug — it is the
driver's phase balance. Worth rebalancing if we ever do very long soaks.
- Evidence: `PF002` `open_windows=9` → `PF032` `open_windows=12`.

## To carry into future runs

- Surface stays **attached** (reset/detach/watchdog all take the attached path);
  X1 (has i8042) and Latitude (AHCI) will likely **detach** → exercise the
  native-IDT + LAPIC-watchdog + CF9-reset paths instead. Compare boot-env blocks.
- Grab `\BOOTENV.TXT` first each run (a re-boot overwrites it), then the whole
  `\CRASH` folder.
