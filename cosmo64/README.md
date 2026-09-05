# cosmo64 — pc64 on ARM64 (Cosmo Communicator, MediaTek MT6771)

The platform layer that will carry the full C pc64 system — window manager,
`.UNO` apps, browser, Office, Python — on the Planet Cosmo Communicator, the
device the self-contained assembly port (`cosmo/`, branch `cosmo-port`) reached
first light on 2026-08-31. The two ports coexist deliberately: `cosmo/` is the
instant-boot minimal UnoDOS and the hardware bring-up crib; `cosmo64/` is the
pocket workstation.

**The plan** — buckets, milestones, toolchain decision, risks — lives in the
research repo: `research/pc64-arm-port-plan.md` in `hmofet/cosmo`. It stops at
M5; M6 (the URC remote channel) was added after it was written and is
documented here.

## State: M0-M7 COMPLETE, all hardware-proven (2026-09-03)

Everything below has run on the phone, not just under the gate:

| | |
|---|---|
| M0 | the llvm-mingw toolchain and the flat LK payload |
| M1 | CPU glue, MMU and caches at EL2, the pc64 Aurora desktop |
| M2 | the AW9523 matrix keyboard and the NT36672 touch panel |
| M3 | the eMMC as a block device, and pc64's FAT stack on top |
| M4 | USB host: xHCI on MediaTek's SSUSB, a mouse and a keyboard |
| M5 | networking: a DHCP lease over the AX88179 USB Ethernet |
| M6 | URC: the box is remotely driven from the dev PC, with a live log |
| M7 | the SD card as a persistent volume, the rear panel as a touchpad, three perf wins |
| M8 | **`.UNO` modules: the aarch64 module ABI -- seven apps load from the SD card** |

**What runs the machine today.** The desktop is the panel's native landscape at
a 2x zoom. Local input is the matrix keyboard, the touch panel, and the rear
cover panel as a touchpad; a USB mouse and keyboard work through a plain USB
2.0 hub. The eMMC is readable and its log window writable. **A 29 GB microSD
card is mounted read-write as the boot volume, so the session persists.** The
shell holds a DHCP lease and serves URC on `:5099`.

Still missing: no audio, no WiFi (CONNSYS has no bare-metal route), no
cellular, and no RTC. `.UNO` apps load as of M8: the seven the launcher
rosters live under `APPS\` on the SD card and open from the desktop.

**Where we left off.** p38 carries the M8 image and it is booted and
verified: every module in `APPS\` loaded and drew over URC. The loop is:

```sh
./build.sh shell && ./flashp38.sh   # when there is something new to try
./build.sh apps                     # the .UNO modules -> build/apps/*.UNO
./urctail.py                        # watch the log live, from the dev PC
./readlog.sh                        # or read it afterwards, from Trixie
```

### What M7's first good boot said

The whole chain, in the order the log prints it:

```
pmic: MT6358 answering over the wrapper (SWCID=5820, WACS2_EN=00000001)
sd: VEMC en=1 sel=3  (3000 mV)   <- the eMMC's rail: the control, and it passed
sd: VMCH en=0 sel=3  (3000 mV)   <- the card's supply, OFF
sd: VMC  en=0 sel=11 (3000 mV)   <- the card's I/O, OFF
sd: MAP CONFIRMED
sd: VMCH and VMC are on at 3000 and 3000 mV
sd: pins after rails -- MSDC_PS=010f0002 CMD=1 DAT3..0=1111
sd: CMD8 echoed 0x1AA -- v2 (SDHC/SDXC capable) card
sd: OCR=c0ff8000 -- block (SDHC/SDXC) addressing
sd: RCA 0001, 61067264 sectors (29818 MiB)
sd: bus width 4 / sd: card clock 24000 kHz
sd: MBR entry 0: type 0c, LBA 8192, 61059072 sectors
blk: sd0 = the card's FAT partition at LBA 8192, ... addressed partition-relative
storage: 1 block device(s), 2 volume(s)
  vol 0: ram "RAM" rw
  vol 1: fat "NO NAME    " rw boot
