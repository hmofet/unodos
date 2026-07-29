# USB — subsystem contract (pc64)

Owner of `xhci.*`, `usbio.*`, `usbhid.*`, `usbmsc.*`, `usbboot.*`.
Consumers: unofs (`uno_fat_native_eligible`), the detach path in `uefi_main.c`,
the USB NIC drivers (`ax88179`, `rtl8152`).

## The one rule: two transports, never at the same time

A USB device on pc64 is reachable two ways, and which one is legal depends
entirely on whether the firmware is still alive:

| | attached (boot services live) | detached (post-ExitBootServices) |
|---|---|---|
| transport | `usbio.c` — `EFI_USB_IO_PROTOCOL` | `xhci.c` — `uno_usb_*` |
| owns the controller | the firmware | us |
| what it can do | control + bulk on interfaces the firmware enumerated | anything |

`uno_xhci_init()` begins by calling `uno_pc64_pci_disconnect()`, which rips the
firmware's USB stack off the controller. While attached that stack is carrying
the USB boot volume and, on many laptops, the keyboard — so taking it is how
you lose the machine you are standing on. **`uno_xhci_init()` therefore returns
0 unless `uno_pc64_detached()`.** It is also idempotent: three callers
(`main()`, `usbmsc`, `usbhid`) each ask for the controller, and only the first
brings it up.

`-DUNO_XHCI_EAGER` (implied by the older `-DUNO_USBHID_TEST`) opts a test build
out of that rule so QEMU can exercise the native path attached. Pair it with
`-DUNO_NO_DETACH`, and never ship it.

The mirror-image rule: `uno_pc64_pci_disconnect()` is a BOOT service. Calling
it after ExitBootServices jumps into freed memory (it announced itself as a #UD
at a stale RIP). It returns 0 when detached, and so does every `uno_usbio_*`
entry point.

## `-DUNO_XHCI` ships (2026-07-29)

The stack used to be compiled out by default, precisely because bringing it up
attached was unsafe. With the attached-mode guard above, the danger is gone and
the flag is on in `build.sh` for every build. That is what makes a USB-booted
machine able to detach at all (below), and what gives detached machines USB
keyboards and mice.

## usbboot: is the USB boot volume reclaimable?

Detach is a one-way door. On a USB-stick boot the system volume only exists on
the far side if `usbmsc` can claim the stick — and that has to be decided
BEFORE the door closes, without taking the controller away from the firmware
we are still standing on.

`usbboot.c` answers it from descriptors and device paths only (no transfers, so
the firmware's live MSC driver is never disturbed):

1. the boot image's device path contains a `MESSAGING/USB` node, and
2. the build has the native stack (`uno_xhci_supported()`), and
3. the first `PCI()` node of that path is a class `0C`/`03`/**prog-if `30`**
   function — an xHCI. EHCI (prog-if `20`) is not something `usbmsc` can drive,
   so the prog-if check is not optional, and
4. an `EFI_USB_IO` interface of class `08` / subclass `06` / protocol `50`
   (SCSI transparent, Bulk-Only Transport) with both bulk endpoints, that
   either (a) has a device path the boot path extends, or (b) is the only such
   interface on the machine.

Two unmatched BOT candidates is a guess, and a guess costs the machine, so it
answers no. `uno_usbboot_target()` hands the winner's VID:PID to `usbmsc` so it
binds *that* device rather than whichever enumerates first, and the bound
device inherits `is_boot` — which the storage safety gate needs, because
`fw_scan()`'s own `is_boot` marking dies with the firmware.

The answer is computed once while attached and latched; every read after EBS is
the cached value.

## usbmsc: Bulk-Only Transport

`usbmsc.c` is CBW/CSW framing plus INQUIRY, TEST UNIT READY, READ CAPACITY(10),
READ(10), WRITE(10), REQUEST SENSE — the subset every flash stick implements.
512-byte logical blocks only; anything else is rejected loudly. It registers
with `blkdev` as a native backend, so `uno_blk_detach()` picks it up alongside
AHCI/NVMe/SDHCI, and it binds the device `usbboot` identified rather than
whichever one enumerated first.

Three rules it learned the hard way, all of which used to fail silently:

- **DMA never targets the stack.** `xhci.c` puts the caller's pointer straight
  into the TRB, so a local array is a DMA into the firmware-provided stack at
  whatever address the build's frame layout produced. `g_cbw` / `g_csw` /
  `g_bounce` are static and 64-byte aligned, and the data phase bounces through
  `g_bounce` because callers pass FAT cache lines and sense arrays that are
  DMA-fit only by luck.
- **The CSW tag is checked.** BOT tags every command precisely so a
  desynchronised pipe is detectable; without the check a stale CSW makes the
  current command "succeed" while the caller reads the previous one's data.
- **A short data phase is not success.** Falling through to the CSW after a
  failed bulk-IN is how a read of nothing became 512 bytes of zeros that FAT
  then mounted as an empty volume.

`uno_usbmsc_why()` names the failure. Post-detach there is no console and no
volume to log to, so the string has to exist in production builds too.

## KNOWN GAP: SuperSpeed, and no endpoint error recovery

**USB-boot detach is opt-in (`DETACH.CFG`: `usb`) because of this.**

QEMU's `usb-storage` on `qemu-xhci` enumerates as SuperSpeed — bulk max packet
1024 — and some `READ(10)`s come back with a transfer error. `xhci.c` is
missing two things a SuperSpeed device needs:

1. **Endpoint companion descriptors** (type `0x30`). `bMaxBurst` has to reach
   the endpoint context's Max Burst Size; today it is left at 0.
2. **Error recovery.** After a transfer error the endpoint is halted in the
   CONTROLLER, and the spec's way out is `Reset Endpoint` followed by
   `Set TR Dequeue Pointer`. `usbmsc`'s `clear_halt()` sends the USB-level
   CLEAR_FEATURE, which the device hears and the host controller does not — so
   the ring stays wedged and every later transfer on that endpoint fails.

The symptom is distinctive: the first `READ(10)` works, a later one does not,
and two builds with identical logic behave differently. Fix both, confirm in
QEMU, confirm on the ZimaBlade (a desktop, not a laptop — if it goes wrong
there is no way back past ExitBootServices), then flip the opt-in.

## Stability

`[STABLE]`: `uno_xhci_init/supported/dev_count/dev`, the `uno_usb_*` transfer
API, `uno_usbio_*`, `uno_usbmsc_supported/init`, `uno_usbboot_*`.

Changelog:

- **2026-07-29** — `uno_xhci_init()` gained the attached-mode refusal and
  idempotence; `usbboot.*` added; `uno_usbio_iface()` / `uno_usbio_devpath()`
  added; `-DUNO_XHCI` now on by default.
