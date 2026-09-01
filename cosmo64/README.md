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

## M1 COMPLETE ON HARDWARE (2026-09-01): the desktop on the phone

The Aurora desktop boots on the Cosmo from the p38 slot. Getting there took a
six-cycle visual bisect (stage beacons painted over LK's splash; a fault
handler that draws ESR/ELR/FAR as bit-cells -- no UART, no /dev/mem, and
expdb only keeps the last two boots), which established two platform laws:

1. **Ship no .bss** -- LK's decompress caps at 28 MB (mt_boot.c budgets the
   kernel window); flatten.py trims to the last real byte and entry.s
   re-zeroes from the header's res2/res3 range.
2. **EVERYTHING compiles `-mstrict-align`** -- unaligned access wedges this
   guest silently at a level EL1 vectors never see (consistent with
   GenieZone's stage-2 imposing Device-type attributes). Every build with any
   non-strict-align code died at its first merged wide load; the fully
   strict-align image runs clean. This applies to all future cosmo64 code
   and any pc64 file added to the target.

Corollary rule: **mutable state lives in the image's own .bss** (stack, debug
page) -- the one DRAM the boot proves writable -- with the debug page's
address published at image offset 0x30 for the harness.

## The debug log (2026-09-01): forensics without a photograph

Every bring-up above was debugged through the panel: stage beacons, and fault
registers painted as 32 bit-cells to be photographed and decoded by hand. That
was not stubbornness. There is no exposed UART on this device, and the Gemian
4.4 kernel trixie runs is built `# CONFIG_DEVMEM is not set`, so a plain DRAM
buffer cannot be read back after the fact either.

But that kernel reserves a ramoops region and mounts pstore, and it announces
its own layout at boot:

```
ramoops: pstore:address is 0x54410000, size is 0xe0000,
         console_size is 0x40000, pmsg_size is 0x10000
ramoops: attached 0xe0000@0x54410000, ecc: 0/0
```

with `record_size` 0x1000 and `ftrace_size` 0x1000 (all readable from
`/sys/module/ramoops/parameters/`). `fs/pstore/ram.c` lays the zones down in
one fixed order -- dump records, console, ftrace, pmsg -- so the console zone
is at `0x54410000 + (0xE0000 - 0x40000 - 0x1000 - 0x10000)` = **0x5449F000,
0x40000 bytes**, and the four zones then end at 0x544F0000, which is the
reservation's own end: the arithmetic checks itself.

`log.c` writes there in ramoops' own `persistent_ram_buffer` format, so the
kernel would find it, save it and expose it with no eMMC driver involved.

**That does not work on this device, and the test said so.** UnoDOS ran, was
reset into trixie, and `/sys/fs/pstore` was empty. The cause is not the zone
address: MTK's own `ram_console` at 0x54400000, a **separate** reservation,
came up equally empty (`/proc/last_kmsg` held 74 bytes of freshly generated
header and no prior log). Two independent reservations losing their contents
at once is DRAM being wiped, not a layout mistake -- the preloader
re-initialises DRAM on this reset path, and ram_console/pstore only ever
worked for the abnormal resets MTK carries mrdump for.

So **the log's durable home is the eMMC** (see M3 below): `msdc.c` writes it
into the unused tail of p38, UnoDOS's own boot partition.

```sh
# boot UNODOS from the LK menu, do the thing, reboot into trixie, then:
./readlog.sh          # -r also tries the DRAM/pstore path
```

The DRAM log stays, because it is free, it reaches the QEMU gate (where
`qharness.py` reads the zone directly), and it is the **only** log that exists
before storage comes up -- which is exactly when storage is being debugged.
`c64_log_survey()` now counts surviving ramoops signatures at every boot and
records the verdict in the log, so the DRAM question re-answers itself instead
of being assumed.

- **`mmu.c` maps 0x54400000 Normal-NC, and that is load-bearing.** A warm reset
  does not flush the D-cache, so a write-back mapping would strand the last
  lines of the log -- which are the ones naming whatever went wrong.