storage: persisting to vol 1 (NO NAME    )
codi: firmware "OurCodi-0.1" / UART1 is on GPIO110/112 f7 / rear touchpad armed
kbd: AW9523 up, matrix scanning at 390 kHz
touch: NT36xxx ver=0b, reported maxima 1080x2160, bus 390 kHz
perf: ... per loop input ~3600 us
```

`CMD=1 DAT3..0=1111` on the line before CMD8 is the one that matters: it is the
bus coming alive the instant the rails do, and it is why everything after it
worked first try.

Writes were proven separately, over URC rather than by inference:

```
put 1 \SDTEST.TXT 0 <base64>   -> ok 42
put 1 \SDTEST.TXT done 2a      -> ok verified 42
```

### The perf result, measured against a measured baseline

| | M6 | M7 |
|---|---|---|
| `per loop input` | 11344 us | **~3600 us** |
| input's share of the frame | 32% | 13% |
| loop rate | 56 loops / 2 s (28 Hz) | **~72 loops / 2 s (36 Hz)** |

Input is 3.1x cheaper and the shell runs 28% more frames -- and the M7 number
*includes* the CoDi UART poll, which did not exist in the baseline.

**Open, in the order worth doing:**

1. **Fill in behind the modules.** M8 proved the ABI; what the apps find
   behind their possible imports is often a stub. The first three portable
   groups are now real (see "The providers" below): `uno_binds.c`,
   `unolog.c`, and `uno3d.c` + `uno3d_soft.c`. Next the Office modules, which
   need `unodoc/` staged. See M8 below.
2. **The CoDi link drops frames.** The shell polls that UART about 36 times a
   second and OurCodi reports at 100 Hz during contact, which is ~58 bytes into
   a 16-byte FIFO between two polls. `codi.c` recovers (a gesture with no
   frames for 250 ms is abandoned, and anything held is let go), but the real
   fix is the UART's DMA path. The `codi:` stats line counts the bytes resynced
   past, which is the measurement that says how badly it is needed.
3. **The rear touchpad has no lid sensor.** The Linux daemon suppresses touch
   while the lid is closed by asking UPower; bare metal has no such oracle, so
   a pocket touch moves the pointer.
4. **The `reboot` verb is untested by choice** -- its TOPRGU `SWRST` path is
   written but never fired, because coming back needs someone at the LK menu.
   That menu times out to NORMAL boot and cannot be told to pick our slot, which
   is also why a live build push over URC needs a person at the keyboard; see
   the request filed in `pc64/UNOAUTOMATE-REQUESTS.md`.
5. Smaller: `net_link_speed_mbps()` reports 0; the 256-byte control-IN anomaly
   from M4 is still root-caused only as a suspicion; `usb.c`'s config-descriptor
   bounce truncates rather than refuses a descriptor over 512 bytes; SD writes
   are one block per command where reads stream up to 64; there is no RTC, so
   the clock starts at zero every boot.

## Build

```sh
./build.sh            # the m0/m1 test payload
./build.sh shell      # THE SHELL — what you normally want
./build.sh calib      # the touch-calibration payload
./build.sh usb        # the USB host probe
```

Cross-compiles on quill (`/opt/llvm-mingw-20260826-…/bin`); `mkbootimg.py`
comes from the asm port's in-tree `cosmo/` lane. `URC_PIN=<6 digits>` closes
the remote-control gate (see M6). A fresh worktree needs the two generated
headers the shell build consumes, which are not in git — copy them from quill
or another checkout:

```sh
mkdir -p ../pc64/build && scp quill:/work/unodos-pc64arm/pc64/build/{font_data.h,world_map.h} ../pc64/build/
```

Gate it on quill, and **run all three** — the EL2 path is where the device
actually runs, and the URC path is the only cover the remote channel has:

```sh
python3 qharness.py build/shell.bin /tmp/a.png 8
QHARNESS_EL2=1 python3 qharness.py build/shell.bin /tmp/b.png 8
QHARNESS_URC=1 QHARNESS_EL2=1 python3 qharness.py build/shell.bin /tmp/c.png 25
```

Load-bearing compile flags, explained at the top of `m0.c`: `-mstrict-align`
(**everything**, see M1's platform laws), `-fsigned-char`, `-fno-builtin`. The
linker recipe: `-nostdlib -Wl,--image-base,0x40080000 -e _start`;
`flatten.py` refuses real PE imports and puts a `b _start` at image offset 0,
where the PE headers would have been.

## Install (same slot and loop as the asm port)

```sh
./flashp38.sh          # after ./build.sh shell
```

That writes `build/pc64arm-boot.img` to `/dev/mmcblk0p38` over ssh and
verifies the readback. It **confirms the target by hostname and by the
bootloader's serial number before writing anything**, because the device's
address is a DHCP lease that moves, one of the addresses it has held is also
`galaxy`'s, and a `dd` to `/dev/mmcblk0` aimed at the wrong host does not come
back. The device has to be in Trixie for this, not UnoDOS. Then reboot and
pick UNODOS from the LK menu.

p38 is a RECOVERY_BOOT2 slot (no SPM/SCP/ccci firmware — fine for bare metal,
proven at the asm port's first light). Never write `lk`/`lk2`/`preloader`:
they are the only partitions here with no recovery path.

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

## M6 (2026-09-02): the unoautomate remote channel (URC)

pc64's whole URC subsystem -- `unoauto.c`, `unoauto_probe.c`,
`unoauto_gate.c`, `unoauto_remote.c` (the line protocol and its verbs),
`unoauto_screen.c` (QOI screen grabs), `netdisc.c`, `unostorage.c` --
**compiles for this platform unchanged** (a probe of every file first, then
the build). What it needed from the platform is in `urc.c`, and it is small:
a DEBUG.CFG reader that answers from what the platform knows about itself,
the production fallbacks `unoauto_compat.c` would have supplied (with a real
clock and a log that reaches the eMMC, which that file does not have), a
handful of "this machine cannot have that" answers (UEFI boot entries,
RDRAND, the Intel WiFi hook), and a serial transport. The nine symbols the
link was missing are all of it; the ten `unosec_*` calls the gate makes are
answered "no session" in `stubs.c`, which keeps its production arming path
fail-closed.

**Which gate, and why not the production one.** In production the channel
stays disarmed until a console user arms it, and arming needs a bound
unosecure session, which needs an account, which needs a persistent store
on a FAT volume. This device mounts none yet (the SD card is the next
storage milestone), so the production arming path is unreachable here
today. `unoauto_gate.c` and `unoauto_remote.c` are therefore compiled
`-DUNO_DEBUG` -- per file, the same trick as the usb renames; none of the
headers they share with the rest of the build changes a layout under it --
so the channel comes up on its own, as it does on a debug stick. With no
PIN, **every verb is open to anyone who can reach :5099**; that is a dev
device on a home LAN. `URC_PIN=123456 ./build.sh shell` closes it: the gate
then runs the production auth rules with that token (the `urc-auth` hook),
grants OBSERVE and DRIVE only, refuses every SYSTEM verb (`put`, `mkfs`,
`reboot`, `py`, ...), and three bad tokens stand the channel down for the
boot. The PIN travels in a generated, git-ignored `urc_pin.h`.

**Who brings it up.** On x86 `unoauto_remote_boot()` is called from the
debug net test, from the arming panel, or from the shell only when accounts
exist; none of those exist here. `c64_urc_tick()` (pumped from
`uno_pc64_poll`) calls it once, the frame after `netup.c`'s bring-up has
had its turn -- the listen transport needs an address to bind. Related fix:
`pc64_net_up()` used to re-run the entire bring-up (8 s link wait, a fresh
`net_init`, another DHCP window) on every call that found no lease, and the
listen transport calls it on every connect retry; it now runs once per boot
and answers with the lease state afterwards.

**On the device**: the box is a URC *server* on `:5099`, the shape a box
with a moving DHCP lease wants. The CLI in `pc64/tools/unoauto_remote.py`
only listens or opens a serial port; dial IN from Python with the library
it exports (the WinForms client's Connect button does the same):

```python
import socket, sys; sys.path.insert(0, "pc64/tools")
from unoauto_remote import UnoAutoLink
link = UnoAutoLink().attach_stream(socket.create_connection((ip, 5099)))
link.wait_hello(15); print(link.probe())
```

Two operational traps from the x86 listen-mode work apply here too:
the box serves one connection at a time and reclaims the slot only after a
silent-link timeout, so never probe the port with a bare `connect()`; and
the `reboot` verb goes through `uno_native_reset`, which is the TOPRGU's
immediate software reset (`SWRST` +0x14, key 0x1209, from the vendor
`mtk_wdt.h`) with the slow re-armed-watchdog restart as the fallback --
**unverified on hardware** as of this writing.

**The QEMU gate drives the whole dispatcher without hardware.** The virt
board has no USB and so no NIC, but the channel's serial transport is three
functions (`uart_init/write/read`, `unoauto_serial.h`), and `urc.c` puts
them on the virt board's PL011 at 0x09000000. `urc.c` tells the two boards
apart by the DTB's root `compatible` (`linux,dummy-virt`; `qharness.py`'s
FDT now carries it), so **one image boots both**: `listen` on the Cosmo,
`remote-serial` on the gate. `QHARNESS_URC=1 python3 qharness.py ...` runs
QEMU with the UART as a TCP server, attaches the real client library
(`pc64/tools/unoauto_remote.py`, staged beside the harness by `build.sh`)
to that socket exactly as it attaches to a serial port, and exercises one
verb from each family on the same image the device boots:

```
urc: HELLO received (serial transport, PL011)
urc: uptime 2314 ms and advancing
urc: probe: 6 rows, subsystems heap net fs shell perf
urc: apps: 25 registered
urc: pointer injected
urc: screen grab 270x135 (scale 4), 145800 bytes decoded
```

Green on the EL1 path, the EL2 path and the URC session (2026-09-02).
Inbound serial throughput is bounded by the PL011's 16-byte FIFO times the
frame rate plus a short top-up -- plenty for verbs, not for `put`.
`QHARNESS_URC_SKIP=pointer,screen` leaves a family out, to bisect.

**Hardware boot 1 (2026-09-03): the listener came up, the lease never
did.** The log has `urc: bringing the remote channel up (listen :5099)` and
`ua: remote: listening for a dev-PC dial-in on :5099` -- and `usb-bulk: in
arm=1 land=0` for the whole session: fifteen DISCOVERs went out (a capture
on quill saw one every 2.1 s), nothing ever came back, and the M5 receive
watchdog never fired, because it refused to act until at least one frame
had landed ("nothing has ever worked yet"). That guard hid exactly the case
that matters most: a receiver deaf from its very first frame. It is gone --
a link that is up and armed with nothing landing for two seconds is a stall
too, with a back-off to one repair per half minute so an empty network is
not a log flood. Boot 11 of M5 leased only after the watchdog's first
repair, so this is the same path made reachable from the start.

**Boot 3 answered it, and the live log caught the whole thing.** Streamed to
the dev PC as it happened, no reboot, no `readlog.sh`:

```
net: DHCP
usb-bulk: RX NEVER LANDED -- armed, link up, nothing received; endpoint state 1; repairing (try #1)
asix[at stall]:     usb-speed=3 RX_CTL=02b8 MEDIUM=00b3      <- RECEIVE_EN gone, before ANY frame
asix: medium rewritten with RECEIVE_EN set (01b3)
asix[after repair]: usb-speed=3 RX_CTL=02b8 MEDIUM=01b3
net: LEASED 192.168.2.254 gw 192.168.2.1, tx=3 rx=6
```

So the chip had cleared its own `RECEIVE_EN` again — and this time before a
single frame had landed, which is exactly the case the old guard skipped. The
new path caught it on the first try and the lease followed immediately. That
is the proof boot 2's wrapped log could not give. Two other things that boot taught: "the mouse is
dead for the first twenty seconds" is the synchronous net bring-up on the
frame loop (8 s link wait plus two DHCP windows), and `per loop input` is
still ~11 ms of polled I2C and xHCI -- the next perf lead.

**Hardware boot 2 (2026-09-03): M6 WORKS.** The repair fired, the box
leased 192.168.2.254, and a dial-in from the dev PC over the LAN ran the
whole surface: HELLO, `uptime`, `probe` (the net row reporting link+lease
and 7884 received frames), `apps` 25, `caps` (0/0/0: the debug-open gate),
`vols`, `screen info` 1080x540, a QOI `screen grab`, a `log` line that
came back on the LOG stream, and `pointer` injects that walked the cursor
round the four corners of the panel. UnoDOS on the Cosmo is now remotely
driven. Still unverified: the `reboot` verb's SWRST path.

**The platform log is live on the dev PC (2026-09-03).** Until now every
`msdc:` / `usb-bulk:` / `net:` / `perf:` / `pc64:` line went to the DRAM
ring and the eMMC only, so reading any of it meant a reboot into Linux. The
ring is now streamed over the link on unoauto's KERNEL channel (`urc.c`
`stream_log`): on every connect a replay of the last 32 KB -- the boot story
-- then each line as it is logged, paced eight lines a frame so a burst of
driver chatter can neither overflow the link's transmit queue nor stall the
shell. `c64_log_total()` gives the ring an absolute byte count so a reader
can follow it across wraps. The mirror runs one way: lines that came from
the ring are not written back by unoauto's kernel-ring sink (`g_streaming`),
so the eMMC log carries each line once. `urctail.py` is the one-command
version -- finds the box, replays, tails, optionally runs a verb or grabs
the screen first. The URC gate checks both halves: the replay must contain
`entering uno_main`, and a line logged during the session must come back.

**The gate is now immune to where the stop lands.** Twice a run failed
with a fifth of the screen differing, and the shadow PNG the harness now
writes on a failure showed why: fb[] held only the desktop background at
every stop -- QEMU had frozen the guest inside `unoui_render_ui()`, and
with two gates sharing quill the render fills most of the idle tick. So
`display.c` publishes its dirty-compare shadow (`fb_presented`, FBINFO
+104), which is by construction what the panel holds, and the harness
compares against that when fb[] is caught mid-frame. The pixel-exact check
is unchanged when the stop lands between frames.

Cost: `unoauto_remote.o` carries a 10.5 MB `.bss` (the `put` staging
buffer) and `unoauto_screen.o` 4.2 MB, so the runtime-zero range grew from
~88 MB to ~103 MB; the shipped image is 662 KB. That zeroing happens MMU-off
on Device memory in `entry.s`, which is the next boot-time lead.

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

## M7 COMPLETE ON HARDWARE (2026-09-03): the SD card, the rear touchpad, three perf wins

Three things the M6 handoff named as the next work, done together because the
first unblocks the others and the third is what makes the machine feel like a
machine.

### The SD card (MSDC1) -- `sdmmc.c`

The eMMC driver next door is a command issuer and says so at length: LK
brought MSDC0 up, tuned it, and read our own boot image with it, so `msdc.c`
only has to point a live controller at a block. **Nothing has ever touched
MSDC1.** The preloader does not use it, LK does not use it, and LK's
`platform_uninit()` never mentions it. So this is the real thing: clock,
pinmux, pad config, controller reset, and the public SD initialisation
sequence from CMD0 to a card in the transfer state.

Why it matters more than a second disk: without a writable volume the shell
runs on the RAM disk, `SHELL.CFG` does not survive a reboot, the Control Panel
opens at every boot, `.UNO` apps have nowhere to live and URC's `put` has
nowhere to put anything. The eMMC cannot supply that -- every partition on it
belongs to Android, to Gemian, to the GPT or to the preloader -- and the
2026-09-01 decision was that it stays that way. The card is ours, it is
already vfat, and nothing else is competing for it.

**The fact that made it a day's work rather than a week's.** LK's own
`msdc_io.c` carries the comment *"Preload and LK need not touch power since it
is default on"*, and `msdc_init`'s *"since VEMC/VMC/VMCH are default on"*.
VMCH (the card's 3.0 V supply) and VMC (its 3.3 V I/O rail) come up with the
PMIC and are never gated -- so there is no PMIC-wrapper code here and there
does not need to be. If a unit disagrees, the symptom is an ACMD41 that never
answers, and the log says exactly that.

What is deliberately modest:

- **The clock mux is read, decoded, logged, and never written.** The same
  topckgen word carries MSDC0's selector, and MSDC0 is where the debug log
  lives; a slip there takes away the only channel that could report it. Only
  the mux's power-down bit is touched, and only if it is set. A selector
  outside the known table is treated as the fastest possible source, which
  makes every divider conservative rather than the card overclocked.
- **SD default speed, no more.** From a 26 MHz source that is 13 MHz on four
  lines. No CMD6 high-speed switch, no UHS, no voltage switch, no tuning. A
  4-bit bus at 13 MHz is ~6 MB/s of card, far more than a PIO loop will draw.
- **PIO, not DMA**, for `msdc.c`'s reason: a wild DMA write on this device is
  a brick, and PIO cannot make one.
- **Writes are fenced** to the partition the driver found. The card's own MBR
  is somebody else's data -- it is what keeps the card readable in a PC.

Reads stream: CMD18 runs up to 64 sectors on one command instead of one
command per sector, which is what fat.c wants for a multi-sector cluster.
Writes stay one block per command, because a write is rarer and the failure
mode is worse.

`blk.c` registers the card's FAT partition as `sd0`, partition-relative like
`emmc0`, and marks it the boot disk -- so `uno_fs_pref_vol()` puts session
state on the card. `emmc0`'s own boot flag becomes conditional: two volumes
both claiming to be the boot disk is exactly the ambiguity that flag exists to
remove.

Both an MBR and a "superfloppy" (a BPB right at LBA 0, which is what a camera
leaves) are handled, because the alternative is telling somebody their card is
broken when it is not.

### The rear cover panel as a touchpad -- `codi.c`

The cover panel is not a peripheral this SoC can touch: it is an STM32L4R9 on
the always-on VBAT rail with its own display and its own FocalTech FT3x67, and
the AP reaches it through one UART. So the driver is a UART, a message codec
and a small state machine. The protocol and the electrical facts are
`docs/codi-driver-spec.md` in hmofet/cosmo; the *feel* is a port of
`scripts/cosmo-rear-touchpad`, the Linux daemon that has driven this panel
since 2026-08-31, with its user-validated tuning carried across unchanged --
those numbers were chosen with a finger on the glass, and re-deriving them by
feel on a device we can only observe through a log would be worse than
useless.

What that buys: pointer motion through a response curve (sub-unity gain below
speed 8 for precision, capped +30% acceleration above 25), coarse-report
smoothing that turns one 50-unit report into a short glide rather than a
teleport, tap to left-click, tap-then-touch to drag, and two-finger tap to
right-click via OurCodi's mode-4 finger-count extension. All in thousandths,
because the inputs are integer panel units and the output is integer pixels;
a floating-point response curve would buy nothing a divide does not.

**Which pins.** This is the one fact the spec could not pin down, so it is
measured rather than guessed. UART1 can come out on three pin pairs (110/112
function 7, 46/47 function 2, 19/20 function 5); 41/42 is a fourth on paper
and is deliberately NOT probed, because those two carry USB IDDIG and DRVVBUS
in their function 1 and muxing the live USB host's ID and VBUS lines away on
the chance that a UART is behind them is a bad trade for a guess. (If all
three candidates come up empty, that pair is the next thing to try, with the
USB stack quiet.) MediaTek's
MT6771 reference DWS -- which Planet built this device from, and which is in
the vendor kernel -- routes UART1 to 110/112, so that pair is tried first.
Each candidate gets the pinmux, a version query and a 300 ms window; the pair
the CoDi answers on wins and the losers are put back exactly as they were
found.

**And the probe never blocks.** The first shape of it did the three windows
inline in the shell's first frame. It worked, and it was still wrong: a driver
that cannot find its device must not be able to stall the machine for a second
while it fails to. The QEMU gate caught it as a lost URC handshake -- 900 ms
of spin in frame zero was enough -- and on the device the same second is one
where the desktop is up and frozen. The probe is now a state machine the poll
drives: `init()` arms the first candidate and returns, and each poll gives the
current one a look. Same 900 ms of wall clock, and the shell renders through
all of it.

The three AP control GPIOs (reset 77, download-select 80, wake 157) are
deliberately not touched. The MCU is always powered and never needs a reset to
talk, and a stray reset with download-select high leaves it in its flashing
stub with the panel dark -- a service call rather than a bug.

Stock Planet firmware defines the mouse messages and never sends one (measured
exhaustively, `docs/codi-third-party.md`), so on a stock CoDi this driver
reports the version and goes quiet. That is the right outcome, and the log
names it.

### Three measured perf wins

The M6 handoff named the first two off its own `perf:` lines.

1. **Both I2C buses now run at 400 kHz, not 100.** That was never a decision:
   100 kHz is the vendor driver's default for a client that asks for nothing,
   and both parts here -- the AW9523 expander and the NT36672 controller --
   are Fast-mode devices. The shell polls them every frame with no interrupt
   to lean on, so the bus rate is straightforwardly a quarter of the per-loop
   input cost. The timing register is computed from the source mux at runtime
   by the vendor's own recipe (`clk / (2 * sample * step)`, each count stored
   minus one) rather than picked from a table, and it always lands at or below
   the rate asked for -- overshooting an I2C bus is how a part that works
   becomes a part that intermittently NAKs. Both drivers retry once at 100 kHz
   if their probe comes back empty, so a part that turns out not to like Fast
   mode degrades instead of vanishing.

2. **The keyboard's idle poll is one I2C transaction, not three.** It used to
   re-write `P1_CFG` and `P1_OUT` before every read -- two thirds of the
   traffic on an idle desktop, and both writes were setting registers to the
   values the previous poll had already parked them at. The park state is
   tracked now; the writes happen only when something has actually moved the
   columns.

3. **`entry.s` no longer zeroes 100 MB of `.bss` with the MMU off.** Every
   store there is its own Device-memory bus transaction, so the old
   zero-it-all loop spent about a second of every boot. The build now collects
   everything the boot touches before `mmu_init()` returns -- stack, fault
   stack, debug page, page tables, log bookkeeping -- into a `.early` section
   via the `C64_EARLY` attribute; `flatten.py` records its range at image
   offset 0x50; `entry.s` zeroes only that (552 KB); and `c64_bss_zero_rest()`
   clears the remaining ~102 MB as the last thing `mmu_init()` does, with the
   caches on, where it costs milliseconds.

   The guarantee that made the original loop worth having is unchanged: no
   byte of DRAM is trusted before something has written it. Each half is
   zeroed before anything that lives in it runs. Zero at offset 0x50 means "no
   early section" and `entry.s` falls back to zeroing the lot, so an older
   payload or a build without the attribute still boots.

   Two things to know if you touch it. The section name has to fit COFF's
   eight bytes -- the first attempt was `.bssearly`, lld silently dropped it,
   and `flatten.py` printed no `.early` line, which is the tell. And the
   zero-the-rest half lives in `mmu.c` rather than `platform.c` so that
   *every* payload gets it: the m0 image, the touch calibrator and the USB
   probe all reach the MMU through `mmu_init()`, and a payload that quietly
   skipped the second half would be one whose statics are whatever the last
   boot left in DRAM.

### The rails, which is where the SD card actually was

The first hardware boot failed, and the way it failed is the useful part. The
pads were muxed (the preloader had already done it), the pull-ups were on, and
`MSDC_PS` still read `CMD=0 DAT3..0=0000`. A pad with a pull-up enabled reads
zero only if something drives it low or it has no supply, and nothing drives an
idle bus -- so the pads had no supply, and LK's "since VEMC/VMC/VMCH are
default on", the one thing this port had taken on the vendor's word, was false
on this board. Trixie on the same machine enumerated the card at 200 MHz and
reported both rails as LDOs it switches on itself.

So `pmic.c`: the MT6358 over MediaTek's PMIC wrapper, an adoption rather than a
bring-up because the preloader leaves the wrapper live and WACS2 enabled.

It shipped unable to write, and that was worth a flash cycle. A PMIC write is
the one class of mistake on this device that does not come back -- every rail
is behind those registers -- so the write path was compiled out entirely, and
a read-only build went first to confirm the address map. It confirmed it three
ways: **VEMC, the rail the working eMMC runs on, read enabled at 3.0 V** (an
experiment with a known answer, run beside the one whose answer we did not
have); every selector decoded to one of the three or four legal codes its field
allows; and VEMC and VMCH both matched, to the millivolt, what the running
kernel reports through its own sysfs.

The read pass also shrank the change. Both rails were already *selected* at
3.0 V and merely switched off, and 3.0 V is inside default speed's 2.7-3.6 V
window -- so the driver writes no voltage at all. It sets two enable bits.
Copying Linux's live values would have been wrong twice over: Linux keeps VMC
at 1.8 V because it negotiated UHS SDR104, and this driver runs default speed.

What guards it now that the writes are armed: a whitelist with no address
parameter (there is no way to name a register this port has not thought
about), `MAP CONFIRMED` gating every write, a voltage sanity gate that refuses
to enable a rail selected below 2.9 V, and a read-back verify on each write.
`PMIC_WRITE=0 ./build.sh shell` still produces an image that physically cannot
write the PMIC -- reach for it on a new unit, or after touching any address in
that table.

### Gating

All three QEMU gates green (plain, `QHARNESS_EL2=1`, and the URC session), and
`BLKTEST=1` still passes its mkfs / mount / write / read / verify / delete
round trip. The virt board has neither an SD slot, a cover MCU nor a PMIC, so
what the gate proves about M7 is that all three drivers come up, find nothing,
say so, and get out of the way -- which is the failure mode that has to be
right before the hardware failure modes are worth reading.

(The BLKTEST image's framebuffer checks fail, as they did before M7: its extra
36 MiB of `.bss` swallows the address QEMU puts the DTB at, so the DTB is gone
before `c64_fb_adopt` looks for it. The storage round trip -- the reason
BLKTEST exists -- passes. Never ship a BLKTEST image.)

## M8 COMPLETE ON HARDWARE (2026-09-04): `.UNO` modules on aarch64

M7 gave apps somewhere to live; M8 gives them a way in. The loader is the
real `pc64/pc64_modload.c` now -- compiled unchanged but for two seams -- and
a `.UNO` built for this machine is the same container as an x86 one: the
loader's job (copy the image, add the base to a list of u64 cells, write a
kernel address into each named slot) has nothing architecture-specific in
it, and llvm-mingw's aarch64 target emits the same `DIR64` base relocations
for absolute pointers that x86 does (ADRP/ADD pairs are PC-relative and need
none). What differs is small and all of it is listed here.

### What is different, and why each piece exists

**The ABI word names the machine.** `UnoModHdr.abi` was always `1`; it is
now `machine << 8 | 1`, with `0x00` for x86-64 (so every module built before
this date still reads as `1`) and `0xA6` for aarch64. `uno_app.h` keys
`UNO_ABI_VERSION` on `__aarch64__`, and `mkuno.py convert` reads the machine
off the PE header and stamps the matching word. The point is the check the
loader already had: `h->abi != UNO_ABI_VERSION` now refuses a module built
for the other architecture, and `uno_mod_desc_read` refuses it too, so an
x86 `.UNO` on this SD card does not even appear in the launcher. The
alternative was jumping into foreign code. (A PYAPP container carries `1`
and is refused here as a consequence; PYRT is not ported, so nothing is
lost yet -- when it is, the source tier wants a machine-neutral word.)

**The thunk is three instructions through x16.** An import on x86 is
`jmp *slot(%rip)`. Here `mkuno.py thunks <syms> <out.s> aarch64` emits

```
    adrp x16, slot
    ldr  x16, [x16, :lo12:slot]
    br   x16
```

x16 is the AAPCS64 intra-procedure-call register, which a veneer may clobber
without the caller noticing -- so the thunk is transparent to the callee's
argument registers, which is the property that lets a module call `snprintf`
through it. ADRP's reach is +/-4 GB page-relative, so the slot may sit
anywhere in the image; `.p2align 3` on the slot section keeps every `.quad`
8-aligned for the LDR under `-mstrict-align` (the records are 32 bytes, so
the padding is always zero, but the fault would be silent).

**The I-side is not coherent with the stores that wrote the code.** x86
keeps instruction fetch coherent with data writes; AArch64 does not, and a
module whose pages were last seen by the I-cache as zeros (the `.bss` clear)
or as the previous occupant of the arena slot (Studio's build-run loop
reuses one) executes stale bytes. So `pc64_modload.c` calls
`uno_pc64_code_sync(base, pages)` after the last slot is written, declared
under `__aarch64__` and a static no-op otherwise, and `cpu.s` implements it
as `dc cvau` over every D line of the range, `dsb ish`, `ic ivau` over every
I line, `dsb ish`, `isb` -- line sizes read from `CTR_EL0` rather than
assumed, because the A53 and A73 halves of this SoC agree on 64 bytes today
and this stays right if a later part does not. This is the one piece the
gate cannot fully vouch for: QEMU's TCG has no separate I-cache to be stale.

**The arena is the loader's no-firmware seam.** `mod_alloc()` takes pages
from EFI `AllocatePages` while firmware is live, or, when `uno_pc64_st()`
answers NULL, from the arena `uno_modload_reserve()` carved at boot -- and
that reservation, on the no-system-table path, asks `uno_bios_find_ram()`,
the E820 walk a BIOS boot uses. `platform.c` answers that call with a
static 4.5 MB page-aligned array in `.bss` (`MOD_ARENA_PAGES` +
`USER_SLOT_PAGES`, checked against the request rather than trusted).
`.bss` is Normal write-back with no execute-never bit in `mmu.c`'s map,
which is what code that will be jumped into needs. `c_main` calls
`uno_modload_reserve()` before the shell, because the loader's own comment
is right: with nothing reserved every module "fails to load" while the
desktop draws fine, which is the easiest failure to call working.

**The loader now talks.** `-DUNO_MODLOAD_LOG` (build.sh, this file only)
routes `mdbg()` -- "modload: bad crc", "modload: unresolved import X",
"modload: ok" -- through `uno_dbg_log` and so onto the eMMC log and the URC
KERNEL channel, gathered into whole lines from the pieces the loader emits.
A load that fails on the device is otherwise silent, and silent is the
failure mode that costs a flash cycle.

**The other side of the export table.** `kExports[]` takes the ADDRESS of
every function a module may import, so each one must link even where the
subsystem behind it does not exist on this SoC: 191 of the 457 did not
(TLS, unojs, unovirt, unolog's surface, unoscript, the PCIe NIC families,
uno3d, sampled audio, bindings/prefs, unopkg). `stubs.c` carries them,
each answering "absent" in its header's own shape -- NULL for a handle,
`-1` where the header says 0 is success (`tls_connect`), `""` for a name,
an emptied buffer for an out-string. A module whose import resolves to one
of these loads fine and finds the feature missing at run time, which is the
right order: Network says "no adapter", it does not fail to open. Three of
those groups are portable code that could compile in instead, and should,
once the ABI is hardware-proven: `uno_binds.c` (prefs, now that SHELL.CFG
has a volume), `unolog.c` (what LOGVIEW.UNO exists to show) and
`uno3d.c` + `uno3d_soft.c`.

**Cost:** 2 MB (`gModBuf`, the read buffer) + 4.5 MB (the arena) of `.bss`,
all runtime-zeroed, none shipped. The payload is unchanged at 715 KB.

### Building the modules

`./build.sh apps` stages the tree and runs `mkapps.sh` ON quill: it is
`pc64/build.sh`'s [3b]/[3d] pipeline with the target changed and nothing
else -- compile each app with the shell's own flags (so a module obeys
`-mstrict-align`, LLP64 and freestanding exactly as the kernel does), list
the symbols the objects leave undefined, refuse any that `pc64_modload.c`
does not export, thunk, link a DLL with no libraries and no exports but
the entry, flatten. Seven modules, all with only `DIR64` relocations:
DOSTRIS, PACMAN, OUTLAST, TRACKER, PAINT (classic tier, 24-29 KB each),
LOGVIEW and PHOTOS (unoui-class, 34 and 169 KB; Photos carries the image
half of unomedia inside it, as on x86). MUSIC and NETWORK are not built:
x86 packs them, but neither has had a launcher slot since its pane went
native, and a file nothing can open is not worth shipping. aarch64 clang
emits no `__chkstk` probe even for an 8 KB frame, so the
`-mno-stack-arg-probe` dance the x86 Office modules need does not arise.

The modules are not part of the boot image. They go to `APPS\` on the SD
card -- over URC (`put 1 APPS\X.UNO`, which is how the current set got
there), or from Trixie -- or onto the RAM disk for the gate.

### Gating

`QHARNESS_UNO=<a.UNO>[,<b.UNO>...]` extends the URC gate: each file is
pushed onto the RAM disk (volume 0), the launcher rescans, the app is
launched by id, and the run requires the loader's own `modload: ok` on the
KERNEL channel plus a guest that still answers `uptime` afterwards -- the
entry was called and came back. The screen after each launch is saved as
`uno-<stem>.png`. The RAM disk made this possible without a new transport:
its namespace is flat, but a name is up to 31 characters and `APPS\X.UNO`
is 16, so `mod_read`'s search path finds the file exactly as it would on
the card.

```sh
U=$(ls build/apps/*.UNO | tr '\n' ',' | sed 's/,$//')
QHARNESS_UNO="$U" QHARNESS_URC=1 QHARNESS_EL2=1 python3 qharness.py build/shell.bin /tmp/c.png 120
```

Result on 2026-09-04, under EL2:

```
modload: arena 4608 KB at 0x467fd000
ua: modload: DOSTRIS.UNO
ua: modload: ok
urc: uno: DOSTRIS.UNO loaded and launched as 'dostris' (launched; uptime 5570)
urc: uno: LOGVIEW.UNO loaded and launched as 'logview' ...
urc: uno: OUTLAST.UNO ... PACMAN.UNO ... PAINT.UNO ... PHOTOS.UNO ... TRACKER.UNO
```

All seven load and draw -- Dostris's board and side panel, Paint's tool
palette and colour bar, Tracker's pattern grid, Photos's file pane, LogView's
status line -- through 4 to 33 thunked imports each. The plain and EL2 gates
stay green. `BLKTEST=1` was not re-run: nothing in the storage stack changed.

### On the hardware

The modules reached the card before the image did: the Cosmo was found
running the M7 image at .254, the SD card mounted read-write as volume 1,
and `put 1 APPS\X.UNO` over URC landed and verified all of them (that
image's URC predates `apps list` and `mkdir`, which time out; `put` is
older and works). Then `./flashp38.sh` from Trixie, a person at the LK
menu, and over URC from the dev PC:

```
launch dostris -> launched     ua: modload: DOSTRIS.UNO / ua: modload: ok
launch pacman  -> launched     ua: modload: PACMAN.UNO  / ua: modload: ok
launch outlast, tracker, paint                         ... modload: ok
launch photos  -> launched     ua: modload(uui): PHOTOS.UNO / modload: ok
launch logview -> launched     ua: modload(uui): LOGVIEW.UNO / modload: ok
```

Seven for seven, each followed by an `uptime` that still answered and a
screen grab showing the app drawn -- Photos listing the SD card's root
(`FILES`, `SHELL.CFG`, `APPS`), Paint's palette, Dostris's board. That is
the I-cache question answered: `uno_pc64_code_sync` is sufficient on the
A53/A73 pair for a module that was just written into `.bss`. One thing the
session did not show: the boot story. After two hours of `perf:` lines the
96 KB URC replay window no longer reached back to `modload: arena`, which is
open item 2 above doing exactly what it says.

## The providers (2026-09-04): three stubbed subsystems made real

M8 left 191 of the kernel's exports as `stubs.c` placeholders, so a module
that imported one loaded and then found the feature absent. Three of those
groups were never absent by necessity -- they are portable pc64 code that only
needed a volume to exist and a compiler to reach them. Now that M8's SD card
gives the first and M8's ABI the second, they compile into the shell for real:

- **`uno_binds.c`** -- key bindings and per-app preferences. They persist
  beside `SHELL.CFG` on the SD card through `uno_fs_pref_vol`, so a rebind or
  an app's remembered setting survives a reboot. Every dependency
  (`uno_fs_read/write/pref_vol`, the libc it uses) the shell already defined.
- **`unolog.c`** -- the system log. `platform.c` calls `unolog_init()` after
  the storage report, exactly where `uefi_main.c` calls it, so `LOGS\` is
  reachable on the first record. This is the subsystem `LOGVIEW.UNO` exists to
  show, and the URC gate now proves the loop end to end: the module loads and
  draws real records (`union file sink: fat volume 1`, `union started:
  level=notice`), not the empty ring the stub returned.
- **`uno3d.c` + `uno3d_soft.c`** -- the 3D library and its software
  rasteriser, plus `uno3d_game.c` and `pc64_games.c` for Runner3D on top. The
  kernel side names the Intel backend by default (`pc64_games.c`, unchanged on
  x86); this build defines `UNO_U3D_BACKEND=u3d_backend_soft` at compile time
  through a new `#ifndef` seam, because the MT6771 has no PCI GPU for
  `uno3d_intel.c` to bind and no reason to drag that file in. One export
  (`u3d_backend_intel`) leaves the roster and one file (`uno3d_intel.c`) is
  never compiled; nothing else about uno3d changes.

Every dependency these five files reach for was already defined by the shell
(checked against the M7 symbol table), so this is purely a matter of moving
them out of `stubs.c` and into `build.sh`'s `PCORE`/`U3D` lists. Cost: about
9 MB more `.bss`, almost all of it `uno3d_soft.c`'s full-panel z-buffer, all
runtime-zeroed and none shipped.

### Proving Runner3D: `rncap.py`, because URC cannot

A fullscreen native game defeats the URC command channel, and the reason is
structural, not a bug. Runner3D drops the desktop to 540x270 and redraws every
frame, so the guest emits `perf:` lines continuously; those share the one URC
tx queue with command replies, and the queue stays backed up with log spam, so
`uptime`, `screen grab` and `close` all time out. The link is up the whole
time -- the log keeps streaming, which is how you can see the guest is
healthy -- but nothing can be *asked* of it. `qharness.py`'s end-of-run panel
check has the same problem from the other side: it reconstructs the panel at
the default 1080x540 and a fullscreen game legitimately changed that size.

So a fullscreen native app is captured the way CLAUDE.md's screenshot rule
says to when a control channel exists -- straight off the panel, no command
reply needed. `rncap.py` boots the payload on the same QEMU virt board,
launches the app over URC (the `launch` verb answers *before* the game takes
the desktop), then reads the framebuffer with QMP `pmemsave` and reconstructs
the eye view from the payload's own published geometry:

```sh
python3 rncap.py build/shell.bin /tmp/runner3d.png runner3d
# -> geometry 540x270 scale 4 ...; 145800 of 145800 px non-background
```

The result shows the textured corridor, the score, the control hint, and
`soft` -- `u3d_backend_name()`, i.e. the software rasteriser reporting itself,
which is the whole point of the `UNO_U3D_BACKEND` seam working. `rncap.py` is
not part of the merge gate (`qharness.py` is, unchanged); it is how a change to
uno3d or any native game is eyeballed without a flash.

### Gate

Plain, EL2 and URC gates green. The URC gate additionally loads `LOGVIEW.UNO`
(real records) and `PAINT.UNO`. `rncap.py` captures Runner3D. `BLKTEST=1` was
not re-run: nothing in the storage stack changed. On x86, `pc64_games.c`'s new
seam defaults to the Intel backend, so the shared file is byte-for-byte
behaviour-identical there; the pc64 prod + debug builds and `tools/gate.sh`
confirm it.

## The Office suite (2026-09-04): UnoWord, UnoCalc, UnoShow built for aarch64

The three unoui-class Office modules now build for this machine, on the same
PHOTOS pattern the other big modules use: each statically links the uoffice
chrome lane (command bars, dialogs, the file dialog, the document model and
layout) and ONLY its own half of `unodoc` (Word / Excel / PowerPoint), plus
`unomedia`'s `unomedia.c` + `um_inflate.c` for the OOXML deflate. So no module
pays for another's format and the kernel gains no document code at all.

`build.sh apps` builds them through `mkapps.sh`'s `office()` helper, which is
`pc64/build.sh`'s [3d2]/[3d3]/[3d4] recipes with the `-I` paths adjusted for
the cosmo64 tree layout. `build.sh`'s staged tar now includes `unodoc` (the one
tree the shell build never needed). Sizes and import counts, all resolved
against the kernel export table:

| module | .UNO size | imports |
|---|---|---|
| UOWORD | 776 KB | 39 |
| UOCALC | 799 KB | 33 |
| UOSHOW | 436 KB | 38 |

Two build facts worth keeping:

- **`-mno-stack-arg-probe` is needed here too.** unodoc's `.doc` reader keeps a
  4 KB FIB on the stack and the OOXML path has large frames, and the aarch64
  mingw target -- unlike the 8 KB case M8 tested -- emits a `__chkstk` call for
  them. That probe walks Windows guard pages this OS does not have, and a
  freestanding module has nothing to resolve it against (`cpu.s`'s `__chkstk` is
  in the shell image, not in a module). The flag drops the probe, exactly as on
  x86.
- **`um_alloc` comes from `unomedia.c`, not `um_inflate.c`.** The office modules
  link both, as x86 does; linking only `um_inflate.c` leaves `um_alloc`/
  `um_free`/`um_set_alloc` undefined.

### On the hardware

These cannot go through the QEMU module gate the way the small modules do:
`QHARNESS_UNO` pushes onto the RAM disk (volume 0), whose per-file ceiling is
256 KB (`pc64_io.c`, sized for Paint images), and every Office module is ~450 KB
to ~800 KB. That is a gate limitation, not a defect -- on the device these live
on the SD card (FAT, no such cap), which is where they belong. So the QEMU
module gate covers modules up to 256 KB; larger ones are gated on hardware.

And they are. Staged onto the SD card's `APPS\` over ssh from Trixie (an ssh
copy, not a URC `put` -- more reliable for ~2 MB of files), then over URC
against the providers image (whose boot story shows `modload: arena 4608 KB`):

```
launch uoword -> launched  ua: modload(uui): UOWORD.UNO / ua: modload: ok
launch uocalc -> launched  ua: modload(uui): UOCALC.UNO / ua: modload: ok
launch uoshow -> launched  ua: modload(uui): UOSHOW.UNO / ua: modload: ok
```

Each loaded, drew, and left a guest that still answered `uptime`: UnoWord with
its ruler, toolbar and document; UnoCalc with the cell grid, formula bar and
Sheet1/2/3 tabs; UnoShow with its slide placeholders and the Slide Show menu.
The imports told the truth -- all 33-39 resolve to core exports (`fb_*`,
`uno_fs_*`, `unoui_*`, `uno_font_*`, `pc64_shell_*`) the image carries.

## The debug log keeps the boot story (2026-09-04)

The eMMC log window is 128 KiB and used to keep only the tail, so an hour of
`perf:` lines pushed the boot story -- the bring-up every hard bug starts
from -- out of the durable log. Two changes fix it, at the source so every
reader benefits (the eMMC slot, the URC replay, a future pstore):

- **A frozen boot preamble.** `log.c` copies the first 16 KiB it ever logs into
  a buffer the ring's wrap can never reach (`C64_EARLY`, so it survives being
  written MMU-off before `.bss` is zeroed). That is the whole boot story with
  room to spare: the DRAM survey, MMU, framebuffer, PMIC/SD, storage, the
  module arena, USB, and `entering uno_main`.
- **A two-shape window.** `c64_log_window()` returns the whole ring verbatim
  while a session is short enough to fit, and `[boot preamble][marker][recent
  tail]` once it has wrapped -- the preamble puts the boot story back at the
  front, the tail fills the rest with the newest lines. `c64_log_flush()`
  (msdc.c) now lays that out on the eMMC and gates on the MONOTONIC total, not
  the ring's live size: a size-based check froze the eMMC log the moment the
  ring filled, which was a second way to lose a long session.

The perf chatter is also throttled: the first 15 windows (~30 s) log verbatim,
then one window in 15, cutting a boot's perf output from ~250 KB/hour to
~17 KB so the ring churns far more slowly.

The window model is pure log arithmetic with no block I/O, which is why it
lives in `log.c` and not in the SD driver: the QEMU virt board has no MSDC, so
the gate cannot exercise the eMMC path, but `test/logwin_test.c` compiles the
real `log.c` on the host and checks both shapes byte-for-byte -- a short
session verbatim, and a 420 KB session wrapping to
`[16 KiB preamble][marker][114 KiB newest tail]` that opens on the boot banner
and ends on the newest byte. Build and run it on quill (there is no `cc` on
amanuensis):

```sh
cc -o /tmp/logwin_test cosmo64/test/logwin_test.c && /tmp/logwin_test
```
