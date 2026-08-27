# Cosmo port — handoff / start-here

State as of 2026-08-27. Read this first when resuming the Cosmo Communicator (MT6771)
UnoDOS port. The port CODE is here (`hmofet/unodos`, branch `cosmo-port`); the device
RESEARCH and the multi-GB firmware/source ASSETS live in a separate repo, `hmofet/cosmo`
— see "Where everything is" below.

## Where we are

- **Phase 0 (offline bring-up research): DONE.** All register facts are in the research
  repo's `research/COSMO-BRINGUP.md`.
- **Phase 1 (device prep): DONE on hardware.** The Cosmo is a three-OS multiboot —
  Android V25 + Gemian (Debian/KDE, root shell `gemian`/`gemian`) + an **empty `p42`
  slot reserved for UnoDOS**. Factory recovery is guaranteed (see the research repo).
  Details: `research/device-state.md`.
- **Phase 3 (port scaffold): DONE + harness-verified.** This `cosmo/` port builds
  end-to-end and emits `build/unodos-boot.img`; the M1 launcher, M2 nav, and M3 apps
  render (`shots/`). Based on the rpi 640×480 landscape core.
- **Phase 2 (run-time framebuffer discovery): DONE + harness-verified.** `fb_init`
  walks the DTB LK passes in `x0` for the `videolfb` handoff, so the port no longer
  depends on a hardcoded framebuffer address. A stage beacon is wired in. **The image
  is ready for a first hardware boot** — that is the next thing to do, and it needs
  the phone in hand.

The scaffold: `_start` saves the DTB (`x0`), disables the TOPRGU watchdog
(`0x10007000 ← 0x22000000`), runs cache-off/MMU-on, then `fb_init` **adopts LK's
framebuffer** (no display bring-up). Frame pacing = `cntpct_el0`. Input is the AW9523
I2C keyboard matrix (Phase 5) — AUTOTEST scripts drive for now; audio is Phase 9.

## What Phase 2 landed, and two corrections it forced

`fb_init` now calls `fb_dtb_scan`, which linearly walks the FDT struct block and takes
the framebuffer from, in order of preference: the device tree, a pre-seeded `FBINFO`
(the harness playing LK, and only when `FB_SEED_MAGIC` says so — on the device `FBINFO`
is uninitialised DRAM and must never be trusted), then the `COSMO_FB` guess. Where it
got the base is recorded at `fb_src`, and the vramSize it read at `fb_vram`, both
inside `FBINFO`.

Two things the LK source says that the earlier research did not:

1. **A production LK does NOT emit `atag,videolfb-fb_base_l`.** That property, and the
   `-fb_base_h` / `-vramSize` pair beside it, are written by
   `mt_disp_config_frame_buffer` (`platform/mt6771/mt_disp_drv.c:743`), which
   `app/mt_boot/mt_boot.c:1139` calls **only under `MACH_FPGA_NO_DISPLAY`** — and
   `platform/mt6771/rules.mk:45` defines that only when `DEVELOP_STAGE=FPGA`. The
   normal path (`mt_boot.c:1122`) writes **one packed blob property, `atag,videolfb`**,
   built by `target_atag_videolfb` (`platform/mt6771/atags.c:413`):

   | offset | field |
   |---|---|
   | +0  | `u64 fb_addr_pa_k` (= `g_fb_base`; LK identity-maps DRAM) |
   | +8  | `u32 islcmfound` |
   | +12 | `u32 fps` |
   | +16 | `u32 vramSize` |
   | +20 | `char lcmname[]`, NUL-terminated |

   Those fields are written with plain stores, so they are **native little-endian**
   inside an otherwise big-endian FDT property. `fb_dtb_scan` accepts both forms and
   lets the split properties win if a tree somehow carries both.

