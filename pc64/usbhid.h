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

/* poll all USB keyboards: emit(scan, uni, ctrl, ctx) per newly-pressed key
 * (EFI SimpleTextIn scan codes + unicode, same space as the firmware path). */
typedef void (*uno_usb_key_fn)(int scan, int uni, int ctrl, void *ctx);
int  uno_usb_hid_kbd_poll(uno_usb_key_fn emit, void *ctx);

/* poll all USB mice: accumulate relative dx/dy + button mask since last call.
 * Returns 1 if a mouse is present (dx/dy/btn written), 0 if none. */
int  uno_usb_hid_mouse_poll(int *dx, int *dy, int *btn);

int  uno_usb_hid_present(void);       /* any HID endpoint claimed        */
int  uno_usb_hid_kbd_present(void);   /* at least one keyboard           */
void uno_usb_hid_status(int *nkbd, int *nmouse);

#endif
