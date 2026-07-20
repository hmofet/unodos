/* ===========================================================================
 * UnoDOS/pc64 - Marvell / NXP PCIe WiFi driver (mrvlwifi.c).
 *
 * Marvell (now NXP) PCIe WiFi, the mwifiex family: 88W8897 / 88W8997 and the
 * newer IW-series parts that share the command ABI. Firmware-driven: the host
 * downloads a Marvell firmware image (user-supplied on the ESP under FIRMWARE\;
 * Marvell/NXP's licence forbids us bundling it), then talks a HostCmd/event
 * interface over PCIe rings to scan, associate and move frames. WPA2-PSK via
 * wifi_wpa.c; CCMP by hardware once the key material is installed. Publishes
 * uno_nic_t; inert when no supported card is on the PCI bus.
 * ======================================================================== */
#ifndef PC64_MRVLWIFI_H
#define PC64_MRVLWIFI_H
#include "uno_nic.h"

int  mrvlwifi_present(void);            /* a supported Marvell/NXP WiFi card on PCI? */
uno_nic_t *mrvlwifi_nic(void);          /* full bring-up; NULL on any failure         */
const unsigned char *mrvlwifi_mac(void);
void mrvlwifi_status_str(char *buf, int cap);

#endif
