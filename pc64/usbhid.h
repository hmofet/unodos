/* ===========================================================================
 * UnoDOS/pc64 - USB HID boot-protocol driver (keyboard + mouse) on xHCI.
 *
 * Sits on the xhci.c host stack (control + interrupt-IN transfers): finds HID
 * boot-protocol interfaces (class 3, subclass 1, protocol 1=keyboard / 2=mouse)
 * on the enumerated devices, puts them in BOOT protocol, and polls their
 * interrupt-IN endpoints for the fixed boot reports. Keyboard reports go
 * through the shared hid_kbd translator; mouse reports become relative deltas.
 *
 * Gated behind UNO_XHCI (inert stubs otherwise). Because bringing up native
 * xHCI takes the controller from the firmware, this is a DETACHED-mode input
 * source in production (brought up after ExitBootServices) - with an eager
 * test hook (UNO_USBHID_TEST) so QEMU can exercise it with -device usb-kbd.
 * ======================================================================== */
#ifndef PC64_USBHID_H
#define PC64_USBHID_H

/* Bring up xHCI (if needed) + configure every HID boot kbd/mouse found.
 * Returns the number of HID endpoints claimed. Safe/idempotent. */
int  uno_usb_hid_init(void);

/* poll all USB keyboards: emit(scan, uni, ctrl, mods, ctx) per newly-pressed
 * key (EFI SimpleTextIn scan codes + unicode, same space as the firmware
 * path). `mods` is the UI_MOD_* mask held when that key went down - see
 * hid_kbd.h; `ctrl` is the same bit on its own, kept for callers that only
 * ever wanted Ctrl. */
typedef void (*uno_usb_key_fn)(int scan, int uni, int ctrl, int mods, void *ctx);
int  uno_usb_hid_kbd_poll(uno_usb_key_fn emit, void *ctx);

/* poll all USB mice: dx/dy accumulated since the last call, plus the LATCHED
 * button mask (bit0 left, bit1 right, bit2 middle) - a boot mouse reports only
 * on change, so the mask is held state and survives frames with no report,
 * exactly like uno_ps2_mouse(). Deltas are edges and reset every call.
 * Returns 1 if a mouse is present (dx/dy/btn written), 0 if none. */
int  uno_usb_hid_mouse_poll(int *dx, int *dy, int *btn);

/* wheel notches accumulated by the polls above, cleared on read (+ = down) */
int  uno_usb_hid_wheel(void);

/* Modifiers HELD NOW on the claimed USB keyboards, as unoui UI_MOD_* bits
 * (SHIFT 1, CTRL 2, ALT 4, GUI 8), left and right folded. Read-only: it
 * reports what uno_usb_hid_kbd_poll() has already latched, so calling it does
 * not consume a report. This is the USB source for uno_pc64_mods(); without it
 * Alt and GUI are dead on every USB keyboard. */
int  uno_usb_hid_mods(void);
int  uno_usb_hid_keys_held(void);   /* UNO_KH_* bits (hid_kbd.h) */

int  uno_usb_hid_present(void);       /* any HID endpoint claimed        */
int  uno_usb_hid_kbd_present(void);   /* at least one keyboard           */
void uno_usb_hid_status(int *nkbd, int *nmouse);

#endif
