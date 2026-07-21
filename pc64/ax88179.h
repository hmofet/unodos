/* ASIX AX88179 / AX88179A USB 3.0 Gigabit Ethernet driver (see ax88179.c).
 * Publishes a uno_nic_t, so the existing TCP/IP + TLS/HTTPS stack runs over
 * it on machines with no wired NIC.
 *
 * Two transports: EFI UsbIo while firmware-attached (always available), the
 * native xHCI stack (uno_usb_*, gated on -DUNO_XHCI) once detached.
 * ax88179_nic() returns NULL if no adapter. */
#ifndef PC64_AX88179_H
#define PC64_AX88179_H
#include "uno_nic.h"

uno_nic_t *ax88179_nic(void);          /* bring up an attached AX88179; NULL if none */
const unsigned char *ax88179_mac(void);/* 6-byte MAC (valid after ax88179_nic) */
int ax88179_present(unsigned short *vid, unsigned short *pid); /* probe only, no bind */

/* diagnostics for the System app */
void ax88179_status(int *found, int *bound, int *link, unsigned short *vid, unsigned short *pid);

#endif
