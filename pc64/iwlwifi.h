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

/* ---- runtime network selection (the Network app's join UI) ---------------
 * One entry per network found by a scan. `rssi` is dBm from the RX descriptor
 * (0 = unknown), `chan` the channel the scanner heard it on. */
typedef struct {
    char          ssid[33];
    unsigned char bssid[6];
    unsigned char chan;
    signed char   rssi;
} iwl_ap_t;

/* Scan and fill up to `max` entries, strongest first, one row per SSID.
 * Brings the card up first if needed; does NOT join. Returns the count. */
int iwl_scan_aps(iwl_ap_t *out, int max);

/* Join an SSID with a WPA2-PSK passphrase (overriding WIFI.CFG for this boot).
 * Blocks for the scan + association + 4-way handshake (~10 s). 0 = joined. */
int iwl_join_ssid(const char *ssid, const char *psk);

/* Interactive F12 debug entry point (for the unoautomate remote channel -
 * see the 2026-07-22 request in UNOAUTOMATE-REQUESTS.md). Parses ONE command
 * line, acts on the live card, writes a NUL-terminated reply into out:
 *   csr <hexoff>           read a CSR dword        -> "xxxxxxxx"
 *   csw <hexoff> <hexval>  write a CSR dword       -> "ok"
 *   prr <hexreg>           read a PRPH reg (grab)  -> "xxxxxxxx"
 *   prw <hexreg> <hexval>  write a PRPH reg (grab) -> "ok"
 *   rerun                  full bring-up retry     -> iwl_status_str
 *   status                 no side effects         -> iwl_status_str
 * Association + data path:
 *   scan / pick <n>        scan, then target the n-th AP by hand
 *   mvm <n> / mld <n>      the bring-up and link-API steppers
 *   auth / assoc / eapol   one step of the join each
 *   connect [ssid|psk]     the WHOLE join in one command (WIFI.CFG if omitted)
 *   data [a.b.c.d]         prove the encrypted data path (ARP + listen) WITHOUT
 *                          touching the IP stack, which is bound elsewhere
 *   netup / netwifi        the real DHCP/ping/DNS suite over WiFi; netwifi
 *                          leaves the stack on WiFi, netup hands it back
 *   netres                 the stashed result of the last netup (URC is down
 *                          while it runs, so its own reply reaches nobody)
 *   qos <0|1>              plain vs QoS data frames on TX
 *   band <24|5|any>        which band scan_pick() may choose from
 * Returns reply length, or -1 (unknown command / card not mapped). Debug
 * tooling only - never called by the production stack. */
int  iwl_dbg_cmd(const char *line, char *out, int cap);

#endif
