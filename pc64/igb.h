/* ===========================================================================
 * UnoDOS/pc64 - Intel igb Gigabit Ethernet (igb.c).
 *
 * The common Intel *discrete* PCIe 1GbE: I210 / I211 (desktops, embedded) and
 * the 82575/82576/82580 / I350 / I354 (server, multi-port). Unlike e1000/e1000e
 * these use ADVANCED rx/tx descriptors and per-queue ring registers. No
 * firmware. QEMU emulates this family as `-device igb` (82576). Publishes the
 * nic service; inert when no supported controller is present.
 * ======================================================================== */
#ifndef PC64_IGB_H
#define PC64_IGB_H
#include "uno_nic.h"

uno_nic_t *igb_nic(void);                 /* bring up an I210/I211/82576; NULL if none */
const unsigned char *igb_mac(void);
int  igb_present(void);

#endif
