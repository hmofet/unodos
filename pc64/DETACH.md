# detachgate, the contract

The pre-ExitBootServices decision: can this machine run on its own drivers, and
if not, which device is stopping it? Owner: the `detach gate` row in
[/AGENTS.md](../AGENTS.md). Surface: [`detachgate.h`](detachgate.h),
`UNO_DETACHGATE_API 1`, `[STABLE]`.

Programme context is [docs/DETACH-COMPLETION-PLAN.md](../docs/DETACH-COMPLETION-PLAN.md);
this file is the contract, not the plan.

## 1. The problem the gate exists to solve

Detach is one-way. `ExitBootServices` kills every firmware protocol at once,
and a service that turns out not to survive cannot be recovered, only reported.
So every question here is asked about a world that does not exist yet, while
the firmware still owns the hardware that would answer it.

That rules out the obvious method. We cannot self-test the i8042 aux port to
see whether a PS/2 mouse is there, because the firmware is driving that
controller. We cannot ask xHCI whether a USB keyboard enumerates, because
`xhci.c` refuses the controller until the firmware is gone. Interrogating live
hardware out from under the firmware's driver is how you wedge a machine.

The method that does work is the one `usbboot.c` established for the boot
volume: **ask the firmware's own descriptors what our native stack will be able
to claim.** Descriptors are inert bytes. Reading a device path touches no
hardware, cannot disturb a firmware driver, and is valid at any point before
EBS. The answer is latched while boot services live and read back afterwards.

## 2. Transports

`uno_dg_fw_ptr_transport()` / `uno_dg_fw_kbd_transport()` classify what is
behind the firmware's pointer and keyboard by walking each publishing handle's
`EFI_DEVICE_PATH`:

| Value | Means | Recognised by |
|---|---|---|
| `UNO_DGT_ABSENT` | the firmware publishes no such device | no handles |
| `UNO_DGT_PS2` | the i8042 | an ACPI node with an EISA-packed `PNP03xx` (keyboard) or `PNP0Fxx` (pointer) `_HID` |
| `UNO_DGT_USB` | a USB device | a `USB()` messaging node |
| `UNO_DGT_OTHER` | readable, neither of the above (I2C, vendor paths) | anything else |
| `UNO_DGT_UNKNOWN` | no readable device path | a firmware aggregate |

Two details are load-bearing:

- **The PNP _family_ is matched, not a list of ids.** `PNP0303`, `PNP0301` and
  `PNP030B` are all i8042 keyboard ports; `PNP0F03`, `PNP0F13`, `PNP0F12` and
  `PNP0F0E` are all i8042 mouse ports. Real firmware uses the whole spread, so
  the test is `PNP03xx` / `PNP0Fxx`.
- **The ConIn splitter handle is skipped.** It aggregates the device instances
  and has no device path of its own; counting it turns every machine into
  `UNO_DGT_UNKNOWN`. (`collect()` in `uefi_main.c` skips it for the same
  reason, in its case because polling it double-applies every movement.)

## 3. The two survival predicates

`uno_dg_kbd_survives()` is true when any of these holds: the i8042 answers, an
I2C-HID keyboard is bound, a USB HID keyboard is bound, or usbboot's preflight
says a USB boot keyboard is reachable on the xHCI.

That last arm is not a nicety. Without it a USB-only machine can never detach:
the keyboard cannot exist until the firmware is gone, and we will not leave the
firmware until the keyboard exists. Every desktop with USB-only input sat on
the wrong side of that circle, the ZimaBlade included.

`uno_dg_ptr_survives()` is true when an I2C-HID pointer is bound, a USB HID
pointer is bound, usbboot predicts a reachable USB boot mouse, **or the
firmware's pointer is on the i8042** and the controller answers.

The last arm is the ThinkPad X1 Carbon, and it is what Phase B was for. That
machine's touchpad is I2C-HID and may fail to bind; its TrackPoint is a PS/2
Elan on the aux port, arriving through firmware SimplePointer, which
`uno_ps2_init()` claims the moment the firmware lets go. The old gate could not
see the TrackPoint at all: it counted LPSS I2C controllers as a proxy for
"modern laptop whose pointer lives on I2C" and refused. The device path settles
it outright, because the firmware itself declares that pointer a `PNP0Fxx`
device.

## 3a. The transport is not the authority on whether a pointer EXISTS

This is the subtlest thing in the file and it was wrong in the first version,
so it is worth stating flatly: **`uno_dg_fw_ptr_transport()` cannot tell you
whether the machine has a pointer.**

