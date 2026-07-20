/* ===========================================================================
 * UnoDOS/pc64 - Realtek RTL8152/RTL8153/RTL8156 USB Ethernet driver
 * (rtl8152.c).
 *
 * The other common USB NIC family besides ASIX: the chips inside most cheap
 * USB dongles, docks and monitor hubs (Lenovo/Dell/HP docks are almost all
 * RTL8153). Same shape as ax88179.c: vendor control transfers for registers,
 * bulk endpoints for frames, publishes uno_nic_t over the xHCI stack.
 * Inert unless -DUNO_XHCI (uno_usb_* stubs return -1).
 * ======================================================================== */
#ifndef PC64_RTL8152_H
#define PC64_RTL8152_H
#include "uno_nic.h"

/* find + bring up an RTL815x on the enumerated USB list; NULL if none */
uno_nic_t *rtl8152_nic(void);
const unsigned char *rtl8152_mac(void);

/* diagnostics for the Network app */
void rtl8152_status(int *found, int *bound, int *link,
                    unsigned short *vid, unsigned short *pid, int *version);

#endif
