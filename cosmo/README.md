# UnoDOS 3 — Cosmo Communicator (MediaTek MT6771, AArch64)

The third AArch64 world after `rpi` and `pinephone`, and the first non-Linux OS
targeted at any MediaTek phone SoC. It reuses the rpi 640×480 landscape core (the
Cosmo is a keyboard clamshell) retargeted to the MT6771 / Helio P70. Register-level
bring-up facts and the device layout live in the companion research repo
(`hmofet/cosmo`: `research/COSMO-BRINGUP.md`, `research/device-state.md`).

## How it boots

Planet's LK loads this payload (wrapped in an Android `boot.img`) from multiboot slot
**`p38`** (`UNODOS`; slot map since 2026-08-28: p38 = UNODOS, p41 = DEBIAN12, p42 =
TRIXIE -- do NOT dd to p42) as the "kernel", per the arm64 boot protocol: DTB in `x0`, entry at
`0x40080000`, MMU off. `_start`:

1. saves the DTB pointer, parks secondary cores;
2. **disables the TOPRGU watchdog** (`0x10007000` ← `0x22000000`) — LK armed it, and a
   payload that ignores it reboots after ~30 s;
3. runs cache-off / MMU-on so writes to the framebuffer reach DRAM coherently;
4. **adopts LK's live framebuffer** rather than bringing up the display — LK already
   lit the NT36672 panel and left it scanning a surface out of DRAM. `fb_init` finds
   that surface by **walking the DTB in `x0`** for LK's `videolfb` handoff in
   `/chosen` (its base is allocated at boot, so it is not a fixed address);
5. points every drawing primitive at an upright 640×480 **shadow surface**, and
   `fb_present` rotates that onto the panel once per frame, centred.

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
`ALIGN(1080,32)*4 = 4352`-byte stride — the panel is physically mounted rotated, which
is why the Cosmo's LK project sets `MTK_LCM_PHYSICAL_ROTATION = 270`
(`project/k71v1_64_bsp.mk:16`).

So we rotate. Drawing goes to a plain upright 640×480 **shadow surface** — `pchar`,
`pstr`, `frect` and `picon` need no rotation awareness at all — and `fb_present`
rotates the whole surface onto the panel once per frame. The handedness is not a
guess: LK's own console blit for this device (`dev/video/mtk_cfb.c`, the branch
commented *"THIS IS THE DEFAULT ROTATION FOR COSMO"*) steps `+PIXEL_SIZE` per glyph
row and puts the glyph's leftmost pixel at the highest multiple of `VDO_COLS`, i.e.
**down the upright image is `+1` in framebuffer x, and right is `-1` in framebuffer
y**. That is `FB_ROT 270`, the default.

`fb_present` walks the *destination* as an ascending raster and lets the source carry
the rotation, so the framebuffer writes stay contiguous (four pixels per `stp`) — with
the D-cache off that is the difference between merged bursts and 307k scattered word
writes. It costs roughly 1M instructions per frame at
1:1, ~3M at the default `FB_SCALE=2` (each UI pixel becomes a 2×2 block, the largest
integer scale the 1080-px short side fits); its real cost on hardware is memory-bound,
so measure before optimising it further. `SCALE=1 ./build.sh` restores 1:1.

The shadow lives in **page 1 of LK's own VRAM** (`fb_base + PANEL_H*pitch`) whenever the
DTB's `vramSize` proves there is room: LK reserved 33 MB and has finished with all of
it, so that memory is free and cannot collide with an SSPM/SCP/consys carveout the way
a guessed low-DRAM address could. `COSMO_SHADOW` is the fallback.

## Build

WSL is dead on the build host, so the AArch64 toolchain runs on `quill` over SSH; the
data generator and boot-image wrapper run locally.

```sh
./build.sh              # -> build/unodos.bin + build/unodos-boot.img (launcher)
./build.sh dostris      # AUTOTEST build the harness drives (also: nav app clock theme
                        #   paint pacman outlast tracker fs)
ROT=90 ./build.sh       # override the panel rotation (default 270); BEACON=1 adds the
                        #   vibrator stage pulses
```

### The boot image is not just "the payload in a header"

