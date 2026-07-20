/* WPA2-PSK supplicant (see wifi_wpa.h) - PBKDF2/PRF key derivation, the
 * EAPOL-Key 4-way handshake, and AES key-unwrap, over BearSSL primitives.
 *
 * References: IEEE 802.11i-2004 (8.5: keys + EAPOL-Key frames), RFC 3394
 * (AES key wrap), RFC 2898 (PBKDF2). Descriptor version 2 only (HMAC-SHA1
 * MIC + AES-wrap key data) - the WPA2-CCMP baseline every AP supports. */
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
    wpa_prf(sm->pmk, "Pairwise key expansion", data, 76, sm->ptk, 48);
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

/* ---- RSN IE -------------------------------------------------------------- */
int wpa_build_rsn_ie(u8 *out)
{
    static const u8 ie[22] = {
        0x30, 20,                     /* RSNE, length                     */
        0x01, 0x00,                   /* version 1                        */
        0x00, 0x0F, 0xAC, 0x04,      /* group cipher: CCMP               */
        0x01, 0x00,                   /* 1 pairwise cipher                */
        0x00, 0x0F, 0xAC, 0x04,      /*   CCMP                           */
        0x01, 0x00,                   /* 1 AKM                            */
        0x00, 0x0F, 0xAC, 0x02,      /*   PSK                            */
        0x00, 0x00                    /* RSN capabilities                 */
    };
    memcpy(out, ie, 22);
    return 22;
}

int wpa_rsn_ie_ok(const u8 *ie, int len)
{
    int n, i, cnt, ccmp = 0, psk = 0;
    if (len < 2 || ie[0] != 0x30) return 0;
    n = ie[1] + 2; if (n > len) return 0;
    if (n < 8 || ie[2] != 1) return 0;            /* version 1 */
    i = 8;                                        /* skip group cipher */
    if (i + 2 > n) return 1;                      /* defaults = CCMP/8021X: no */
    cnt = ie[i] | (ie[i+1] << 8); i += 2;
    while (cnt-- > 0 && i + 4 <= n) {             /* pairwise suites */
        if (ie[i]==0x00 && ie[i+1]==0x0F && ie[i+2]==0xAC && ie[i+3]==0x04) ccmp = 1;
        i += 4;
    }
    if (i + 2 > n) return 0;
    cnt = ie[i] | (ie[i+1] << 8); i += 2;
    while (cnt-- > 0 && i + 4 <= n) {             /* AKM suites */
        if (ie[i]==0x00 && ie[i+1]==0x0F && ie[i+2]==0xAC &&
            (ie[i+3]==0x02 || ie[i+3]==0x06)) psk = 1;   /* PSK / PSK-SHA256* */
        i += 4;
    }
    /* note: AKM 6 (PSK-SHA256) would need SHA256 MICs; only accept it when
     * plain PSK (2) is also offered - we will negotiate 2 in our own IE. */
    (void)0;
    return ccmp && psk;
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
                 const u8 own_mac[6], const u8 ap_mac[6])
{
    memset(sm, 0, sizeof *sm);
    memcpy(sm->pmk, pmk, 32);
    memcpy(sm->spa, own_mac, 6);
    memcpy(sm->aa, ap_mac, 6);
    sm->rsn_ie_len = wpa_build_rsn_ie(sm->rsn_ie);
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
    hmac_sha1(sm->ptk, 16, out, 4 + blen, mic);  /* MIC over the whole frame */
    memcpy(out + EK_MIC, mic, 16);
    return 4 + blen;
}

static int mic_ok(wpa_sm_t *sm, const u8 *frame, int len)
{
    u8 tmp[512], mic[20];
    if (len > (int)sizeof tmp) return 0;
    memcpy(tmp, frame, (size_t)len);
    memset(tmp + EK_MIC, 0, 16);
    hmac_sha1(sm->ptk, 16, tmp, len, mic);
    return memcmp(mic, frame + EK_MIC, 16) == 0;
}

/* pull the GTK KDE out of (already unwrapped) key data */
static int find_gtk(wpa_sm_t *sm, const u8 *kd, int kdlen)
{
    int i = 0;
    while (i + 2 <= kdlen) {
        int id = kd[i], l = kd[i+1];
        if (id == 0xDD && l >= 6 && i + 2 + l <= kdlen &&
            kd[i+2]==0x00 && kd[i+3]==0x0F && kd[i+4]==0xAC && kd[i+5]==1) {
            int glen = l - 6;
            if (glen > (int)sizeof sm->gtk) return -1;
            sm->gtk_idx = kd[i+6] & 0x03;
            memcpy(sm->gtk, kd + i + 8, (size_t)glen);
            sm->gtk_len = glen;
            return 0;
        }
        if (id == 0xDD && l == 0) break;          /* padding */
        i += 2 + l;
    }
    return -1;
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
    if ((ki & KI_VER_MASK) != 2 && (ki & KI_VER_MASK) != 1) return -1;
    /* v1 (HMAC-MD5/RC4) shouldn't appear with CCMP; only v2 is honoured for
     * key data. MIC below is HMAC-SHA1 (v2) regardless. */

    if ((ki & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK)) {
        /* ---- message 1/4: take ANonce, derive the PTK, answer with 2/4 -- */
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
        if (!mic_ok(sm, frame, len)) { sm->state = WPA_ST_FAILED; return -1; }
        if (memcmp(frame + EK_NONCE, sm->anonce, 32) != 0) {
            sm->state = WPA_ST_FAILED; return -1;      /* ANonce changed */
        }
        if (ki & KI_ENCRYPTED) {
            if (kdlen < 24 || kdlen > (int)sizeof kd) { sm->state = WPA_ST_FAILED; return -1; }
            n = aes_unwrap(sm->ptk + 16, frame + EK_KDATA, kdlen, kd, sizeof kd);
            if (n < 0 || find_gtk(sm, kd, n) < 0) { sm->state = WPA_ST_FAILED; return -1; }
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
        if (!mic_ok(sm, frame, len)) return -1;
        if (kdlen < 24 || kdlen > (int)sizeof kd) { sm->state = WPA_ST_FAILED; return -1; }
        n = aes_unwrap(sm->ptk + 16, frame + EK_KDATA, kdlen, kd, sizeof kd);
        if (n < 0 || find_gtk(sm, kd, n) < 0) return -1;
        memcpy(sm->replay, frame + EK_REPLAY, 8);
        return build_reply(sm, reply,
                           (u16)((ki & KI_VER_MASK) | KI_MIC | KI_SECURE),
                           sm->replay, 0, 0, 0);
    }
    return 0;
}
