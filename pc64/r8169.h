/* ===========================================================================
 * UnoDOS/pc64 - Realtek RTL8168/8111 Gigabit Ethernet (r8169.c).
 *
 * The single most common consumer GbE NIC: the Realtek RTL8168/8111 family on
 * the vast majority of desktop motherboards and many laptops, plus the RTL8101/
 * 8106 Fast Ethernet and the RTL8125 2.5GbE. Chip-version is read from the
 * TxConfig register; the mainstream RTL8168 datapath is a 16-byte descriptor
 * ring over MMIO. No firmware. Publishes the nic service; inert when absent.
 * (QEMU has no RTL8168 model - only the older rtl8139 - so this is metal-pending.)
 * ======================================================================== */
#ifndef PC64_R8169_H
#define PC64_R8169_H
#include "uno_nic.h"

uno_nic_t *r8169_nic(void);               /* bring up an RTL8168/8111/8125; NULL if none */
const unsigned char *r8169_mac(void);
int  r8169_present(void);

#endif
