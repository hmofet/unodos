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

/* Reserve the card's DMA arena while boot services are still alive.
 *
 * Must be called BEFORE ExitBootServices on any machine that may detach: the
 * fallback arena is .bss, and on a firmware that loads the kernel above 4 GB
 * (the Surface Laptop Go, image_base 0x140000000) the device cannot reach it -
 * the firmware goes ALIVE and then every scan returns zero receive buffers.
 * No-op when the card is absent, and idempotent. */
void uno_iwl_reserve(void);
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
/* The same scan in slices, for a single-threaded UI that must keep taking
 * input while it runs (iwl_scan_aps() blocks for a five-second dwell).
 *   iwl_scan_begin()      0, or <0 if the radio will not come up
 *   iwl_scan_step(ms)     pump for up to ms; 1 = finished, 0 = still scanning
 *   iwl_scan_results()    fold the BSS table, exactly as iwl_scan_aps() does
 *   iwl_scan_busy()       is a stepped scan in flight */
int iwl_scan_begin(void);
int iwl_scan_step(int slice_ms);
int iwl_scan_results(iwl_ap_t *out, int max);
int iwl_scan_busy(void);

/* Join an SSID with a WPA2-PSK passphrase (overriding WIFI.CFG for this boot).
 * Blocks for the scan + association + 4-way handshake (~10 s). 0 = joined.
 * A join that SUCCEEDS is remembered (see below). */
int iwl_join_ssid(const char *ssid, const char *psk);

/* ---- progress reporting ----------------------------------------------------
 * A scan is a 5-second dwell and a join is an association plus a 4-way
 * handshake: the driver blocks its caller for seconds at a time with no way for
 * the UI to repaint, so a joining machine and a hung one look identical. That
 * is not a theoretical complaint - "it looks like it's frozen" is what a real
 * session reported.
 *
 * This is the driver saying where it is up to. `what` names the current phase
 * in words a user can read; `step`/`steps` place it in the sequence. It is
 * called on ENTERING each phase and then repeatedly (a few times a second)
 * while that phase waits, so a caller can animate on it as well as label it.
 *
 * Called on the CALLER'S stack, inside the blocking call - so a UI hook may
 * repaint and present, but must not re-enter the driver. NULL disables. */
typedef void (*iwl_progress_fn)(void *ctx, const char *what, int step, int steps);
void iwl_progress_set(iwl_progress_fn fn, void *ctx);

/* ---- remembered networks -------------------------------------------------
 * Every successful join is written to WIFINETS.CFG on a persistent volume,
 * most recent first, and entry 0 is what the next boot rejoins - so a machine
 * with no wired port comes back onto the network it was last on without anyone
 * retyping a passphrase.  A hand-staged WIFI.CFG still works and is the
 * fallback when the remembered network will not join.
 *
 * The passphrases are stored in PLAINTEXT, the same as WIFI.CFG has always
 * been: WPA2 needs the passphrase back to derive the PMK, and there is no
 * user-keyed store on this OS to wrap it with.  iwlwifi.c's section 10b has
 * the full argument. */
int  iwl_saved_count(void);                       /* 0 if nothing remembered   */
int  iwl_saved_get(int i, char *ssid, int cap);   /* 0 = last joined; 0 = ok   */
int  iwl_saved_psk(const char *ssid, char *psk, int cap); /* 1 if remembered   */
void iwl_saved_forget(const char *ssid);          /* drop it from the store    */
#ifdef UNO_DEBUG
/* Round-trip the store through the real filesystem (SPECTEST spec:wifistore).
 * 0 = ok, -1 = skipped because the store is full, else the failing step. */
int  iwl_saved_selftest(void);
#endif

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
