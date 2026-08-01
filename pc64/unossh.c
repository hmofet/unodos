/* ===========================================================================
 * unossh - the SSH transport: version exchange, curve25519-sha256 key
 * exchange, ssh-ed25519 host-key verification, and the aes256-ctr +
 * hmac-sha2-256 packet layer.  RFC 4253, RFC 8731.
 *
 * Sits on netsock (net_socket / net_connect / net_send / net_recv), NOT on
 * pc64/tls.*, whose state is four file-scope statics and one global
 * connection.  All the pure arithmetic - wire encoding, X25519, the exchange
 * hash, key derivation - lives in unossh_wire.c and is gated on the host.
 * This file is the part that needs a real server on the other end.
 *
 * MAC ORDER IS ENCRYPT-AND-MAC. RFC 4253 section 6.4 computes the MAC over the
 * sequence number and the UNENCRYPTED packet and sends it in the clear after
 * the ciphertext. Encrypt-then-MAC in SSH is an OpenSSH extension negotiated
 * as hmac-sha2-256-etm@openssh.com; assuming it without negotiating it fails
 * every packet.
 *
 * The handshake blocks with a timeout, because it is a bounded operation with
 * a fixed message order and nothing useful to do in between. Packet read after
 * that is non-blocking (pass timeout 0), which is what a shell session driven
 * from the shell's frame loop needs.
 *
 * HOST KEY TRUST IS NOT IMPLEMENTED HERE. The server's key is verified to have
 * signed the exchange hash - so the peer really does hold it - but nothing
 * checks it is the key we expected. ssh_host_fingerprint() exposes the SHA-256
 * that a known-hosts store will compare in ssh-d. Until then this authenticates
 * the channel, not the identity.
 * ======================================================================== */
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "bearssl_block.h"
#include "netsock.h"
#include "net.h"
#include "tls_entropy.h"
#include "ed25519.h"
#include "unossh.h"
#include "unossh_int.h"
#include <stdlib.h>

void uno_pc64_delay_ms(int ms);
int  pc64_net_up(void);

#define SSH_IDENT   "SSH-2.0-UnoDOS_1.0"
#define SSH_MAXPKT  35000            /* RFC 4253's required minimum payload */
#define RXCAP       (SSH_MAXPKT + 256)

#define MSG_DISCONNECT 1
#define MSG_IGNORE     2
#define MSG_DEBUG      4
#define MSG_KEXINIT    20
#define MSG_NEWKEYS    21
#define MSG_ECDH_INIT  30
#define MSG_ECDH_REPLY 31


static ssh_conn g_conn[SSH_MAXCONN];

static void us_copy(unsigned char *d, const unsigned char *s, int n)
{ int i; for (i = 0; i < n; i++) d[i] = s[i]; }
static int us_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
/* Also mirrored into a file-scope copy: ssh_connect() reports failures BEFORE
 * it has a handle to hand back, and closes the slot on the way out, so without
 * this every setup error reads as "bad handle" - which is exactly what it did
 * the first time this gate ran against a real server. */
static char g_last_err[96];
void uns_err(ssh_conn *c, const char *m)
{
    int i;
    for (i = 0; i < (int)sizeof c->err - 1 && m[i]; i++) c->err[i] = m[i];
    c->err[i] = 0;
    for (i = 0; i < (int)sizeof g_last_err - 1 && m[i]; i++) g_last_err[i] = m[i];
    g_last_err[i] = 0;
}
ssh_conn *uns_get(int h)
{ return (h >= 0 && h < SSH_MAXCONN && g_conn[h].used) ? &g_conn[h] : 0; }

/* ---- socket helpers ------------------------------------------------------ */
static int rx_exact(ssh_conn *c, unsigned char *buf, int n, int timeout_ms)
{
    int got = 0, waited = 0;
    while (got < n) {
        int r;
        net_poll();
        r = net_recv(c->sock, buf + got, n - got);
        if (r > 0) { got += r; waited = 0; continue; }
        if (net_sock_state(c->sock) == TCP_CLOSED) return -1;
        if (++waited > timeout_ms) return -1;
        uno_pc64_delay_ms(1);
    }
    return got;
}

static int tx_all(ssh_conn *c, const unsigned char *buf, int n)
{
    int sent = 0, waited = 0;
    while (sent < n) {
        int r;
        net_poll();
        r = net_send(c->sock, buf + sent, n - sent);
        if (r > 0) { sent += r; waited = 0; continue; }
        if (net_sock_state(c->sock) == TCP_CLOSED) return -1;
        if (++waited > 20000) return -1;
        uno_pc64_delay_ms(1);
    }
    return sent;
}

/* ---- the packet layer ----------------------------------------------------
 * BearSSL's CTR takes a 12-byte nonce plus a 32-bit block counter, while SSH's
 * counter is the whole 16-byte IV incrementing as one big-endian integer. They
 * agree until the low 32 bits wrap, which is 64 GB into a stream - so the
 * carry below will realistically never fire, and is here because "never" and
 * "cannot" are different claims. */
static void ctr_run(ssh_conn *c, int out, unsigned char *data, int len)
{
    unsigned char *iv = out ? c->iv_out : c->iv_in;
    unsigned *cc = out ? &c->cc_out : &c->cc_in;
    br_aes_ct64_ctr_keys *k = out ? &c->enc : &c->dec;
    unsigned before = *cc;
    *cc = br_aes_ct64_ctr_run(k, iv, *cc, data, (size_t)len);
    if (*cc < before) { int i; for (i = 11; i >= 0; i--) if (++iv[i]) break; }
}

static void mac_compute(const unsigned char key[32], unsigned seq,
                        const unsigned char *pkt, int n, unsigned char out[32])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    unsigned char s[4];
    s[0] = (unsigned char)(seq >> 24); s[1] = (unsigned char)(seq >> 16);
    s[2] = (unsigned char)(seq >> 8);  s[3] = (unsigned char)seq;
    br_hmac_key_init(&kc, &br_sha256_vtable, key, 32);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, s, 4);
    br_hmac_update(&hc, pkt, (size_t)n);
    br_hmac_out(&hc, out);
}

