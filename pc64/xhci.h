/* ===========================================================================
 * UnoDOS/pc64 - xHCI USB host-controller driver (see xhci.c).
 *
 * The USB core for the driver tail: brings up the xHCI controller QEMU exposes
 * as `-device qemu-xhci` and the one built into every modern PC, enumerates
 * attached devices, and drives control transfers. The foundation for a USB
 * Ethernet NIC (ASIX AX88179) so the X1 - which has no wired NIC - can network.
 *
 * Gated behind UNO_XHCI. When off it compiles to inert stubs, so the shipped
 * build is byte-identical (and there's no risk of fighting the firmware's own
 * USB while boot services are alive). Turn on with -DUNO_XHCI to test/develop.
 * ======================================================================== */
#ifndef PC64_XHCI_H
#define PC64_XHCI_H

/* Bring up the controller: find it on PCI, reset, set up the rings, run, and
 * reset the root-hub ports. Returns 1 if a controller was initialised. */
int  uno_xhci_init(void);

/* A discovered USB device (root-hub port), after enumeration. */
typedef struct {
    int  slot;                 /* xHCI slot id (0 = none) */
    int  port;                 /* 1-based root-hub port   */
    int  speed;                /* PORTSC speed field      */
    unsigned short vendor, product;
    unsigned char  dev_class, dev_subclass, dev_proto;
} uno_usb_dev;

int  uno_xhci_dev_count(void);
const uno_usb_dev *uno_xhci_dev(int i);

/* Diagnostics for the System app. */
void uno_xhci_status(int *present, int *nports, int *ndevs, unsigned *err);
/* enumeration debug: slot id (or -completion_code), Address Device completion,
 * descriptor result (1=ok, -1=none), and the port speed. */
void uno_xhci_diag(int *slot, int *addr_cc, int *desc, int *speed);
void uno_xhci_diag2(unsigned *usbsts, unsigned *ev0, int *disc); /* USBSTS, 1st event, disconnect count */

#endif
