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

## The router + detach gate

`poll_keyboard`: if a native keyboard is bound (I2C-HID or USB HID), drive from
it and **stop polling firmware ConIn** — on a touch device that's what keeps the
firmware on-screen keyboard drawn. Else PS/2 (detached) or firmware ConIn.
`poll_pointer`: native I2C-HID pointer (1:1, no smoothing) → native USB mouse →
firmware Absolute/Simple Pointer (now a lighter, target-weighted filter, since
the old 2:1-toward-previous read as "floaty"). The M3 detach gate generalized
from "requires i8042" to "requires **any** native keyboard that survives EBS".

## Verifying

- QEMU: `harness.py unoapps` (keyboard nav opens every app); `-device usb-kbd`
  + `-DUNO_XHCI -DUNO_USBHID_TEST` logs `usbhid: claimed kbd=1 mouse=1`
  (enumeration + claim; key *routing* to the emulated usb-kbd is a QEMU limit,
  so real USB typing is metal-first). I2C-HID has no QEMU model → metal-only.
- Metal: see `METAL-CHECKLIST.md` (Surface trackpad/keyboard bind + float gone).
