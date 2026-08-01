# USB, subsystem contract (pc64)

Owner of `xhci.*`, `usbio.*`, `usbhid.*`, `usbmsc.*`, `usbboot.*`.
Consumers: unofs (`uno_fat_native_eligible`), the detach path in `uefi_main.c`,
the USB NIC drivers (`ax88179`, `rtl8152`).

## The one rule: two transports, never at the same time

A USB device on pc64 is reachable two ways, and which one is legal depends
entirely on whether the firmware is still alive:

| | attached (boot services live) | detached (post-ExitBootServices) |
|---|---|---|
| transport | `usbio.c`, `EFI_USB_IO_PROTOCOL` | `xhci.c`, `uno_usb_*` |
| owns the controller | the firmware | us |
| what it can do | control + bulk on interfaces the firmware enumerated | anything |

`uno_xhci_init()` begins by calling `uno_pc64_pci_disconnect()`, which rips the
firmware's USB stack off the controller. While attached that stack is carrying
the USB boot volume and, on many laptops, the keyboard, so taking it is how
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
the far side if `usbmsc` can claim the stick, and that has to be decided
BEFORE the door closes, without taking the controller away from the firmware
we are still standing on.

`usbboot.c` answers it from descriptors and device paths only (no transfers, so
the firmware's live MSC driver is never disturbed):

1. the boot image's device path contains a `MESSAGING/USB` node, and
2. the build has the native stack (`uno_xhci_supported()`), and
3. the first `PCI()` node of that path is a class `0C`/`03`/**prog-if `30`**
   function, an xHCI. EHCI (prog-if `20`) is not something `usbmsc` can drive,
   so the prog-if check is not optional, and
4. an `EFI_USB_IO` interface of class `08` / subclass `06` / protocol `50`
   (SCSI transparent, Bulk-Only Transport) with both bulk endpoints, that
   either (a) has a device path the boot path extends, or (b) is the only such
   interface on the machine.

Two unmatched BOT candidates is a guess, and a guess costs the machine, so it
answers no. `uno_usbboot_target()` hands the winner's VID:PID to `usbmsc` so it
binds *that* device rather than whichever enumerates first, and the bound
device inherits `is_boot`, which the storage safety gate needs, because
`fw_scan()`'s own `is_boot` marking dies with the firmware.

The answer is computed once while attached and latched; every read after EBS is
the cached value.

## Hubs

`xhci.c` walks hubs. It has to: the ZimaBlade has a **single USB port**, so a
hub is not an accessory there, it is the only way to have a keyboard and a boot
stick at the same time, and until the driver landed, both were unreachable and
the machine could never detach.

xHCI addresses a device by **where it is**, not by walking to it. The slot
context carries the root-hub port the chain hangs off plus a 20-bit **Route
String** of 4-bit port numbers, one nibble per tier. So addressing a device
three hubs deep is the same command as one on a root port. The work is in the
hub class (USB 2.0 §11.24): read the hub descriptor for `bNbrPorts`, power the
ports and wait `bPwrOn2PwrGood`, then per connected port reset, wait for enable,
read the speed out of the port status, and enumerate.

Four things that are easy to miss and fatal to omit:

- **CONFIGURE THE HUB FIRST.** A hub does not power its downstream ports until
  it is in the Configured state, and in the Address state a class request to it
  may simply STALL. Skip `SET_CONFIGURATION` and every port behind the hub
  stays dark: no keyboard, no mouse, no boot volume. **QEMU's hub model powers
  up regardless and hides this completely** (three green runs on a driver that
  could never have worked on silicon), so treat a green QEMU hub result as
  necessary and nowhere near sufficient.
- **The controller must be told a slot is a hub.** Set the Hub bit and Number
  of Ports in its slot context (Configure Endpoint with only `A0`), or Address
  Device for anything downstream is rejected, the controller will not route
  through something it believes is an ordinary peripheral.
- **Transaction Translators.** A low- or full-speed device behind a
  high-speed hub is reached by split transactions, so the slot context needs TT
  Hub Slot ID and TT Port Number. Without them the device answers nothing.
  Devices deeper down inherit the TT of the hub that owns it.
- **Five tiers is the architectural limit**, the route string is five nibbles.
  Deeper than that cannot be addressed at all, so the preflights range-check
  depth (1 = root port … 6 = five hubs up) rather than trusting it.

The remaining limit both preflights still enforce: **anything not on an xHCI
function**. `usbmsc` and `usbhid` ride `xhci.c` alone, so an EHCI companion
(class `0C`/`03`, prog-if `20`) does not count.

## Input: the gate that could not be satisfied

`native_kbd_for_detach()` refuses to leave the firmware without a native
keyboard - the shell is keyboard-driven and firmware ConIn dies with EBS. But
`uno_usb_hid_kbd_present()` only becomes true once `xhci.c` owns the
controller, and `xhci.c` only takes the controller *after* ExitBootServices.
On a machine whose only keyboard is USB that is unsatisfiable: no detach until
the keyboard exists, no keyboard until the detach. **Every desktop with
USB-only input was permanently attached** - which the ZimaBlade demonstrated on
2026-07-29 (`detached: 0`, `ps2 kbd=0`, `i2c-hid present=0`, `usb-hid kbd=0`,
with a Logitech receiver plugged in).

`uno_usbboot_hid_kbd()` breaks the cycle the same way the boot volume does: a
HID interface with the **boot subclass** (`03`/`01`), protocol `01` for a
keyboard or `02` for a mouse, on a root-hub port of an xHCI controller, is
exactly what `uno_usb_hid_init()` claims at detach. Boot subclass matters
because the native driver speaks boot protocol and nothing else.

A debug boot log now prints the verdict, because the bound counts never could:
`usb-hid preflight: kbd=1 ptr=0 (USB boot keyboard on an xHCI root port)`.

## Boot HID reports on CHANGE: what is a level, and what is an edge

The single rule the poll path has to get right, and got wrong twice. A boot
keyboard and a boot mouse both report only when something changes, so a poll
that brings no report means *nothing changed* - never *everything is zero*.

| Field | Kind | On a poll with no report |
|---|---|---|
| mouse dx / dy, wheel | EDGE (deltas the caller consumes) | reset to 0 |
| mouse button mask | LEVEL | unchanged, still held |
| keyboard key presses | EDGE (diffed against the previous report) | nothing emitted |
| keyboard modifier byte | LEVEL | unchanged, still held |

`uno_usb_hid_mouse_poll()` used to start its button mask at 0 and only OR in
`rep[0]` when a report arrived, so a button held still read as released on the
very next frame: the shell saw press-release-press and **a drag with a USB
mouse committed before it started**. The mask is now latched per endpoint
(`g_eps[i].btn`) - per endpoint, not once per file, because the loop ORs
several mice together and a shared latch would let one mouse's report clear a
button held on another. `uno_ps2_mouse()` has always had this shape (`gMBtn`,
"latched button since the last call").

`uno_usb_hid_mods()` [STABLE, 2026-07-31] answers the same question for the
keyboard: the modifier byte of the last report, folded to unoui `UI_MOD_*` bits
(SHIFT 1, CTRL 2, ALT 4, GUI 8), left and right together. It is a read of what
`uno_usb_hid_kbd_poll()` already latched, so it consumes no report and can be
called at any rate. `hid_kbd_report()` records the byte on EVERY report,
including the rollover one it otherwise ignores - rollover means too many keys
are down, not that the modifiers were released. This is `uno_pc64_mods()`'s USB
source; without it Alt and GUI were dead on every USB keyboard.

Both are QEMU-gated: `harness.py usbhid_drag` (`-device usb-mouse`) and
`harness.py usbhid_mods` (`-device usb-kbd`), on an eager
`-DUNO_USBHID_TEST -DUNO_NO_DETACH -DUNO_DBGCON` build.

**Still one transfer in flight.** `uno_usb_intr_in()` keeps exactly one TRB
outstanding and re-posts on each successful read, so the controller fetches at
most one report per host poll. Queueing more is the change that killed input on
the ZimaBlade on 2026-07-30 (see the CORRECTION entry in
`UNOAUTOMATE-REQUESTS.md`); it was reverted, and it is not to be retried
without an xHCI error counter and queue depth readable over URC, plus a
recovery path for a head that never completes.

## Full speed needs its real EP0 max packet

`mps_for_speed()` can only guess for one speed. High speed EP0 is always 64 and
low speed always 8, but FULL speed may be 8, 16, 32 or 64 and is knowable only
by reading the descriptor. Guessing 8 works right up until a transfer needs a
second packet: an 18-byte device-descriptor fetch against a device that
packetises at 64 errors out, and a healthy device looks absent.

So `enumerate_dev()` does the spec's two-step for full speed: read the first 8
bytes (safe at MPS 8 for anything), take `bMaxPacketSize0` from offset 7, and
issue **Evaluate Context** to correct EP0 before any longer read.

This is why a hub full of devices can come up with only its high-speed member
present. On the ZimaBlade that was a boot stick enumerating while a keyboard
and two mice did not, all three on healthy ports.

## usbmsc: Bulk-Only Transport

`usbmsc.c` is CBW/CSW framing plus INQUIRY, TEST UNIT READY, READ CAPACITY(10),
READ(10), WRITE(10), REQUEST SENSE, the subset every flash stick implements.
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

## SuperSpeed, endpoint recovery, and deadlines (fixed 2026-07-29)

Three faults that together made a USB boot volume unreliable, and which
presented as one baffling symptom: a read that worked in a debug build and
timed out in a production build of the same source.

**Deadlines must be durations, not spin counts.** `poll_xfer`'s old
`5000000` was loop iterations, so the real wait depended on the optimiser -
the debug build accidentally granted several times the patience. A 512-byte
`READ(10)` needs the device to do actual I/O; production gave up before the
answer arrived, and the completed event was sitting in the ring moments later.
`poll_event_ms()` now waits in TSC-backed milliseconds via
`uno_pc64_delay_ms()`, NOT the local `mdelay()` calibrated spin. Control 2 s,
bulk 5 s, commands 1 s. **If you add a wait to this driver, use real time.**

**A transfer error halts the endpoint IN THE CONTROLLER.** `clear_halt()`'s
USB-level CLEAR_FEATURE is heard only by the device; the host controller keeps
the endpoint Halted with the ring cursor parked on the dead TRB, so every
later transfer fails too, one error, then the next CBW never goes out, then
nothing. `ep_recover()` runs the spec's sequence: **Reset Endpoint** (Halted →
Stopped), **Set TR Dequeue Pointer** to restart the ring, then drain the
abandoned transfer's event so the next caller is not handed a completion for a
transfer that no longer exists. After a *timeout* it uses **Stop Endpoint**
first, there the transfer may still be running, and resetting a running
endpoint is a context-state error.

**SuperSpeed needs Max Burst Size.** `ss_burst()` reads `bMaxBurst` from the SS
Endpoint Companion descriptor (type `0x30`, immediately after each endpoint
descriptor) and programs it into the endpoint context. Zero is right for
Full/High Speed bulk and wrong for SuperSpeed. It is done inside the driver
rather than through the `setup_bulk` signature because burst is a property of
the device, not of the class driver's intent, and the descriptor fetch runs
*before* any controller state is touched, since a control transfer half way
through building an input context is a transfer against a half-configured
endpoint.

Diagnosing the next one: on failure the driver prints `cc` and the endpoint
state. `cc=-1` with `epstate=1` (Running) means we gave up waiting and the
device had simply not answered; any other `cc` is a real error completion.

**METAL CONFIRMED 2026-07-30 (ZimaBlade):** detached, with the boot stick,
a Logitech unifying receiver and a Razer mouse all enumerated behind one hub
(`devs=5`), `usb-hid: kbd=3 mouse=2`, and keyboard and mouse both working. That
box has a single USB port, so a hub is mandatory there and every one of these
faults had to be fixed before it could detach at all.

**What QEMU could not test, three times running:** its hub powers ports
regardless of configuration state, it is single-TT, and its HID devices report
`bMaxPacketSize0 = 8`. So the hub SET_CONFIGURATION, the multi-TT programming
and the full-speed EP0 fix all went green in the emulator on code that could
not work on hardware. Treat a green QEMU USB result as necessary and nowhere
near sufficient.

**USB-boot detach is the DEFAULT as of 2026-07-30.** `DETACH.CFG: nousb`
keeps a machine attached, `off` disables detach entirely, and neither needs a
rebuild: drop the file on the stick from any other computer. The risk that
motivated the opt-in has not changed shape, only likelihood - the preflight
proves the stick's SHAPE, never that sustained reads will work, because proving
that means taking the controller, which is the irreversible step. There is no
way back past ExitBootServices.

Superseded note, kept for the reasoning: QEMU is green, 5 of 5 USB-storage boots detach with the
system volume intact, plus `install_test` (both phases) and `storage_test`
read/write, all post-detach. The remaining gate is metal: the ZimaBlade, a
desktop rather than a laptop, because past ExitBootServices there is no way
back if a real stick behaves differently from an emulated one.

## Stability

`[STABLE]`: `uno_xhci_init/supported/dev_count/dev`, the `uno_usb_*` transfer
API, `uno_usbio_*`, `uno_usbmsc_supported/init`, `uno_usbboot_*`.

Changelog:

- **2026-07-29**, `uno_xhci_init()` gained the attached-mode refusal and
  idempotence; `usbboot.*` added; `uno_usbio_iface()` / `uno_usbio_devpath()`
  added; `-DUNO_XHCI` now on by default.
- **2026-07-31**, `uno_usb_hid_mouse_poll()`'s button mask is now a LATCH (a
  held button survives a poll with no report) - a semantic change, not a
  signature one, so re-read "Boot HID reports on CHANGE" above.
  `uno_usb_hid_mods()` added; `hid_kbd_mods()` added alongside it in the shared
  translator.