int uns_send(ssh_conn *c, const unsigned char *pay, int n)
{
    int bs = c->encrypted ? 16 : 8;
    int pad = ssh_pad_len(n, bs);
    int plen = 1 + n + pad, total = 4 + plen;
    unsigned char *p = c->tx;

    if (n < 0 || total + 32 > RXCAP) { uns_err(c, "packet too large to send"); return -1; }
    p[0] = (unsigned char)((unsigned)plen >> 24); p[1] = (unsigned char)((unsigned)plen >> 16);
    p[2] = (unsigned char)((unsigned)plen >> 8);  p[3] = (unsigned char)plen;
    p[4] = (unsigned char)pad;
    us_copy(p + 5, pay, n);
    if (!tls_entropy_get(p + 5 + n, pad)) { uns_err(c, "no entropy source for padding"); return -1; }

    if (c->encrypted) {
        mac_compute(c->mac_out, c->seq_out, p, total, p + total);
        ctr_run(c, 1, p, total);
        if (tx_all(c, p, total + 32) < 0) { uns_err(c, "send failed"); return -1; }
    } else {
        if (tx_all(c, p, total) < 0) { uns_err(c, "send failed"); return -1; }
    }
    c->seq_out++;
    return 0;
}

/* Reads ONE packet into c->pay / c->paylen. 0 = got one, -1 = error. */
static int recv_packet_raw(ssh_conn *c, int timeout_ms)
{
    unsigned char *p = c->rx;
    int plen, total, pad;

    if (c->encrypted) {
        if (rx_exact(c, p, 16, timeout_ms) < 0) { uns_err(c, "recv timed out"); return -1; }
        ctr_run(c, 0, p, 16);
    } else {
        if (rx_exact(c, p, 8, timeout_ms) < 0) { uns_err(c, "recv timed out"); return -1; }
    }
    plen = (int)(((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
                 ((unsigned)p[2] << 8) | (unsigned)p[3]);
    total = 4 + plen;
    if (plen < 12 || total > RXCAP - 32) { uns_err(c, "absurd packet length"); return -1; }
    if (c->encrypted && (total & 15)) { uns_err(c, "packet not a whole block"); return -1; }

    {   int have = c->encrypted ? 16 : 8;
        if (total > have) {
            if (rx_exact(c, p + have, total - have, timeout_ms < 200 ? 200 : timeout_ms) < 0) {
                uns_err(c, "short packet"); return -1;
            }
            if (c->encrypted) ctr_run(c, 0, p + have, total - have);
        }
    }
    if (c->encrypted) {
        unsigned char want[32], got[32];
        if (rx_exact(c, got, 32, 200) < 0) { uns_err(c, "no MAC"); return -1; }
        mac_compute(c->mac_in, c->seq_in, p, total, want);
        {   int i, diff = 0;
            for (i = 0; i < 32; i++) diff |= want[i] ^ got[i];
            if (diff) { uns_err(c, "MAC mismatch"); return -1; } }
    }
    pad = p[4];
    c->paylen = plen - 1 - pad;
    if (c->paylen < 0 || pad < 4) { uns_err(c, "bad padding"); return -1; }
    c->pay = p + 5;
    c->seq_in++;
    return 0;
}

/* ...and the same, with the three transport messages that can arrive at any
 * time handled here so no caller has to think about them. */
int uns_recv(ssh_conn *c, int timeout_ms)
{
    int guard;
    for (guard = 0; guard < 32; guard++) {
        if (recv_packet_raw(c, timeout_ms) < 0) return -1;
        if (c->paylen < 1) continue;
        if (c->pay[0] == MSG_IGNORE || c->pay[0] == MSG_DEBUG) continue;
        if (c->pay[0] == MSG_DISCONNECT) { uns_err(c, "server disconnected"); return -1; }
        return 0;
    }
    uns_err(c, "too many transport messages");
    return -1;
}

/* ---- name lists ---------------------------------------------------------- */
static int namelist_has(const unsigned char *list, int len, const char *want)
{
    int wl = us_len(want), i = 0, start = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || list[i] == ',') {
            if (i - start == wl) {
                int k, ok = 1;
                for (k = 0; k < wl; k++) if (list[start + k] != (unsigned char)want[k]) ok = 0;
                if (ok) return 1;
            }
            start = i + 1;
        }
    }
    return 0;
}

/* macros, not pointers: check_kexinit() puts them in a static initializer */
#define kKex   "curve25519-sha256,curve25519-sha256@libssh.org"
#define kHost  "ssh-ed25519"
#define kCiph  "aes256-ctr"
#define kMac   "hmac-sha2-256"
#define kNone  "none"

static int build_kexinit(ssh_conn *c, unsigned char *out, int cap, int *n)
{
    ssh_buf b;
    unsigned char cookie[16];
    if (!tls_entropy_get(cookie, 16)) { uns_err(c, "no entropy source"); return -1; }
    ssh_buf_init(&b, out, cap);
    ssh_put_u8(&b, MSG_KEXINIT);
    ssh_put_raw(&b, cookie, 16);
    ssh_put_cstr(&b, kKex);  ssh_put_cstr(&b, kHost);
    ssh_put_cstr(&b, kCiph); ssh_put_cstr(&b, kCiph);
    ssh_put_cstr(&b, kMac);  ssh_put_cstr(&b, kMac);
    ssh_put_cstr(&b, kNone); ssh_put_cstr(&b, kNone);
    ssh_put_cstr(&b, "");    ssh_put_cstr(&b, "");
    ssh_put_u8(&b, 0);                 /* first_kex_packet_follows */
    ssh_put_u32(&b, 0);
    if (b.err) { uns_err(c, "kexinit did not fit"); return -1; }
    *n = b.len;
    return 0;
}

/* Confirm the server offers everything we picked. Failing here with a specific
 * name beats failing later with "invalid signature". */