LK's 64-bit path is picky, and a raw payload fails it three ways. All three are read out
of the archived LK source and confirmed against the stock v23 `boot.img`:

1. **The payload must be gzipped.** `mt_boot.c:712` calls `decompress_kernel()`
   unconditionally on the 64-bit path; there is no "is it compressed?" test. A raw arm64
   `Image` gets `decompress kernel image fail!!!` and LK spins in `while(1)`.
2. **A device tree must be appended.** `fdt_op.c:373-395` reads the last `DTB_MAX_SIZE`
   bytes of the kernel region and scans **backwards** for `d00dfeed`; no hit means
   `can't find dtb`. LK then fixes that tree up — this is where `atag,videolfb` lands —
   and hands it to us in `x0`, so the tree we append is the tree `fb_dtb_scan` walks.
3. **The kernel region must be at least `DTB_MAX_SIZE - page_size` = 522,240 bytes.**
   That scan computes `offset = page_sz + kernel_sz - 512K` and reads from it; for a
   23 KB payload it goes negative. `mkbootimg.py` pads to exactly the floor, which puts
   LK's scan window exactly over our image.

Result: `[2048-byte header][gzip(payload)][zero pad][device tree]`, 524,288 bytes total,
well under the 32 MiB slot ceiling.

**The device tree is a vendor blob and is not in this repo.** Point `--dtb` or
`$COSMO_DTB` at `analysis/v23/cosmo-boot.dtb` in the research repo; `mkbootimg.py` looks
in `C:/Repos/cosmo/...` by default and fails with an explicit message if it cannot find
one.

## Install / iterate (on the device)

Boot into Gemian (root shell `gemian`/`gemian`), copy the image over, and:

```sh
dd if=unodos-boot.img of=/dev/mmcblk0p38 bs=1M conv=fsync   # as root; p38 = UNODOS
reboot                                        # then pick UNODOS from LK's menu
```

A bad image cannot brick the device — LK still offers Android and Gemian. **Never write
`lk`/`lk2`/`preloader`.**

## Harness

`python cosmo/harness.py <unodos.bin> <out.png> [instr_millions] [--fdt=MODE]
[--no-preseed]` runs the real payload on Unicorn (UC_ARCH_ARM64), playing LK: it builds
a **real flattened device tree**, hands it to the payload in `x0`, and checks that
`fb_init` took the framebuffer from where the run intended (exit 1 if not). It then
renders the centred 640×480 UI to a PNG. See `shots/` for milestone captures.

The PNG is **what the eye sees**: the harness reads the panel back through the physical
mounting and checks the result is the shadow surface, pixel for pixel. That mounting is
fixed at 270 in the harness because it is a property of the hardware, so only a payload
whose blit matches it reads back upright — a `ROT=90` build fails the check, which is
what makes it a test rather than a self-consistency loop.

`--fdt=` selects the tree: `blob` (default, the production LK shape), `props` (the
FPGA shape), `both` (both forms present and disagreeing, as the real device does — the blob must win), `empty` (a valid tree with no
framebuffer), `none` (no FDT at all). `--no-preseed` empties `FBINFO` so only the tree
can supply a base; `--junk-fbinfo` fills it with plausible garbage instead, because the
device hands us uninitialised DRAM there and `fb_init` must ignore anything not marked
with `FB_SEED_MAGIC`. `--rot=N` says the payload was built with `FB_ROT=N`, which
relaxes the upright check for a deliberate diagnostic build. All fifteen combinations
are green.

**Budget:** the per-frame present is ~1M instructions at 1:1, ~3M at the default
`FB_SCALE=2`, so AUTOTEST scenes need a much larger instruction budget than they used
to — Dostris wants ~400M (about 40 s) to play out, which is why build.sh keeps
AUTOTEST builds at scale 1. A static shot like the launcher is fine at 30M (the
full-vram clear at boot costs ~8M of that). The harness reads the build's scale back
from `FBINFO+88` and checks every sub-position of each scaled block against the
shadow, so no flag is needed to test a `SCALE=1` build.

```sh
for m in blob props both empty none; do
  for x in "" --no-preseed --junk-fbinfo; do
    python cosmo/harness.py cosmo/build/unodos.bin /tmp/$m.png 30 --fdt=$m $x || exit 1
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
