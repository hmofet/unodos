/* Intel e1000 NIC driver (e1000.c) - publishes a uno_nic_t. */
#ifndef PC64_E1000_H
#define PC64_E1000_H
#include "uno_nic.h"
uno_nic_t          *e1000_nic(void);     /* bring up the card; NULL if none */
const unsigned char *e1000_mac(void);    /* 6-byte MAC (valid after e1000_nic) */
int                 e1000_present(void);
#endif