static int check_kexinit(ssh_conn *c, const unsigned char *p, int n)
{
    ssh_rd r;
    const unsigned char *l;
    int ln, i;
    static const char *want[6] = { 0, kHost, kCiph, kCiph, kMac, kMac };
    static const char *what[6] = { "kex", "host key", "cipher c2s", "cipher s2c",
                                   "mac c2s", "mac s2c" };
    ssh_rd_init(&r, p, n);
    ssh_get_u8(&r);                    /* msg type */
    { int k; for (k = 0; k < 16; k++) ssh_get_u8(&r); }
    for (i = 0; i < 6; i++) {
        l = ssh_get_str(&r, &ln);
        if (r.err || !l) { uns_err(c, "malformed KEXINIT"); return -1; }
        if (i == 0) {
            if (!namelist_has(l, ln, "curve25519-sha256") &&
                !namelist_has(l, ln, "curve25519-sha256@libssh.org")) {
                uns_err(c, "server offers no curve25519-sha256"); return -1;
            }
        } else if (!namelist_has(l, ln, want[i])) {
            char m[96]; int k = 0, j;
            const char *a = "server offers no ";
            for (j = 0; a[j]; j++) m[k++] = a[j];
            for (j = 0; want[i][j]; j++) m[k++] = want[i][j];
            m[k++] = ' '; m[k++] = '(';
            for (j = 0; what[i][j]; j++) m[k++] = what[i][j];
            m[k++] = ')'; m[k] = 0;
            uns_err(c, m); return -1;
        }
    }
    return 0;
}

/* ---- the handshake ------------------------------------------------------- */
static int do_kex(ssh_conn *c)
{
    unsigned char ic[512], sec[32], qc[32], msg[128];
    unsigned char *is = 0;
    int icn, isn, rc = -1;

    /* 1. version exchange. The hash covers the idents WITHOUT their CR LF. */
    {   char line[64]; int i, n = 0;
        const char *id = SSH_IDENT;
        for (i = 0; id[i]; i++) line[n++] = id[i];
        line[n++] = '\r'; line[n++] = '\n';
        if (tx_all(c, (const unsigned char *)line, n) < 0) { uns_err(c, "ident send failed"); return -1; }
    }
    {   int n = 0, waited = 0;
        for (;;) {
            unsigned char ch;
            int r;
            net_poll();
            r = net_recv(c->sock, &ch, 1);
            if (r <= 0) {
                if (net_sock_state(c->sock) == TCP_CLOSED) { uns_err(c, "closed during ident"); return -1; }
                if (++waited > 15000) { uns_err(c, "no server ident"); return -1; }
                uno_pc64_delay_ms(1); continue;
            }
            waited = 0;
            if (ch == '\n') {
                c->v_s[n] = 0;
                if (n && c->v_s[n - 1] == '\r') { n--; c->v_s[n] = 0; }
                if (n >= 4 && c->v_s[0] == 'S' && c->v_s[1] == 'S' &&
                    c->v_s[2] == 'H' && c->v_s[3] == '-') break;
                n = 0;                 /* banner text before the ident */
                continue;
            }
            if (n < (int)sizeof c->v_s - 1) c->v_s[n++] = (char)ch;
        }
    }

    /* 2. KEXINIT both ways; both payloads are hashed verbatim. */
    if (build_kexinit(c, ic, (int)sizeof ic, &icn) < 0) return -1;
    if (uns_send(c, ic, icn) < 0) return -1;
    if (uns_recv(c, 15000) < 0) return -1;
    if (c->paylen < 1 || c->pay[0] != MSG_KEXINIT) { uns_err(c, "expected KEXINIT"); return -1; }
    isn = c->paylen;
    is = (unsigned char *)malloc((size_t)(isn > 0 ? isn : 1));
    if (!is) { uns_err(c, "out of memory"); return -1; }
    us_copy(is, c->pay, isn);
    if (check_kexinit(c, is, isn) < 0) goto done;

    /* 3. ECDH init / reply */
    if (!tls_entropy_get(sec, 32)) { uns_err(c, "no entropy source"); goto done; }
    if (!ssh_x25519_base(qc, sec)) { uns_err(c, "x25519 failed"); goto done; }
    {   ssh_buf b;
        ssh_buf_init(&b, msg, (int)sizeof msg);
        ssh_put_u8(&b, MSG_ECDH_INIT);
        ssh_put_str(&b, qc, 32);
        if (b.err || uns_send(c, msg, b.len) < 0) goto done;
    }
    if (uns_recv(c, 20000) < 0) goto done;
    if (c->paylen < 1 || c->pay[0] != MSG_ECDH_REPLY) { uns_err(c, "expected ECDH reply"); goto done; }

    {   ssh_rd r;
        const unsigned char *ks, *qs, *sig;
        int ksn, qsn, sign;
        unsigned char k[32], sigblob[64];
        ssh_exch e;

        ssh_rd_init(&r, c->pay, c->paylen);
        ssh_get_u8(&r);
        ks  = ssh_get_str(&r, &ksn);
        qs  = ssh_get_str(&r, &qsn);
        sig = ssh_get_str(&r, &sign);
        if (r.err || !ks || !qs || !sig || qsn != 32) { uns_err(c, "malformed ECDH reply"); goto done; }

        /* the host key blob: string "ssh-ed25519", string key(32) */
        {   ssh_rd kr; const unsigned char *ty, *kb; int tn, kn;
            ssh_rd_init(&kr, ks, ksn);
            ty = ssh_get_str(&kr, &tn); kb = ssh_get_str(&kr, &kn);
            if (kr.err || !ty || !kb || kn != 32 || !namelist_has(ty, tn, "ssh-ed25519")) {
                uns_err(c, "host key is not ssh-ed25519"); goto done;
            }
            us_copy(c->hostkey, kb, 32);
        }
        /* the signature blob: string "ssh-ed25519", string sig(64) */
        {   ssh_rd sr; const unsigned char *ty, *sb; int tn, sn2;
            ssh_rd_init(&sr, sig, sign);
            ty = ssh_get_str(&sr, &tn); sb = ssh_get_str(&sr, &sn2);
            if (sr.err || !ty || !sb || sn2 != 64 || !namelist_has(ty, tn, "ssh-ed25519")) {
                uns_err(c, "signature is not ssh-ed25519"); goto done;
            }
            us_copy(sigblob, sb, 64);
        }

        if (!ssh_x25519(k, sec, qs)) { uns_err(c, "x25519 failed"); goto done; }

        /* K is the raw X25519 output read as a big-endian integer - it goes
         * into the hash as an mpint, the one field that is not a string. */
        e.v_c = SSH_IDENT; e.v_s = c->v_s;
        e.i_c = ic;  e.i_c_len = icn;
        e.i_s = is;  e.i_s_len = isn;
        e.k_s = ks;  e.k_s_len = ksn;
        e.q_c = qc;  e.q_s = qs;
        e.k = k;     e.k_len = 32;
        ssh_exchange_hash(c->h, &e);

        if (!ed25519_verify(sigblob, c->h, 32, c->hostkey)) {
            uns_err(c, "host key signature did not verify"); goto done;
        }
        {   br_sha256_context sc;                 /* the known-hosts identity */
            br_sha256_init(&sc);
            br_sha256_update(&sc, ks, (size_t)ksn);
            br_sha256_out(&sc, c->hostfp); }
        if (!c->encrypted) us_copy(c->sess_id, c->h, 32);   /* first kex only */

        /* 4. NEWKEYS, then the keys come alive in both directions */
        {   unsigned char nk = MSG_NEWKEYS;
            if (uns_send(c, &nk, 1) < 0) goto done; }
        if (uns_recv(c, 15000) < 0) goto done;
        if (c->paylen < 1 || c->pay[0] != MSG_NEWKEYS) { uns_err(c, "expected NEWKEYS"); goto done; }

        {   unsigned char ivc[16], ivs[16], kc2[32], ks2[32];
            ssh_derive_key(ivc, 16, 'A', k, 32, c->h, c->sess_id);
            ssh_derive_key(ivs, 16, 'B', k, 32, c->h, c->sess_id);
            ssh_derive_key(kc2, 32, 'C', k, 32, c->h, c->sess_id);
            ssh_derive_key(ks2, 32, 'D', k, 32, c->h, c->sess_id);
            ssh_derive_key(c->mac_out, 32, 'E', k, 32, c->h, c->sess_id);
            ssh_derive_key(c->mac_in,  32, 'F', k, 32, c->h, c->sess_id);
            br_aes_ct64_ctr_init(&c->enc, kc2, 32);
            br_aes_ct64_ctr_init(&c->dec, ks2, 32);
            us_copy(c->iv_out, ivc, 12);
            us_copy(c->iv_in,  ivs, 12);
            c->cc_out = ((unsigned)ivc[12] << 24) | ((unsigned)ivc[13] << 16) |
                        ((unsigned)ivc[14] << 8)  | (unsigned)ivc[15];
            c->cc_in  = ((unsigned)ivs[12] << 24) | ((unsigned)ivs[13] << 16) |
                        ((unsigned)ivs[14] << 8)  | (unsigned)ivs[15];
            c->encrypted = 1; }
        rc = 0;
    }
done:
    if (is) free(is);
    return rc;
}

