/* ===========================================================================
 * UnoDOS/pc64 - WPA2-PSK supplicant (wifi_wpa.c).
 *
 * The software half of WiFi security: derives the PMK from the passphrase
 * (PBKDF2-SHA1), runs the EAPOL-Key 4-way handshake (PRF-SHA1 PTK derivation,
 * HMAC-SHA1 MICs, AES key-unwrap for the GTK) and hands the resulting
 * pairwise/group keys to the driver, which installs them in the NIC firmware
 * for hardware CCMP. Pure computation over BearSSL primitives - no hardware,
 * no I/O - so it is testable on the host and shared by any WiFi driver.
 * WPA2-CCMP only (no WPA1/TKIP, no WPA3) by design.
 * ======================================================================== */
#ifndef WIFI_WPA_H
#define WIFI_WPA_H

/* PMK from an SSID + passphrase (IEEE 802.11i: PBKDF2-SHA1, 4096 rounds). */
void wpa_pmk_from_psk(const char *ssid, int ssid_len,
                      const char *passphrase, unsigned char pmk[32]);

/* The RSN information element we advertise (WPA2-PSK, CCMP pairwise+group).
 * Writes elem-id + len + body into out (>= 22 bytes); returns total length. */
int  wpa_build_rsn_ie(unsigned char *out);

/* Does a beacon/probe RSN IE (pointing at the element id byte) offer
 * CCMP pairwise + PSK AKM?  1 = yes, this supplicant can join. */
int  wpa_rsn_ie_ok(const unsigned char *ie, int len);

/* ---- 4-way handshake state machine -------------------------------------- */
typedef struct {
    unsigned char pmk[32];
    unsigned char aa[6], spa[6];          /* authenticator (AP) / supplicant   */
    unsigned char anonce[32], snonce[32];
    unsigned char ptk[48];                /* KCK[16] | KEK[16] | TK[16]        */
    unsigned char gtk[32];  int gtk_len;  /* group key from msg 3 (or rekey)   */
    int  gtk_idx;                         /* group key index (1..3)            */
    unsigned char rsn_ie[24]; int rsn_ie_len;   /* IE echoed in msg 2          */
    unsigned char replay[8];              /* highest replay counter seen       */
    int  state;                           /* WPA_ST_* below                    */
} wpa_sm_t;

enum { WPA_ST_IDLE = 0, WPA_ST_SENT_2, WPA_ST_DONE, WPA_ST_FAILED };

void wpa_sm_init(wpa_sm_t *sm, const unsigned char pmk[32],
                 const unsigned char own_mac[6], const unsigned char ap_mac[6]);

/* Feed one received EAPOL frame (starting at the 802.1X version byte).
 * If a reply must be transmitted, it is written to `reply` (cap >= 512) and
 * its length returned.  0 = nothing to send; -1 = handshake failure.
 * After msg 3 the reply is msg 4 AND sm->state becomes WPA_ST_DONE: the
 * caller must TRANSMIT THE REPLY FIRST (plaintext), then install the keys
 * (sm->ptk+32 pairwise TK, sm->gtk group).  Handles group-key rekeys too. */
int  wpa_sm_rx_eapol(wpa_sm_t *sm, const unsigned char *frame, int len,
                     unsigned char *reply, int reply_cap);

#endif
