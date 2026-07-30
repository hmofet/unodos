# Can the Surface Laptop Go detach? The keyboard question, and what it would cost

Phase B item 3 of [DETACH-COMPLETION-PLAN.md](DETACH-COMPLETION-PLAN.md). The
plan asked for "a definitive answer plus a one-page sizing if a SAM driver is
needed", and was explicit that a SAM keyboard driver is "a separate, sized
decision for arin, not something to slip into this plan."

**This document does not contain the definitive answer, because that answer
requires booting the machine and nobody has.** What it does contain: why the
question is genuinely open rather than merely unresearched, the one boot that
settles it, and what each outcome costs. Writing it the other way round - an
assertion dressed up from code comments - is how the parked-PHY theory got two
rounds of work before its own telemetry refuted it.

## The state of the evidence

The detach gate refuses a machine with no native keyboard, because the shell is
keyboard-driven and firmware ConIn dies with `ExitBootServices`. On the Surface
that gate is the whole question: it has an eMMC system volume (SDHCI, native
driver exists) and a linear framebuffer, so storage and display are settled.

Two readings of the keyboard, and they disagree:

**For I2C-HID.** `i2c_hid.c` is written as though the Surface is a solved case.
Its bring-up comment names "a laptop whose keyboard AND touchpad are both
I2C-HID (the Surface)" as the reason it keeps separate pointer and keyboard
slots and keeps scanning until both fill, and the scan-grid comment notes "the
Surface has them on one controller at different addresses". If that is right,
`uno_i2c_hid_kbd_present()` already satisfies the gate and there is nothing to
build.

**For SAM.** The detach plan's own reading is that the internal keyboard rides
the Surface Aggregator Module, the same subsystem we already talk to for the
battery gauge. Modern Surface hardware does move internal HID onto SAM, and the
64-bit-BAR bug that hid the Surface's LPSS controllers was found *because* the
SAM battery path had hit the same wall - so SAM is demonstrably present and
carrying something on that machine.

Neither reading is backed by a boot log from the machine. The `i2c_hid.c`
comments predate the metal run that would confirm them, and
[pc64/METAL-CHECKLIST.md](../pc64/METAL-CHECKLIST.md) still carries "Keyboard
binds" as an unchecked item with a question mark over exactly this. So: open.

## The one boot that settles it

A `UNO_DEBUG=1` boot on the Surface, then read three lines out of the env block
in `CRASH\<machine>\BOOTLOG.TXT`. Two of them already existed; the third is new
with this phase and exists for this question:

```
i2c-hid: ctrls=N present=N addr=0xNN desc_parsed=N acpi_hits=N
usb-hid: kbd=N mouse=N
detach gate: fw ptr=... kbd=...  survives ptr=N kbd=N  blocker=...
```

The decision table:

| `i2c-hid present` | `detach gate kbd=` | Verdict |
|---|---|---|
| a keyboard bound | anything | **I2C-HID. Nothing to build**, the gate already passes |
| nothing bound | `usb` | a USB HID keyboard; usbboot's preflight covers it, nothing to build |
| nothing bound | `other` | not the i8042, not USB, and I2C-HID could not claim it. **SAM is the leading candidate**, size it below |
| nothing bound | `ps2` | the i8042 after all, `uno_ps2_init()` covers it |

`detach gate kbd=` is `uno_dg_fw_kbd_transport()`, which classifies the
firmware keyboard by walking its device path: `PNP03xx` means the i8042, a
`USB()` node means USB, anything else readable is `other`. It is the piece that
turns "the keyboard did not bind" into "the keyboard is not on any bus we
drive", which is the distinction the whole question turns on. See
[pc64/DETACH.md](../pc64/DETACH.md) §2.

If the answer is `other`, the follow-up that names the part is a devices dump
(`uno.devices()`, or the URC `devices` verb) plus the ACPI namespace: a SAM
keyboard appears under the Surface Serial Hub, not on any LPSS I2C controller.

## Sizing, IF the answer is SAM

Only relevant in one of the four rows above. Roughly, in dependency order:

1. **The serial hub transport.** SAM speaks a framed protocol over a UART
   (`SSH`: SYN, frame header, CRC16, per-command sequence numbers and ACKs).
   We have no UART transport in pc64 for this - `unoauto_serial.c` is a polled
   16550 for the URC channel and is debug-only, so this is a production driver
   over the same hardware, not a reuse. Call it the bulk of the work.
2. **Request/response plumbing.** Target/instance addressing, retries, and an
   event path, because keyboard input arrives as unsolicited SAM events rather
   than as replies.
3. **The HID layer on top.** SAM presents HID descriptors through its own
   commands; once they are readable, the existing report parser in `i2c_hid.c`
   does the interpretation, so this part is a transport shim rather than new
   HID work.
4. **Bring-up ordering.** It has to be alive *before* `try_detach()` runs, since
   the gate needs it to answer, and it must survive EBS - meaning no firmware
   protocols anywhere in it.

The honest risk note: every step is metal-only. QEMU models no part of this,
and the USB detach work of 2026-07-29 spent three consecutive green QEMU runs
on a driver that could not work on silicon. A SAM driver would have no
emulation safety net at all, on a machine that must be physically present to
test, to make one laptop detach.

**Which is why the recommendation, if it comes to that, is to leave the Surface
attached.** The gate already handles this correctly and says so: an attached
machine routes every service through the firmware exactly as before and loses
nothing except the things detaching would have bought. It is a supported
posture, not a failure. Phase D's default is "always detach unless a critical
device would be lost", and a machine whose keyboard we cannot claim is exactly
the case that clause exists for.