/* ---- public API ---------------------------------------------------------- */
int ssh_connect(const char *host, int port)
{
    int h, i, waited = 0;
    ssh_conn *c;
    unsigned char ip[4];

    for (h = 0; h < SSH_MAXCONN && g_conn[h].used; h++) ;
    if (h == SSH_MAXCONN) return -1;
    c = &g_conn[h];
    for (i = 0; i < (int)sizeof *c; i++) ((unsigned char *)c)[i] = 0;

    if (!net_dns_query(host, ip)) {
        int a, b2, d, e2;                        /* maybe it is a dotted quad */
        const char *s = host;
        a = b2 = d = e2 = -1;
        { int v = 0, n = 0, got = 0;
          for (;; s++) {
              if (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); n = 1; }
              else if (*s == '.' || *s == 0) {
                  if (!n) { got = -1; break; }
                  if      (a < 0) a = v; else if (b2 < 0) b2 = v;
                  else if (d < 0) d = v; else if (e2 < 0) e2 = v;
                  else { got = -1; break; }
                  v = 0; n = 0;
                  if (*s == 0) break;
              } else { got = -1; break; }
          }
          if (got < 0 || e2 < 0) { uns_err(c, "cannot resolve host"); return -1; } }
        ip[0] = (unsigned char)a; ip[1] = (unsigned char)b2;
        ip[2] = (unsigned char)d; ip[3] = (unsigned char)e2;
    }

    c->sock = net_socket(SOCK_TCP);
    if (c->sock < 0) { uns_err(c, "no socket"); return -1; }
    c->used = 1;
    c->rx = (unsigned char *)malloc(RXCAP);
    c->tx = (unsigned char *)malloc(RXCAP);
    c->rb = (unsigned char *)malloc(SSH_RBCAP);
    if (!c->rx || !c->tx || !c->rb) { uns_err(c, "out of memory"); ssh_close(h); return -1; }

    if (net_connect(c->sock, ip, (unsigned short)port) < 0) {
        uns_err(c, "connect failed"); ssh_close(h); return -1;
    }
    while (net_sock_state(c->sock) != TCP_ESTABLISHED) {
        net_poll();
        if (net_sock_state(c->sock) == TCP_CLOSED || net_sock_state(c->sock) == TCP_DONE) {
            uns_err(c, "connection refused"); ssh_close(h); return -1;
        }
        if (++waited > 15000) { uns_err(c, "connect timed out"); ssh_close(h); return -1; }
        uno_pc64_delay_ms(1);
    }
    return h;
}

int ssh_handshake(int handle)
{
    ssh_conn *c = uns_get(handle);
    if (!c) return -1;
    return do_kex(c);
}

int ssh_send(int handle, const unsigned char *payload, int n)
{
    ssh_conn *c = uns_get(handle);
    if (!c) return -1;
    return uns_send(c, payload, n);
}

int ssh_recv(int handle, const unsigned char **payload, int *n, int timeout_ms)
{
    ssh_conn *c = uns_get(handle);
    if (!c) return -1;
    if (uns_recv(c, timeout_ms) < 0) return -1;
    if (payload) *payload = c->pay;
    if (n) *n = c->paylen;
    return 0;
}

void ssh_close(int handle)
{
    ssh_conn *c = uns_get(handle);
    if (!c) return;
    if (c->sock >= 0) net_sock_close(c->sock);
    if (c->rx) free(c->rx);
    if (c->tx) free(c->tx);
    if (c->rb) free(c->rb);
    c->used = 0; c->rx = 0; c->tx = 0; c->rb = 0; c->sock = -1;
}

