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
- **Panel rotation: DONE + harness-verified.** Drawing goes to an upright 640×480
  shadow surface and `fb_present` rotates it onto the portrait panel each frame, so
  the first photograph should be **upright**, not sideways. Direction derived from LK,
  not guessed; `ROT=90 ./build.sh` if the photo disagrees.

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
   Cross-check: `ALIGN(1080,32)*ALIGN(2160,32)*4*3 + (1080*2160*2+4096)`, aligned to
   64 KB, is exactly the `0x1F90000` vramSize the research doc derived. `PANEL_W`,
   `PANEL_H` and `COSMO_PITCH` in `kernel.s` were corrected to match.

   The panel is physically mounted rotated, which is what
   `MTK_LCM_PHYSICAL_ROTATION = 270` in the Cosmo's own LK project
   (`project/k71v1_64_bsp.mk:16`, next to `CUSTOM_LK_LCM` naming this device's two
   panels) records. **That is now handled — see the next section.**

## The rotated blit

Drawing goes to a plain upright SCRW×SCRH **shadow surface**; `fb_present` rotates the
whole surface onto the panel once per frame. `pchar`, `pstr`, `frect` and `picon` are
the only four primitives that touch the framebuffer and **none of them changed** —
`fb_base`/`fb_pitch` simply point at the shadow now.

**The direction is derived, not guessed.** LK's own console blit for this device
(`dev/video/mtk_cfb.c`, `CFB_X888RGB_32BIT`, annotated `// USED BY COSMO LCD`, with the
270 branch commented `// THIS IS THE DEFAULT ROTATION FOR COSMO`) advances `tdest` by
`PIXEL_SIZE` per glyph **row** and writes the glyph's **leftmost** pixel at the
**highest** multiple of `VDO_COLS`. So on this panel:

- moving **down** the upright image ⇒ **+1 in framebuffer x**
- moving **right** the upright image ⇒ **−1 in framebuffer y**

which is `FB_ROT 270`. The `90` branch is the exact mirror of that, which is the
self-consistency check that the reading is the right way round.

Design notes worth keeping:

- `fb_present` walks the **destination** as an ascending raster and lets the source
  carry the rotation (a start offset plus two signed steps). That keeps framebuffer
  writes contiguous — four pixels per `stp` — which matters a lot with the D-cache
  off. The strided side is the shadow reads, which are the cheaper side to stride.
- The shadow lives in **page 1 of LK's own VRAM** (`fb_raw + PANEL_H*ppitch`) whenever
  `vramSize` proves there is room. LK reserved 33 MB (triple buffer + DAL layer) and is
  finished with all of it, so that memory is free *and* cannot collide with the
  SSPM/SCP/consys carveouts that make any guessed low-DRAM address risky.
  `COSMO_SHADOW` (0x40500000) is the fallback for when the DTB gave no `vramSize`.
- Cost is ~1M instructions per frame. On hardware the real cost is memory-bound
  (307k uncached strided reads), so **measure before optimising**; a 4×4 block
  transpose, or enabling the D-cache with explicit maintenance, are the obvious levers.
- `ROT=90|180|0 ./build.sh` overrides it, so any of the four possible first-photo
  outcomes is one rebuild away rather than a rewrite.

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

It also renders **what the eye sees**: it reads the panel back through the physical
mounting and requires the result to equal the shadow surface pixel for pixel. The
modelled mounting is fixed at 270 because that is a property of the hardware, so a
`ROT=90` build **fails** the check (verified: 24,552 of 307,200 pixels differ) — that
is what makes it a test of the blit rather than a self-consistency loop. `--rot=N`
declares a deliberate diagnostic build and relaxes the upright requirement.

**Instruction budgets went up.** The per-frame present is ~1M instructions, so AUTOTEST
scenes need roughly 7× what they used to: Dostris wants ~400M (about 40 s) to play out
to the same frame. A static shot like the launcher is still fine at 20M.

