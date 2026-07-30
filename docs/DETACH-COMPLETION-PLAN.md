# Detach completion, finish removing firmware from the running system (pc64)

Status: **A done on metal. B and C code-complete and QEMU-green. D
code-complete, fleet validation outstanding.** Details per phase below; the
2026-07-30 state in one screen:

| Phase | State |
|---|---|
| A, USB mass storage | **DONE**, metal-confirmed on the ZimaBlade 2026-07-30 |
| B, input gates | **CODE DONE** (branch `detach-bcd`). Gates decided from device paths, not shape; `detachgate.c` + [pc64/DETACH.md](../pc64/DETACH.md). The X1 claim is METAL-PENDING and could be wrong, see §B below |
| B3, Surface keyboard | **NOT ANSWERED**, and deliberately not guessed at: [docs/SURFACE-KEYBOARD.md](SURFACE-KEYBOARD.md) has the one boot that settles it and a conditional sizing |
| C, firmware sweep | **DONE**. Firmware `Stall` down to one call site, one clock, one power policy; the audit is [pc64/FIRMWARE-SURFACE.md](../pc64/FIRMWARE-SURFACE.md) |
| D, posture + fleet | **CODE DONE; ZimaBlade VALIDATED 2026-07-30** (all checklist items, operator-confirmed). Yoga, X1 and Surface still outstanding - and the X1 is the one that can falsify phase B, so the pointer claim remains unproven |

**What metal has and has not seen.** The ZimaBlade ran the whole thing on
2026-07-30 and passed: detach, native storage and network, keyboard and mouse
behind a hub, the clock, restart, and a device tree with five drivers bound.
That is one machine of five, and deliberately the easiest one - it is a desktop
that already detached. **The X1 Carbon is the box that can falsify phase B**,
because its pointer claim is the prediction QEMU cannot test, and it has not
been booted.

**Two things this programme did NOT do, stated plainly so nobody assumes
otherwise.** Phase B item 2 asked for the gate to become a unodevices registry
query; half of it is (which device), half of it is not (whether a driver is
bound), because the driver registry is unodevices **phase 2 and is not on
master** - `uno_devmgr.c` enumerates, nothing binds, and every real device
reports `UNCLAIMED`. A gate keyed to bind state today would refuse every
machine in the fleet. DETACH.md §4 is explicit about the substitution that
lands when phase 2 does. And nothing here has been near real hardware: the USB
work of 2026-07-29 spent three consecutive green QEMU runs on a driver that
could not work on silicon, and phase B's pointer prediction is exactly the same
shape of claim.

## Phase A progress, 2026-07-29

Landed: `-DUNO_XHCI` ships in every build (`uno_xhci_init()` now refuses to
touch hardware until `uno_pc64_detached()`, which is what made that safe);
`usbboot.c` decides before EBS whether the stick is reclaimable;
`uno_fat_native_eligible()` accepts a USB system volume; `usbmsc` binds the
identified boot device and marks it `is_boot`; the detach path verifies the
system volume actually came back and says so if it did not; `DETACH.CFG`
(`off` / `nousb` / `usb`) is a no-rebuild override. A USB-booted machine
detaching and running on its own drivers is QEMU-verified end to end.

Also, and not foreseen by this plan: **the GUI installer had to go native.**
It refused outright when detached, so the moment a stick boot detaches, "boot
the stick, run Install", the only way UnoDOS gets onto a machine, would have
broken. It now routes through `unostorage_prepare_esp()` + `uno_fs_copytree()`
when detached, the same pieces the URC `install <disk>` verb used. The lesson
generalises: **anything gated on `uno_pc64_detached()` is a feature that
disappears the day the machine detaches.** Grep for that call before flipping
any posture.

**PHASE A DONE 2026-07-30.** USB-boot detach is the default; metal-confirmed on
the ZimaBlade (no firmware: storage, network, keyboard and mouse all native,
every USB device behind a hub). Superseded note follows.

