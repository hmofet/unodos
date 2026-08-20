/* ===========================================================================
 * UnoDOS/pc64 - the WiFi supplicant (wifi_wpa.c): WPA2-PSK and WPA3-SAE.
 *
 * The software half of WiFi security.  Two ways in, one way out:
 *
 *   WPA2-PSK   passphrase -> PBKDF2-SHA1 -> PMK
 *   WPA3-SAE   passphrase -> the SAE exchange (wifi_sae.c) -> PMK
 *
 * and from the PMK both run the SAME EAPOL-Key 4-way handshake, which derives
 * the PTK, unwraps the group key and hands the pairwise/group keys to the
 * driver to install for hardware CCMP.  Pure computation over BearSSL
 * primitives - no hardware, no I/O - so it is testable on the host and shared
 * by any WiFi driver.
 *
 * What differs BELOW the 4-way, and why this file is AKM-aware rather than
 * hardcoded (802.11-2020 Table 12-10):
 *
 *   AKM 00-0F-AC-02 (PSK)  key descriptor version 2: PRF-SHA1 for the PTK,
 *                          HMAC-SHA1 for the EAPOL MIC.
 *   AKM 00-0F-AC-08 (SAE)  key descriptor version 0: KDF-HMAC-SHA256 for the
 *                          PTK, AES-128-CMAC for the EAPOL MIC.
 *
 * Getting that pairing wrong does not fail loudly - it fails as a bad MIC and
 * a deauthentication, indistinguishable from a wrong password.
 *
 * MANAGEMENT FRAME PROTECTION.  WPA3 requires PMF (802.11w), so the RSN
 * element we send has to carry MFPC, a group management cipher suite, and -
 * for an SAE-only AP - MFPR.  That is not cosmetic: an AP with MFPR set
 * REFUSES an association request whose RSN element lacks MFPC, which is the
 * proximate reason a WPA2-only supplicant is not merely downgraded on a
 * modern network but locked out of it entirely.
 * ======================================================================== */
#ifndef WIFI_WPA_H
#define WIFI_WPA_H

/* ---- AKM suites we can speak, as a bitmask ------------------------------ */
#define WPA_AKM_PSK        0x01   /* 00-0F-AC-02  WPA2-Personal              */
#define WPA_AKM_PSK_SHA256 0x02   /* 00-0F-AC-06  PSK with SHA-256           */
#define WPA_AKM_SAE        0x04   /* 00-0F-AC-08  WPA3-Personal              */
#define WPA_AKM_FT_SAE     0x08   /* 00-0F-AC-09  fast transition + SAE      */
#define WPA_AKM_SAE_EXT    0x10   /* 00-0F-AC-24  SAE-EXT-KEY (not spoken)   */

/* ---- pairwise / group ciphers ------------------------------------------- */
#define WPA_CIPH_TKIP      0x02   /* 00-0F-AC-02                             */
#define WPA_CIPH_CCMP      0x04   /* 00-0F-AC-04  the only one we install    */
#define WPA_CIPH_GCMP256   0x08   /* 00-0F-AC-09                             */

/* ---- management-frame protection ---------------------------------------- */
#define WPA_MFP_OFF        0      /* no PMF at all (legacy WPA2)             */
#define WPA_MFP_CAPABLE    1      /* MFPC - required for WPA2/WPA3 transition */
#define WPA_MFP_REQUIRED   2      /* MFPC + MFPR - what an SAE-only AP wants  */

/* What an AP's beacon says about how it may be joined. */
typedef struct {
    unsigned short akm;           /* WPA_AKM_* the AP offers                 */
    unsigned char  pairwise;      /* WPA_CIPH_* the AP offers                */
    unsigned char  group;         /* WPA_CIPH_* the AP's group cipher        */
    unsigned char  mfpc, mfpr;    /* RSN capabilities bits 7 and 6           */
    unsigned char  group_mgmt;    /* BIP suite selector (6 = BIP-CMAC-128)   */
    unsigned char  h2e;           /* RSNXE bit 5: SAE hash-to-element        */
    unsigned char  has_rsn;       /* an RSN element was present at all       */
} wpa_ap_sec_t;

/* Parse a beacon/probe RSN element (pointing at the element id byte, 0x30).
 * Returns 1 when it parsed, 0 when the element is malformed. */
int wpa_parse_rsn_ie(const unsigned char *ie, int len, wpa_ap_sec_t *out);

/* Fold an RSN Extension element (id 244) into the same profile - this is the
 * only place the AP advertises SAE hash-to-element support. */
void wpa_parse_rsnxe(const unsigned char *ie, int len, wpa_ap_sec_t *out);