```sh
for m in blob props both empty none; do
  for x in "" --no-preseed --junk-fbinfo; do
    python cosmo/harness.py cosmo/build/unodos.bin /tmp/$m.png 20 --fdt=$m $x || exit 1
  done
done
# and an AUTOTEST scene, which now needs a much larger budget (see below)
./cosmo/build.sh dostris && python cosmo/harness.py cosmo/build/unodos_dt.bin /tmp/dt.png 400
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

## Measured on the device (2026-08-27, over SSH from Gemian)

The device is reachable as `ssh cosmo` (`cosmo@192.168.2.56`, root key installed too;
hostname `cosmocom`, vendor kernel 4.4.146 aarch64). That let the biggest first-light
unknowns be answered **without flashing anything**.

### LK does pass a framebuffer, and there are TWO answers that disagree

`/proc/device-tree/chosen` carries **both** handoff forms:

| property | fb_base | vramSize | lcmname |
|---|---|---|---|
| `atag,videolfb` (blob, native LE) | **0x7DF70000** | **0x1F90000** | `aeon_nt36672_fhd_dsi_vdo_x800_datong` |
| `atag,videolfb-fb_base_l` (+`-vramSize`, BE) | 0x5E605000 | 0x17BB000 | `nt35595_fhd_dsi_cmd_truly_nt50358_drv` |

**The blob is the right one**, on three independent grounds: its `vramSize` is exactly
the `0x1F90000` the 1080×2160 arithmetic predicts; `base + vram = 0x7FF00000`, i.e. hard
against `mblock_reserve_ext`'s `0x80000000` cap exactly as the reservation model says;
and it names the panel this unit actually has. The split trio is **stale preloader
data** naming a different panel entirely — mt6771 builds with
`CFG_DTB_EARLY_LOADER_SUPPORT=yes`, so the preloader pre-populates the tree and LK
later adds the blob without clearing the old properties. (The preloader also leaves
`-fps`, `-islcmfound`, `-islcm_inited` and `-lcmname` behind, which is the tell: those
are not written by `mt_disp_config_frame_buffer` at all.)

**This corrected a real bug.** `fb_dtb_scan` originally preferred the split properties,
following COSMO-BRINGUP.md. On this device that would have pointed the framebuffer at
`0x5E605000` with a wrong size and drawn into nothing, and the symptom (a blank screen)
looks identical to a dozen other faults. The blob now wins; the split trio is the
fallback for an FPGA-style LK that emits no blob. `--fdt=both` in the harness models the
device exactly, stale values and all, so this cannot regress.

Also confirmed: the panel is the `x800_datong` variant, not the `x600_xinli` in the
archived LK tree. Its LCM driver is not in that tree, but `vramSize 0x1F90000` only
arises from a 1080×2160 panel, so `PANEL_W`/`PANEL_H`/`COSMO_PITCH` are unaffected.

### The boot image recipe is confirmed against a known-good image

`dd` of Gemian's own `p41` and dissecting it: gzip stream at `0x800`, DTB appended near
the end (`totalsize` 147,482), `kernel_addr 0x40080000`, `tags_addr 0x54000000`,
`page_size 2048`. Exactly the shape `mkbootimg.py` now emits, and exactly what the LK
source predicted. (Gemian uses header v0, stock v23 and we use v1; LK accepts both.)

Both stock and Gemian leave a 512-byte tail after the DTB. We leave none, deliberately:
`kernel_sz` of exactly 522,240 puts LK's scan offset at 0, so its 512 KB window covers
precisely our image and no leftover partition bytes can land in range.

### Partition state

`p42` is `EMPTY_NORMAL_BOOT_4`, 32 MiB at 5024 MiB, and is **still named that** — the
`parted /dev/mmcblk0 name 42 UNODOS` step has not been done yet.

### The keyboard cannot type half of ASCII

Planet's `aw9523_key.c` has a single 56-entry `key_map[]`, and the only symbol keycodes
in it are `KEY_APOSTROPHE`, `KEY_COMMA`, `KEY_DOT`. There is no `KEY_SLASH`,
`KEY_MINUS`, `KEY_EQUAL`, `KEY_SEMICOLON`, `KEY_LEFTBRACE`, `KEY_RIGHTBRACE`,
`KEY_BACKSLASH`, `KEY_GRAVE`, `KEY_YEN` or `KEY_RO` anywhere in the driver. `Fn` is
reported to userspace as plain `KEY_FN` (`input_set_capability(..., EV_KEY, KEY_FN)`),
so **the entire Fn layer printed on the chassis is a userspace concern** — Android does
it in Planet's IME, and Gemian's XKB does it incompletely, which is why `/` and `-` are
untypeable there today. No keyboard-layout change can fix it, because the scancode is
never generated.

**Phase 5 inherits this exactly.** Our own AW9523 driver will receive `Fn` as just
another matrix position, so the port must implement its own Fn layer or UnoDOS will have
no way to type a slash either.

## The next task — first hardware boot

This needs the phone. Nothing else in the port is blocked on it.

0. **The boot image needs the stock DTB.** `mkbootimg.py` gzips the payload, appends
   `analysis/v23/cosmo-boot.dtb` from the research repo and pads to LK's 522,240-byte
   floor — LK's 64-bit path refuses anything else (see "The boot image" below). It looks
   for the tree at `C:/Repos/cosmo/analysis/v23/cosmo-boot.dtb`, or `$COSMO_DTB`, or
   `--dtb PATH`, and fails loudly rather than emitting an image that cannot boot.
1. `./build.sh` (add `BEACON=1` if you want the vibrator pulses).
2. Boot Gemian, copy `build/unodos-boot.img` over, then in its root shell:
   ```sh
   sudo dd if=unodos-boot.img of=/dev/mmcblk0p42 bs=1M
   sudo parted /dev/mmcblk0 name 42 UNODOS      # once
   sudo reboot                                  # then pick UNODOS from LK's menu
   ```
3. **Photograph whatever appears** and read it against the beacon table above. The UI
   should be **upright and centred**. If it is sideways or upside-down, the rotation
   direction is the only thing wrong: rebuild with `ROT=90`, `ROT=180` or `ROT=0` and
   re-flash. Everything else about the image (position, size, colours) is unaffected
   by that choice.
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
whether LK populates the `videolfb` property for a `p42` payload at all; **rotation
direction** (derived from LK, so upright is the predicted result — but one photo settles
it, and `ROT=` is the fix either way); pixel byte order (BGRA vs RGBA — mis-colours, does
not black out; LK picks this at run time via `redoffset_32bit`, so it genuinely cannot be
settled offline); FB-write cache coherency; how fast `fb_present` actually is with the
D-cache off; `cntpct` pacing rate (read `CNTFRQ_EL0` and compare against `FRAME_TICKS`).

Then: Phase 5 keyboard (AW9523 I2C matrix — addr `0x58`, map in the research repo),
Phase 7 storage (eMMC `msdc@11230000` → Android `p44`/userdata), Phase 9 stretch.

## The boot image

Found while preparing the first flash: **a raw payload in an AOSP header does not boot.**
LK's 64-bit path (`app/mt_boot/mt_boot.c:679-733`, `app/mt_boot/fdt_op.c:373-405`)
requires all three of:

1. **gzip.** `decompress_kernel()` is called unconditionally at `mt_boot.c:712` — there
   is no compression test. A raw arm64 `Image` produces `decompress kernel image fail!!!`
   and LK spins in `while(1)`, which from outside looks like a dead device.
2. **An appended device tree.** `bldr_load_dtb` reads the last `DTB_MAX_SIZE` bytes of
   the kernel region and scans **backwards** for `d00dfeed`; with no hit it gives up
   with `can't find dtb`. LK fixes that tree up (this is where `atag,videolfb` is
   written) and passes it to us in `x0` — so the tree we append is the tree
   `fb_dtb_scan` reads back.
