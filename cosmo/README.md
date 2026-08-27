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
   lit the NT36672 panel and left it scanning a surface out of DRAM. `fb_init` finds
   that surface by **walking the DTB in `x0`** for LK's `videolfb` handoff in
   `/chosen` (its base is allocated at boot, so it is not a fixed address), then
   centres our 640×480 UI in it.

There is no display bring-up, no DSI, no CCU programming — the whole PinePhone panel
wall is bypassed because LK always runs before us. Frame pacing is the ARM generic
timer (`cntpct_el0`). Input is the AW9523 I2C keyboard matrix (Phase 5); until then the
AUTOTEST scripted pad drives the milestones. Audio (MTK AFE) is Phase 9 — silent.

### The framebuffer handoff, precisely

LK writes the handoff into `/chosen`, and **which property it uses depends on how LK
was built** (`app/mt_boot/mt_boot.c`):

| build | property | encoding |
|---|---|---|
| production (what the Cosmo runs) | `atag,videolfb` — one packed blob: `u64 fb_base`, `u32 islcmfound`, `u32 fps`, `u32 vramSize`, `char lcmname[]` | **native little-endian** (written with plain stores) |
| `MACH_FPGA_NO_DISPLAY` (`DEVELOP_STAGE=FPGA`) | `atag,videolfb-fb_base_h` / `-fb_base_l` / `-vramSize` | big-endian, like every other FDT property |

`fb_dtb_scan` accepts both. `vramSize` should read back `0x1F90000` (~31.6 MB) on the
device — that is the sanity check that the walk found the real thing.

**Geometry:** LK's buffer is the panel's *native portrait* 1080×2160 with a
`ALIGN(1080,32)*4 = 4352`-byte stride; the 270° rotation to landscape is applied later
by the Android kernel, not by LK. So the first photograph is expected to show the UI
rotated 90°, and a rotated blit is the fix if it does.

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

`python cosmo/harness.py <unodos.bin> <out.png> [instr_millions] [--fdt=MODE]
[--no-preseed]` runs the real payload on Unicorn (UC_ARCH_ARM64), playing LK: it builds
a **real flattened device tree**, hands it to the payload in `x0`, and checks that
`fb_init` took the framebuffer from where the run intended (exit 1 if not). It then
renders the centred 640×480 UI to a PNG. See `shots/` for milestone captures.

`--fdt=` selects the tree: `blob` (default, the production LK shape), `props` (the
FPGA shape), `both` (the split properties must win), `empty` (a valid tree with no
framebuffer), `none` (no FDT at all). `--no-preseed` empties `FBINFO` so only the tree
can supply a base; `--junk-fbinfo` fills it with plausible garbage instead, because the
device hands us uninitialised DRAM there and `fb_init` must ignore anything not marked
with `FB_SEED_MAGIC`. All fifteen combinations are green.

```sh
for m in blob props both empty none; do
  for x in "" --no-preseed --junk-fbinfo; do
    python cosmo/harness.py cosmo/build/unodos.bin /tmp/$m.png 20 --fdt=$m $x || exit 1
  done
done
```

## Debug beacon (there is no UART)

A blank screen cannot tell "the payload never ran" from "it ran and the framebuffer
address is wrong". Three channels answer that:

- `bcn_stage` / `bcn_magic` at `FBINFO+44` record the last stage reached (1 = alive and
  the watchdog is off, 2 = no framebuffer in the DTB, 3 = one came from the DTB,
  4 = launcher drawn, main loop entered). Always on.
- `fb_init` paints one white 32-px bar per stage at the **top-left of the raw
  framebuffer**, outside the centred UI. Bars but no UI ⇒ the base is right and the
  geometry is not.
- `BEACON=1 ./build.sh` additionally pulses the **vibrator** that many times through
  the PMIC wrapper. This is the only channel that works with the framebuffer address
  completely wrong. **Off by default** — the PWRAP/MT6358 register facts come from LK
  source and have never been executed on this device.

## Status

- **M1 (launcher) + M2 (nav / app launch) + M3 (apps): harness-verified.** The full
  11-icon desktop renders and the AUTOTEST pad launches apps full-screen (`shots/`).
- **Not yet on hardware.** `fb_init` currently reads the FB base from `FBINFO` (harness)
  or a fixed fallback; the run-time **DTB `videolfb` walk** (the real hardware FB base,
  which LK allocates dynamically) is the Phase 2 task — see the `TODO` in `fb_init` and
  `COSMO-BRINGUP.md`. Keyboard (AW9523), eMMC storage, USB, and audio follow.
