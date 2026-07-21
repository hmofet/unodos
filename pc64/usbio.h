/* ===========================================================================
 * UnoDOS/pc64 - attached-mode USB transport over EFI_USB_IO_PROTOCOL.
 *
 * While the OS is firmware-attached, the xHCI controller belongs to the
 * firmware: taking it over (uno_xhci_init) would tear down the firmware's own
 * USB stack - including the mass-storage path to the USB boot stick, which is
 * the F8 "detach strands a USB boot" failure all over again, just earlier.
 * This transport drives USB devices through the firmware's per-interface
 * EFI_USB_IO handles instead: control + bulk transfers, no controller
 * ownership change, keyboard and boot volume stay alive.
 *
 * Device indices are positions in a snapshot list (uno_usbio_count refreshes).
 * All functions return -1 once boot services are gone (post-detach) - the
 * native uno_usb_* xHCI path is the transport for that world.
 * ======================================================================== */
#ifndef PC64_USBIO_H
#define PC64_USBIO_H

int uno_usbio_count(void);              /* enumerate; returns device count */
int uno_usbio_info(int i, unsigned short *vid, unsigned short *pid,
                   unsigned char *if_class, unsigned char *if_subclass);
int uno_usbio_control(int i, unsigned char bmRequestType, unsigned char bRequest,
                      unsigned short wValue, unsigned short wIndex,
                      void *data, int len);
/* First bulk IN/OUT endpoint addresses of the interface (0 if absent). */
int uno_usbio_bulk_eps(int i, int *in_ep, int *out_ep);
int uno_usbio_bulk_out(int i, int ep, const void *data, int len);
/* Returns bytes received, 0 on timeout (nothing arrived), -1 on error. */
int uno_usbio_bulk_in(int i, int ep, void *data, int cap, int timeout_ms);

#endif