/* ---- the live gate -------------------------------------------------------
 * Registered into SPECTEST's existing "network" area. That needs no edit to
 * pc64_spectest.c and none to unoautomate: unoauto_test_register() takes the
 * suite name and that IS the registration, which is exactly the zero-edit path
 * HARNESS-POLICY documents.
 *
 * The target defaults to 10.0.2.2, which is the host as seen through QEMU's
 * user-mode networking - so the gate needs no LAN, no second machine and no
 * DHCP server of its own.
 *
 * Progress goes to the debug console as well as the TEST channel, because a
 * handshake that fails needs to say WHERE, and one grep-able line per step is
 * worth more here than a pass/fail count.
 * ======================================================================== */
#ifdef UNO_DEBUG
#include "unoauto.h"
#include "pc64_fs.h"
#include "tools/sshtestkey.h"

#ifndef SSH_TEST_HOST
#define SSH_TEST_HOST "10.0.2.2"
#endif
#ifndef SSH_TEST_PORT
#define SSH_TEST_PORT 22
#endif

#ifdef UNO_DBGCON
static void sd_ob(unsigned short p, unsigned char v)
{ __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(p)); }
static void sd_s(const char *s) { while (*s) sd_ob(0x402, (unsigned char)*s++); }
static void sd_hex(const unsigned char *d, int n)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        sd_ob(0x402, (unsigned char)hx[d[i] >> 4]);
        sd_ob(0x402, (unsigned char)hx[d[i] & 15]);
    }
}
#else
static void sd_s(const char *s) { (void)s; }
static void sd_hex(const unsigned char *d, int n) { (void)d; (void)n; }
#endif

static int ssh_t_transport(void *ctx)
{
    char host[64];
    int h, ok = 0, waited = 0, i;
    (void)ctx;
    for (i = 0; SSH_TEST_HOST[i] && i < (int)sizeof host - 1; i++) host[i] = SSH_TEST_HOST[i];
    host[i] = 0;

    /* Announce entry BEFORE anything that can block. Without this a stall in
     * the DHCP wait below is indistinguishable from the test never running. */
    sd_s("sshtest: begin\n");

    /* The target comes from \SSHTEST.CFG when it exists, so the harness can
     * retarget without a rebuild. It needs to: 10.0.2.2 is QEMU's alias for
     * the machine RUNNING qemu, which on this box is the WSL VM - while the
     * OpenSSH server is on Windows, one NAT hop further out. */
    {   unsigned char cfg[64];
        long got = uno_fs_read(0, "SSHTEST.CFG", cfg, (long)sizeof cfg - 1);
        int v;
        if (got <= 0) got = uno_fs_read(1, "SSHTEST.CFG", cfg, (long)sizeof cfg - 1);
        if (got > 0) {
            cfg[got] = 0;
            for (v = 0; v < (int)got && v < (int)sizeof host - 1; v++) {
                if (cfg[v] == '\r' || cfg[v] == '\n' || cfg[v] == ' ') break;
                host[v] = (char)cfg[v];
            }
            host[v] = 0;
        }
    }
    sd_s("sshtest: target="); sd_s(host); sd_s("\n");

    /* SPECTEST runs BEFORE the boot network test, so nothing has brought the
     * NIC up yet - the stack is lazy and pc64_net_up() is the entry point.
     * Without this the test reports a protocol failure that is really "there
     * was no network at all". */
    if (!pc64_net_up()) { sd_s("sshtest: no NIC\nsshtest: RESULT FAIL\n"); return 1; }
    while (!net_dhcp_done() && waited < 15000) {
        net_poll();
        uno_pc64_delay_ms(1);
        if (++waited % 3000 == 0) sd_s("sshtest: waiting for DHCP\n");
    }
    if (!net_dhcp_done()) { sd_s("sshtest: no DHCP lease\nsshtest: RESULT FAIL\n"); return 1; }
    sd_s("sshtest: link up, connecting\n");

    h = ssh_connect(host, SSH_TEST_PORT);
    if (h < 0) { sd_s("sshtest: connect failed: "); sd_s(ssh_error(h)); sd_s("\nsshtest: RESULT FAIL\n"); return 1; }

    if (ssh_handshake(h) == 0 && ssh_is_encrypted(h)) {
        const unsigned char *fp = ssh_host_fingerprint(h);
        const unsigned char *sid = ssh_session_id(h);
        int i, nz = 0;
        for (i = 0; i < 32; i++) nz |= sid[i];
        sd_s("sshtest: server="); sd_s(ssh_server_ident(h)); sd_s("\n");
        sd_s("sshtest: hostkey-fp="); sd_hex(fp, 8); sd_s("\n");
        sd_s("sshtest: session-id="); sd_hex(sid, 8); sd_s("\n");
        ok = (nz != 0);
        if (!ok) sd_s("sshtest: session id is all zero\n");
    } else {
        sd_s("sshtest: handshake failed: "); sd_s(ssh_error(h)); sd_s("\n");
    }
    ssh_close(h);
    sd_s(ok ? "sshtest: RESULT PASS\n" : "sshtest: RESULT FAIL\n");
    return ok ? 0 : 1;      /* SPECTEST reads 0 as PASS */
}

/* ssh-c: authenticate with a key and run a command. The target and the user
 * come from \SSHTEST.CFG ("host [user]"); the key is the throwaway seed in
 * tools/sshtestkey.h, which the harness has put in the test sshd's
 * authorized_keys using OUR OWN ed25519 to derive the public half. */
