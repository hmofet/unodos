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

**That does not reach pstore on this device, and the cause is still open.**
UnoDOS ran, was reset into trixie, and `/sys/fs/pstore` was empty.

The first diagnosis was wrong, and this README said so for one commit. MTK's
`ram_console` at 0x54400000 came up empty too, and two reservations losing
their contents at once looked like the preloader wiping DRAM. Then
`c64_log_survey()` measured it directly on the next boot and disproved it:
**82 signatures were still standing** in the reservation when UnoDOS took
over. DRAM *is* preserved across this reset.

So the buffer survives and the kernel does not read it, which points back at
the zone address: MTK patches `fs/pstore/ram.c`, and if their zone order
differs from mainline's dump/console/ftrace/pmsg then 0x5449F000 is a *dump*
zone, and a dump record whose header does not parse as ramoops'
`"%lld.%lu-%c"` is quietly dropped. The survey now records where each **run**
of signatures starts, which is the zone layout, so the next boot settles it
instead of producing another guess.

Either way **the log's durable home is the eMMC** (see M3 below): `msdc.c`
writes it into the unused tail of p38, UnoDOS's own boot partition, and that
is proven working on hardware.

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

## M3a COMPLETE ON HARDWARE (2026-09-01): the eMMC as a block device

Confirmed on silicon, first try, and it brought M2's hardware test with it.
The log the device wrote to its own eMMC and handed back through `readlog.sh`:

```
msdc: adopting LK's controller: CFG=02200199 SDC_CFG=09020000 STS=00000000 ver=20170314
msdc: LBA 0 read OK, MBR signature 55aa (want 55aa)
msdc: GPT at LBA 2, 128 entries of 128 bytes
msdc: UNODOS boot slot at LBA 10158080 (65536 sectors); log window LBA 10162176, 256 sectors
msdc: no UNODATA partition; taking Android's userdata at LBA 186136576, 58027968 sectors (28333 MiB)
kbd: AW9523 up, matrix scanning
touch: NT36xxx ver=0b, reported maxima 1080x2160
input: keyboard present, touch present
```

