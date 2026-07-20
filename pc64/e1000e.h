/* ===========================================================================
 * UnoDOS/pc64 - Intel e1000e Gigabit Ethernet (e1000e.c).
 *
 * The common Intel *client* 1GbE: the PCH-integrated LOM found on most laptops
 * and desktops since ~2010 - I219 (the big one), I218, I217, and the older
 * 82577/8/9. (The discrete 82540/82545/82574 are handled by e1000.c; the QEMU
 * `-device e1000e` model is the 82574L, so this family's datapath is the same
 * proven legacy ring path.) No firmware. Publishes the family nic service;
 * inert when no supported controller is on the PCI bus.
 * ======================================================================== */
#ifndef PC64_E1000E_H
#define PC64_E1000E_H
#include "uno_nic.h"

uno_nic_t *e1000e_nic(void);              /* bring up an I217/I218/I219 LOM; NULL if none */
const unsigned char *e1000e_mac(void);
int  e1000e_present(void);

#endif
