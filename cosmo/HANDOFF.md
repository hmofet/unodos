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

The scaffold: `_start` saves the DTB (`x0`), disables the TOPRGU watchdog
(`0x10007000 ← 0x22000000`), runs cache-off/MMU-on, then `fb_init` **adopts LK's
framebuffer** (no display bring-up). Frame pacing = `cntpct_el0`. Input is the AW9523
I2C keyboard matrix (Phase 5) — AUTOTEST scripts drive for now; audio is Phase 9.

## The next task — first hardware boot

**`fb_init` needs the run-time DTB `videolfb` walk.** Today it reads the FB base from
`FBINFO` (the harness pre-seeds it) or a fixed `COSMO_FB` fallback. On real hardware
neither is right: **LK allocates the framebuffer base dynamically** (`mblock_reserve_ext`,
top-down under `0x80000000`) and passes it in the DTB. So:

1. In `fb_init` (see the `TODO` there), parse the flattened device tree at `dtb_ptr`
   (the DTB pointer LK passed in `x0`, already saved) for the properties
   **`atag,videolfb-fb_base_l`** (FB base) and **`atag,videolfb-vramSize`**. Fall back to
   `COSMO_FB` only if absent. `g_fb_size` should read back ≈ `0x1F90000` (sanity check).
2. Extend `harness.py` to hand the payload a minimal real FDT in `x0` carrying those
   properties, so the walk is verified before touching hardware.
3. Optionally add a **1-bit debug beacon** (vibrator or camera flash via the PMIC) that
   blinks a stage count — with no UART, this is the only way a blank-screen boot tells
   you "the payload ran and the watchdog stayed off." GPIO/PMIC register is a
   `[KERNEL-TODO]` in `COSMO-BRINGUP.md`.
4. Build (`./build.sh`), then on the device (in Gemian): copy `build/unodos-boot.img`
   over and `sudo dd if=unodos-boot.img of=/dev/mmcblk0p42 bs=1M`; once,
   `sudo parted /dev/mmcblk0 name 42 UNODOS`; `reboot` and pick UNODOS from LK's menu.
   Photograph the result and iterate.

Source of truth for the walk: the LK source in the research repo
(`archive/lk-src/platform/mt6771/mt_disp_drv.c` writes the props; `platform.c`/`atags.c`
show the values). See `COSMO-BRINGUP.md` §"The exact fb_base is NOT a static address".

Hardware-only unknowns to expect (a few iteration cycles, like rpi/pinephone bring-up):
whether LK populates the `videolfb` props for a `p42` payload; pixel byte order
(BGRA vs RGBA — could mis-colour, not black); exact panel geometry/rotation; FB-write
cache coherency; `cntpct` pacing rate.

Then: Phase 5 keyboard (AW9523 I2C matrix — addr `0x58`, map in the research repo),
Phase 7 storage (eMMC `msdc@11230000` → Android `p44`/userdata), Phase 9 stretch.

## Where everything is

### Port code (this repo — `hmofet/unodos`, branch `cosmo-port`)
- Worktree on amanuensis: `C:\Users\arin\Documents\Github\unodos-cosmo`.
- `cosmo/kernel.s` (boot + fb_init + launcher/apps), `cosmo/*.inc.s` (apps),
  `cosmo/build.sh` (cross-assembles on **quill** — WSL is dead on the build host —
  then `mkbootimg.py` wraps the payload), `cosmo/harness.py`, `cosmo/shots/`.
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