2. **LK's framebuffer is portrait 1080×2160 with a 4352-byte stride, not landscape
   2160×1080 at 8640.** The blit LK configures uses
   `src_pitch = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT) * 4`
   (`mt_disp_drv.c:489`) where `CFG_DISPLAY_WIDTH = DISP_GetScreenWidth()` = the LCM's
   `params->width` = **1080** (`aeon_nt36672…` `FRAME_WIDTH 1080`, `FRAME_HEIGHT 2160`).
   `MTK_LCM_PHYSICAL_ROTATION=270` is a kernel-side rotation applied *after* LK.
   Cross-check: `ALIGN(1080,32)*ALIGN(2160,32)*4*3 + (1080*2160*2+4096)`, aligned to
   64 KB, is exactly the `0x1F90000` vramSize the research doc derived. `PANEL_W`,
   `PANEL_H` and `COSMO_PITCH` in `kernel.s` were corrected to match.

   **Consequence to expect in the first photograph:** the UI will be a correctly
   shaped 640×480 rectangle *rotated 90°* on the open clamshell. That is the panel
   doing its job, not a bug in the walk; the fix is a rotated blit, and it is worth
   confirming from a photo before writing one.

### The debug beacon (there is no UART)

Three channels, so a blank screen still says something:

- **`bcn_mark`, always on.** Stamps a stage number at `FBINFO+44` with `BCN_MAGIC`
  ("UNO1") beside it. Stages: `1` core 0 alive + watchdog off, `2` fb_init ran but the
  DTB had no framebuffer, `3` fb_init took a framebuffer from the DTB, `4` launcher
  drawn and the main loop entered. `_start` zeroes the magic first, so a stale mark
  from a previous boot cannot be misread.
- **The bar beacon, always on.** `fb_init` paints one white 32-px block per stage along
  the top-left of the **raw** framebuffer, outside the centred UI. Bars but no UI ⇒ the
  base is right and the geometry/centring is wrong. No bars ⇒ wrong address entirely.
- **`BEACON=1 ./build.sh`, off by default.** Pulses the vibrator that many times through
  the PMIC wrapper — the only channel that survives a completely wrong framebuffer
  address. Registers, all `[LK-SRC]`: PWRAP `0x1000D000`, `WACS2_CMD +0xC20`,
  `WACS2_RDATA +0xC24`, FSM `= (RDATA>>16)&7`, IDLE `= 0`; a write is "spin to IDLE,
  then store `(1<<31) | ((addr>>1)<<16) | data16`"; MT6358 `LDO_VIBR_CON0 = 0x1D08`,
  `RG_LDO_VIBR_EN` = bit 0 ⇒ on `0x8E840001`, off `0x8E840000`. The idle spin is
  **bounded** (`PWRAP_SPIN`), so a wedged wrapper drops the pulse instead of hanging the
  boot. It is off by default because none of this has ever been executed on the device.

### Harness coverage

`harness.py` now builds a **real FDT** and hands it to the payload in `x0`, then asserts
which source `fb_init` believed, the base, the vramSize, the bar beacon and the stage
reached. It exits non-zero on a mismatch, so it is a gate, not just a screenshot tool.

```sh
for m in blob props both empty none; do
  for x in "" --no-preseed --junk-fbinfo; do
    python cosmo/harness.py cosmo/build/unodos.bin /tmp/$m.png 20 --fdt=$m $x || exit 1
  done
done
```

`blob` = the production LK shape (default), `props` = the FPGA shape, `both` = the split
properties must win over a decoy blob, `empty` = a valid tree with no framebuffer,
`none` = no FDT at all. `--no-preseed` empties `FBINFO` so only the tree can supply a
base; with `--fdt=none --no-preseed` the `COSMO_FB` last-resort guess is exercised too.
`--junk-fbinfo` fills `FBINFO` with a plausible-looking base and stride that carry no
`FB_SEED_MAGIC` — **the device hands us uninitialised DRAM at `FBINFO`**, so `fb_init`
zeroes anything not explicitly marked as seeded rather than jumping to whatever DRAM
held. All fifteen combinations are green, and the tree carries a decoy
`atag,videolfb-not-really` property in a node *before* `/chosen`, so a walk that cannot
skip nodes, or that prefix-matches names, fails in the harness rather than on the
device.

## The next task — first hardware boot

This needs the phone. Nothing else in the port is blocked on it.

1. `./build.sh` (add `BEACON=1` if you want the vibrator pulses).
2. Boot Gemian, copy `build/unodos-boot.img` over, then in its root shell:
   ```sh
   sudo dd if=unodos-boot.img of=/dev/mmcblk0p42 bs=1M
   sudo parted /dev/mmcblk0 name 42 UNODOS      # once
   sudo reboot                                  # then pick UNODOS from LK's menu
   ```
