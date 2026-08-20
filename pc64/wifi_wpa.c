/* WPA2-PSK / WPA3-SAE supplicant (see wifi_wpa.h) - key derivation, the
 * EAPOL-Key 4-way handshake, and AES key-unwrap, over BearSSL primitives.
 *
 * References: IEEE 802.11-2020 12.7 (keys + EAPOL-Key frames), RFC 3394 (AES
 * key wrap), RFC 2898 (PBKDF2), RFC 4493 (AES-CMAC).
 *
 * TWO key descriptor shapes live here, selected by the negotiated AKM:
 *   version 2 (AKM PSK)  PRF-SHA1 PTK, HMAC-SHA1 MIC   - the WPA2 baseline
 *   version 0 (AKM SAE)  KDF-SHA256 PTK, AES-CMAC MIC  - WPA3
 * Key data is AES-wrapped in both, which is why the unwrap below is shared. */
#include "wifi_wpa.h"
#include <string.h>
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "bearssl_block.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* ---- HMAC-SHA1 ----------------------------------------------------------- */
static void hmac_sha1(const u8 *key, int keylen, const u8 *msg, int msglen,
                      u8 out[20])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha1_vtable, key, (size_t)keylen);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, msg, (size_t)msglen);
    br_hmac_out(&hc, out);
}

/* ---- the 802.11 KDF over HMAC-SHA256 (12.7.1.6.2) -----------------------
 * NOT the same construction as the SHA-1 PRF below, in three ways that each
 * silently produce a plausible-looking wrong key: the counter comes FIRST and
 * is little-endian, the label is NOT NUL-terminated, and the output length in
 * BITS is appended. */
void wpa_kdf_sha256(const u8 *key, int keylen, const char *label,
                    const u8 *ctx, int ctxlen, u8 *out, int outbits)
{
    br_hmac_key_context kc;
    u8 dig[32], ctr[2], nb[2];
    int pos = 0, outlen = (outbits + 7) / 8, llen = (int)strlen(label);
    unsigned i = 1;
    nb[0] = (u8)outbits; nb[1] = (u8)(outbits >> 8);
    br_hmac_key_init(&kc, &br_sha256_vtable, key, (size_t)keylen);
    while (pos < outlen) {
        br_hmac_context hc;
        int n;
        ctr[0] = (u8)i; ctr[1] = (u8)(i >> 8);
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, ctr, 2);
        br_hmac_update(&hc, label, (size_t)llen);
        if (ctxlen) br_hmac_update(&hc, ctx, (size_t)ctxlen);
        br_hmac_update(&hc, nb, 2);
        br_hmac_out(&hc, dig);
        n = outlen - pos; if (n > 32) n = 32;
        memcpy(out + pos, dig, (size_t)n);
        pos += n; i++;
    }
    if (outbits & 7) out[outlen - 1] &= (u8)(0xff << (8 - (outbits & 7)));
    memset(dig, 0, sizeof dig);
}

/* ---- AES-128-CMAC (RFC 4493) - the WPA3 EAPOL-Key MIC -------------------
 * BearSSL has no CMAC, but it has AES; a CBC encryption of one block with a
 * zero IV is the raw block cipher, which is all CMAC needs. */
static void aes1_enc(const br_aes_big_cbcenc_keys *k, u8 blk[16])
{
    u8 iv[16];
    memset(iv, 0, 16);
    br_aes_big_cbcenc_run(k, iv, blk, 16);
}

static void cmac_dbl(u8 out[16], const u8 in[16])
{
    int i; u8 carry = 0, msb = (u8)(in[0] >> 7);
    for (i = 15; i >= 0; i--) { u8 v = in[i]; out[i] = (u8)((v << 1) | carry); carry = (u8)(v >> 7); }
    if (msb) out[15] ^= 0x87;                 /* the GF(2^128) reduction poly */
}

