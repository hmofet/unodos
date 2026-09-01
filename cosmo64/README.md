# cosmo64 — pc64 on ARM64 (Cosmo Communicator, MediaTek MT6771)

The platform layer that will carry the full C pc64 system — window manager,
`.UNO` apps, browser, Office, Python — on the Planet Cosmo Communicator, the
device the self-contained assembly port (`cosmo/`, branch `cosmo-port`) reached
first light on 2026-08-31. The two ports coexist deliberately: `cosmo/` is the
instant-boot minimal UnoDOS and the hardware bring-up crib; `cosmo64/` is the
pocket workstation.

**The plan** — buckets, milestones M0–M5, toolchain decision, risks — lives in
the research repo: `research/pc64-arm-port-plan.md` in `hmofet/cosmo`.

## State: M0 COMPLETE (2026-08-31)

The toolchain is proven end to end. `m0.c` is the asm port's
`fb_init`/`fb_present` contract reimplemented in freestanding C — the videolfb
FDT walk, full-vram clear, shadow pick, and the rotated+scaled present — built
with llvm-mingw for `aarch64-w64-mingw32` (PE/COFF + LLP64, the same object
format and data model as x86 pc64), linked `-nostdlib` at LK's load address,
flattened by `flatten.py`, wrapped by the asm port's `mkbootimg.py`. Because it
honours the same FBINFO debug contract, **the existing `cosmo/harness.py` gates
it unchanged: all fifteen FDT combinations green**, including the pixel-exact
eye check.

## Build

```sh
./build.sh      # -> build/m0.bin + build/pc64arm-boot.img
```

Cross-compiles on quill (`/opt/llvm-mingw-20260826-…/bin`). Verify from the
`cosmo-port` worktree (the harness and `mkbootimg.py` live in the `cosmo/`
lane there until the branches meet — `MKBOOTIMG=` overrides the path):

```sh
python cosmo/harness.py ../unodos-pc64arm/cosmo64/build/m0.bin /tmp/m0.png 40
```

Load-bearing compile flags, explained at the top of `m0.c`: `-mstrict-align`
(MMU off ⇒ Device memory), `-mgeneral-regs-only` (no CPACR yet),
`-fno-builtin`. The linker recipe: `-nostdlib -Wl,--image-base,0x40080000
-e _start`; `flatten.py` refuses real PE imports and puts a `b _start` at
image offset 0, where the PE headers would have been.

## Install (same slot and loop as the asm port)

`dd` `build/pc64arm-boot.img` to `/dev/mmcblk0p38` from Gemian as root, reboot,
pick UNODOS. p38 is a RECOVERY_BOOT2 slot (no SPM/SCP/ccci firmware — fine for
bare metal, proven at the asm port's first light). Never write
`lk`/`lk2`/`preloader`.

## M1 part 1 DONE (2026-08-31): CPU glue + the QEMU gate

- `cpu.s`: 16-slot vector table that leaves a **crash record at 0x40321000**
  (magic/vec/ESR/ELR/FAR/EL — there is no UART, so faults must leave forensics
  in DRAM) and paints a red block at the panel origin; whole-D-cache
  set/way invalidate; `mmu_on`. `entry.s` programs CPACR before any C runs, so
  `-mgeneral-regs-only` is retired and the compiler may emit FP anywhere.
- `mmu.c`: identity map, 4 KB granule, 39-bit VA — MMIO Device below 1 GB,
  DRAM Normal WB, and **0x7C000000+ Normal non-cacheable** (LK reserves the
  framebuffer top-down under 2 GB, so display DMA stays coherent with zero
  per-frame cache maintenance).
- The shadow moved into the payload's **own .bss**: the first-light photos
  showed LK's vram page 1 scanned out as a garbage band by a leftover display
  layer. Image memory is ours by definition (and cacheable = faster reads).
- **The gate moved to real QEMU (`qharness.py`, runs on quill).** Unicorn
  stops cold at the first fetch through an enabled EL1 MMU (minimal repro
  verified) and can never do the GIC; `-M virt` has DRAM at 0x40000000 like
  the Cosmo, so the payload runs UNMODIFIED at the real device framebuffer
  address. `flatten.py` stamps an **ARM64 Image header** into the flat
  image's spare header page — LK still just executes offset 0 (`code0` is the
  branch), and QEMU's `-kernel` loader lands the image at RAM+0x80000 = the
  link address with the DTB in x0, the same contract. `-dtb` injects the
  videolfb tree; readback is QMP `pmemsave`; a payload fault is reported from
  the crash record instead of a mute failure.

## M1 part 2 DONE on QEMU (2026-09-01): the pc64 shell runs on ARM64

The full desktop -- Aurora theme, icons, taskbar, Start menu, virtual
desktops, Control Panel, the tray uptime ticking off CNTPCT -- renders under
`qharness.py` and passes the pixel-exact blit gate. What it took:

- the 23-file Tier-1 core (+ `pc64_io.c`, the portable Toolbox FS layer)
  compiled UNCHANGED except one guard: `pc64_libc.c`'s `___chkstk_ms` asm is
  x86-only (clang emits `__chkstk` here; `cpu.s` provides it as `ret`);
- `display.c` / `platform.c` / `input.c` implement the ~40-function seam
  (fb[] presented rotated 270 + 2x + R<->B swizzle -- fb.h is 0xAABBGGRR, the
  panel wants 0xAARRGGBB; time = the pc64_native.h contract on CNTPCT/CNTFRQ,
  no calibration dance);
- `stubs.c` (~120 symbols) covers storage/net/USB/audio/apps until their
  milestones land; `videolfb.c` is the adopt path shared with the m0 payload;
- the image carries ~67 MB of .bss (pc64's 32 MB static-arena heap + screen
  buffers), so the stack + FBINFO/crash block moved above it (0x53Exxxxx,
  below LK's DTB at 0x54000000); flatten.py asserts the ceiling;
- clang-vs-gcc deltas: `-fsigned-char` (limits.h hardcodes it),
  `-Wno-error=implicit-function-declaration` (pc64_uui.c's
  declared-later-in-file pattern), `-fno-builtin`.

Still M1 on hardware: boot the shell image on the device (pending -- the
device is busy). Then M2: AW9523 keyboard + Novatek touch into input.c's ring.