- **`cpu.s` writes a formatted fault report** (vec/ESR/EC/ELR/FAR/EL) as the
  *last* thing the handler does, on a stack of its own: the fault may well be a
  wrecked stack, and the crash record and painted bit-cells are already safely
  down by then, so a re-fault in the logger costs nothing.
- **`qharness.py` reads the same zone at the same address** (QEMU's virt board
  puts DRAM at 0x40000000 like the Cosmo), prints it on every run, and fails
  the build if it is missing -- *before* the crash-record exit, so a payload
  that died still hands over everything it managed to say.
- The eMMC sink is flushed from the poll loop about twice a second (a no-op
  when nothing new was logged), after `c64_blk_init()` so the boot story lands
  even if the shell never comes up, and **from the fault handler**, which is
  the case that matters: the log of a boot that died is the one worth having.

`touch.c` and `kbd.c` stopped failing silently at the same time: every bail
says which one it was, and each touch-DOWN logs raw, panel and UI coordinates
-- the whole `TOUCHDBG` bit-cell diagnostic, in text.

## M3 STARTED (2026-09-01): the eMMC as a block device

`msdc.c`. **Not an eMMC bring-up.** LK already did that part: it brought MSDC0
(0x11230000) up, ran the SK hynix 128 GB card through its init sequence, tuned
the pads and then read this very boot image off p38 with it. And it leaves all
of it running -- LK's `platform_uninit()` (`mt6771/platform.c`) does
`leds_deinit()`, `platform_clear_all_on_mux()` and
`platform_deinit_interrupts()`, and **nothing else**: no `msdc_deinit()`, no
clock gating, no deselect. The controller is live and the card is in the
transfer state at the instant LK branches to us, exactly the way the
framebuffer is adopted rather than reprogrammed.

So what is left is a command issuer. No clock tree, no pinmux, no voltage
negotiation, no CMD0/1/2/3/9/7, no tuning -- and deliberately **no controller
reset** (`MSDC_CFG_RST`), which would throw away the tuning that makes the
adopted state worth having. PIO rather than DMA: these transfers are a GPT
header and, later, session files, and PIO cannot scribble on DRAM if a
register field is wrong, which matters where a wild write is a brick.

**The log lives here.** `c64_log_flush()` writes the ring into a 128 KiB window
2 MiB into p38 -- UnoDOS's own 32 MiB boot partition, of which the boot image
uses 512 KiB, so the tail is ours by a wide margin and nothing else on the
device touches it. Block 0 of the window is a header (`(UNOLOG)`, byte count,
and the offset the text starts at, so a truncated log is distinguishable from a
whole one). `readlog.sh` `dd`s it straight back out of `/dev/mmcblk0p38`. The
window's LBA is **derived from the GPT**, never a hardcoded absolute block.

**Writes are fenced at the bottom of the driver.** `c64_blk_write()` refuses
any LBA outside two windows: the UnoDOS data partition, and that log window.
Both are found by the GPT walk. Everything
else on this eMMC belongs to Android, to Gemian, to the GPT, or to the
preloader, and the preloader does not come back. There is no unfenced path for
a caller to route around. The GPT walk prefers a partition named `UNODATA` and
falls back to Android's `userdata` (p44, LBA 186136576..244164543, 27.7 GiB),
which is the partition the port plan says to take over, and logs which one it
took.

QEMU's virt board has no MSDC, **so there is no gate for this: the log is the
gate.** Under QEMU the driver reads all-ones off unassigned MMIO, times out
once at a bounded 500 ms, reports `eMMC NOT available` and lets the boot
continue, which is what a missing controller should do.

The register map (offsets, bit positions, response-type encoding) is the MT6771
MSDC programming interface. The code is this project's; nothing is copied from
MediaTek's LK sources, which are proprietary and license-incompatible with
UnoDOS.

Next -- confirm the read on hardware from the log, then M3b: wire `uno_fs_*` /
`uno_blk_*` in `stubs.c` to the real driver so `.UNO` apps and session
persistence work (which also stops the Control Panel opening at every boot).
M2's hardware test (does the pointer land where you touch? does the keyboard
type?) is still outstanding and is now a log read rather than a photograph.