static void aes_cmac(const u8 key[16], const u8 *msg, int len, u8 out[16])
{
    br_aes_big_cbcenc_keys k;
    u8 l[16], k1[16], k2[16], blk[16], x[16];
    int nblk, i, j, rem;
    br_aes_big_cbcenc_init(&k, key, 16);
    memset(l, 0, 16); aes1_enc(&k, l);
    cmac_dbl(k1, l);
    cmac_dbl(k2, k1);
    nblk = (len + 15) / 16; if (nblk == 0) nblk = 1;
    memset(x, 0, 16);
    for (i = 0; i < nblk - 1; i++) {
        for (j = 0; j < 16; j++) blk[j] = (u8)(x[j] ^ msg[i * 16 + j]);
        aes1_enc(&k, blk);
        memcpy(x, blk, 16);
    }
    rem = len - (nblk - 1) * 16;
    if (rem == 16) { for (j = 0; j < 16; j++) blk[j] = (u8)(msg[(nblk - 1) * 16 + j] ^ k1[j]); }
    else {
        u8 pad[16];
        memset(pad, 0, 16);
        if (rem > 0) memcpy(pad, msg + (nblk - 1) * 16, (size_t)rem);
        pad[rem] = 0x80;
        for (j = 0; j < 16; j++) blk[j] = (u8)(pad[j] ^ k2[j]);
    }
    for (j = 0; j < 16; j++) blk[j] ^= x[j];
    aes1_enc(&k, blk);
    memcpy(out, blk, 16);
    memset(l, 0, 16); memset(k1, 0, 16); memset(k2, 0, 16);
}

/* ---- PBKDF2-HMAC-SHA1 (PMK = 4096 rounds over the SSID as salt) ---------- */
static void pbkdf2_block(const char *pass, int plen, const char *salt, int slen,
                         u32 blkidx, u8 out[20])
{
    u8 u[20], acc[20], seed[36];
    int i, k;
    if (slen > 32) slen = 32;                    /* SSIDs are <= 32 bytes */
    memcpy(seed, salt, (size_t)slen);
    seed[slen]   = (u8)(blkidx >> 24); seed[slen+1] = (u8)(blkidx >> 16);
    seed[slen+2] = (u8)(blkidx >> 8);  seed[slen+3] = (u8)blkidx;
    hmac_sha1((const u8 *)pass, plen, seed, slen + 4, u);
    memcpy(acc, u, 20);
    for (i = 1; i < 4096; i++) {
        hmac_sha1((const u8 *)pass, plen, u, 20, u);
        for (k = 0; k < 20; k++) acc[k] ^= u[k];
    }
    memcpy(out, acc, 20);
}

void wpa_pmk_from_psk(const char *ssid, int ssid_len,
                      const char *passphrase, u8 pmk[32])
{
    u8 b1[20], b2[20];
    int plen = (int)strlen(passphrase);
    pbkdf2_block(passphrase, plen, ssid, ssid_len, 1, b1);
    pbkdf2_block(passphrase, plen, ssid, ssid_len, 2, b2);
    memcpy(pmk, b1, 20);
    memcpy(pmk + 20, b2, 12);
}

/* ---- PRF-X (802.11i 8.5.1.1): PTK = PRF-384(PMK, label, AA|SPA|AN|SN) ---- */
static void wpa_prf(const u8 key[32], const char *label,
                    const u8 *data, int dlen, u8 *out, int outlen)
{
    u8 buf[128], dig[20];
    int llen = (int)strlen(label), pos = 0;
    u8 ctr = 0;
    while (pos < outlen) {
        int n = 0;
        memcpy(buf, label, (size_t)llen);         n  = llen;
        buf[n++] = 0;                              /* the label's NUL is hashed */
        memcpy(buf + n, data, (size_t)dlen);       n += dlen;
        buf[n++] = ctr++;
        hmac_sha1(key, 32, buf, n, dig);
        { int c = outlen - pos; if (c > 20) c = 20; memcpy(out + pos, dig, (size_t)c); pos += c; }
    }
}

static void minmax6(const u8 *a, const u8 *b, const u8 **lo, const u8 **hi)
{ *lo = (memcmp(a, b, 6) < 0) ? a : b; *hi = (*lo == a) ? b : a; }
static void minmax32(const u8 *a, const u8 *b, const u8 **lo, const u8 **hi)
{ *lo = (memcmp(a, b, 32) < 0) ? a : b; *hi = (*lo == a) ? b : a; }