**Was opt-in, and this was the remaining Phase A work:** `xhci.c` has no
SuperSpeed endpoint-companion burst sizing and no xHCI Reset-Endpoint /
Set-TR-Dequeue recovery, so a SuperSpeed stick (bulk mps 1024) wedges its
endpoint on the first transfer error. See `pc64/USB.md`.

## Where detachment actually stands (verified 2026-07-23)

The firmware-decoupling campaign (c8b4b28) already made detach the shipping
default: `try_detach()` runs automatically at the end of `uno_pc64_init()`
(uefi_main.c) and on success ExitBootServices kills the firmware; storage
remounts on native AHCI/NVMe/SDHCI, input moves to PS/2 + I2C-HID + USB
HID, timing moves to TSC + CMOS. Networking never used firmware at all
(net.c has zero SNP/BootServices calls; NICs are native, lazily bound by
`pc64_net_up()`).

A machine stays ATTACHED (all services routed through firmware forever)
only when a gate in `try_detach()` fails:

| Gate | Which machines | Fix |
|---|---|---|
| System volume not on AHCI/NVMe/SDHCI (`uno_fat_native_eligible`) | EVERY USB-stick boot, i.e. the whole flasher flow; ZimaBlade today | Phase A: USB MSC driver |
| Would strand the only pointer (`detach_would_strand_pointer`) | X1 Carbon when the I2C-HID pad doesn't bind | Phase B |
| No native keyboard (`native_kbd_for_detach`) | Surface Laptop Go (internal kbd likely SAM, not USB HID) | Phase B (investigate) |
| No linear framebuffer (`gUseBlt`) | None known in the fleet; BltOnly firmware is rare on x86 | NON-GOAL, stays gated |

**Display is a non-issue.** Post-detach we keep scanning out of the
GOP-negotiated linear framebuffer; that is hardware state, not a firmware
service, and it is exactly what Windows' Basic Display Adapter (safe mode)
does on UEFI machines. A native modeset driver (per-generation PLL /
transcoder / DDI programming, per vendor) is explicitly out of scope. The
only cost of no display driver: no resolution switching, and BltOnly
machines cannot detach.

## Phase A, USB mass storage (the big one)

Un-blocks every USB-stick boot. The hard plumbing already exists in
`xhci.c`: control transfers, SET_CONFIGURATION, bulk rings
(`uno_usb_setup_bulk` / `uno_usb_bulk_in` / `uno_usb_bulk_out`, already
proven by the AX88179 NIC). The new driver is Bulk-Only Transport plus a
minimal SCSI set.

1. `pc64/usbmsc.c`: registry driver matching interface class 08 / subclass
   06 (SCSI transparent) / protocol 50 (BOT):
   - CBW/CSW framing over bulk-out/bulk-in, tag counter, CSW status check,
     bulk-only mass storage reset + clear-halt recovery path.
   - SCSI: INQUIRY, TEST UNIT READY, READ CAPACITY(10), READ(10),
     WRITE(10), REQUEST SENSE. Nothing else. 512-byte logical blocks
     assumed; reject others loudly.
   - Register with blkdev as a native backend (same shape as ahci/nvme
     backends that `uno_blk_detach()` rescans).
2. **Detach transition (the tricky part).** While attached, the FIRMWARE
   owns xHCI and firmware Block IO is carrying the system volume, so MSC
   cannot come up early. Sequence mirrors USB HID today:
   - Pre-detach eligibility: extend `uno_fat_native_eligible()` to also
     pass when the system volume's controller is xHCI-class AND the boot
     device path says USB. Read the boot path from LoadedImage ->
     DeviceHandle -> DevicePath (a `USB()` node identifies stick boot);
     require an xHCI function to exist in the (unodevices) tree.
   - At detach, after `uno_usb_hid_init()`: bring up MSC on the enumerated
     bulk device, then `uno_blk_detach()` + `uno_fat_remount()` pick up the
     stick natively.
   - **Risk: past ExitBootServices there is no fallback.** If MSC bring-up
     fails, the system volume is gone. Mitigations, all required:
     (a) QEMU-first: boot the actual built image from
     `-device usb-storage` (a real mkuefi FAT image, never vvfat) and run
     the full storage self-test (`UNO_STORTEST`) against it;
     (b) a config opt-out (e.g. `DETACH.CFG` / build flag) to force
     stay-attached for USB boots while this is fresh;
     (c) first metal validation on the ZimaBlade over the URC bridge
     (Verbatim stick boot, r8169 gives us remote eyes), NOT on a laptop.