static int ssh_t_exec(void *ctx)
{
    char host[64], user[32];
    int h, ok = 0, waited = 0, i, got = 0, port = SSH_TEST_PORT;
    static char out[512];
    (void)ctx;

    for (i = 0; SSH_TEST_HOST[i] && i < (int)sizeof host - 1; i++) host[i] = SSH_TEST_HOST[i];
    host[i] = 0;
    user[0] = 0;
    sd_s("sshexec: begin\n");
    if (!pc64_net_up()) { sd_s("sshexec: no NIC\nsshexec: RESULT FAIL\n"); return 1; }
    while (!net_dhcp_done() && waited < 15000) { net_poll(); uno_pc64_delay_ms(1); waited++; }
    if (!net_dhcp_done()) { sd_s("sshexec: no DHCP\nsshexec: RESULT FAIL\n"); return 1; }

    {   unsigned char cfg[96];
        long n = uno_fs_read(0, "SSHTEST.CFG", cfg, (long)sizeof cfg - 1);
        int k = 0, v;
        if (n <= 0) n = uno_fs_read(1, "SSHTEST.CFG", cfg, (long)sizeof cfg - 1);
        if (n > 0) {
            cfg[n] = 0;
            for (v = 0; v < (int)n && cfg[v] > ' ' && k < (int)sizeof host - 1; v++)
                host[k++] = (char)cfg[v];
            host[k] = 0;
            while (v < (int)n && (cfg[v] == ' ' || cfg[v] == '\t')) v++;
            k = 0;
            for (; v < (int)n && cfg[v] > ' ' && k < (int)sizeof user - 1; v++)
                user[k++] = (char)cfg[v];
            user[k] = 0;
            while (v < (int)n && (cfg[v] == ' ' || cfg[v] == '\t')) v++;
            if (v < (int)n && cfg[v] >= '0' && cfg[v] <= '9') {
                port = 0;
                for (; v < (int)n && cfg[v] >= '0' && cfg[v] <= '9'; v++)
                    port = port * 10 + (cfg[v] - '0');
            }
        }
    }
    if (!user[0]) { sd_s("sshexec: no user in SSHTEST.CFG\nsshexec: RESULT FAIL\n"); return 1; }
    sd_s("sshexec: target="); sd_s(host); sd_s(" user="); sd_s(user); sd_s("\n");

    h = ssh_connect(host, port);
    if (h < 0) { sd_s("sshexec: connect: "); sd_s(ssh_error(h)); sd_s("\nsshexec: RESULT FAIL\n"); return 1; }
    if (ssh_handshake(h) != 0) {
        sd_s("sshexec: handshake: "); sd_s(ssh_error(h));
        sd_s("\nsshexec: RESULT FAIL\n"); ssh_close(h); return 1;
    }
    sd_s("sshexec: transport up\n");

    i = ssh_auth_key(h, user, kSshTestSeed);
    if (i != 0) {
        sd_s("sshexec: auth: "); sd_s(ssh_error(h));
        sd_s("\nsshexec: RESULT FAIL\n"); ssh_close(h); return 1;
    }
    sd_s("sshexec: authenticated\n");

    if (ssh_exec(h, "echo unodos-ssh-ok; exit 7") != 0) {
        sd_s("sshexec: exec: "); sd_s(ssh_error(h));
        sd_s("\nsshexec: RESULT FAIL\n"); ssh_close(h); return 1;
    }
    for (waited = 0; waited < 20000; waited++) {
        int n;
        ssh_poll(h);
        n = ssh_read(h, out + got, (int)sizeof out - 1 - got);
        if (n > 0) got += n;
        if (!ssh_channel_open(h) && n <= 0) break;
        uno_pc64_delay_ms(1);
    }
    out[got] = 0;
    sd_s("sshexec: output="); sd_s(out); sd_s("\n");
    sd_s("sshexec: exit=");
    {   int st = ssh_exit_status(h), d, started = 0;
        if (st < 0) { sd_s("-1"); }
        else for (d = 100; d >= 1; d /= 10) {
            int dig = (st / d) % 10;
            if (dig || started || d == 1) { char t[2]; t[0] = (char)('0' + dig); t[1] = 0; sd_s(t); started = 1; }
        }
    }
    sd_s("\n");

    {   const char *want = "unodos-ssh-ok";
        int k;
        for (k = 0; k + 13 <= got; k++) {
            int j, m = 1;
            for (j = 0; j < 13; j++) if (out[k + j] != want[j]) { m = 0; break; }
            if (m) { ok = 1; break; }
        }
    }
    if (ok && ssh_exit_status(h) != 7) { sd_s("sshexec: wrong exit status\n"); ok = 0; }
    ssh_close(h);
    sd_s(ok ? "sshexec: RESULT PASS\n" : "sshexec: RESULT FAIL\n");
    return ok ? 0 : 1;
}

/* ssh-d: the store must survive a power cycle. There is no "which boot is
 * this" flag anywhere, so the store answers that itself - an empty one means
 * seed it, a populated one means verify it. The harness boots the same real
 * FAT image twice and reads the two verdicts. */
static int ssh_t_store(void *ctx)
{
    char name[SSH_NAMELEN], pub[128], host[SSH_HOSTLEN], user[SSH_NAMELEN];
    unsigned char seed[32], fp[32];
    int i, port = 0, bad = 0;
    (void)ctx;

    sd_s("sshstore: begin\n");
    sd_s(ssh_store_persistent() ? "sshstore: volume=native\n"
                                : "sshstore: volume=RAMDISK (will not persist)\n");
    for (i = 0; i < 32; i++) fp[i] = (unsigned char)(i + 1);

    if (ssh_key_list(0, name, sizeof name, 0, 0) != 0) {
        /* ---- first boot: seed it -------------------------------------- */
        sd_s("sshstore: seeding\n");
        if (ssh_key_generate("gate", "pw") != 0) { sd_s("sshstore: generate failed\n"); return 1; }
        sd_s("sshstore: key generated\n");
        if (ssh_sess_set("s1", "10.0.2.2", 2222, "unosshtest", "gate") != 0)
            { sd_s("sshstore: session save failed\n"); return 1; }
        if (ssh_known_add("store.test.invalid", fp) != 0) { sd_s("sshstore: known add failed\n"); return 1; }
        sd_s("sshstore: session+host saved\n");
        {   static unsigned char pem[2048];   /* static: 2 KB of stack in a SPECTEST test is enough to run off the frame */
            long n;
            n = uno_fs_read(0, "SSHIMP.KEY", pem, (long)sizeof pem - 1);
            if (n <= 0) n = uno_fs_read(1, "SSHIMP.KEY", pem, (long)sizeof pem - 1);
            sd_s(n > 0 ? "sshstore: read ok\n" : "sshstore: read empty\n");
            if (n > 0) {
                int r;
                r = ssh_key_import("imp", (const char *)pem, (int)n, "");
                sd_s(r == 0 ? "sshstore: imported an OpenSSH key\n"
                            : "sshstore: import FAILED\n");
                if (r == 0 && ssh_key_export_pub("imp", pub, (int)sizeof pub) > 0) {
                    sd_s("sshstore: imported-pub="); sd_s(pub); sd_s("\n");
                }
            } else sd_s("sshstore: no SSHIMP.KEY to import\n");
        }
        if (ssh_key_export_pub("gate", pub, (int)sizeof pub) > 0) {
            sd_s("sshstore: gate-pub="); sd_s(pub); sd_s("\n");
        }
        sd_s("sshstore: RESULT SEEDED\n");
        return 0;
    }

    /* ---- a later boot: everything must still be here ------------------- */
    if (ssh_key_load("gate", "pw", seed) != 0) { sd_s("sshstore: key lost\n"); bad = 1; }
    if (ssh_key_load("gate", "wrong", seed) != -2) { sd_s("sshstore: bad passphrase accepted!\n"); bad = 1; }
    if (ssh_sess_get("s1", host, sizeof host, &port, user, sizeof user, 0, 0) != 0) {
        sd_s("sshstore: session lost\n"); bad = 1;
    } else {
        sd_s("sshstore: session="); sd_s(host); sd_s(" user="); sd_s(user); sd_s("\n");
        if (port != 2222) { sd_s("sshstore: port wrong\n"); bad = 1; }
    }
    if (ssh_known_check("store.test.invalid", fp) != SSH_HOST_KNOWN) { sd_s("sshstore: host forgotten\n"); bad = 1; }
    fp[0] ^= 0xFF;
    if (ssh_known_check("store.test.invalid", fp) != SSH_HOST_MISMATCH) { sd_s("sshstore: mismatch not caught!\n"); bad = 1; }
    fp[0] ^= 0xFF;
    if (ssh_known_check("nowhere.invalid", fp) != SSH_HOST_UNKNOWN) { sd_s("sshstore: unknown host claimed\n"); bad = 1; }
    if (ssh_key_export_pub("gate", pub, (int)sizeof pub) > 0) {
        sd_s("sshstore: gate-pub="); sd_s(pub); sd_s("\n");
    }
    if (ssh_key_export_pub("imp", pub, (int)sizeof pub) > 0) {
        sd_s("sshstore: imported-pub="); sd_s(pub); sd_s("\n");
    }
    sd_s(bad ? "sshstore: RESULT FAIL\n" : "sshstore: RESULT VERIFIED\n");
    return bad ? 1 : 0;
}

