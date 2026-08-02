# BIOS boot for pc64, a phased plan

Status: **A-E done in QEMU, plus the installer and the flasher. TWO METAL RUNS,
NEITHER CONCLUSIVE.** Run 1 (Acer Revo RL100, 2026-07-31) does not boot and does
not yet distinguish a dead video mode from a failed long-mode transition. Run 2
(Asus Eee PC 1005, 2026-08-01) got further and may have reached the kernel, but
ran from a stress-driver stick that powers the machine off by itself, so it is
confounded. **Both metal sections are at the end of this file; read run 2 first,
it has the cleaner next step.**

In QEMU: one image
boots both firmwares, a BIOS boot is a full system including on hardware with no
AHCI, the OS installs itself onto a disk that then boots, and the Windows
flasher writes hybrid sticks by default. Remaining: everything that needs real
metal. Details per phase below.

| Phase | State |
|---|---|
| A, the loader reaches long mode | **DONE**, QEMU + SeaBIOS: `tools/mkbios.sh test` then `tools/bios_qemu.py` paints 1920x1200 from 64-bit C |
| B, the kernel boots from it | **DONE**, QEMU + SeaBIOS: the full desktop, `build/unodos-hybrid.img` |
| C, storage + input with no firmware at all | **DONE except the installer**: AHCI, i8042 keyboard+mouse, an OS volume, loadable modules and ACPI all work |
| D, the old-machine tier (P4 / Core) | **DONE in QEMU**: `ide.c` (PIO ATA) boots a PIIX3/Core2 machine with no AHCI; the no-long-mode refusal verified. VBE quirks need metal |
| E, one image that boots both ways | **DONE**, QEMU: one file boots SeaBIOS and OVMF from identical copies |

## Why this is a bootloader project and not an OS project

The detach programme already finished the hard half. After `ExitBootServices`
pc64 runs on its own drivers for storage, filesystems, network, input, timing,
the clock, power and module loading, and [pc64/FIRMWARE-SURFACE.md](../pc64/FIRMWARE-SURFACE.md)
audits what is left: **29 `gBS->`/`gST->` calls in `uefi_main.c` and one
`AllocatePages` in `pc64_modload.c`.** Section 1 of that document lists the 13
boot-phase sites, and that list is this plan's work list.

Two facts make it smaller than it looks:

- **Nothing allocates from firmware.** The heap is a static 32 MB `.bss` array
  (`pc64_libc.c`), so there is no memory map to replace for allocation. The one
  `GetMemoryMap` call exists solely to obtain the ExitBootServices key, and on
  BIOS there is nothing to exit.
- **A BIOS boot is a machine that was detached from birth.** `gDetached` is 1
  and `gST` is NULL from the first instruction, and much of the tree already
  guards on exactly those (`if (!ST) return;`, `if (uno_pc64_detached())`). The
  posture the detach programme spent months reaching is the BIOS starting
  state.

What genuinely has to be built is everything the firmware does BEFORE handing
over: get off the ground in 16-bit real mode, find and load the kernel, choose a
video mode, and reach long mode.

## The target, and the one hard exclusion

Mostly modern UEFI machines run in CSM / legacy mode, plus some Pentium 4 and
Core era hardware.

**LONG MODE IS REQUIRED, AND THAT EXCLUDES PART OF THAT ERA.** pc64 is x86-64.
Pre-Prescott Pentium 4 (Willamette, Northwood) has no EM64T, and Core Solo /
Core Duo (Yonah) is 32-bit only - those machines cannot run this kernel at all,
by BIOS or any other route. Prescott-era P4s (2004+) and Core 2 (Merom, 2006+)
are fine.

This is not something to discover on the machine. Stage2 tests `CPUID` leaf
`0x80000001` bit 29 **before** it touches a page table, and a machine that fails
gets a printed refusal naming the reason, in text mode, where it can still be
read. A triple fault would tell the user nothing.

The rest of the era shapes phase D: P4-era chipsets (ICH5 and earlier) predate
AHCI and run SATA in IDE compatibility mode, and nothing before Nehalem has an
xHCI. Both are real gaps in pc64's driver set, and both are phase D rather than
phase A because the machines this plan is mostly aimed at do not have them.