3. Write-path caution: FAT write-back sync before EBS already exists
   (`uno_fat_sync`); confirm it runs in the USB-eligibility path too.

Acceptance: QEMU usb-storage boot detaches, storage self-test passes
post-detach, WRTEST proof file survives a reboot; ZimaBlade detaches on its
Verbatim stick with all disks visible and net up.

## Phase B, input gates

> **RESULT, 2026-07-30 (branch `detach-bcd`).** Items 1 and 2 landed as
> `detachgate.c` (contract: [pc64/DETACH.md](../pc64/DETACH.md)); item 3 is
> answered with a decision procedure rather than an answer, see below.
>
> The route taken for item 1 was the SECOND out, not the first, and it turned
> out to be the general one. Rather than wait on the metal I2C-HID fix, the
> gate reads the transport of the firmware's pointer out of its DEVICE PATH: a
> `PNP0Fxx` ACPI node means the i8042 aux port, which `uno_ps2_init()` claims
> at detach. So the X1 should detach whether or not its touchpad binds, and no
> hardware is touched to decide it. **This is a prediction and it is
> metal-pending** - the same category of claim QEMU signed off on three times
> during phase A before hardware refused it. `DETACH.CFG: off` is the way back.
>
> Item 2 is half done, and the plan was wrong about what blocked the other
> half. "Which device" is a registry query; "has a native driver bound" is NOT,
> because the driver registry is unodevices phase 2 and is not on master. See
> the status table at the top and DETACH.md §4.
>
> The LPSS-counting heuristic is deleted, as asked. It is not kept as a
> fallback: an empty device tree now yields an empty blocker STRING, while the
> gate itself falls through to the service owners, who answer with or without
> the registry.

1. **X1 pointer**: two independent outs, either clears the gate honestly:
   - The metal-pending I2C-HID fix (LPSS 216 MHz clock divisor + detach-gate
     pointer guard, see the trackpad notes): once confirmed on metal, the
     pad binds natively and `native_ptr_for_detach()` passes.
   - The TrackPoint is a PS/2 Elan on the i8042 aux port: post-detach
     `uno_ps2_init()` claims it anyway. The gate refuses only because it
     cannot interrogate aux while firmware owns the controller.
2. Replace the shape-heuristic gates with registry queries (needs
   unodevices phase 2): detach iff every device currently providing a
   critical service (system-volume transport, keyboard, pointer-if-any) has
   a native driver BOUND or committed-at-detach (MSC, USB HID). Delete
   `detach_would_strand_pointer()`'s LPSS-counting heuristic once the query
   exists; keep its conservative behavior as the fallback if the tree is
   somehow empty.
3. **Surface Laptop Go keyboard, investigate before promising.** We skip
   USB/PS2 takeover on Microsoft SMBIOS today, and the internal keyboard
   most likely rides the Surface Aggregator Module (we already talk to SAM
   for battery). If it is not a USB HID device, the Surface cannot detach
   until someone writes a SAM keyboard driver; that is a separate, sized
   decision for arin, not something to slip into this plan. Deliverable
   here: a definitive answer (unodevices tree dump + descriptor walk on the
   Surface) plus a one-page sizing if a SAM driver is needed.

## Phase C, attached-mode firmware sweep

