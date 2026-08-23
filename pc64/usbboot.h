/* ===========================================================================
 * UnoDOS/pc64 - "will the USB boot volume survive ExitBootServices?"
 *
 * Detach is a one-way door: past ExitBootServices the firmware's Block IO is
 * gone and there is no way back. On a USB-stick boot the system volume then
 * belongs to usbmsc.c or to nobody, so the decision has to be made BEFORE the
 * door closes, using only what can be learned without taking the controller
 * away from the firmware we are still standing on.
 *
 * That is what this file does: it walks the boot image's device path, matches
 * it against the firmware's own EFI_USB_IO handles, and answers whether the
 * device carrying the running system is a Bulk-Only-Transport mass-storage
 * interface sitting behind an xHCI controller - the exact shape usbmsc.c can
 * reclaim post-detach. Descriptor reads only: no transfers, no ownership
 * change, nothing that could disturb the firmware's live MSC driver.
 *
 * A "no" here is not a failure, it is the system declining to strand itself.
 * ======================================================================== */
#ifndef PC64_USBBOOT_H
#define PC64_USBBOOT_H

/* 1 if the image we booted rode in over USB (device path has a USB node). */
int uno_usbboot_is_usb(void);

/* 1 if the boot volume is USB *and* the native stack will reclaim it at
 * detach. 0 for every other case, including "we did not boot from USB" -
 * callers asking this question are asking about the USB path specifically.
 * Answer is computed once while attached and then cached. */
int uno_usbboot_native_ok(void);

/* Diagnostics for the System window and the boot log. *why is a static
 * string naming the reason for the current verdict, never NULL. */
void uno_usbboot_status(int *is_usb, int *nbot, int *matched, const char **why);

/* USB id of the device identified as the boot stick, so the native driver
 * binds THAT one rather than whichever mass-storage device enumerates first.
 * Returns 1 when an id is known (i.e. uno_usbboot_native_ok()). */
int uno_usbboot_target(unsigned short *vid, unsigned short *pid);

/* Will a USB HID boot keyboard / mouse exist once we own the controller?
 *
 * The detach gate cannot ask uno_usb_hid_kbd_present(): that only becomes true
 * after ExitBootServices, which is the step it is gating. These answer the same
 * question from the firmware's descriptors instead - a HID boot-subclass
 * interface on a root-hub port of an xHCI controller is precisely what
 * uno_usb_hid_init() claims at detach. Attached-only; latched like the rest. */
int uno_usbboot_hid_kbd(void);
int uno_usbboot_hid_ptr(void);
void uno_usbboot_hid_status(int *kbd, int *ptr, const char **why);

/* The PCI dev/fn the boot device path names (a device path carries no bus
 * number). Returns 1 when the boot path has a PCI node. Attached-only. */
int uno_usbboot_hc_loc(int *dev, int *fn);

/* One line naming what the post-detach USB takeover achieved: controller up,
 * ports, devices, HID endpoints, and how the boot stick's bind went. For the
 * SCREEN - on a machine with no serial and no volume to log to, that is the
 * only channel left. Returns the length snprintf would have written. */
int uno_usb_takeover_str(char *buf, int cap);

#endif
