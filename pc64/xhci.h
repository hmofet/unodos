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
int  uno_xhci_supported(void);   /* 1 = compiled with UNO_XHCI (no hw touch) */

/* A discovered USB device (root-hub port), after enumeration. */
typedef struct {
    int  slot;                 /* xHCI slot id (0 = none) */
    int  port;                 /* 1-based root-hub port   */
    int  speed;                /* PORTSC speed field      */
    unsigned short vendor, product;
    unsigned char  dev_class, dev_subclass, dev_proto;
} uno_usb_dev;

int  uno_xhci_dev_count(void);

/* unodevices phase 3: publish the enumerated devices and their interfaces
 * into the device tree under this controller, then bind. Idempotent and
 * destructive - existing USB children are dropped (remove() called on any
 * bound driver) before republishing, so nothing lingers BOUND for hardware
 * that has gone. No-op unless the controller came up. */
void uno_xhci_publish_tree(void);
const uno_usb_dev *uno_xhci_dev(int i);

/* USB transfer API for class drivers (dev = index into the enumerated list). */
int  uno_usb_control(int dev, unsigned char bmRequestType, unsigned char bRequest,
                     unsigned short wValue, unsigned short wIndex, void *data, int len);
int  uno_usb_get_config(int dev, void *buf, int len);      /* GET_DESCRIPTOR(config) */
int  uno_usb_set_config(int dev, int cfg);                 /* SET_CONFIGURATION */
/* Configure the device's bulk in/out endpoints (addresses from the config
 * descriptor, e.g. 0x81 / 0x02). Returns 0 on success. */
int  uno_usb_setup_bulk(int dev, int in_ep_addr, int out_ep_addr, int in_mps, int out_mps);
int  uno_usb_bulk_out(int dev, void *data, int len);       /* returns bytes sent / -1 */
int  uno_usb_bulk_in(int dev, void *data, int len);        /* returns bytes received / -1 */
/* Async bulk-IN (NIC recv): arm posts one transfer and returns immediately
 * (1 = armed, 0 = already armed, -1 = error); poll is NON-BLOCKING and returns
 * received bytes once it lands, 0 while outstanding, -1 on error/not armed.
 * The buffer passed to arm must stay valid until poll returns nonzero. */
int  uno_usb_bulk_in_arm(int dev, void *data, int len);
int  uno_usb_bulk_in_poll(int dev);
/* endpoint state: 1 Running, 2 Halted, 3 Stopped, 4 Error, -1 none */
int  uno_usb_bulk_in_epstate(int dev);

/* HID interrupt-IN endpoints. setup claims one, posts the first transfer and
 * returns a HANDLE (>=0) or -1; intr_in polls THAT endpoint, non-blocking,
 * returning the report length (0 = none ready yet, -1 err).
 *
 * A handle rather than a device index because one device can have several:
 * a wireless keyboard/mouse combo is a single device with two HID interfaces,
 * and keying on the device silently kept only the last one configured. */
int  uno_usb_setup_intr_in(int dev, int in_ep_addr, int mps);
int  uno_usb_intr_in(int handle, void *data, int maxlen);

/* Platform hook on every endpoint context, called after the spec-defined words
 * are filled and before Configure Endpoint. `epctx` points at the endpoint
 * context in the input context (DW0 at +0); `eptype` is the xHCI endpoint
 * type field (7 = Interrupt IN, etc.), `speed` the device's PORTSC speed id,
 * `has_tt` 1 when the device is reached through a hub. For controllers that
 * need vendor words in the context (MediaTek's scheduler fields). No hook by
 * default. Register before uno_xhci_init(). */
void uno_xhci_set_ep_quirk(void (*fn)(unsigned char *epctx, int eptype,
                                      int speed, int has_tt));

/* Diagnostics for the System app. */
void uno_xhci_status(int *present, int *nports, int *ndevs, unsigned *err);
/* enumeration debug: slot id (or -completion_code), Address Device completion,
 * descriptor result (1=ok, -1=none), and the port speed. */
void uno_xhci_diag(int *slot, int *addr_cc, int *desc, int *speed);
void uno_xhci_diag2(unsigned *usbsts, unsigned *ev0, int *disc);
/* last hub scan's per-port status words (index = hub port). Bit 31 is ours:
 * "connected and reset cleanly, but the device would not enumerate". */
void uno_xhci_hub_ports(unsigned *out, int max); /* USBSTS, 1st event, disconnect count */

/* Log every USB host controller on the machine (bdf, ids, prog-if, BAR0, PCI
 * command, power state) and mark the one the boot path names.
 *
 * CONFIG SPACE ONLY, so it is safe while the firmware still owns everything -
 * which is the point: on a machine that hangs after detaching, the pre-detach
 * telemetry is the copy that survives. Call it before try_detach(). */
void uno_xhci_inventory_log(void);

#endif