static void derive_ptk(wpa_sm_t *sm)
{
    u8 data[76];
    const u8 *lo, *hi;
    minmax6(sm->aa, sm->spa, &lo, &hi);
    memcpy(data, lo, 6); memcpy(data + 6, hi, 6);
    minmax32(sm->anonce, sm->snonce, &lo, &hi);
    memcpy(data + 12, lo, 32); memcpy(data + 44, hi, 32);
    if (sm->akm == WPA_AKM_SAE)
        wpa_kdf_sha256(sm->pmk, 32, "Pairwise key expansion", data, 76, sm->ptk, 384);
    else
        wpa_prf(sm->pmk, "Pairwise key expansion", data, 76, sm->ptk, 48);
}

/* The EAPOL-Key MIC, whichever algorithm this AKM calls for.  Always 16
 * bytes, so the frame layout is identical either way. */
static void eapol_mic(const wpa_sm_t *sm, const u8 *frame, int len, u8 mic[16])
{
    if (sm->akm == WPA_AKM_SAE) {
        aes_cmac(sm->ptk, frame, len, mic);        /* KCK = ptk[0..15] */
    } else {
        u8 d[20];
        hmac_sha1(sm->ptk, 16, frame, len, d);
        memcpy(mic, d, 16);
    }
}

/* ---- AES-128 key unwrap (RFC 3394) - GTK arrives wrapped under the KEK --- */
static void aes1_dec(const br_aes_big_cbcdec_keys *k, u8 blk[16])
{
    u8 iv[16];
    memset(iv, 0, 16);                 /* CBC-decrypt of 1 block, IV 0 == ECB */
    br_aes_big_cbcdec_run(k, iv, blk, 16);
}

static int aes_unwrap(const u8 kek[16], const u8 *in, int inlen, u8 *out, int outcap)
{
    br_aes_big_cbcdec_keys k;
    u8 a[8], b[16];
    int n = inlen / 8 - 1, i, j;
    if (inlen < 24 || (inlen & 7)) return -1;
    if (n * 8 > outcap) return -1;               /* never write past out[outcap] */
    br_aes_big_cbcdec_init(&k, kek, 16);
    memcpy(a, in, 8);
    memcpy(out, in + 8, (size_t)(n * 8));
    for (j = 5; j >= 0; j--)
        for (i = n; i >= 1; i--) {
            u64 t = (u64)(n * j + i);
            memcpy(b, a, 8);
            b[7] ^= (u8)t;      b[6] ^= (u8)(t >> 8);  b[5] ^= (u8)(t >> 16);
            b[4] ^= (u8)(t >> 24);
            memcpy(b + 8, out + (i - 1) * 8, 8);
            aes1_dec(&k, b);
            memcpy(a, b, 8);
            memcpy(out + (i - 1) * 8, b + 8, 8);
        }
    for (i = 0; i < 8; i++) if (a[i] != 0xA6) return -1;
    return n * 8;
}

/* ---- the RSN element: what the AP offers, and what we answer ------------- */

static int suite_akm(const u8 *s)
{
    if (s[0] != 0x00 || s[1] != 0x0F || s[2] != 0xAC) return 0;   /* vendor AKM */
    switch (s[3]) {
    case 2:  return WPA_AKM_PSK;
    case 6:  return WPA_AKM_PSK_SHA256;
    case 8:  return WPA_AKM_SAE;
    case 9:  return WPA_AKM_FT_SAE;
    case 24: return WPA_AKM_SAE_EXT;
    }
    return 0;
}

static int suite_ciph(const u8 *s)
{
    if (s[0] != 0x00 || s[1] != 0x0F || s[2] != 0xAC) return 0;
    switch (s[3]) {
    case 2: return WPA_CIPH_TKIP;
    case 4: return WPA_CIPH_CCMP;
    case 9: return WPA_CIPH_GCMP256;
    }
    return 0;
}

/* The RSN element is defined POSITIONALLY with trailing fields optional, so
 * every step below has to cope with the element simply ending.  A truncated
 * element leaves the fields we never reached at zero, which reads downstream
 * as "offers nothing we can speak" - the safe direction. */
