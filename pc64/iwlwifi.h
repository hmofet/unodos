/* ===========================================================================
 * UnoDOS/pc64 - Intel WiFi (iwlwifi-class) driver (iwl_trans.c + iwl_mvm.c).
 *
 * A from-scratch driver for Intel's PCIe WiFi families:
 *   AC (WiFi 5): 7260 / 7265 / 3165 / 8260 / 8265 / 9260 / 9560 / 9462
 *   AX (WiFi 6): AX200 / AX201 (CNVi) / AX210 / AX211
 * These cards are firmware-driven: the host loads a (user-supplied) Intel
 * .ucode image, then talks a command protocol. The blobs are NOT in the repo -
 * copy them from linux-firmware onto the ESP root under 8.3 names (see
 * NETWORK.md for the exact file per card). WIFI.CFG on the ESP root holds
 * `ssid=` and `psk=` lines. WPA2-PSK via wifi_wpa.c; CCMP is done by the
 * card's hardware once the keys are installed.
 *
 * iwl_nic() does the whole bring-up (firmware, scan, join, 4-way handshake)
 * and publishes the family nic service; the stack above is unchanged.
 * ======================================================================== */
#ifndef PC64_IWLWIFI_H
#define PC64_IWLWIFI_H
#include "uno_nic.h"

int  iwl_present(void);                 /* a supported Intel WiFi device on PCI? */
uno_nic_t *iwl_nic(void);               /* full bring-up; NULL on any failure    */
const unsigned char *iwl_mac(void);

/* Human-readable state ("AX201, fw loaded, joined MyNet -52dBm" / the error
 * that stopped bring-up) for the Network app + diagnostics. */
void iwl_status_str(char *buf, int cap);

#endif
