# Detach completion — finish removing firmware from the running system (pc64)

Status: PLANNED, run AFTER unodevices phases 1-2
(docs/UNODEVICES-PLAN.md). New drivers here are written as registry
drivers, and phase D's detach predicate is a registry query.

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

## Phase A — USB mass storage (the big one)

Un-blocks every USB-stick boot. The hard plumbing already exists in
`xhci.c`: control transfers, SET_CONFIGURATION, bulk rings
(`uno_usb_setup_bulk` / `uno_usb_bulk_in` / `uno_usb_bulk_out`, already
proven by the AX88179 NIC). The new driver is Bulk-Only Transport plus a
minimal SCSI set.

1. `pc64/usbmsc.c` — registry driver matching interface class 08 / subclass
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

## Phase B — input gates

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
3. **Surface Laptop Go keyboard — investigate before promising.** We skip
   USB/PS2 takeover on Microsoft SMBIOS today, and the internal keyboard
   most likely rides the Surface Aggregator Module (we already talk to SAM
   for battery). If it is not a USB HID device, the Surface cannot detach
   until someone writes a SAM keyboard driver; that is a separate, sized
   decision for arin, not something to slip into this plan. Deliverable
   here: a definitive answer (unodevices tree dump + descriptor walk on the
   Surface) plus a one-page sizing if a SAM driver is needed.

## Phase C — attached-mode firmware sweep

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

## Phase D — flip the posture + fleet checklist

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
- After meaningful pc64 builds: `pc64\flash\deploy-to-share.ps1`.
- Commit per step; do NOT push without asking (master carries unpushed
  work by policy).
- ZimaBlade remote driving goes through the URC bridge on devbuntu
  (~/urc_bridge.py :5099, file-driven ~/urc/cmd.txt); the URC verb
  territory belongs to the unoautomate agent, file requests instead of
  editing its frozen core.