int wpa_parse_rsn_ie(const u8 *ie, int len, wpa_ap_sec_t *out)
{
    int n, i, cnt;
    memset(out, 0, sizeof *out);
    if (len < 2 || ie[0] != 0x30) return 0;
    n = ie[1] + 2; if (n > len) return 0;
    if (n < 4 || ie[2] != 1 || ie[3] != 0) return 0;      /* RSN version 1 */
    out->has_rsn = 1;
    i = 4;
    if (i + 4 > n) return 1;
    out->group = (u8)suite_ciph(ie + i); i += 4;
    if (i + 2 > n) return 1;
    cnt = ie[i] | (ie[i + 1] << 8); i += 2;
    while (cnt-- > 0 && i + 4 <= n) { out->pairwise |= (u8)suite_ciph(ie + i); i += 4; }
    if (i + 2 > n) return 1;
    cnt = ie[i] | (ie[i + 1] << 8); i += 2;
    while (cnt-- > 0 && i + 4 <= n) { out->akm |= (u16)suite_akm(ie + i); i += 4; }
    if (i + 2 > n) return 1;
    { unsigned caps = (unsigned)(ie[i] | (ie[i + 1] << 8)); i += 2;
      out->mfpr = (u8)((caps >> 6) & 1);
      out->mfpc = (u8)((caps >> 7) & 1); }
    if (i + 2 > n) return 1;
    cnt = ie[i] | (ie[i + 1] << 8); i += 2;               /* PMKID list */
    i += cnt * 16;
    if (i + 4 > n) return 1;
    if (ie[i] == 0x00 && ie[i + 1] == 0x0F && ie[i + 2] == 0xAC)
        out->group_mgmt = ie[i + 3];
    return 1;
}

void wpa_parse_rsnxe(const u8 *ie, int len, wpa_ap_sec_t *out)
{
    if (len < 3 || ie[0] != 0xF4 || ie[1] < 1) return;
    /* Extended RSN Capabilities, first octet: low nibble is the field length,
     * bit 5 is "SAE hash-to-element supported". */
    out->h2e = (u8)((ie[2] >> 5) & 1);
}

int wpa_pick_akm(const wpa_ap_sec_t *ap)
{
    if (!ap->has_rsn) return 0;
    if (!(ap->pairwise & WPA_CIPH_CCMP)) return 0;    /* CCMP is all we install */
    /* SAE whenever it is offered, including on a transition-mode AP: the
     * passphrase is the same and the resulting PMK is not grindable offline
     * from a captured handshake, which is the entire point of WPA3. */
    if (ap->akm & WPA_AKM_SAE) return WPA_AKM_SAE;
    if (ap->akm & WPA_AKM_PSK) return WPA_AKM_PSK;
    return 0;
}

int wpa_pick_mfp(const wpa_ap_sec_t *ap, int akm)
{
    if (akm == WPA_AKM_SAE) return WPA_MFP_REQUIRED;  /* SAE mandates PMF */
    /* For plain PSK, claim PMF only when the AP demands it.  An AP with
     * MFPR set refuses an association request without MFPC; one with only
     * MFPC does not, and leaving the long-proven WPA2 request byte-identical
     * to what already joins is worth more than the optional protection. */
    if (ap->mfpr) return WPA_MFP_CAPABLE;
    return WPA_MFP_OFF;
}

int wpa_build_rsn_ie(u8 *out, int cap, const wpa_rsn_cfg_t *cfg)
{
    static const u8 ccmp[4]    = { 0x00, 0x0F, 0xAC, 0x04 };
    static const u8 akm_psk[4] = { 0x00, 0x0F, 0xAC, 0x02 };
    static const u8 akm_sae[4] = { 0x00, 0x0F, 0xAC, 0x08 };
    static const u8 bip[4]     = { 0x00, 0x0F, 0xAC, 0x06 };  /* BIP-CMAC-128 */
    unsigned caps = 0;
    int n = 2;
    if (cap < 48) return -1;
    out[n++] = 0x01; out[n++] = 0x00;                    /* RSN version 1     */
    memcpy(out + n, ccmp, 4); n += 4;                    /* group cipher      */
    out[n++] = 0x01; out[n++] = 0x00;
    memcpy(out + n, ccmp, 4); n += 4;                    /* 1 pairwise: CCMP  */
    out[n++] = 0x01; out[n++] = 0x00;
    memcpy(out + n, cfg->akm == WPA_AKM_SAE ? akm_sae : akm_psk, 4); n += 4;
    if (cfg->mfp >= WPA_MFP_CAPABLE)  caps |= 0x0080;    /* MFPC              */
    if (cfg->mfp >= WPA_MFP_REQUIRED) caps |= 0x0040;    /* MFPR              */
    out[n++] = (u8)caps; out[n++] = (u8)(caps >> 8);
    /* From here the element is positional with no tags: the group management
     * cipher may only FOLLOW a PMKID count, so with PMF on we emit a count of
     * zero rather than omit the field - otherwise the AP reads the BIP suite
     * as a PMKID count of 0xAC00 and rejects the association. */
    if (cfg->pmkid) {
        out[n++] = 0x01; out[n++] = 0x00;
        memcpy(out + n, cfg->pmkid, 16); n += 16;
    } else if (cfg->mfp >= WPA_MFP_CAPABLE) {
        out[n++] = 0x00; out[n++] = 0x00;
    }
    if (cfg->mfp >= WPA_MFP_CAPABLE) { memcpy(out + n, bip, 4); n += 4; }
    out[0] = 0x30; out[1] = (u8)(n - 2);
    return n;
}

