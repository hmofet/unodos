/* ===========================================================================
 * UnoDOS/pc64 - Realtek PCIe WiFi driver (rtwifi.c).
 *
 * Modern Realtek PCIe WiFi, the rtw88/rtw89 families:
 *   rtw88 (WiFi 5, 802.11ac): RTL8822BE / RTL8822CE / RTL8821CE / RTL8723DE
 *   rtw89 (WiFi 6/6E/7):      RTL8852AE / RTL8852BE / RTL8852CE / RTL8922AE
 * Firmware-driven like the Intel part: the host loads a Realtek firmware blob
 * (user-supplied on the ESP under FIRMWARE\, Realtek's licence forbids us
 * bundling it), then drives an H2C/C2H command interface to scan, join and move
 * frames. WPA2-PSK via wifi_wpa.c; CCMP by the chip's security CAM.
 * Distinct from rtl8152.c (that is Realtek USB *Ethernet*). Publishes uno_nic_t;
 * inert when no supported card is on the PCI bus.
 * ======================================================================== */
#ifndef PC64_RTWIFI_H
#define PC64_RTWIFI_H
#include "uno_nic.h"

int  rtwifi_present(void);              /* a supported Realtek WiFi card on PCI? */
uno_nic_t *rtwifi_nic(void);            /* full bring-up; NULL on any failure     */
const unsigned char *rtwifi_mac(void);
void rtwifi_status_str(char *buf, int cap);

#endif