Some firmware publishes its pointer instances only through the ConIn
*splitter*, an aggregate handle with no device path of its own. There is then
nothing to classify, even though a pointer is live and moving the cursor.
`collect()` in `uefi_main.c` already knows this and falls back to the splitter
when no device handles exist, which is why its `gNPtr` / `gNAbs` counts are the
authority and the classifier is not.

The first version asked the classifier, and QEMU's own boot log caught it
within one run:

```
pointer: fw_simple=1 fw_abs=1        <- a pointer exists
detach gate: fw ptr=none             <- ...and the gate could not see it
```

"Nothing to lose" on a machine that is about to lose something is the dangerous
direction to be wrong in, and it fails silently. So the transport REFINES the
question rather than answering it, and `uno_dg_would_strand_pointer()` reads:

1. no firmware pointer at all (`!gNPtr && !gNAbs`) - nothing to lose, detach
2. something native survives - detach (this is the phase B win)
3. transport known, nothing native - the pointer really does die, refuse
4. transport UNKNOWN (splitter-only) - we cannot tell, so fall back to the
   pre-phase-B LPSS-counting heuristic

Case 4 is why that heuristic is still in the tree rather than deleted outright,
and it is the fallback the plan asked to keep. On a machine with I2C
controllers and no bound I2C-HID pointer, the thing moving the cursor is most
likely a firmware protocol EBS will kill; a PS/2-mouse desktop has no LPSS
controller and is unaffected. Every QEMU guest lands in case 4 with `nctrl == 0`
and detaches, exactly as it did before phase B.

The status string prints both halves (`fw inst=1/1 ptr=unknown`) for this
reason: seen alone, either number looks like a contradiction.

## 4. What this gate does NOT do, and the seam that will change

The registry query in Phase B item 2 of the plan reads "detach iff every device
providing a critical service has a native driver BOUND or committed-at-detach".
Half of that is here and half is not, and the split is deliberate:

- **"which device"** is answered from the unodevices registry, through a weak
  seam (`devmgr_count` / `devmgr_info` / `devmgr_class_name`, declared locally
  with weak fallbacks so this file links in builds that do not compile
  `uno_devmgr.c`). That is `uno_dg_dev_str()` and the blocker string.
- **"has a native driver"** is answered by asking the service owners
  (`uno_ps2_present`, `uno_i2c_hid_present`, `uno_usb_hid_present`,
  `uno_usbboot_hid_*`), NOT by reading a bind state out of the registry.

The reason is that the driver registry is **unodevices phase 2, and it is not
on master**. `uno_devmgr.c` enumerates and introspects; nothing binds, and
every real device reports `UNCLAIMED`. A gate keyed to registry bind state
today would refuse every machine in the fleet.

When phase 2 lands and drivers adopt `UNO_DRIVER()`, those four predicates
collapse into one registry lookup and this section goes with them. The shape of
the gate does not change, so that is a substitution inside `detachgate.c` and
not a contract break. Filing it as a request against the unodevices lane rather
than building it here is the AGENTS.md §1 rule, not timidity.

## 5. Honesty on the far side of the door

Every predicate above is inference. `uno_dg_ptr_arrived()` makes the
authoritative check where it is finally possible, after EBS, and names the
failure if a promised pointer did not turn up.

It reports rather than strands. A missing boot volume is fatal and
`try_detach()` treats it that way; a missing pointer is not, because the shell
is keyboard-driven throughout. But it must not be silent: a machine that
quietly loses its mouse is indistinguishable from a machine with a broken
driver, and that ambiguity costs a diagnosis cycle every time.

## 6. Blocker attribution

`uno_dg_set_blocker()` / `uno_dg_blocker()` carry a short device string
(`"00:15.0 8086:34e8 i2c"`) set by whichever gate refused. `try_detach()`
blames the bus the firmware is reading the lost device from: the xHCI
(`0C/03`) when the transport is USB, an LPSS I2C controller (`0C/80`)
otherwise. The System window and the debug env block read it.

The string is empty when the device manager is not in the build or the device
is not in the tree; callers must render an empty blocker as "no device named",
never as a device called "".

## Changelog

- **2026-07-30, `UNO_DETACHGATE_API 1`.** First release. Phase B of the detach
  completion plan: the keyboard and pointer gates move out of `uefi_main.c`,
  device-path classification replaces the LPSS-counting pointer heuristic as
  the PRIMARY test (it survives as the case-4 fallback, see §3a), and refusals
  name a device (phase D).