int wpa_build_rsnxe(u8 *out, int cap, int h2e)
{
    if (!h2e || cap < 3) return 0;
    out[0] = 0xF4;      /* element id 244 - RSN Extension                    */
    out[1] = 1;         /* one octet of extended capabilities                */
    out[2] = 0x20;      /* field-length nibble 0 (= 1 octet) | bit 5 = SAE H2E */
    return 3;
}

/* ---- EAPOL-Key frames ---------------------------------------------------- */
/* body offsets from the start of the 802.1X frame (4-byte header + body):   */
enum {
    EK_DESC = 4, EK_INFO = 5, EK_KLEN = 7, EK_REPLAY = 9, EK_NONCE = 17,
    EK_IV = 49, EK_RSC = 65, EK_ID = 73, EK_MIC = 81, EK_KDLEN = 97,
    EK_KDATA = 99
};
#define KI_VER_MASK  0x0007
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENCRYPTED 0x1000

static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

/* a weak but adequate SNonce: TSC + previous state hashed together (no user
 * data depends on SNonce secrecy beyond handshake uniqueness) */
static void gen_snonce(u8 out[32])
{
    static u8 pool[20];
    br_sha1_context c;
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    br_sha1_init(&c);
    br_sha1_update(&c, pool, 20);
    br_sha1_update(&c, &lo, 4);
    br_sha1_update(&c, &hi, 4);
    br_sha1_out(&c, pool);
    memcpy(out, pool, 20);
    br_sha1_update(&c, "x", 1);
    br_sha1_out(&c, pool);
    memcpy(out + 20, pool, 12);
}

void wpa_sm_init(wpa_sm_t *sm, const u8 pmk[32],
                 const u8 own_mac[6], const u8 ap_mac[6],
                 int akm, const u8 *rsn_ie, int rsn_ie_len)
{
    memset(sm, 0, sizeof *sm);
    memcpy(sm->pmk, pmk, 32);
    memcpy(sm->spa, own_mac, 6);
    memcpy(sm->aa, ap_mac, 6);
    sm->akm = akm;
    sm->igtk_idx = -1;
    /* The caller passes the EXACT element it put in the association request.
     * This used to be rebuilt here from a private literal, and the two drifted
     * - the AP compares them and deauthenticates on any difference. */
    if (rsn_ie_len > 0 && rsn_ie_len <= (int)sizeof sm->rsn_ie) {
        memcpy(sm->rsn_ie, rsn_ie, (size_t)rsn_ie_len);
        sm->rsn_ie_len = rsn_ie_len;
    }
    sm->state = WPA_ST_IDLE;
}

/* build an EAPOL-Key reply; key_data (may be NULL) is copied in plaintext */
static int build_reply(wpa_sm_t *sm, u8 *out, u16 key_info,
                       const u8 *replay, const u8 *nonce,
                       const u8 *kd, int kdlen)
{
    int blen = 95 + kdlen;                       /* fixed body + key data */
    u8 mic[20];
    memset(out, 0, (size_t)(4 + blen));
    out[0] = 0x02;                               /* 802.1X v2 (2004)      */
    out[1] = 0x03;                               /* EAPOL-Key             */
    put16(out + 2, (u16)blen);
    out[EK_DESC] = 2;                            /* RSN key descriptor    */
    put16(out + EK_INFO, key_info);
    put16(out + EK_KLEN, 16);                    /* CCMP TK length        */
    memcpy(out + EK_REPLAY, replay, 8);
    if (nonce) memcpy(out + EK_NONCE, nonce, 32);
    put16(out + EK_KDLEN, (u16)kdlen);
    if (kd && kdlen) memcpy(out + EK_KDATA, kd, (size_t)kdlen);
    eapol_mic(sm, out, 4 + blen, mic);           /* MIC over the whole frame */
    memcpy(out + EK_MIC, mic, 16);
    return 4 + blen;
}