/* ssh-e: the automation verb, driven DIRECTLY rather than over URC.
 *
 * unoautomate's dispatch clause is their commit, not ours - the request is
 * filed - so there is no URC path to this yet. Driving ssh_dbg_cmd() here
 * tests exactly what this lane owns: the sub-verb grammar, and the slicing
 * that exists because their tx buffer is 8 KB and drops silently past it.
 * The command below deliberately produces MORE than 8 KB, which is the whole
 * reason `run` returns an id instead of the output. */
static int ssh_t_verb(void *ctx)
{
    static char acc[32768];
    char r[2048];
    int n, id = 0, len = 0, off = 0, got = 0, bad = 0, waited = 0;
    (void)ctx;

    sd_s("sshverb: begin\n");
    if (!pc64_net_up()) { sd_s("sshverb: no NIC\nsshverb: RESULT FAIL\n"); return 1; }
    while (!net_dhcp_done() && waited < 15000) { net_poll(); uno_pc64_delay_ms(1); waited++; }
    if (!net_dhcp_done()) { sd_s("sshverb: no DHCP\nsshverb: RESULT FAIL\n"); return 1; }

    /* the key the harness authorised, stored unprotected so the verb - which
     * has nobody to ask for a passphrase - can load it */
    if (ssh_key_add("auto", kSshTestSeed, "") != 0) { sd_s("sshverb: key add failed\n"); return 1; }

    n = ssh_dbg_cmd("keys", r, (int)sizeof r);
    sd_s("sshverb: keys -> "); sd_s(r); sd_s("\n");   /* r carries the reason even on failure */

    n = ssh_dbg_cmd("sessadd t1 10.0.2.2 2222 unosshtest auto", r, (int)sizeof r);
    sd_s("sshverb: sessadd -> "); sd_s(r); sd_s("\n");   /* r carries the reason even on failure */
    if (n <= 0) bad = 1;
    n = ssh_dbg_cmd("sess", r, (int)sizeof r);
    sd_s("sshverb: sess -> "); sd_s(r); sd_s("\n");   /* r carries the reason even on failure */

    /* seq 1 2000 is a shade under 9 KB - past the 8 KB tx buffer on purpose */
    n = ssh_dbg_cmd("run t1 seq 1 2000", r, (int)sizeof r);
    sd_s("sshverb: run -> "); sd_s(r); sd_s("\n");   /* r carries the reason even on failure */
    if (n <= 0) { sd_s("sshverb: RESULT FAIL\n"); return 1; }
    {   const char *s = r;
        while (*s && !(s[0] == 'i' && s[1] == 'd' && s[2] == '=')) s++;
        if (*s) { s += 3; while (*s >= '0' && *s <= '9') id = id * 10 + (*s++ - '0'); }
        while (*s && !(s[0] == 'l' && s[1] == 'e' && s[2] == 'n' && s[3] == '=')) s++;
        if (*s) { s += 4; while (*s >= '0' && *s <= '9') len = len * 10 + (*s++ - '0'); } }
    if (len <= 8192) { sd_s("sshverb: output did not exceed the tx buffer\n"); bad = 1; }

    while (off < len && got < (int)sizeof acc) {
        char q[64];
        int k = 0, j;
        const char *pre = "get ";
        for (j = 0; pre[j]; j++) q[k++] = pre[j];
        {   int v = id, t[8], ti = 0;
            if (!v) t[ti++] = 0;
            while (v) { t[ti++] = v % 10; v /= 10; }
            while (ti) q[k++] = (char)('0' + t[--ti]); }
        q[k++] = ' ';
        {   int v = off, t[8], ti = 0;
            if (!v) t[ti++] = 0;
            while (v) { t[ti++] = v % 10; v /= 10; }
            while (ti) q[k++] = (char)('0' + t[--ti]); }
        q[k] = 0;
        n = ssh_dbg_cmd(q, r, (int)sizeof r);
        if (n <= 0) { sd_s("sshverb: get failed\n"); bad = 1; break; }
        for (j = 0; j < n && got < (int)sizeof acc; j++) acc[got++] = r[j];
        off += n;
    }
    sd_s("sshverb: reassembled=");
    {   int v = got, t[8], ti = 0; char d[10]; int k = 0;
        if (!v) t[ti++] = 0;
        while (v) { t[ti++] = v % 10; v /= 10; }
        while (ti) d[k++] = (char)('0' + t[--ti]);
        d[k] = 0; sd_s(d); }
    sd_s("\n");
    if (got != len) { sd_s("sshverb: slices did not reassemble to len\n"); bad = 1; }
    if (got < 4 || acc[0] != '1' || acc[1] != '\n') { sd_s("sshverb: output does not start at 1\n"); bad = 1; }
    {   /* ...and ends at 2000. The window starts at got-4, not got-6: the last
         * line is "2000\n", so the '2' sits four back from the end and a
         * window that began further in never saw it. */
        int k, found = 0;
        for (k = got - 4; k >= 0 && k > got - 16; k--)
            if (acc[k] == '2' && acc[k+1] == '0' && acc[k+2] == '0' && acc[k+3] == '0') found = 1;
        if (!found) { sd_s("sshverb: output does not end at 2000\n"); bad = 1; } }

    sd_s(bad ? "sshverb: RESULT FAIL\n" : "sshverb: RESULT PASS\n");
    return bad ? 1 : 0;
}