3. **`kernel_sz >= DTB_MAX_SIZE - page_size` = 522,240.** The scan computes
   `offset = page_sz + kernel_sz - 512K` (`mt_boot.h:39`) and reads from it; a 23 KB
   payload makes that negative.

`mkbootimg.py` now emits `[2048 header][gzip(payload)][zero pad][stock DTB]` = 524,288
bytes, which puts LK's scan window exactly over our image. Verified by parsing the
result the way LK does: the gzip member round-trips to the exact payload, the backward
scan finds the tree at window+377001 with `totalsize` 147,287, and the arm64 `Image`
magic and `text_offset` survive. The stock v23 image has the same shape, including
non-gzip bytes inside the range LK feeds the decompressor, so trailing data after the
stream is demonstrably fine.

The stock tree is a **vendor blob and is deliberately not committed here** (same
treatment as `fw-blobs/` elsewhere in the project). It lives at
`analysis/v23/cosmo-boot.dtb` in the research repo.

Still unverified on hardware. The cheapest confirmation is to dissect Gemian's own
boot partition, which is a known-good LK-accepted image on this exact unit:
`sudo dd if=/dev/mmcblk0p41 of=/tmp/gemian-boot.img bs=1M`, then
`python analysis/dissect_boot.py gemian-boot.img`.

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