static int mic_ok(wpa_sm_t *sm, const u8 *frame, int len)
{
    u8 tmp[512], mic[16];
    int i, diff = 0;
    if (len > (int)sizeof tmp) return 0;
    memcpy(tmp, frame, (size_t)len);
    memset(tmp + EK_MIC, 0, 16);
    eapol_mic(sm, tmp, len, mic);
    for (i = 0; i < 16; i++) diff |= mic[i] ^ frame[EK_MIC + i];
    return diff == 0;
}

/* Pull the group keys out of (already unwrapped) key data.  Message 3 also
 * carries the AP's own RSN element (id 0x30) and, under PMF, an IGTK KDE, so
 * this walks the whole list rather than stopping at the first match.
 * Returns a bitmask: 1 = a GTK was found, 2 = an IGTK was found, -1 on a KDE
 * whose length would overrun a key buffer.  The CALLER decides what it needed,
 * because that differs: message 3/4 must carry a GTK, while a group rekey
 * under PMF may legitimately refresh only the integrity key. */
#define FK_GTK  1
#define FK_IGTK 2
static int find_keys(wpa_sm_t *sm, const u8 *kd, int kdlen)
{
    int i = 0, got = 0;
    while (i + 2 <= kdlen) {
        int id = kd[i], l = kd[i+1];
        if (id == 0xDD && l == 0) break;          /* padding to the wrap block */
        if (i + 2 + l > kdlen) break;
        if (id == 0xDD && l >= 4 &&
            kd[i+2]==0x00 && kd[i+3]==0x0F && kd[i+4]==0xAC) {
            int type = kd[i+5];
            if (type == 1 && l >= 6) {            /* GTK KDE: KeyID, rsv, key */
                int glen = l - 6;
                if (glen > (int)sizeof sm->gtk) return -1;
                sm->gtk_idx = kd[i+6] & 0x03;
                memcpy(sm->gtk, kd + i + 8, (size_t)glen);
                sm->gtk_len = glen;
                got |= FK_GTK;
            } else if (type == 9 && l >= 12) {    /* IGTK KDE: KeyID, IPN, key */
                int ilen = l - 12;
                if (ilen > (int)sizeof sm->igtk) return -1;
                sm->igtk_idx = kd[i+6] | (kd[i+7] << 8);
                memcpy(sm->ipn, kd + i + 8, 6);
                memcpy(sm->igtk, kd + i + 14, (size_t)ilen);
                sm->igtk_len = ilen;
                got |= FK_IGTK;
            }
        }
        i += 2 + l;
    }
    return got;
}

/* Compare two 8-byte big-endian EAPOL-Key replay counters.
 * <0 if a < b, 0 if equal, >0 if a > b. */