3. **Photograph whatever appears** and read it against the beacon table above.
4. If the screen is blank, boot back into Gemian and peek at the beacon before
   anything overwrites it — `FBINFO` is at physical `0x40320000`, and the interesting
   words are `+40` `fb_src`, `+44` stage, `+48` `BCN_MAGIC` (`0x554E4F31`):
   ```sh
   sudo dd if=/dev/mem bs=4 skip=$((0x40320000/4)) count=16 2>/dev/null | xxd
   ```
   If that reads back zeros or is refused, the vendor kernel has `STRICT_DEVMEM` on;
   `devmem2 0x40320030` (three times, `+0x2C`/`+0x30`) or a small `/dev/mem` mmap helper
   gets the same three words. Treat a missing magic as "nothing to read", **not** as
   "the payload never ran" — DRAM survives a warm reboot only opportunistically, so this
   channel confirms, it does not refute. The vibrator (`BEACON=1`) is the channel that
   refutes.

Hardware-only unknowns to expect (a few iteration cycles, like rpi/pinephone bring-up):
whether LK populates the `videolfb` property for a `p42` payload at all; **rotation**
(see correction 2 — the UI appearing sideways is the predicted first result); pixel byte
order (BGRA vs RGBA — mis-colours, does not black out); FB-write cache coherency;
`cntpct` pacing rate (read `CNTFRQ_EL0` and compare against `FRAME_TICKS`).

Then: Phase 5 keyboard (AW9523 I2C matrix — addr `0x58`, map in the research repo),
Phase 7 storage (eMMC `msdc@11230000` → Android `p44`/userdata), Phase 9 stretch.

## Where everything is

### Port code (this repo — `hmofet/unodos`, branch `cosmo-port`)
- Worktree on amanuensis: `C:\Users\arin\Documents\Github\unodos-cosmo`.
- `cosmo/kernel.s` (boot + `fb_init`/`fb_dtb_scan` + beacon + launcher/apps),
  `cosmo/*.inc.s` (apps), `cosmo/build.sh` (cross-assembles on **quill** — WSL is dead
  on the build host — then `mkbootimg.py` wraps the payload), `cosmo/harness.py`,
  `cosmo/shots/`.
- Contract: `[world.cosmo]`/`[port.cosmo]` in `unodef/unodef.toml` → `unodef/gen/cosmo/
  sys_gen.inc` (regenerate with `unodef/unogen.py`, then stage ONLY `gen/cosmo` — a full
  run churns every port's gen file cosmetically; `git checkout -- unodef/gen/` the rest).

### Research + assets (the OTHER repo — `hmofet/cosmo`)
- On amanuensis: `C:\Repos\cosmo`. Read its `ARCHIVE-ACCESS.md` for full access details.
- **Docs (git-tracked):** `research/COSMO-BRINGUP.md` (all register facts: WDT, FB
  handoff/videolfb, AW9523 keyboard, eMMC, panel), `research/device-state.md` (device
  layout, `p42`, the Gemian `dd` loop, recovery), `research/unodos-port-plan.md` (the
  phased plan), `docs/multiboot-runbook.md` (factory rebuild).
- **Assets (multi-GB, gitignored — NOT on GitHub):** the stock firmware, vendor MT6771
  kernel mirrors, and the **LK bootloader source** (`archive/lk-src/`, the surviving
  copy from Software Heritage). Two locations:
  - **NAS (durable, use when amanuensis is offline):**
    `root@192.168.2.20:/mnt/bulk/archives/cosmo/`
  - amanuensis working copy: `C:\Repos\cosmo\archive\`
  - LK source as a compact tarball on the NAS: `cosmo-lk-v23.tar.gz`.
  - **License:** LK core is MIT, MediaTek `platform/` is proprietary → read-for-facts
    only, never copy into UnoDOS (flagged in `COSMO-BRINGUP.md`).

### Device
Configured, `p42` empty, recovery guaranteed. Iterate via Gemian's root shell (`dd` to
`p42`); a bad image cannot brick. **Never write `lk`/`lk2`/`preloader`.**
