# BIOS boot for pc64, a phased plan

Status: **A, B and most of C DONE, QEMU-verified - a BIOS boot is a
full system.** Details per phase below.

| Phase | State |
|---|---|
| A, the loader reaches long mode | **DONE**, QEMU + SeaBIOS: `tools/mkbios.sh test` then `tools/bios_qemu.py` paints 1920x1200 from 64-bit C |
| B, the kernel boots from it | **DONE**, QEMU + SeaBIOS: the full desktop, `build/unodos-bios.img` |
| C, storage + input with no firmware at all | **DONE except the installer**: AHCI, i8042 keyboard+mouse, an OS volume, loadable modules and ACPI all work |
| D, the old-machine tier (P4 / Core) | NOT STARTED |
| E, one image that boots both ways | NOT STARTED |

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

**Gate: MET 2026-07-31.** `build/unodos-bios.img`, booted under SeaBIOS on a
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

**Still open: the installer's BIOS target.** `Install` writes an ESP and a
`Boot####` variable, and `SetVariable` does not exist here; the BIOS equivalent
is writing the MBR, stage2 and the kernel to the reserved area of the target
disk. `tools/mkbios.py` already knows that layout, so the work is porting it
into `installer.c` rather than designing it. Until then a BIOS machine is
installed by writing the image to its disk from another computer.

## Phase D, the old-machine tier

Only reached once C is solid, and every item here is conditional on the
hardware actually being in the fleet:

- **VBE quirks.** Pre-3.0 implementations, modes that report a linear
  framebuffer and do not deliver one, and machines whose best mode is 1024x768.
- **IDE / PATA PIO.** ICH5 and earlier have no AHCI. pc64 has no IDE driver;
  if a target machine needs one it is a self-contained addition behind the
  existing `uno_bdev` seam.
- **No xHCI.** Nothing pre-Nehalem has one. USB boot and USB HID are therefore
  out of scope on this tier, which is survivable precisely because these
  machines have PS/2.
- The CPUID refusal path, tested on a machine that actually fails it if one is
  available.

## Phase E, one image that boots both ways

A stick that boots on a UEFI machine and a CSM machine without being reflashed:
GPT with a protective MBR that is also a real MBR, an ESP carrying
`BOOTX64.EFI`, and stage2 plus the flat kernel in the same volume. The flasher
grows a "BIOS bootable" option; `tools/mkuefi.py` grows the MBR/VBR write.

Deferred until A-D are real, because a hybrid image whose BIOS half does not
work is worse than two images.

## Standing rules for this lane

- **The UEFI path is not to regress.** Every change is additive or behind
  `if (gST)`. The merge gate stays "builds `UNO_DEBUG=0` and `UNO_DEBUG=1`, and
  the QEMU UEFI gate is still green" plus, from phase B, a BIOS gate beside it.
- **Prove each phase in QEMU before touching metal.** SeaBIOS is the reference
  BIOS and it is stricter than most CSMs about EDD and VBE, so passing there is
  meaningful.
- **No CHS.** Every machine in scope has EDD. A CHS fallback is untestable code
  that will be wrong when it is finally needed.