static int replay_cmp(const u8 *a, const u8 *b)
{
    int i;
    for (i = 0; i < 8; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

int wpa_sm_rx_eapol(wpa_sm_t *sm, const u8 *frame, int len,
                    u8 *reply, int reply_cap)
{
    u16 ki;
    int kdlen;
    if (len < EK_KDATA || frame[1] != 0x03 || frame[EK_DESC] != 2) return 0;
    if (reply_cap < 512) return -1;
    ki = be16(frame + EK_INFO);
    kdlen = be16(frame + EK_KDLEN);
    if (EK_KDATA + kdlen > len) return -1;
    if ((ki & KI_VER_MASK) > 3) return -1;
    /* Key Descriptor Version is echoed back, not acted on: the MIC and PTK
     * algorithms follow the NEGOTIATED AKM (802.11-2020 Table 12-10), which
     * is the authority.  Version 0 is what an SAE AP sends and what the old
     * "must be 1 or 2" test rejected outright; v1's RC4-wrapped key data is
     * the one shape we do not honour, and it cannot occur with CCMP. */

    if ((ki & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK)) {
        /* ---- message 1/4: take ANonce, derive the PTK, answer with 2/4 -- */
        /* THE REPLAY COUNTER IS THE POINT OF THIS FIELD, and it was stored and
         * never compared.  A 1/4 that arrives again after the handshake has
         * finished - the AP retransmitting, or our own RX ring re-presenting a
         * buffer - therefore re-derived the PTK, sent a fresh 2/4, and knocked
         * the state machine back from DONE to SENT_2.  On metal (SURFGO,
         * 2026-08-04) that is exactly what happened 50 ms after a clean
         * handshake, and the AP answered the unexpected 2/4 with a stream of
         * deauthentications; DHCP never completed.
         *
         * 802.11i: only a counter STRICTLY GREATER than the highest seen starts
         * a new handshake (a genuine PTK rekey).  Anything else is a stale copy
         * and must not touch state or produce a reply. */
        if (sm->state != WPA_ST_IDLE &&
            replay_cmp(frame + EK_REPLAY, sm->replay) <= 0)
            return 0;
        memcpy(sm->anonce, frame + EK_NONCE, 32);
        if (sm->state == WPA_ST_IDLE) gen_snonce(sm->snonce);
        derive_ptk(sm);
        memcpy(sm->replay, frame + EK_REPLAY, 8);
        sm->state = WPA_ST_SENT_2;
        return build_reply(sm, reply,
                           (u16)((ki & KI_VER_MASK) | KI_PAIRWISE | KI_MIC),
                           sm->replay, sm->snonce, sm->rsn_ie, sm->rsn_ie_len);
    }

    if ((ki & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK | KI_MIC)) {
        /* ---- message 3/4: verify, unwrap the GTK, answer with 4/4 ------- */
        u8 kd[256];
        int n;
        if (sm->state != WPA_ST_SENT_2 && sm->state != WPA_ST_DONE) return 0;
        /* A 3/4 carrying the SAME counter is the AP retransmitting because our
         * 4/4 did not arrive - answer it again.  A LOWER one is stale and must
         * not be answered (same reasoning as 1/4 above). */
        if (replay_cmp(frame + EK_REPLAY, sm->replay) < 0) return 0;
        if (!mic_ok(sm, frame, len)) { sm->state = WPA_ST_FAILED; return -1; }
        if (memcmp(frame + EK_NONCE, sm->anonce, 32) != 0) {
            sm->state = WPA_ST_FAILED; return -1;      /* ANonce changed */
        }
        if (ki & KI_ENCRYPTED) {
            if (kdlen < 24 || kdlen > (int)sizeof kd) { sm->state = WPA_ST_FAILED; return -1; }
            n = aes_unwrap(sm->ptk + 16, frame + EK_KDATA, kdlen, kd, sizeof kd);
            if (n < 0 || !(find_keys(sm, kd, n) & FK_GTK)) { sm->state = WPA_ST_FAILED; return -1; }
        }
        memcpy(sm->replay, frame + EK_REPLAY, 8);
        sm->state = WPA_ST_DONE;
        return build_reply(sm, reply,
                           (u16)((ki & KI_VER_MASK) | KI_PAIRWISE | KI_MIC | KI_SECURE),
                           sm->replay, 0, 0, 0);
    }

    if ((ki & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_ACK | KI_MIC)) {
        /* ---- group-key rekey (1/2): unwrap the new GTK, ack with 2/2 ---- */
        u8 kd[256];
        int n;
        if (sm->state != WPA_ST_DONE) return 0;
        if (replay_cmp(frame + EK_REPLAY, sm->replay) < 0) return 0;   /* stale rekey */
        if (!mic_ok(sm, frame, len)) return -1;
        if (kdlen < 24 || kdlen > (int)sizeof kd) { sm->state = WPA_ST_FAILED; return -1; }
        n = aes_unwrap(sm->ptk + 16, frame + EK_KDATA, kdlen, kd, sizeof kd);
        if (n < 0 || find_keys(sm, kd, n) <= 0) return -1;
        memcpy(sm->replay, frame + EK_REPLAY, 8);
        return build_reply(sm, reply,
                           (u16)((ki & KI_VER_MASK) | KI_MIC | KI_SECURE),
                           sm->replay, 0, 0, 0);
    }
    return 0;
}