> **RESULT, 2026-07-30. Done**, deliverable is
> [pc64/FIRMWARE-SURFACE.md](../pc64/FIRMWARE-SURFACE.md): 26 remaining call
> sites, each classified. Firmware `Stall` went from seven call sites to one
> (the TSC calibration moved to the top of `uno_pc64_init`, so the splash,
> stripes and chime run on our own timer), the clock is the CMOS RTC on both
> sides of detach, and shutdown/restart are one `power_down()`.
>
> One instruction here was NOT followed, deliberately. "Prefer native CF9 with
> runtime ResetSystem as fallback" holds for reset; generalising it to
> power-off hung the SPECTEST guest, because `uno_acpi_poweroff()` disables
> interrupts and returns on failure - it is terminal by construction and has to
> stay last. FIRMWARE-SURFACE.md §5 records that so nobody re-tidies it.

For machines that still run attached (and the pre-detach window on all
machines), shrink firmware usage to the unavoidable minimum:

- Audit every `gBS->` / `gST->` call site reachable after init
  (grep uefi_main.c + installer.c; net.c is already clean). Expected
  survivors: GOP mode query at init, ConIn/pointer polling while attached,
  `Stall` while attached, memory map + EBS in try_detach, RUNTIME services.
- `uno_pc64_delay_ms`: confirm it routes to TSC when detached and firmware
  Stall only while attached (net.c depends on it).
- Time: CMOS RTC post-detach already exists; make attached mode use it too
  so there is exactly one clock path.
- Reset: prefer native CF9 with runtime ResetSystem as fallback (runtime
  services are legal post-EBS; this is fine either way, just make it one
  function).
- Document the result in a short FIRMWARE-SURFACE.md: the complete list of
  firmware calls that remain and why each is legitimate (bootloader-phase,
  attached-only, or runtime-service). "Traditional OS" parity means this
  list is short and intentional, not zero.

## Phase D, flip the posture + fleet checklist

> **RESULT, 2026-07-30. Code done; the fleet pass needs the hardware.**
> Refusals now name the PCI function whose service would be lost, in the System
> window and in the debug env block.
>
> The posture flip itself turned out to be mostly a non-event, because A and B
> had already done it: detach is the default and the gates refuse only on a
> lost critical service. What B changed is that two of those refusals were
> being triggered by shape arguments rather than by anything actually being
> lost. There was no switch left to throw.
>
> The per-machine ordering, with what each box actually tests, is the "Phase D
> fleet validation" section of
> [pc64/METAL-CHECKLIST.md](../pc64/METAL-CHECKLIST.md). ZimaBlade and Yoga are
> regression checks; the X1 is the one that can falsify phase B.

- Default becomes: ALWAYS detach unless (no linear FB) or (no native
  keyboard after phase B) or (registry query says a critical device is
  unclaimed). Surface stays attached iff the SAM answer from phase B is
  "driver needed and not yet written".
- The System window already surfaces `gDetachBlocked`; extend it to show
  WHICH device blocked detach (from the registry query) so a glance at any
  machine explains itself.
- Per-machine metal validation, in order: QEMU (all storage backends +
  usb-storage boot), ZimaBlade (USB boot + r8169), Yoga (NVMe + eth,
  already detaches today, regression check), X1 (trackpad fix + PS/2
  TrackPoint), Surface Laptop Go (eMMC/SDHCI + keyboard investigation).
  Capture `uno.devices()` output per machine into docs/ as the fleet
  hardware inventory.

## Standing rules for workers

- QEMU via `tools/qemu_test.py` (monitor socket, screendumps; no
  focus/SendKeys). Real FAT images via mkuefi, never vvfat for writes.
- Flasher redeploy is OPT-IN as of 2026-07-23 (network install via the URC
  `install <disk>` verb covers running boxes); build a USB stick only when
  the test itself needs one (e.g. the Phase A usb-storage boot tests).
- Process: follow /AGENTS.md (worktree + branch per slice, commit
  constantly, push the branch daily, land via the merge gate).
- ZimaBlade remote driving goes through the URC bridge on devbuntu
  (~/urc_bridge.py :5099, file-driven ~/urc/cmd.txt); the URC verb
  territory belongs to the unoautomate agent, file requests instead of
  editing its frozen core.
