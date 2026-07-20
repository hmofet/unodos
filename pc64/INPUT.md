# pc64 native input — taking keyboard & mouse from the firmware

Every x86 keyboard/mouse speaks one of three transports; pc64 has a native
driver for each, plus the firmware path as a universal fallback while attached.
The router lives in `uefi_main.c` (`poll_keyboard` / `poll_pointer`).

| Transport | Driver | Devices |
|---|---|---|
| **PS/2 (i8042)** | `pc64_native.c` (M3) | legacy desktops, older laptops |
| **I2C-HID (LPSS)** | `i2c_hid.c` | modern ultrabook / Surface keyboard + touchpad |
| **USB HID (xHCI)** | `usbhid.c` on `xhci.c` | external kbd/mice, docks, internal-USB input |

## The shared keyboard translator (`hid_kbd.c`)

USB and I2C-HID keyboards both deliver the same 8-byte boot report
(`[modifiers][reserved][keycode0..5]`, HID Usage-Table keyboard-page usages).
`hid_kbd_report()` diffs each report against the previous to emit **key-down
edges** as `(scan, uni, ctrl)` in the EFI SimpleTextIn space `map_key()`
already speaks — so both transports feed the existing key ring unchanged. It
tracks Caps Lock and maps arrows/esc/del to EFI scan codes.

## I2C-HID: keyboard + pointer (`i2c_hid.c`)

Self-configuring: scans every LPSS DesignWare I2C controller, probes the
slave-address × descriptor-register grid, validates each by the HID descriptor
signature, and **classifies** each device from its report descriptor into a
pointer slot (Generic-Desktop X/Y) or a keyboard slot (Keyboard/Keypad page).
The Surface has both on one controller at different addresses.

- **The 64-bit-BAR fix:** LPSS controllers on the Surface live in BARs above
  4 GB. The old `(volatile u8 *)(unsigned long)(uintptr_t)bar` truncated to 32
  bits on this LLP64 mingw target → wrong MMIO → `0 DW ctrl / N bars`. Cast
  straight through `uintptr_t` (pointer-width). Same wall the SAM battery hit.

## USB HID boot driver (`usbhid.c`)

On the `xhci.c` host stack: parses each device's config descriptor for HID boot
interfaces (class 3, protocol 1=keyboard / 2=mouse), sets BOOT protocol
(`SET_PROTOCOL 0`) + `SET_IDLE 0`, and arms the interrupt-IN endpoint. `xhci.c`
gained `uno_usb_setup_intr_in` / `uno_usb_intr_in` — a **non-blocking** poll that
keeps exactly one TRB outstanding (post → check-completion → re-post), so a
frame never stalls waiting for a keypress.

Because bringing up native xHCI takes the controller from the firmware, USB HID
is a **detached-mode** source in production (brought up in `try_detach` after
ExitBootServices). `-DUNO_USBHID_TEST` brings it up eagerly for QEMU.

## SCL timing — why the X1 Carbon's trackpad never bound

The DesignWare core counts SCL high/low in cycles of the **LPSS input clock**,
and that clock is a property of the SoC readable from no register: Bay Trail
100 MHz, Sunrise Point 120, Apollo Lake 133, Cannon/Comet/Ice/Tiger Lake 216.
The driver shipped one hardcoded HCNT/LCNT pair written for ~133 MHz. On a
Comet Lake X1 Carbon Gen 8 those same counts clock SCL at roughly **1.4 MHz** —
three and a half times the bus specification — so the Synaptics pad never
answers, which is indistinguishable from an empty bus. That is exactly the
`2 DW ctrl / 2 bars, HID device: not found` readout the machine reported.

`i2c_hid.c` now walks a small table of timing candidates until a device
answers. Entry 0 is the historical pair, kept first and unchanged, so machines
that already work (the Surface) take a bit-identical path; the computed
entries below it only ever run where the probe was failing anyway. The winning
index shows in the System window as `scl#N`.

Two smaller corrections alongside it: the Intel LPSS private block sits at
BAR+0x200 from Sunrise Point onward (the driver only released the older
BAR+0x800 alias, which on a modern part lands in the iDMA64 window), and a
`SET_POWER ON` now waits ~60 ms before `RESET`, which several I2C-HID parts
require.

## The router + detach gate

`poll_keyboard`: if a native keyboard is bound (I2C-HID or USB HID), drive from
it and **stop polling firmware ConIn** — on a touch device that's what keeps the
firmware on-screen keyboard drawn. Else PS/2 (detached) or firmware ConIn.
`poll_pointer`: native I2C-HID pointer (1:1, no smoothing) → native USB mouse →
firmware Absolute/Simple Pointer (now a lighter, target-weighted filter, since
the old 2:1-toward-previous read as "floaty"). The M3 detach gate generalized
from "requires i8042" to "requires **any** native keyboard that survives EBS".

**The gate now also protects the pointer.** It used to treat a native pointer
as "a bonus, not required", which is true until it isn't: `ExitBootServices`
sets `gNAbs = gNPtr = 0`, so a machine whose only working pointer came from
firmware SimplePointer loses it permanently at detach — keyboard fine, mouse
gone. `detach_would_strand_pointer()` refuses that specific trade: a firmware
pointer is live, nothing native would survive, **and** LPSS I2C controllers are
present (so it is a laptop whose pointer belongs on I2C). A PS/2-mouse desktop
has no LPSS controller and is unaffected. When the gate holds a machine
attached, the System window says so.

The System window now also reports what actually bound — firmware pointer
instance counts, PS/2 keyboard/aux state, the aux port's 0xA9 self-test result
and its 0xF2 device id. None of that was observable before, which is why a
dead trackpad on real hardware could not be diagnosed at all.

## Verifying

- QEMU: `harness.py unoapps` (keyboard nav opens every app); `-device usb-kbd`
  + `-DUNO_XHCI -DUNO_USBHID_TEST` logs `usbhid: claimed kbd=1 mouse=1`
  (enumeration + claim; key *routing* to the emulated usb-kbd is a QEMU limit,
  so real USB typing is metal-first). I2C-HID has no QEMU model → metal-only.
- Metal: see `METAL-CHECKLIST.md` (Surface trackpad/keyboard bind + float gone).
