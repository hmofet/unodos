# UnoDOS 3 — Cosmo Communicator (MediaTek MT6771, AArch64)

The third AArch64 world after `rpi` and `pinephone`, and the first non-Linux OS
targeted at any MediaTek phone SoC. It reuses the rpi 640×480 landscape core (the
Cosmo is a keyboard clamshell) retargeted to the MT6771 / Helio P70. Register-level
bring-up facts and the device layout live in the companion research repo
(`hmofet/cosmo`: `research/COSMO-BRINGUP.md`, `research/device-state.md`).

## How it boots

Planet's LK loads this payload (wrapped in an Android `boot.img`) from multiboot slot
**`p42`** as the "kernel", per the arm64 boot protocol: DTB in `x0`, entry at
`0x40080000`, MMU off. `_start`:

1. saves the DTB pointer, parks secondary cores;
2. **disables the TOPRGU watchdog** (`0x10007000` ← `0x22000000`) — LK armed it, and a
   payload that ignores it reboots after ~30 s;
3. runs cache-off / MMU-on so writes to the framebuffer reach DRAM coherently;
4. **adopts LK's live framebuffer** rather than bringing up the display — LK already
   lit the NT36672 panel and left it scanning a landscape 2160×1080 surface out of
   DRAM. `fb_init` takes the panel base/pitch and centres our 640×480 UI in it.

There is no display bring-up, no DSI, no CCU programming — the whole PinePhone panel
wall is bypassed because LK always runs before us. Frame pacing is the ARM generic
timer (`cntpct_el0`). Input is the AW9523 I2C keyboard matrix (Phase 5); until then the
AUTOTEST scripted pad drives the milestones. Audio (MTK AFE) is Phase 9 — silent.

## Build

WSL is dead on the build host, so the AArch64 toolchain runs on `quill` over SSH; the
data generator and boot-image wrapper run locally.

```sh
./build.sh              # -> build/unodos.bin + build/unodos-boot.img (launcher)
./build.sh dostris      # AUTOTEST build the harness drives (also: nav app clock theme
                        #   paint pacman outlast tracker fs)
```

`build/unodos-boot.img` is a header-v1 Android boot image sized well under the 32 MiB
`p42` slot ceiling (payload ≈ 22 KB).

## Install / iterate (on the device)

Boot into Gemian (root shell `gemian`/`gemian`), copy the image over, and:

```sh
sudo dd if=unodos-boot.img of=/dev/mmcblk0p42 bs=1M
sudo parted /dev/mmcblk0 name 42 UNODOS      # once
sudo reboot                                   # then pick UNODOS from LK's menu
```

A bad image cannot brick the device — LK still offers Android and Gemian. **Never write
`lk`/`lk2`/`preloader`.**

## Harness

`python cosmo/harness.py <unodos.bin> <out.png> [instr_millions]` runs the real payload
on Unicorn (UC_ARCH_ARM64), playing LK by pre-seeding `FBINFO` with a panel framebuffer,
then renders the centred 640×480 UI to a PNG. See `shots/` for milestone captures.

## Status

- **M1 (launcher) + M2 (nav / app launch) + M3 (apps): harness-verified.** The full
  11-icon desktop renders and the AUTOTEST pad launches apps full-screen (`shots/`).
- **Not yet on hardware.** `fb_init` currently reads the FB base from `FBINFO` (harness)
  or a fixed fallback; the run-time **DTB `videolfb` walk** (the real hardware FB base,
  which LK allocates dynamically) is the Phase 2 task — see the `TODO` in `fb_init` and
  `COSMO-BRINGUP.md`. Keyboard (AW9523), eMMC storage, USB, and audio follow.