/* ssh-f: the app. Drives the same functions a click drives - the app's own
 * connect, pump and close - so this is the app being exercised, not a copy of
 * it. What a pointer would add is proof it RENDERS, and the harness takes a
 * screenshot for exactly that. */
int  pc64_sshapp_dbg_select(const char *name);
int  pc64_sshapp_dbg_connect(void);
void pc64_sshapp_dbg_pump(void);
void pc64_sshapp_dbg_close(void);
int  pc64_sshapp_dbg_tabs(void);
int  pc64_sshapp_dbg_cur(void);
int  pc64_sshapp_dbg_textlen(void);
const char *pc64_sshapp_dbg_status(void);
void pc64_dbg_open_ssh(void);

static int ssh_t_app(void *ctx)
{
    int waited = 0, bad = 0, tabs0, i;
    (void)ctx;

    sd_s("sshapp: begin\n");
    if (!pc64_net_up()) { sd_s("sshapp: no NIC\nsshapp: RESULT FAIL\n"); return 1; }
    while (!net_dhcp_done() && waited < 15000) { net_poll(); uno_pc64_delay_ms(1); waited++; }
    if (!net_dhcp_done()) { sd_s("sshapp: no DHCP\nsshapp: RESULT FAIL\n"); return 1; }

    /* The app needs a session whose key it can open unattended. The store
     * gate's `s1` uses a passphrase-protected key on purpose, and the app is
     * right to refuse it, so this makes its own. */
    if (ssh_key_add("appkey", kSshTestSeed, "") != 0) { sd_s("sshapp: key add failed\n"); return 1; }
    if (ssh_sess_set("app", "10.0.2.2", 2222, "unosshtest", "appkey") != 0)
        { sd_s("sshapp: session save failed\n"); return 1; }
    if (!pc64_sshapp_dbg_select("app")) { sd_s("sshapp: session not selectable\n"); return 1; }

    tabs0 = pc64_sshapp_dbg_tabs();
    if (!pc64_sshapp_dbg_connect()) {
        sd_s("sshapp: connect: "); sd_s(pc64_sshapp_dbg_status());
        sd_s("\nsshapp: RESULT FAIL\n"); return 1;
    }
    sd_s("sshapp: status="); sd_s(pc64_sshapp_dbg_status()); sd_s("\n");
    if (pc64_sshapp_dbg_tabs() != tabs0 + 1) { sd_s("sshapp: no new tab\n"); bad = 1; }
    if (pc64_sshapp_dbg_cur() <= 0) { sd_s("sshapp: did not switch to it\n"); bad = 1; }

    for (i = 0; i < 8000 && pc64_sshapp_dbg_textlen() == 0; i++) {
        pc64_sshapp_dbg_pump();
        uno_pc64_delay_ms(1);
    }
    sd_s("sshapp: term-bytes=");
    {   int v = pc64_sshapp_dbg_textlen(), t[8], ti = 0, k = 0; char d[10];
        if (!v) t[ti++] = 0;
        while (v) { t[ti++] = v % 10; v /= 10; }
        while (ti) d[k++] = (char)('0' + t[--ti]);
        d[k] = 0; sd_s(d); }
    sd_s("\n");
    if (pc64_sshapp_dbg_textlen() == 0) { sd_s("sshapp: no output in the terminal pane\n"); bad = 1; }

    if (!pc64_sshapp_dbg_connect()) { sd_s("sshapp: second connect failed\n"); bad = 1; }
    else if (pc64_sshapp_dbg_tabs() != tabs0 + 2) { sd_s("sshapp: second tab missing\n"); bad = 1; }
    pc64_sshapp_dbg_close();
    if (pc64_sshapp_dbg_tabs() != tabs0 + 1) { sd_s("sshapp: close did not remove a tab\n"); bad = 1; }
    if (pc64_sshapp_dbg_cur() != 0) { sd_s("sshapp: close did not fall back to Manage\n"); bad = 1; }

    /* leave the window UP so the harness photographs the real thing */
    pc64_dbg_open_ssh();
    sd_s(bad ? "sshapp: RESULT FAIL\n" : "sshapp: RESULT PASS\n");
    return bad ? 1 : 0;
}

void unossh_register_tests(void)
{
    unoauto_test_register("network", "ssh:transport", ssh_t_transport);
    unoauto_test_register("network", "ssh:exec", ssh_t_exec);
    unoauto_test_register("network", "ssh:store", ssh_t_store);
    unoauto_test_register("network", "ssh:verb", ssh_t_verb);
    unoauto_test_register("network", "ssh:app", ssh_t_app);
}
#else
void unossh_register_tests(void) { }
#endif

const char *ssh_error(int handle)
{ ssh_conn *c = uns_get(handle); return c ? c->err : g_last_err; }
const char *ssh_server_ident(int handle)
{ ssh_conn *c = uns_get(handle); return c ? c->v_s : ""; }
const unsigned char *ssh_host_fingerprint(int handle)
{ ssh_conn *c = uns_get(handle); return c ? c->hostfp : 0; }
const unsigned char *ssh_session_id(int handle)
{ ssh_conn *c = uns_get(handle); return c ? c->sess_id : 0; }
int ssh_is_encrypted(int handle)
{ ssh_conn *c = uns_get(handle); return c ? c->encrypted : 0; }