/* Which AKM we will use with this AP: WPA_AKM_SAE, WPA_AKM_PSK, or 0 when it
 * offers nothing we can speak.  SAE wins whenever it is on offer, including
 * on a transition-mode AP - the passphrase is the same and the PMK is not
 * grindable offline afterwards. */
int wpa_pick_akm(const wpa_ap_sec_t *ap);

/* The MFP level our RSN element must claim to be accepted by this AP. */
int wpa_pick_mfp(const wpa_ap_sec_t *ap, int akm);

/* ---- the RSN element we advertise --------------------------------------- */
typedef struct {
    int akm;                            /* WPA_AKM_PSK or WPA_AKM_SAE        */
    int mfp;                            /* WPA_MFP_*                         */
    const unsigned char *pmkid;         /* 16 bytes to advertise, or NULL    */
} wpa_rsn_cfg_t;

/* Write elem-id + len + body into out; returns the total length or -1.
 * `out` needs 64 bytes.  THE SAME BYTES must go in the association request
 * and in EAPOL message 2/4 - the authenticator compares them and
 * deauthenticates on any difference - so build both from this one call and
 * pass the result to wpa_sm_init(). */
int wpa_build_rsn_ie(unsigned char *out, int cap, const wpa_rsn_cfg_t *cfg);

/* The RSN Extension element to send when we are doing SAE hash-to-element.
 * Returns the length written (3), or 0 when none is needed. */
int wpa_build_rsnxe(unsigned char *out, int cap, int h2e);

/* PMK from an SSID + passphrase, the WPA2 way (PBKDF2-SHA1, 4096 rounds).
 * WPA3 does not use this: see wifi_sae.h. */
void wpa_pmk_from_psk(const char *ssid, int ssid_len,
                      const char *passphrase, unsigned char pmk[32]);

/* The 802.11 KDF (12.7.1.6.2) over HMAC-SHA256.  Note the byte order and the
 * absence of a NUL after the label - both differ from the WPA2 PRF, and both
 * are silent wrong-answer bugs if copied from the SHA-1 version.
 *     out = HMAC-SHA256(key, i_LE16 || label || context || nbits_LE16), i=1.. */
void wpa_kdf_sha256(const unsigned char *key, int keylen, const char *label,
                    const unsigned char *ctx, int ctxlen,
                    unsigned char *out, int outbits);

/* ---- 4-way handshake state machine -------------------------------------- */
typedef struct {
    unsigned char pmk[32];
    unsigned char aa[6], spa[6];          /* authenticator (AP) / supplicant   */
    unsigned char anonce[32], snonce[32];
    unsigned char ptk[48];                /* KCK[16] | KEK[16] | TK[16]        */
    unsigned char gtk[32];  int gtk_len;  /* group key from msg 3 (or rekey)   */
    int  gtk_idx;                         /* group key index (1..3)            */
    unsigned char igtk[32]; int igtk_len; /* integrity group key (PMF), msg 3  */
    int  igtk_idx;                        /* 4 or 5, or -1 when absent         */
    unsigned char ipn[6];                 /* IGTK packet number                */
    int  akm;                             /* WPA_AKM_PSK or WPA_AKM_SAE        */
    unsigned char rsn_ie[64]; int rsn_ie_len;   /* IE echoed in msg 2          */
    unsigned char replay[8];              /* highest replay counter seen       */
    int  state;                           /* WPA_ST_* below                    */
} wpa_sm_t;

enum { WPA_ST_IDLE = 0, WPA_ST_SENT_2, WPA_ST_DONE, WPA_ST_FAILED };

/* `akm` selects the PRF and MIC algorithms; `rsn_ie` MUST be the exact
 * element that went out in the association request. */
void wpa_sm_init(wpa_sm_t *sm, const unsigned char pmk[32],
                 const unsigned char own_mac[6], const unsigned char ap_mac[6],
                 int akm, const unsigned char *rsn_ie, int rsn_ie_len);

/* Feed one received EAPOL frame (starting at the 802.1X version byte).
 * If a reply must be transmitted, it is written to `reply` (cap >= 512) and
 * its length returned.  0 = nothing to send; -1 = handshake failure.
 * After msg 3 the reply is msg 4 AND sm->state becomes WPA_ST_DONE: the
 * caller must TRANSMIT THE REPLY FIRST (plaintext), then install the keys
 * (sm->ptk+32 pairwise TK, sm->gtk group, sm->igtk integrity group).
 * Handles group-key rekeys too. */
int  wpa_sm_rx_eapol(wpa_sm_t *sm, const unsigned char *frame, int len,
                     unsigned char *reply, int reply_cap);

#endif