Every LBA matches `parted` exactly. Reads work, the GPT walk works, and the
write path works too -- the log itself is the proof, since it got there
through `c64_blk_write()` and the fence. **The adoption bet paid: LK's
controller state (`CFG=02200199`, MSDC version 0x20170314) was still live and
usable with no re-initialisation at all.** And with the same boot,
`kbd:`/`touch:` confirm M2 on hardware: the AW9523 matrix answers, the
NT36672 reports 1080x2160, and real keypresses arrive as Unicode.

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
a caller to route around. The GPT walk hands out exactly one partition as
writable, the one named `UNODATA`. Android's `userdata` (p44, LBA
186136576..244164543, 27.7 GiB) used to be accepted as a fallback while the
plan was to take it over; that was reversed on 2026-09-01 (p44 stays
Android's, persistence goes to the SD card) and the fallback was removed on
2026-09-02, because a URC `mkfs`/`prepdisk` verb over the LAN would otherwise
have formatted it. p44 is logged as seen and left alone.

QEMU's virt board has no MSDC, **so there is no gate for this: the log is the
gate.** Under QEMU the driver reads all-ones off unassigned MMIO, times out
once at a bounded 500 ms, reports `eMMC NOT available` and lets the boot
continue, which is what a missing controller should do.

The register map (offsets, bit positions, response-type encoding) is the MT6771
MSDC programming interface. The code is this project's; nothing is copied from
MediaTek's LK sources, which are proprietary and license-incompatible with
UnoDOS.

## M3b (2026-09-01): the storage stack above the driver

`blk.c`, plus `fat.c` and `pc64_fs.c` compiled in unchanged. pc64's storage is
three layers -- a registry of 512-byte-sector devices, a native FAT16/32 driver
that mounts volumes off whatever is in the registry, and a filesystem layer that
presents those volumes (plus the RAM disk) to the shell. Only the bottom layer
is machine-specific, so `blkdev.c` (written around EFI Block IO) is the one file
of the three this port replaces; the twenty-odd `uno_fs_*` / `uno_fat_*` /
`uno_blk_*` stubs are gone from `stubs.c`.

**The registered device is a PARTITION, not the disk.** The obvious shape is to
register the whole eMMC and let `fat.c` walk the GPT, which is what the x86
backend does. It is the wrong shape here. Everything on this eMMC except two
partitions belongs to somebody else, and a whole-disk device hands `fat.c` an
address space in which our partition is a small window and every brick is one
arithmetic slip away. So LBA 0 of `emmc0` **is** the first sector of the data
partition and its length is that partition: an address outside it is not
expressible, which makes the containment a property of the address space rather
than of a check somebody has to remember. `c64_blk_write()`'s fence still
stands underneath. `fat.c` probes a table-less device as a superfloppy (a BPB
at LBA 0), which is exactly what a partition-as-device presents, so nothing
above had to cooperate.

`is_boot` is set, and that flag is load-bearing: `session_vol()` and
`uno_fs_pref_vol()` both put persistent state on the volume the machine came up
on rather than on whichever disk enumerated first (the ZimaBlade lesson, written
up in `pc64_fs.h`). Here there is one medium and we did boot from it, so saying
so is what makes `SHELL.CFG` land on the eMMC instead of the RAM disk.

**`BLKTEST=1 ./build.sh shell` is the gate this layer could not otherwise
have.** The virt board has no MSDC, so without it the first thing ever to run
`fat.c` on aarch64 would be the device itself, at a flash-boot-reboot-read per
attempt -- and everything above the transport is portable code meeting a new
compiler, a new word size and `-mstrict-align` for the first time. So the
transport is injectable, the same idea as the Genesis port's `AUTOTEST_BRAM`: a
36 MiB RAM disk is registered in the absent eMMC's place, formatted with the
real `uno_fat_mkfs`, and put through a write / read / verify / delete round trip
through the real `uno_fs_*` calls the shell uses. Green on both QEMU paths:

```
blktest: formatted a 36 MiB RAM disk as FAT32
storage: 1 block device(s), 2 volume(s)
  vol 0: ram "RAM" rw
  vol 1: fat "BLKTEST    " rw boot
storage: persisting to vol 1 (BLKTEST    )
blktest: PASS -- mkfs, mount, write 57 bytes, read back, verify, delete
```

That proves the whole chain minus the eMMC. It is compiled only under
`-DC64_BLKTEST` -- the RAM disk is 36 MiB of `.bss`, fine on QEMU and never in
a shipped image.

`c64_storage_report()` runs before the shell, because `session_load()` asks for
`SHELL.CFG` the moment `uno_main` starts and a report printed after that is a
report of what the shell already decided. It names every device and volume, says
where persistent state will go (and says so loudly when the answer is the RAM
disk), and runs `uno_fat_selftest()`, which is inert unless a `WRTEST.REQ` file
has been left on a writable volume -- drop one there from Linux and a boot
proves read, write and delete on metal in one pass.

**What is still needed on the device: a volume.** There is no `UNODATA`
partition on the eMMC, so nothing mounts and the shell runs on the RAM disk.
The volume will be the SD card (next paragraph), not p44.

`.UNO` apps do NOT arrive with this: the module loader is still stubbed, and it
needs an aarch64 module ABI of its own. Storage was the prerequisite, not the
whole job.

Storage target, decided 2026-09-01: **the SD card, not p44.** p44 is left
alone. There is a 29 GiB card in the slot whose first partition is already
vfat, so persistence needs no destructive step at all -- what it needs is an
MSDC1 driver, and unlike MSDC0 that is a real bring-up: LK never touches the
SD slot (`init_storage()` calls `mmc_legacy_init(1)` = id 0 = eMMC only; the
sole SD-card init in the tree is inside `#if 0`). PMIC VMCH/VMC over PWRAP,
clock, pinmux, then the public SD init sequence. Queued behind USB.

## M4 COMPLETE ON HARDWARE (2026-09-02): a USB mouse and keyboard

**The mouse moves the cursor and the cursor stays where it is left** (fifth
hardware boot). The USB lane's two files, `xhci.c` and `usbhid.c`, compile
here unchanged apart from one registered hook, on top of three seams that all
live in this lane. Both QEMU paths are green (there is no xHCI on the virt
board, and the log says so in one line). Five boots to get there; what each
one taught is below, because each was a different bug and none of them was
in the design.

**Boot 1** -- the controller came up on the first try: the Device-mapped DMA
arena, the rings, Address Device and the hub walk all worked, and the four
devices behind the hub enumerated (0bda:5411 hub, 0b95:1790 AX88179B,
046d:c548 Logi Bolt, 046d:c52b Unifying). usbhid then claimed ZERO endpoints,
silently. **Boot 2** -- with the claim path interposed for logging (compile-
time renames of `uno_usb_get_config`, `uno_usb_control` and
`uno_usb_setup_intr_in` onto this lane's wrappers), the failure moved one
step earlier than expected: every config-descriptor fetch failed, while the
device-descriptor fetches during enumeration had all succeeded. **Boot 3**
-- an experiment, the same request at 256, 64, 25 and 9 bytes, timed, with
USBSTS and ERDP around each: on this controller **a 256-byte control IN on
the hub "completes" in 0 ms with the buffer untouched**, while 64, 25 and 9
return the real descriptor. 64 is a short packet too, so it is the 256 itself
(root cause still open; suspect a TRB length of several max-packets on EP0 --
the AX88179's bulk transfers will need to know). usbhid asks with 256, got
zeros, and issued **`SET_CONFIGURATION(0)` on the HUB**, which unconfigures
it and drops every device behind it -- which is why the receivers, enumerated
fine a moment earlier, timed out on everything afterwards, on every boot.
Fix: the bounce fetches the 9-byte header, then exactly `wTotalLength`, the
way the USB core does, and refuses a header that does not parse. **Boot 4**
-- both HID interfaces of the Bolt receiver claimed and **the mouse moved**,
through the MediaTek endpoint-context words (below); but the cursor snapped
back to the centre, because `touch.c` published its idle position every
poll. **Boot 5** -- the touch panel reports on the contact edge only, and
the cursor stays put.

**The MediaTek endpoint-context words** (`usb.c` `mtk_ep_quirk`, through the
`uno_xhci_set_ep_quirk()` hook added to `xhci.c` as a `seam:` commit with a
note in `pc64/UNOAUTOMATE-REQUESTS.md`). MediaTek's SSUSB host keeps a
bandwidth schedule of its own and reads it from the endpoint context's
reserved DWords (DW5 = BPKTS|BCSCOUNT|BBM, DW6 = BOFFSET|BREPEAT); without
them a periodic endpoint is never serviced. Both receivers are full-speed
behind the high-speed hub's transaction translator, the vendor driver's
scheduled case: one packet per microframe, three complete-splits, no burst,
offset 0, no repeat. The same pass writes the Interval field, which
`setup_ep()` leaves at 0 -- outside the 3..18 the spec allows a full-speed
interrupt endpoint, tolerated by every controller met before this one.

**Open, and filed with the usb lane:** `xhci.c`'s interrupt poll sweeps the
event ring on a thousand-iteration spin per endpoint, ~5 ms each on this
core, so a keyboard and a mouse cost ~12 ms per shell loop while active
(`perf: per loop input` 2 ms -> 12 ms). That caps the loop near 60 Hz and is
why the mouse feels a little jumpy: one report per loop where the device
offers one every 2 ms. `usb.c` gates its drain on IMAN.IP so an idle ring is
skipped (the log prints an `IMAN.IP was set on N of 600 loops` verdict ten
seconds in; the fifth boot was rebooted before it fired), but a moving mouse
pays the sweep every loop. The fix is in the poll budget and the ring depth,
both the usb lane's.

**What the probe found first** (`./build.sh usb`, `usbprobe.c`, two hardware
boots). LK gates nothing: every `INFRA_PDN_STA` register reads 0, so the
SSUSB clock is already running. The IP answers (`HW_ID=20160812`) but arrives
**held in software reset with the host powered down** (`PW_CTRL0=10411021`
bit 0, `CTRL1=00000001`), which is why every xHCI register reads zero -- zeros,
not all-ones: present but reset. Release it, power the host and port in host
role, put the U2 T-PHY in host mode, set `PORTSC.PP` (the root port comes up
unpowered), and the controller identifies as xHCI 1.10 with **one USB2 port and
no SuperSpeed port**, and reports the attached hub:

```
PW_STS1=00000d0f -- clocks STABLE
CAPLENGTH=20 HCIVERSION=0110 HCSPARAMS1=0100010f HCCPARAMS1=01400f99
port 1: PORTSC=000202e1 CCS=1 PED=0 PP=1
*** A DEVICE IS CONNECTED on port 1 ***
```

That last line retires the whole Type-C lane: role, VBUS and cabling are
already right at handover. The FUSB301 (I2C bus 3, 0x25) never has to be
touched and no MT6370 boost is needed -- the hub in use is self-powered
(`bmAttributes 0xe0, bMaxPower 0`).

**`ssusb.c`** is that sequence made reusable: reset release, host + port
enable, STS1 wait, PHY host mode. Everything from `CAPLENGTH` on is the xHCI
specification, and that is `xhci.c`.

**`pci.c`** answers `pc64_pci.h` on a SoC with no PCI. `xhci.c` discovers its
controller by walking config space for class 0C/03/30 and reading BAR0, and
`xhci.*` belongs to the USB lane (AGENTS.md ownership registry) -- so rather
than edit it, this platform implements the API it already consumes: one
function at 00:00.0, MediaTek's vendor id and the SoC's part number, BAR0
`0x11200000`, **exposed only once ssusb.c has powered the block**. Before that
(and forever on the QEMU gate) the bus is empty, so the scan finds nothing in
one line instead of spending five timed-out attempts on a controller that reads
all-ones. The shell calls `uno_xhci_init()` on its own account, not only
through usb.c, which is why that gate lives in the shim and not in the caller.

**DMA memory is the crux on ARM, and it is solved in the build.** The xHCI is
a bus master that is not coherent with the CPU caches on this SoC, and
`xhci.c` keeps every ring, context, scratchpad and transfer buffer in static
`.bss` with no allocator seam -- x86 never needed one. `c64_usbglue.h` is
force-included (`-include`) into `xhci.c` and carries
`#pragma clang section bss=".xdma"`, which moves **every** zero-initialised
static in the translation unit into one section, function-local ones
included (`hub_scan.hd`, `ss_burst.cfg` -- both DMA targets). `flatten.py`
records where the linker put it (image header + 0x40/0x48) and `mmu.c` splits
the 2 MB block it falls in into 4 KB pages, mapping exactly those as
**Device-nGnRnE**. Device rather than merely non-cacheable, because the driver
has no barriers between writing a TRB and ringing the doorbell, and Device
ordering supplies that for free. The driver's ordinary state rides along into
the same section; a bus cycle per access on a driver polled a few times a
frame is nothing.

`control_xfer()` DMAs straight into whatever buffer its caller passes, and
`usbhid.c` passes one on its STACK for the config descriptor. `build.sh`
compiles `usbhid.c` with its two calls renamed
(`-Duno_usb_get_config=c64_usb_get_config`, likewise `uno_usb_control`) onto
`usb.c`'s bounce buffer in the same section. A build tripwire fails if
`xhci.o` still owns a `.bss`: DMA memory left write-back does not fail, it
corrupts.

The same header pre-empts `uno_debug.h`, so `xhci.c`'s `uno_dbg_log`
narration (26 lines of bring-up diagnosis) reaches the eMMC log as `pc64:`
lines instead of compiling to nothing.

**Two traps found on the way, both now tripwired.** (1) The `UNO_DRIVER`
registration tables `xhci.c` and `usbhid.c` emit are 16 initialised bytes in a
`.unodrv` section, which lld placed AFTER `.data`'s 72 MB virtual extent -- so
`flatten.py`'s trailing-zero trim stopped there and the shipped image went
from 0.5 MB to 76 MB, the size that hung LK's decompressor at the splash in
M1. The table is merged into `.data` at link time
(`-Xlink=/merge:.unodrv=.data`; nothing reads it here) and `flatten.py` now
refuses to ship more than 16 MB. (2) A zero-initialised array given a section
ATTRIBUTE comes out as initialised DATA, which the linker keeps apart from the
BSS `.xdma` -- a second arena at a different address the MMU map would have
missed. The bounce buffer uses the pragma instead, and `flatten.py` takes the
union of every `.xdma` it finds regardless.

**Input.** `input.c` keeps per-source state -- touch panel and USB mouse
buttons, matrix and USB keyboard modifiers -- and the shell sees the OR,
which is what x86's poll loop does across its devices. `usb.c` feeds usbhid's
deltas (applied raw, as x86 applies a HID mouse's), latched buttons, wheel
notches and keys into it; `platform.c` brings USB up before the shell and
polls it inside the timed input section, so the `perf:` lines will say what a
polled xHCI costs per frame.

Also unlocked by the same controller, not wired yet: `ax88179.c` (the
AX88179B on that hub = networking) and `usbmsc.c` (a USB stick = a second
route to persistent storage).

Open alongside: whether the touch mapping is *accurate* (the log proves
contacts arrive and shows raw/panel/UI for each one, but only a human can say
whether the pointer landed under the finger), and the ramoops zone-order
question the survey will answer on the next boot.

## The desktop is the panel (2026-09-02): native 2160x1080

Until now the desktop was **640x480 presented at 2x** — a 960x1280 rect on a
1080x2160 panel, with the rest black. That size was never a decision about
this device: it is the rpi port's, inherited when the asm lane forked from it
and carried into cosmo64 because `display.c` was written against the m0
payload's constants. The panel here is *fixed*, so it has exactly one right
answer, and it is the panel: **2160x1080 landscape, zoom 1, filling the glass
edge to edge.** That is what the shell now starts in.

What had to move for it:

- **The geometry became runtime.** `c64_scrw` / `c64_scrh` / `c64_scale` and
  the derived rect live in `videolfb.c` (`c64_geom_set`) — not `display.c`,
  because `touch.c` inverts the same transform and the calib payload links
  `touch.c` without the shell. `display.c`, `touch.c`, `input.c` and
  `calib.c` all read them; only the static payloads (m0, calib's crosshairs)
  still compile geometry in, from the same `C64_SCRW`/`C64_SCRH`/`FB_SCALE`
  that seed the runtime values.
- **`fb.h`'s ceiling had to be overridable.** `FB_MAX_W`/`FB_MAX_H` size
  `fb[]` and unoui's cached desktop background, and they default to a PC
  monitor's 1920x1200. 2160x1080 is *wider and shorter* than that, so neither
  raising nor swapping the two numbers covers it; they are `#ifndef`-guarded
  now and the shell build passes `-DFB_MAX_W=2160 -DFB_MAX_H=1080`
  (`seam:` commit, and a cross-lane note in `pc64/UNOAUTOMATE-REQUESTS.md`).
  Cost: `fb[]`, unoui's `g_bg`, and `display.c`'s shadow and scene buffers are
  each 9.3 MB instead of 1.2 MB, so the image's runtime-zero range went from
  ~67 MB to ~88 MB. Nothing is *shipped* (`flatten.py` trims at the last real
  byte — the boot image is unchanged at 512 KiB), but `entry.s` zeroes that
  range MMU-off, on Device memory, so the extra ~21 MB is real boot time.
- **Control Panel > Display now has a list.** It used to report one fixed
  entry. There are no video modes to enumerate on a fixed panel, but there is
  still a choice, because a smaller desktop is simply presented at a bigger
  integer zoom over the same glass — which is the sense the x86 port's list
  ended up having too. `2160x1080`, `1080x540`, `720x360` and `540x270` divide
  the panel exactly and fill it; the familiar PC sizes between them are
  centred with a black surround, which is what a fixed panel can honestly do
  with them. Whole-pixel zoom only: a fractional nearest-neighbour upscale
  duplicates some source columns and not others and mangles glyph stems (x86
  floors its scale for the same reason). `uno_pc64_lowres()` is real now too —
  540x270 at zoom 4, 1/16 the pixels, still full-screen.
- **A size change clears the whole panel.** A *shrinking* rect leaves the old
  desktop standing in the margin: nothing will ever draw over those pixels
  again. Same class of bug as the stale band at first light.
- **The pointer is carried by its position on the glass**, not its
  coordinate (`c64_input_rescale_pointer`). Every size covers the same panel,
  so the same coordinate is a different physical place either side of a
  change, and clamping alone throws the pointer at an edge.

Two things the gate learned with it. `qharness.py` now takes the desktop size
from the payload's own FBINFO report (`fb_scrw`/`fb_scrh`, +92/+96) and checks
it against what it expects, so changing one without the other fails on quill
rather than on the device. And the white bar beacon at the panel origin can
only survive if the desktop rect does not reach the origin — at native size
the desktop *is* the panel and paints over it, so that check is now scoped to
the case where it can still mean something. The pixel-exact blit check is the
stronger statement of the same fact anyway.

**What this costs on hardware, and the knob for it.** A full repaint now reads
9.3 MB and writes 9.3 MB of non-cacheable panel memory, against 1.2 MB read /
4.9 MB written at 640x480@2x; the shell's own `unoui_render_ui()` covers 7.6x
the pixels. The dirty-box tracking added for exactly this reason still bounds
the steady state to what actually changed, but a full repaint is heavier and
the software render behind it is much heavier. If it reads as slow on the
device, **Control Panel > Display > 1080x540** fills the same panel with a
quarter of the pixels. And at 403 DPI the native desktop is *very* fine — the
`UI scale` dropdown beside the resolution one (100/125/150/200%) is the right
answer for readability before dropping resolution is.

Both gates green at the new size: `./build.sh` (m0) and `./build.sh shell`
under `qharness.py`, pixel-exact, with the Control Panel reporting
`2160x1080`. (`./build.sh` also links `msdc.o` now — the m0 target had been
unbuildable since `log.c`'s crash path started calling `c64_log_flush()`,
which lives there.)

## M5 (2026-09-02): the network stack, wired to the one NIC this box can have

`net_*` was stubbed to zero, so the honest answer to "does networking work"
was no -- nothing was linked. It is linked now: `net.c` (the TCP/IP stack)
and `ax88179.c` (the ASIX USB Gigabit driver) compile unchanged, and
`netup.c` provides the one function the shell actually calls.

**Why a new file and not `pc64_http.c`.** The shell's bring-up entry point is
`pc64_net_boot()`, which lives in `pc64_http.c` -- 1273 lines that walk a
table of eight NIC families (six of them PCIe parts that cannot exist on this
SoC) and pull in TLS, the cookie jar, the HTTP cache and unolog behind them.
Nothing else in the linked set references any of it. `netup.c` is that seam,
Cosmo-shaped: one adapter, the same 8-second budget, the same
link-before-DHCP ordering -- which is not tidiness, because `ax88179.c`
programs the MAC medium from the PHY's negotiated speed inside `nic->link()`
and a mismatched medium silently kills RX (the `tx>0 rx=0` signature the x86
lane hit on a 100M port). Fourth thing now re-implemented from a file this
port replaces, after the cursor, the dirty-row present and the desktop size.

**WiFi is not on the list and will not be.** The radio is MediaTek CONNSYS,
entangled with the consys/CCCI platform devices; there is no route to it from
bare metal. Wired USB Ethernet is the network on this machine.

**The bulk bounce.** `uno_usb_bulk_in/out` put the CALLER's pointer straight
into the TRB, and `ax88179.c`'s `tx[2048]` / `g_rx[4096]` are ordinary cached
`.bss` -- the same non-coherence that made usbhid's stack buffer come back as
whatever the cache held. The xhci pragma would fix it in one line by moving
the driver's statics into `.xdma`, and would then make it parse every received
frame out of Device memory a byte at a time. `usb.c` bounces instead
(`c64_usb_bulk_in/out`, renamed in at compile time like the control path), so
the staging area is uncached and the parse is not.

**A stub that would have lied.** `unoauto_deadline_left_ms()` returns `-1` for
"no deadline armed", not `0` -- `0` means "the budget is spent". `net.c` polls
it inside `net_dns_query`'s wait loop, so the reflex `I0()` stub would have
aborted every DNS lookup on its first iteration: an absent subsystem answering
as though it had already run out of time. Likewise `unoauto_hooks_live` is a
**variable**, not a function; stubbed as a function, `if (unoauto_hooks_live)`
reads its address, which is never zero, so every tap point would have called
through instead of being skipped -- and `net.c` fires one on every frame in
both directions.

**State: written and gated, NOT yet proven on hardware.** The QEMU virt board
has no USB, so the gate can only show the path running and reporting honestly
(`net: no adapter -- ax88179 found=0 bound=0 link=0`). What the device has to
answer: whether a multi-max-packet bulk transfer works on this controller.
The M4 finding that a 256-byte control IN "completes" in 0 ms with the buffer
untouched is still root-caused only as a suspicion -- a TRB length of several
max packets on EP0 -- and a 4096-byte bulk IN is eight max packets at high
speed. If bulk IN comes back empty the same way, that suspicion is confirmed
and the fix is the same shape: ask for less per TRB.

Needs the USB-C hub with the AX88179B (`0b95:1790`, the one M4 enumerated)
and a cable in it. Read the result with `cosmo64/readlog.sh`: the `net:` lines
distinguish no adapter / no link / no lease / leased, because those are four
different bugs and a hardware boot gets one log to tell them apart.

### M5 boot 1: "the mouse stopped working" was a twenty-second frame

The desktop came up, the AW9523 keyboard reported a key, and then the machine
stopped answering. No crash record, no fault -- and the log simply ended, one
line after the keypress, with neither the `net:` line nor the two-second
`perf:` report that should have followed. Three facts read together give the
whole thing:

- `pc64_net_boot()` fires at frame 35, about 1.2 s in -- *before* the first
  `perf:` report, which is why there is no `perf:` line to bound it with;
- the log's push to the eMMC rides the poll loop, so whatever wedges the frame
  loop also stops the log reaching storage. **The log ends where the wedge
  begins, and everything logged after it died in DRAM.**
- `ax88179.c`'s `ax_recv()` calls `uno_usb_bulk_in()` **synchronously**, and a
  bulk IN on an idle NIC never completes -- so it costs `poll_xfer`'s full
  **5-second** timeout. `net_poll()` drains with `while (recv() > 0)`, and the
  shell calls `net_poll()` **four times a frame**.

So a bound adapter with nothing arriving cost about twenty seconds per frame,
for the rest of the session. The machine was never hung; it was alive and
spending all of its time waiting for a packet nobody had sent. From the front
that is indistinguishable from a dead mouse.

Three fixes, and the second and third are the general lessons:

1. **Bulk IN is asynchronous here.** `xhci.h` already carries
   `uno_usb_bulk_in_arm()` / `_poll()`, added "for the NIC recv path", one
   outstanding transfer per device, poll returning 0 while it is in flight.
   `ax88179.c` does not use them and does not have to: this lane already
   renames its bulk calls onto `usb.c` for the bounce, so the same seam is
   where the sync-to-async adaptation goes. "Still in flight" returns 0, which
   is exactly the "nothing arrived" `ax_recv` already handles.
2. **A budget measured in iterations is not a budget.** The first cut copied
   x86's `for (i = 0; i < 600; i++) { ...; budget -= 5; }`, which assumes each
   pass costs the 5 ms it sleeps. One call that pays a 5-second USB timeout
   turns that "8 second" bound into hours. Every wait in `netup.c` is bounded
   by CNTPCT now, so the worst case is the deadline plus one in-flight call.
3. **Do not bind the stack until the link is up.** `net_init()` ran before the
   link wait, so a bring-up that failed still left the adapter bound and the
   shell draining it every frame forever. Failure now leaves the stack
   unbound, and `net_poll()` returns at its first line.

Also restored: `uno_dbg_net_trace()` goes to the log instead of a silent stub,
and every stage in `netup.c` logs **and flushes before** the call that could
block -- because a breadcrumb written after the step is a breadcrumb that
never reaches the disk.

## M5 WORKS ON HARDWARE (2026-09-03): a DHCP lease over USB Ethernet

```
xhci: bulk-in failed cc=-1 len=4096 epstate=1
net: LEASED 192.168.2.254 gw 192.168.2.1, tx=2 rx=16
usb-bulk: in arm=3577 armfail=0 poll=25088 land=3576 err=0 bytes=300880
```

300 KB received, a lease from the LAN's real server, and the desktop
responsive throughout (106 presents per 2 s). Six boots; what each one settled
is worth keeping, because five of them were spent on theories rather than
measurements and the sixth was the first to ask the hardware a question.

**Boot 1** froze -- `ax_recv()` calls `uno_usb_bulk_in()` synchronously and a
bulk IN on an idle NIC never completes, so it cost `poll_xfer`'s full 5-second
timeout, four times a frame. Fixed by adapting `uno_usb_bulk_in_arm()` /
`_poll()` in this lane's rename seam. **Boot 2** gave `tx=4 rx=0`: frames out,
nothing back. **Boot 3** tested "the aggregation parameters are chosen by
ethernet speed where Linux chooses by USB speed" -- wrong. **Boot 4** tested
"the aggregation size (20 KB) exceeds the driver's 4 KB buffer" -- also wrong,
and the write demonstrably landed. Both were inferences off a register dump.
**Boot 5** finally asked the controller, and got nothing back, because
`xhci.c`'s failure line went only to `xd()` (0x402 debugcon, x86 QEMU) -- fixed
as a `seam:` commit. Its three probes also cost 24 s of frozen desktop, which
is what the hardware test reported as "the mouse stopped working" for the
second time.

**Boot 6** leased. What the log finally said:

- `cc=-1 epstate=1` -- the transfer TIMED OUT with the endpoint **Running**.
  Not halted, not stalled, not misconfigured: posted and never serviced.
- The transmit side was never in question. A LAN capture on quill caught 282
  DHCP DISCOVERs from this MAC, and Linux on the *same adapter* holds a lease
  from the same server, so the wire and the server were both proven early.

**A FAILED BULK IN CLEARS THE CHIP'S RECEIVER.** `MEDIUM` read `01b3` after
link and `00b3` after the first failed transfer -- `AX_MEDIUM_RECEIVE_EN` gone,
and gone for the rest of the session. `ax88179.c` cannot notice:
`ax_apply_medium()` caches the last mode it *wrote* and only rewrites on a
change, so the driver believes the medium is still correct while the adapter
has gone deaf. That is why boot 5 failed where boot 6 succeeded.

**What is still open: WHICH half of boot 6 mattered.** It changed two things
against boot 4's control -- the extra `MEDIUM` write, and the failed probe
whose `ep_recover()` issues Stop Endpoint + Set TR Dequeue. Either could be
the cause; `setup_ep` sets DCS=1 against a ring whose cycle starts at 1, so it
is *not* a dequeue-cycle mismatch. Rather than A/B it across two more boots
with a chance of one having no network, `netup.c` now tries the cheap
candidate first and escalates: medium rewrite, 4 s of DHCP, and only if that
does not lease does it spend five seconds priming the endpoint and retry. The
log prints a `net: VERDICT` line naming the one that worked, and if the medium
rewrite is enough the prime never runs.

If the prime turns out to be load-bearing, the fix is a public "reset this
endpoint" entry point from the usb lane, so it costs a command rather than a
deliberate timeout.

### M5 root cause (2026-09-03, boot 11): the ASIX disables its own receiver

The watchdog caught it in the act, which is what nine boots of register dumps
either side of bring-up could not:

```
usb-bulk: RX STALLED -- 4 landings then nothing for 2 s, endpoint state 1
asix[at stall]:     RX_CTL=02b8 MEDIUM=00b3      <- RECEIVE_EN gone
asix[after repair]: RX_CTL=02b8 MEDIUM=01b3      <- put back
net: LEASED 192.168.2.254, tx=3 rx=22
usb-bulk: in arm=759 land=757 bytes=68992        <- one stall, all session
```

**The chip clears `AX_MEDIUM_RECEIVE_EN` by itself**, with the bulk-IN endpoint
still in state 1 Running and nothing on the host touching it. Writing `01b3`
back restarts reception, and after that one repair the session ran to 757
landings and 69 KB with no further stall.

The second half of the bug is why nothing above notices: `ax_apply_medium()`
caches the last mode it *wrote* and only rewrites on a change, so once the chip
has dropped `RECEIVE_EN` the driver believes the medium is still correct while
the adapter is deaf. Every symptom of this milestone -- `tx=4 rx=0`, "one frame
then silence", the bring-up orderings that worked and the ones that did not --
is that one fact seen from different distances.

**What survives, and what was scaffolding.** The watchdog stays: the chip can
do this at any time and the driver above it cannot tell, so a two-second gap
is the right cost. The five-second prime is **gone** -- the fast
`uno_usb_bulk_in_reset()` plus the watchdog covers it, and boot 11's verdict
line said so. The 20 KB diagnostic probe is **gone** with it, and the bounce is
back to 4 KB. The aggregation match stays but is documented as what it is: a
real mismatch (the chip asks for a 20 KB burst against `ax_recv`'s 4 KB buffer)
that was **not** what stopped reception.

**What this milestone cost, and why.** Eleven boots, of which about five went
to theories built on register dumps instead of measurements: aggregation
parameters chosen by ethernet speed rather than USB speed; the 20 KB/4 KB
mismatch; the arm contract. Two things would have collapsed it early. First,
`xhci.c`'s bulk-IN failure line went only to `xd()` (0x402 debugcon, x86 QEMU),
so for three boots the most diagnostic line in the stack was reaching nobody --
**check that an existing diagnostic actually reaches this platform before
building on its silence**. Second, nothing could distinguish "the endpoint
stopped" from "the endpoint is running and the device is silent"; both read as
`land=0`. `uno_usb_bulk_in_epstate()` answers that in one line and should have
been the first thing added, not the ninth.