## The video constraint, stated once because it governs the design

VBE 2.0+ gives a linear framebuffer in the format `fb.c` already wants, so the
pixels are easy. The problem is *when*.

**Mode setting is `INT 10h`, a real-mode call, and long mode has no v8086.**
Once the switch is made there is no way back to the video BIOS short of leaving
long mode or emulating an x86. So the mode is chosen and set in stage2, its
linear framebuffer address, stride and depth are passed forward in the boot
info block, and **it is frozen for the life of the boot**. No resolution
changes, no re-probing, no `set_geometry(-1)` renegotiation.

That is strictly worse than UEFI, where GOP at least enumerates modes before
EBS. It is accepted rather than worked around: an emulator for the video BIOS
is a bigger project than this whole plan, and the payoff is a feature (runtime
resolution change) that the UEFI path does not offer either.

`gUseBlt` is dead code on this path. There is no Blt.

## Phase A, the loader reaches long mode

One chain, `pc64/boot/`, assembled with nasm (already a build dependency for
the family's other ports):

1. **MBR** - relocate to `0x0600`, find the active partition, load the VBR.
2. **VBR** - load stage2 from the FAT volume's reserved sectors.
3. **stage2**, which does the real work:
   - `INT 13h AH=42h` LBA reads, with no CHS fallback (every machine in scope
     has EDD; the 8086 CHS path in the 3.x loader exists for an XT and is not
     wanted here)
   - walk FAT16/FAT32 to find `UNODOS.SYS`
   - `INT 15h E820` memory map into the boot info block
   - `INT 10h AX=4F00/4F01/4F02` - pick the best VBE mode with a linear
     framebuffer, set it, record base/stride/bpp/width/height
   - enable A20 (fast gate first, keyboard controller as fallback)
   - **unreal mode** to load a ~2 MB kernel above 1 MB: `INT 13h` into a low
     bounce buffer, 32-bit `movsd` to its final home at `0x100000`
   - CPUID long-mode gate (see above)
   - identity page tables for the low 4 GB with 2 MB pages, GDT, `CR4.PAE`,
     `EFER.LME`, `CR0.PG`, far jump to 64-bit
   - jump to the kernel's BIOS entry with the boot info pointer in `RDI`

Existing material: `boot/mbr.asm`, `boot/vbr.asm` and `boot/stage2_hd.asm` from
the 3.x line already do LBA reads and a FAT walk, and are proven. They load a
real-mode kernel, so the disk half is a starting point and the mode switch is
new. They are `cpu 8086`-clean, a constraint this chain does not share and
should not inherit.

**Gate: MET 2026-07-31.** `tools/mkbios.sh test` builds `boot/loadertest.c`
into a bootable image and `tools/bios_qemu.py` boots it under SeaBIOS: colour
bars and a frame at 1920x1200, drawn by 64-bit C through a VBE linear
framebuffer, with no triple fault over the whole run.

Two things bit on the way, both invisible from the symptom and both now
commented where they live:

- **File alignment must equal section alignment.** A normal PE packs sections at
  file alignment 0x200 and spreads them at section alignment 0x1000, so file
  offset != RVA. stage2 copies bytes; it is not a PE loader. The symptom was a
  `#UD` at an address that is not an instruction boundary in the disassembly,
  because what ran was never the compiled code.
- **SSE has to be enabled by hand.** x86-64 mandates SSE2 so the compiler emits
  it freely, but a CPU comes out of reset with `CR0.EM` set and `CR4.OSFXSR`
  clear. UEFI does this for us, which is exactly why it is easy to miss: the
  same kernel that runs when firmware loads it dies in its own prologue when
  this loader does.

## Phase B, the kernel boots from it

- `pc64/bootinfo.h` - the stage2 → kernel contract, versioned with a magic and
  a size field so a stale stage2 against a new kernel is a readable refusal
  rather than a wild pointer.
- `pc64/bios_entry.c` - the analogue of `efi_main`: fill the globals from the
  boot info, mark the machine detached, call `uno_main()`.
- **Framebuffer from boot info** rather than GOP. `set_geometry()` keeps its
  scaling logic and loses its mode-setting.
- **Legacy table discovery.** RSDP by scanning the EBDA and `0xE0000-0xFFFFF`
  for `RSD PTR `, SMBIOS the same region for `_SM_`/`_SM3_`. Both are currently
  read from the EFI configuration table (`acpi_host.c`, `uno_debug.c`) and both
  get a fallback rather than a replacement, since the UEFI path still needs the
  config table.
- **TSC calibration without `Stall`.** Calibrate against PIT channel 2, which
  is the standard method and needs no firmware. This is the last of the 13
  boot-phase sites that has a genuine replacement rather than simply
  disappearing.
- `uno_modload_reserve()`'s `AllocatePages` becomes a fixed region taken from
  the E820 map.
- **Packaging.** The image stops being PE32+ / subsystem 10. The kernel links
  flat at `0x100000` and is objcopied to a raw binary with a small header
  carrying the load size and the `.bss` size for stage2 to zero.

**Gate: MET 2026-07-31.** `build/unodos-hybrid.img`, booted under SeaBIOS on a
plain IDE disk with no ESP and no UEFI firmware anywhere: the desktop, its
icons and taskbar, the clock running off the CMOS RTC, and the network up on
the native e1000 driver. `shots/bios_desktop.png`. The UEFI gate
(`harness.py boot`) is unchanged, which is the other half of the promise.

How it landed, and it is less code than the phase deserves:

- **`gBI` is the whole switch.** One pointer in `uefi_main.c`, non-NULL on a
  BIOS boot, and it doubles as "which front end started this machine". Every
  firmware block in `uno_pc64_init` is SKIPPED rather than replaced, because a
  BIOS boot is a machine that was detached from birth - `gST` is NULL and
  `gDetached` is 1 before any of our code runs, which is the posture the detach
  programme spent months reaching.
- **The kernel is one build with two links.** `build.sh` links the same `$OBJS`
  a second time flat at `0x100000` with `-e uno_bios_main`, so a BIOS image can
  never be a stale build of a UEFI kernel.
- **Zeroing `.bss` is ours now.** A PE's `.bss` occupies no file bytes and
  stage2 copies file bytes, so on entry every zero-initialised static held
  whatever the last user of that RAM left - the 32 MB heap, every driver flag,
  and `gBI` itself. `bios_zero_bss()` reads the image's own PE headers, which
  stage2 loaded along with everything else, and clears each section's tail
  before a single global is touched.
- **PIT channel 2 replaces `Stall`.** CPUID's TSC-frequency leaf does not exist
  before Skylake, which is most of the target list; the 8253 method works
  everywhere and is silent (speaker data bit stays clear).

**Not done in phase B, and named so nobody assumes otherwise:** ACPI does not
start on this path. `uno_acpi_start()` needs a 10 ms `Stall` and an 8 MB
`AllocatePool`, which is the same question the module loader's `AllocatePages`
asks - where does a BIOS boot get a large region from the E820 map - so they
are answered together in phase C rather than growing a static 8 MB array that
the UEFI path would also carry. The cost is battery percentage and lid state,
on a target list that is mostly desktops and machines old enough to predate
both. `uno_bios_find_rsdp()` already works; it simply has no consumer yet.

## Phase C, storage and input with no firmware at all

The detach gate's whole question - "will our drivers still reach the boot volume
once we leave?" - dissolves, because there is nothing to leave. The risk does
not: it moves earlier and becomes unconditional. After the mode switch there is
no `INT 13h` and no fallback, so AHCI (or NVMe) must come up before another byte
can be read.

**Gate: MET 2026-07-31, except the installer.** `shots/bios_system.png` is the
System window on a BIOS boot: `x86-64 legacy BIOS`, `DETACHED (native): ahci0
1 disk FAT vols 1 (UNODOS)`, `PS/2: kbd up, aux port ok, mouse streaming`,
`ACPI AML: up, 261 nodes`. `shots/bios_module.png` is Dostris - a `.UNO` module
read off that volume, relocated into an arena carved from the E820 map, running
in its own window.

What this phase actually turned out to be:

- **THE IMAGE NEEDED A FILESYSTEM.** Phase B's image was the boot chain and
  nothing else, and that is the failure mode this whole plan is most likely to
  repeat: the desktop is drawn from the kernel image, so it comes up looking
  perfectly healthy on a machine where every module, font and media file is
  unreachable. `tools/mkbios.py` now lays down an MBR partition and a FAT32
  volume built from `build/esp` - the same tree the UEFI ESP carries - after an
  8 MiB reserved area that keeps the boot chain outside any partition.
- **The module arena comes from E820.** `uno_modload_reserve()` returned early
  with no `AllocatePages`, leaving `gModArena` NULL, so `mod_alloc()` refused
  every module. `uno_bios_find_ram()` takes the top of the highest usable run
  below 4 GiB. It is also called from a different place on this path: the UEFI
  path reserves during `try_detach()`, which a BIOS boot never runs.
- **ACPI works** (`uno_acpi_start_bios`), with the RSDP from a legacy scan, the
  arena from E820, and the clock from the PIT-calibrated TSC instead of a
  `Stall`. The shared bring-up is factored out so the two entries cannot drift.
- **The System window stops saying "UEFI"** on a machine that booted from a
  BIOS. That line was hardcoded, and the System window is exactly where someone
  looks to find out what a machine is doing.

**The installer's BIOS target: DONE 2026-07-31.** `Install` now authors the
MBR shape when the running machine booted from a BIOS
(`unostorage_prepare_mbr` + `unostorage_write_bootchain`), and
`tools/bios_install_test.py` proves it the only way that counts: install onto a
blank second disk, then **boot that disk as the only disk**. It comes up
(`shots/bios_inst_booted.png`), and the same disk also boots under OVMF
(`shots/bios_inst_uefi.png`), because its partition is typed 0xEF.

Three things worth carrying:

- **GPT and this boot chain are mutually exclusive**, which is why the shape is
  chosen rather than added: a GPT's primary header sits at LBA 1 and its entry
  array at LBA 2-33, exactly where stage2 goes. A disk is one shape or the
  other. A UEFI boot keeps authoring GPT, unchanged - the MBR shape is the more
  capable of the two, but it is chosen only where it is necessary rather than
  everywhere it would work.
- **The chain travels with the system.** `tools/mkbios.py` stages the boot
  sector, the ALREADY-PATCHED stage2 and the kernel at `\BOOT\` on the volume,
  so the installer copies three blobs to three LBAs with no PE parsing and no
  arithmetic that could disagree with the image builder's.
- **`prepare_mbr` leaves the disk deliberately BIOS-inert** - a zeroed boot
  sector with a valid partition table - and the chain goes down last, after the
  tree. A disk that says "boot me" before the system it would boot is on it is
  worse than one that does not claim to.

The test's own lesson: it originally waited a fixed 20 s for the copy, which
expired mid-install and produced a disk that "booted" to a hang - a failure
indistinguishable from a broken boot chain. It now polls the target file for the
boot sector, because QEMU writes through and the chain is written last.

## Phase D, the old-machine tier

**Gate: MET 2026-07-31 in QEMU.** The test shape is `--machine pc --cpu
core2duo`: an i440fx board with a PIIX3 IDE controller and **no AHCI anywhere**,
which is what a P4 or Core machine actually looks like. `shots/bios_ide.png` is
the System window on that machine - `DETACHED (native): ide00 1 disk FAT vols 1
(UNODOS)`, the desktop with its disk-loaded apps, fonts read off the volume,
ACPI up.

- **IDE / PATA PIO landed** (`ide.c`), behind the existing `uno_bdev` seam, so
  unofs, the installer and unostorage needed no knowledge of it. Both legacy
  channels, master and slave, LBA28 and LBA48, read and write. Deliberately
  **PIO and not DMA** - bus-master DMA needs a PRD table and either an
  interrupt or a status dance, and buys throughput this OS does not need, on
  hardware that is hard to come by for testing. Deliberately **polled and not
  interrupt-driven**: there is no ATA ISR, and a driver that needs IRQ14 to make
  progress would deadlock exactly where it is least debuggable. Every wait is
  TSC-bounded; none can hang.
- Probed **after** AHCI/NVMe/SDHCI, so a machine that presents both uses the
  better driver and the legacy probe finds nothing left to claim. Probed at the
  fixed ISA ports **regardless of what PCI reports**, because a controller in
  compatibility mode may present a zero BAR or no recognisable class code, and a
  floating bus reads as 0xFF and costs microseconds.
- **The CPUID refusal path is verified** (`shots/bios_nolongmode.png`). A
  Pentium 3 prints `This CPU has no long mode (no EM64T/x86-64). UnoDOS pc64 is
  64-bit only and cannot run here.` and stops - which is the whole point of
  testing it before the page tables.
- **No xHCI.** Nothing pre-Nehalem has one. USB boot and USB HID stay out of
  scope on this tier, which is survivable precisely because these machines have
  PS/2 - and the PS/2 keyboard and mouse both come up (`kbd up, aux port ok,
  mouse streaming`).

**Not addressed: VBE quirks.** Pre-3.0 implementations, modes that advertise a
linear framebuffer and do not deliver one, and panels whose best mode is
1024x768. SeaBIOS's VBE is well behaved, so there is nothing here QEMU can
falsify - this one genuinely needs the metal, and the honest position is that
the mode-selection loop in `bios_stage2.asm` is unproven against a real vendor
VBE BIOS.

## Phase E, one image that boots both ways

**Gate: MET 2026-07-31.** `build/unodos-hybrid.img` boots under SeaBIOS
(`shots/hybrid_bios.png`, i440fx/PIIX3/Core2) and under OVMF
(`shots/hybrid_uefi.png`) - from two copies of the same file, verified by
matching md5 before each boot rather than by booting one image twice and
assuming.

**It came down to one byte, and NOT to a hybrid GPT.** The plan above proposed
GPT with a protective MBR rewritten to hold real entries, which is what Boot
Camp and rEFInd do. That was the wrong call: it is a spec violation, some
firmware rejects it, and some partitioning tools "repair" it. What actually
works is simpler - an MBR-partitioned disk whose single FAT32 partition is typed
**0xEF**, an EFI System Partition. UEFI firmware scanning MBR media looks for
exactly that and runs `\EFI\BOOT\BOOTX64.EFI` from it; a BIOS ignores the type
byte entirely and runs the boot sector, which loads the kernel from the reserved
area by raw LBA. MBR-partitioned media is inside the UEFI spec, and isohybrid
images have used this shape for years.

So there is no duplicated tree and nothing to keep in sync: one FAT volume is
the ESP to one firmware, the OS volume to the running system, and invisible to
the other boot path. `fat.c` already accepted `0xEF`, so the running system
mounts it either way with no change.

`tools/mkuefi.py` (GPT + ESP) stays, unchanged, as the fallback for firmware
that refuses MBR media.

**The Windows flasher: DONE 2026-07-31.** It writes the hybrid layout, ON BY
DEFAULT, with a checkbox ("Also boot older BIOS / CSM machines") that returns it
to GPT for firmware that refuses MBR media.

**And it was verifiable after all**, which is the part I had wrong when I
deferred it. `UnoDisk` writes through a plain `Stream`, and `UnoDiskTest.exe`
already pointed that at an image file precisely so the volume writer could be
checked against real tools instead of by flashing a stick and hoping. So the
same code that writes a USB stick wrote `build/flasher_zip.img`, and QEMU booted
it under SeaBIOS AND OVMF from identical copies. What genuinely cannot be tested
here is narrower than "the flasher": it is the Win32 layer around the write -
drive enumeration, volume dismount, and the physical-drive handle - none of
which the hybrid change touches.

The test takes the SHIPPING path deliberately. `UnoDiskTest -hybrid <zip>` calls
`UnoDisk.ChainFromZip`, the same call the flasher makes against its embedded
resource, rather than reading three files from a folder - testing the folder
path only would have verified code the product does not run.

The chain needs no new resource: `tools/mkbios.py` stages it at `\BOOT\` on the
volume, so it is already inside the ESP zip the flasher embeds, and a chain can
never be from a different build than the system beside it.

## Standing rules for this lane

- **The UEFI path is not to regress.** Every change is additive or behind
  `if (gST)`. The merge gate stays "builds `UNO_DEBUG=0` and `UNO_DEBUG=1`, and
  the QEMU UEFI gate is still green" plus, from phase B, a BIOS gate beside it.
- **Prove each phase in QEMU before touching metal.** SeaBIOS is the reference
  BIOS and it is stricter than most CSMs about EDD and VBE, so passing there is
  meaningful.
- **No CHS.** Every machine in scope has EDD. A CHS fallback is untestable code
  that will be wrong when it is finally needed.


## Metal, 2026-07-31: the Acer Revo RL100. START HERE WHEN RESUMING.

First run on real hardware. **It does not boot.** What is established, what is
not, and the one measurement that comes next.

### The machine

Acer Aspire Revo RL100 nettop: AMD, Radeon HD 4225-class integrated graphics,
**no PS/2 port** (USB keyboard and mouse), HDMI and VGA out. Firmware offered
the stick in its boot menu and was asked to boot it explicitly.

### PROVEN WORKING on that hardware

The loader runs end to end. With `UNO_BIOS_VERBOSE=1` the machine printed:

```
UnoDOS pc64          <- our boot sector, loaded and run by the firmware
pc64 stage2          <- stage2 loaded from LBA 1 via INT 13h AH=42h
a20 ok
.......              <- the kernel read off the stick, chunk by chunk
kernel loaded
e820 entries: N
vbe mode: 1024x768 lfb 0xF9000000 pitch 4096
continuing: ........ <- eight dots, one per second, from the BIOS tick counter
```

So: firmware handoff, INT 13h EDD reads, the kernel load above 1 MB via INT 15h
AH=87h, E820, VBE mode SELECTION, and a live CPU right up to the switch. The
mode line is internally coherent - pitch 4096 is exactly 1024 x 4 bytes.

### THE FAILURE

After the switch: **nothing**. No green marker band (the loader paints one from
64-bit code before the kernel runs), no splash, no desktop.

### WHAT THAT DOES NOT YET TELL US, and the next measurement

"No band" has two opposite causes, because the band is painted from 64-bit code:

1. the VBE mode set succeeded but is not reaching the display, or
2. **the long-mode transition is failing**, so there is no 64-bit code at all.

`UNO_BIOS_NOVIDEO=1` builds the probe that settles it: it SKIPS the VBE mode,
stays in text mode, enters long mode, and writes a white-on-red banner into the
VGA text buffer at 0xB8000 from 64-bit code. That image was written to the test
stick and **has not been run yet**. Run it first.

- **Banner appears** -> long mode, page tables, GDT and SSE all work on this
  hardware; the fault is entirely the video mode. Next: 800x600, then 640x480,
  then forcing a display output. A nettop of this era setting a mode on the HDMI
  transmitter while the user watches VGA (or the reverse) produces exactly this
  symptom, and the mode set returns success either way.
- **No banner** -> the transition is failing and the video was never the issue.
  Different search entirely: page-table placement (0x20000), the stack at
  0x90000 against this machine's EBDA, or the A20 path.

### Two things already fixed as a result

- **The video mode policy was too aggressive.** It took the LARGEST mode the
  card advertised, up to 1920x1200. A card will advertise, and successfully SET,
  a mode the attached display cannot show - and the mode set returns success, so
  nothing downstream can detect it. It now prefers **1024x768**, the mode every
  VBE implementation and every panel since about 1995 supports, and caps the
  fallback at 1280x1024. This is the likeliest fix for the original 1920x1200
  black screen, and it is unproven on metal.
- **The keypress gate was a mistake on this machine.** It blocked on INT 16h,
  which never returned - the RL100 has no PS/2 and its BIOS was not feeding USB
  legacy emulation to INT 16h. Replaced with a timed wait that prints a dot per
  second from the BIOS tick counter, so a live CPU says so and then continues by
  itself.

### A consequence worth knowing before judging any future result

**That machine may have no keyboard in UnoDOS even once it boots.** The BIOS
path drives the i8042, and the RL100's input is USB; pc64's USB stack is
xHCI-only and nothing of that era has an xHCI. A desktop that appears but does
not respond to typing is a driver gap (EHCI/UHCI HID), not a boot failure.

### The diagnostic knobs, which are OFF by default and ship nothing

| Build | What it does |
|---|---|
| `UNO_BIOS_VERBOSE=1 ./build.sh` | stage2 narrates each step, prints the chosen mode/LFB/pitch, waits 8 visible seconds, and paints a green band from 64-bit code before the kernel |
| `UNO_BIOS_NOVIDEO=1 ./build.sh` | skips VBE, stays in text mode, proves long mode at 0xB8000, never boots the kernel |

## Metal, 2026-08-01: the Asus Eee PC 1005. Inconclusive, and the run was confounded

Second machine, and it got further than the Revo. **Do not read this as a
failure yet** - the stick it ran from was misconfigured in a way that produces
the same symptom as a video fault.

### What was observed

The verbose stage2 narration appeared and **the dots were seen**, so the loader
ran end to end on this hardware exactly as it did on the Revo: firmware handoff,
INT 13h reads, the kernel above 1 MB, E820, VBE mode selection, and a live CPU
to the switch. Then "something with 10 in it" flashed past too quickly to read,
and the screen went black.

### Why that flash matters, and why it is NOT a stage2 message

**Nothing stage2 prints after the dots.** The dots are the last text it emits;
past that its only output is the graphical green marker band. So the flash was
one of:

1. earlier text glimpsed as the screen changed - `vbe mode: 1024x768 ...` and
   `e820 entries: N` both contain "10"; or
2. **the kernel's own boot-test console.** The stick carried a `UNO_DEBUG=1`
   build, which runs SPECTEST as a text console long before the shell paints a
   desktop.

Reading (2) would mean the VBE mode DID display and this machine reached the
kernel - substantially further than the Revo, which never proved long mode at
all. It also supplies an innocent explanation for the black screen: the stick's
`DEBUG.CFG` carried `passes=3`, so the stress driver was armed and **powers the
machine off by itself** after three passes. Black screen, no fault.

### The confound, stated plainly so the next run avoids it

The image was written with `dd` rather than by the flasher, and `build.sh`
stages a `DEBUG.CFG` with `passes=3` into `build/esp` which `mkbios.py` packages
verbatim. A hands-on boot therefore got a fuzz run that shuts the machine down,
plus a text console that scrolls, plus no URC key. All three work against
reading a first boot on new hardware. See the 2026-08-01 metal entry in
`pc64/UNOAUTOMATE-REQUESTS.md`.

### The next boot, and what it settles

A **production** image (no `DEBUG.CFG` at all, therefore no stress driver, no
auto power-off and no console) with the verbose stage2 kept. It boots straight
to a desktop, so the outcome is unambiguous. Built and staged as
`~/unodos-eee.img` on devbuntu, SeaBIOS-verified.

Watch for the **green marker band** specifically. If a green stripe appears
across the top before anything else, VBE, the LFB, the page tables and long mode
are all proven and any later fault is past the loader. If the desktop follows,
phase D is validated on real IDE-mode silicon, which is the thing QEMU cannot
falsify.

### Why this machine is worth the second boot

- **A real i8042.** The netbook's keyboard and touchpad are on the internal PS/2
  controller, unlike the Revo (USB-only input, no xHCI in that era), so it
  should be *drivable* if it comes up.
- **SATA almost certainly in IDE compatibility mode** (NM10 / ICH7-M), which
  exercises `ide.c` on real silicon for the first time - currently only
  QEMU-verified against a PIIX3.
- **A 1024x600 panel** against a mode policy that prefers 1024x768. This is
  exactly the "panels whose best mode is 1024x768" quirk this plan names as
  needing metal. If the production boot is black too, that is the strong
  hypothesis and the fix is forcing a lower mode.

### One variant trap for whoever picks this up

The Eee PC 1005 line straddles the long-mode exclusion. **1005HA/HAB/HAG are
Atom N270/N280 and have no EM64T** - they cannot run pc64 at all and should
produce stage2's printed CPUID refusal, which would itself be the first metal
test of that path (only ever exercised in QEMU against a simulated Pentium 3).
**1005PE/PEB/P/PR are Atom N450/N455** and can run it. Check before spending a
boot.
