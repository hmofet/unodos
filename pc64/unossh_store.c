/* ===========================================================================
 * unossh - the persistent store: private keys, saved sessions, known hosts.
 *
 * ONE container file rather than three. The spec asked for unossh_keys.c and
 * unossh_sess.c, but all three tables want the same volume, the same save
 * path and the same "written whole, atomically" behaviour, and splitting them
 * would have meant three copies of that with three chances to disagree about
 * which volume is the persistent one.
 *
 * THE VOLUME MATTERS MORE THAN ANYTHING ELSE HERE. "The first writable volume"
 * is volume 0, the RAM disk, so a store written there is gone at power-off and
 * every save appears to work. The WM lane shipped exactly that bug in
 * session_save() and nobody noticed until a reboot test went looking. pick_vol
 * below prefers a native FAT partition the way unosecure.c does, and falls
 * back to the RAM disk only when there is nothing else - in which case
 * ssh_store_persistent() says so, so a caller can warn instead of pretending.
 *
 * PRIVATE KEYS ARE ENCRYPTED AT REST with PBKDF2-HMAC-SHA256 into AES-256-CTR
 * plus an HMAC, all from BearSSL directly. unosecure.c has its own PBKDF2 but
 * it is `static`, so this is a second implementation by necessity rather than
 * by choice.
 *
 * The public half is cached in the clear beside each entry: listing keys,
 * showing fingerprints and exporting an authorized_keys line are all things a
 * UI does before anyone has typed a passphrase.
 * ======================================================================== */
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "bearssl_block.h"
#include "pc64_fs.h"
#include "tls_entropy.h"
#include "ed25519.h"
#include "unossh.h"
#include "unossh_int.h"      /* ssh_conn, uns_get: record the host-key check */

#define STORE_FILE "SSHSTORE.DAT"
#define STORE_MAGIC "UNOSSH01"
/* PBKDF2-HMAC-SHA256 work factor for at-rest private-key protection. Raised
 * from a token 4096 - which a modern GPU clears at billions of guesses a
 * second - to a figure that makes an offline dictionary attack on a stolen
 * SSHSTORE.DAT cost real time per candidate. It is paid once per key load or
 * save, not per packet. The AES-CTR zero IV in key_crypt() below is safe
 * BECAUSE each entry carries its own 16-byte random salt, so the derived key
 * (and thus the keystream) never repeats across entries - leave it. */
#define PBKDF2_ITERS 200000

typedef struct {
    char name[SSH_NAMELEN];
    unsigned char salt[16], ct[32], mac[32], pub[32];
    int used, guarded;             /* guarded = a non-empty passphrase */
} key_ent;

typedef struct {
    char name[SSH_NAMELEN], host[SSH_HOSTLEN];
    char user[SSH_NAMELEN], key[SSH_NAMELEN];
    int port, used;
} sess_ent;

typedef struct {
    char host[SSH_HOSTLEN];
    unsigned char fp[32];
    int used;
} host_ent;

typedef struct {
    char magic[8];
    int version;
    key_ent  keys[SSH_MAXKEYS];
    sess_ent sess[SSH_MAXSESS];
    host_ent hosts[SSH_MAXHOSTS];
} ssh_store;

static ssh_store g_db;
static int g_vol = -2, g_loaded;

/* ---- small helpers ------------------------------------------------------- */
static void sk_copy(unsigned char *d, const unsigned char *s, int n)
{ int i; for (i = 0; i < n; i++) d[i] = s[i]; }
static void sk_zero(unsigned char *d, int n) { int i; for (i = 0; i < n; i++) d[i] = 0; }
static int  sk_eq(const char *a, const char *b)
{ int i; for (i = 0; a[i] && b[i]; i++) if (a[i] != b[i]) return 0; return a[i] == b[i]; }
static void sk_set(char *d, int cap, const char *s)
{ int i; for (i = 0; i < cap - 1 && s && s[i]; i++) d[i] = s[i]; d[i] = 0;
  for (++i; i < cap; i++) d[i] = 0; }

static int pick_vol(void)
{
    int n = uno_fs_volumes(), v;
    for (v = 1; v < n; v++)                       /* a real partition first */
        if (uno_fs_kind(v) == 1 && uno_fs_writable(v)) return v;
    for (v = 1; v < n; v++)
        if (uno_fs_writable(v)) return v;
    if (n > 0 && uno_fs_writable(0)) return 0;    /* the RAM disk: NOT persistent */
    return -1;
}

static int magic_ok(void)
{ int i; for (i = 0; i < 8; i++) if (g_db.magic[i] != STORE_MAGIC[i]) return 0; return 1; }

static void store_load(void)
{
    long sz;
    if (g_loaded) return;
    g_loaded = 1;
    g_vol = pick_vol();
    if (g_vol < 0) return;
    sz = uno_fs_read(g_vol, STORE_FILE, (unsigned char *)&g_db, (long)sizeof g_db);
    /* magic is 8 chars with NO terminator, so it must be compared as 8 bytes;
     * a string compare truncates it to 7 and never matches, which silently
     * wipes the store on every single load. */
    if (sz != (long)sizeof g_db || !magic_ok() || g_db.version != 1) {
        sk_zero((unsigned char *)&g_db, (int)sizeof g_db);
        for (sz = 0; sz < 8; sz++) g_db.magic[sz] = STORE_MAGIC[sz];
        g_db.version = 1;
    }
}

static int store_save(void)
{
    store_load();
    if (g_vol < 0) return -1;
    { int i; for (i = 0; i < 8; i++) g_db.magic[i] = STORE_MAGIC[i]; }
    g_db.version = 1;
    return uno_fs_write(g_vol, STORE_FILE, (const unsigned char *)&g_db,
                        (long)sizeof g_db) >= 0 ? 0 : -1;
}

int ssh_store_persistent(void)
{ store_load(); return g_vol > 0; }

/* ---- PBKDF2-HMAC-SHA256 --------------------------------------------------
 * unosecure.c has one of these and it is static, so this is a second copy by
 * necessity. Straight RFC 2898: T_i = F(P, S, c, i), and one block is enough
 * for the 64 bytes wanted here (32 to encrypt with, 32 to authenticate with). */
static void pbkdf2(const char *pass, const unsigned char *salt, int saltlen,
                   int iters, unsigned char *out, int outlen)
{
    br_hmac_key_context kc;
    int plen = 0, done = 0;
    unsigned block = 1;
    while (pass && pass[plen]) plen++;
    br_hmac_key_init(&kc, &br_sha256_vtable, pass, (size_t)plen);
    while (done < outlen) {
        unsigned char u[32], t[32], idx[4];
        br_hmac_context hc;
        int i, j, n;
        idx[0] = (unsigned char)(block >> 24); idx[1] = (unsigned char)(block >> 16);
        idx[2] = (unsigned char)(block >> 8);  idx[3] = (unsigned char)block;
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, salt, (size_t)saltlen);
        br_hmac_update(&hc, idx, 4);
        br_hmac_out(&hc, u);
        sk_copy(t, u, 32);
        for (i = 1; i < iters; i++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, 32);
            br_hmac_out(&hc, u);
            for (j = 0; j < 32; j++) t[j] ^= u[j];
        }
        n = outlen - done; if (n > 32) n = 32;
        sk_copy(out + done, t, n);
        done += n;
        block++;
    }
}

static void key_crypt(const char *pass, const unsigned char salt[16],
                      unsigned char data[32], unsigned char mackey[32])
{
    unsigned char kd[64], iv[12];
    br_aes_ct64_ctr_keys ck;
    pbkdf2(pass, salt, 16, PBKDF2_ITERS, kd, 64);
    sk_zero(iv, 12);
    br_aes_ct64_ctr_init(&ck, kd, 32);
    br_aes_ct64_ctr_run(&ck, iv, 0, data, 32);
    sk_copy(mackey, kd + 32, 32);
    sk_zero(kd, 64);
}

static void key_mac(const unsigned char mackey[32], const unsigned char salt[16],
                    const unsigned char ct[32], unsigned char out[32])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha256_vtable, mackey, 32);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, salt, 16);
    br_hmac_update(&hc, ct, 32);
    br_hmac_out(&hc, out);
}

/* ---- keys ---------------------------------------------------------------- */
static key_ent *key_find(const char *name)
{
    int i;
    store_load();
    for (i = 0; i < SSH_MAXKEYS; i++)
        if (g_db.keys[i].used && sk_eq(g_db.keys[i].name, name)) return &g_db.keys[i];
    return 0;
}

int ssh_key_add(const char *name, const unsigned char seed[32], const char *pass)
{
    key_ent *k = key_find(name);
    unsigned char mackey[32], tmp[32];
    int i;
    store_load();
    if (!k) {
        for (i = 0; i < SSH_MAXKEYS && g_db.keys[i].used; i++) ;
        if (i == SSH_MAXKEYS) return -1;
        k = &g_db.keys[i];
    }
    sk_zero((unsigned char *)k, (int)sizeof *k);
    sk_set(k->name, SSH_NAMELEN, name);
    if (!tls_entropy_get(k->salt, 16)) return -1;
    ed25519_pubkey(k->pub, seed);
    sk_copy(tmp, seed, 32);
    key_crypt(pass, k->salt, tmp, mackey);
    sk_copy(k->ct, tmp, 32);
    key_mac(mackey, k->salt, k->ct, k->mac);
    k->used = 1;
    k->guarded = (pass && pass[0]) ? 1 : 0;
    sk_zero(tmp, 32); sk_zero(mackey, 32);
    return store_save();
}

int ssh_key_generate(const char *name, const char *pass)
{
    unsigned char seed[32];
    int r;
    if (!tls_entropy_get(seed, 32)) return -1;   /* fail closed, never fall back */
    r = ssh_key_add(name, seed, pass);
    sk_zero(seed, 32);
    return r;
}

int ssh_key_load(const char *name, const char *pass, unsigned char seed[32])
{
    key_ent *k = key_find(name);
    unsigned char mackey[32], want[32], tmp[32], pub[32];
    int i, diff = 0;
    if (!k) return -1;
    sk_copy(tmp, k->ct, 32);
    key_crypt(pass, k->salt, tmp, mackey);       /* CTR: same call decrypts */
    key_mac(mackey, k->salt, k->ct, want);
    for (i = 0; i < 32; i++) diff |= want[i] ^ k->mac[i];
    sk_zero(mackey, 32);
    if (diff) { sk_zero(tmp, 32); return -2; }   /* wrong passphrase */
    /* The MAC already proves the passphrase. Deriving the public half again
     * and comparing proves the SEED survived intact, which is a different
     * claim and the one that matters if the file was truncated or patched. */
    ed25519_pubkey(pub, tmp);
    for (i = 0; i < 32; i++) diff |= pub[i] ^ k->pub[i];
    if (diff) { sk_zero(tmp, 32); return -2; }
    sk_copy(seed, tmp, 32);
    sk_zero(tmp, 32);
    return 0;
}

int ssh_key_delete(const char *name)
{
    key_ent *k = key_find(name);
    if (!k) return -1;
    sk_zero((unsigned char *)k, (int)sizeof *k);
    return store_save();
}

int ssh_key_list(int idx, char *name, int cap, unsigned char pub[32], int *guarded)
{
    int i, n = 0;
    store_load();
    for (i = 0; i < SSH_MAXKEYS; i++) {
        if (!g_db.keys[i].used) continue;
        if (n++ != idx) continue;
        if (name) sk_set(name, cap, g_db.keys[i].name);
        if (pub) sk_copy(pub, g_db.keys[i].pub, 32);
        if (guarded) *guarded = g_db.keys[i].guarded;
        return 0;
    }
    return -1;
}

/* ---- the OpenSSH one-line public key format ------------------------------ */
static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_enc(char *out, int cap, const unsigned char *d, int n)
{
    int i, k = 0;
    for (i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16;
        int have = n - i;
        if (have > 1) v |= (unsigned)d[i + 1] << 8;
        if (have > 2) v |= (unsigned)d[i + 2];
        if (k + 4 >= cap) return -1;
        out[k++] = kB64[(v >> 18) & 63];
        out[k++] = kB64[(v >> 12) & 63];
        out[k++] = have > 1 ? kB64[(v >> 6) & 63] : '=';
        out[k++] = have > 2 ? kB64[v & 63] : '=';
    }
    out[k] = 0;
    return k;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* The accumulator is UNSIGNED and masked. It was a plain int shifted left 6
 * bits per character with nothing ever masking it off, which overflows a
 * signed int after a handful of characters - defined-looking code that is
 * undefined behaviour, and on the debug build a UBSan trap, i.e. the machine
 * reboots with no message. Correct arithmetic and DEFINED arithmetic are not
 * the same property; only one of them is what this OS enforces. */
static int b64_dec(unsigned char *out, int cap, const char *s, int n)
{
    int i, k = 0, bits = 0;
    unsigned acc = 0;
    for (i = 0; i < n; i++) {
        int v = b64_val(s[i]);
        if (v < 0) continue;                     /* newlines, '=' padding */
        acc = ((acc << 6) | (unsigned)v) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) { bits -= 8; if (k >= cap) return -1; out[k++] = (unsigned char)(acc >> bits); }
    }
    return k;
}

int ssh_key_export_pub(const char *name, char *out, int cap)
{
    key_ent *k = key_find(name);
    unsigned char blob[64];
    int n = 0, i, w;
    static const char ty[] = "ssh-ed25519";
    if (!k) return -1;
    blob[n++] = 0; blob[n++] = 0; blob[n++] = 0; blob[n++] = 11;
    for (i = 0; i < 11; i++) blob[n++] = (unsigned char)ty[i];
    blob[n++] = 0; blob[n++] = 0; blob[n++] = 0; blob[n++] = 32;
    for (i = 0; i < 32; i++) blob[n++] = k->pub[i];
    for (i = 0; i < 12 && i < cap - 1; i++) out[i] = ty[i] ? ty[i] : ' ';
    out[11] = ' ';
    w = b64_enc(out + 12, cap - 12, blob, n);
    if (w < 0) return -1;
    return 12 + w;
}

/* ---- importing an OpenSSH private key ------------------------------------
 * openssh-key-v1, UNENCRYPTED only. An encrypted one needs bcrypt_pbkdf -
 * Blowfish plus a modified bcrypt - which we do not have, so it is detected by
 * name and refused with a reason rather than failing somewhere obscure. */
static unsigned rd32(const unsigned char *p)
{ return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
         ((unsigned)p[2] << 8) | (unsigned)p[3]; }

int ssh_key_import(const char *name, const char *pem, int pemlen, const char *pass)
{
    /* static, not a local: 2 KB of stack inside a SPECTEST test is enough to
     * run the kernel off the end of its frame, which presents as a reboot loop
     * with no message rather than as anything resembling a parse error. */
    static unsigned char raw[2048];
    int n, i, body = -1, end = -1;
    unsigned pos, len;

    for (i = 0; i + 5 < pemlen; i++)             /* find the base64 body */
        if (pem[i] == '-' && pem[i + 1] == '-' && body < 0) {
            while (i < pemlen && pem[i] != '\n') i++;
            body = i + 1;
            break;
        }
    if (body < 0) return -1;
    for (i = body; i + 1 < pemlen; i++)
        if (pem[i] == '-' && pem[i + 1] == '-') { end = i; break; }
    if (end < 0) end = pemlen;
    n = b64_dec(raw, (int)sizeof raw, pem + body, end - body);
    if (n < 60) return -1;

    /* EVERY step is bounds-checked against n. This parses a file the user
     * chose, so a malformed one must be a clean -1 - and on the debug build an
     * unchecked walk past the end of `raw` is a UBSan trap, i.e. the machine
     * reboots with no message at all. That is exactly how this first failed. */
    for (i = 0; i < 15; i++)                     /* "openssh-key-v1\0" */
        if (raw[i] != (unsigned char)"openssh-key-v1"[i]) return -1;
    pos = 15;
#define NEED(k) do { if ((long)pos + (long)(k) > (long)n) return -1; } while (0)
    NEED(4); len = rd32(raw + pos); pos += 4;    /* ciphername */
    NEED(len);
    if (len != 4 || raw[pos] != 'n' || raw[pos + 1] != 'o' ||
        raw[pos + 2] != 'n' || raw[pos + 3] != 'e') return -3;   /* encrypted */
    pos += len;
    NEED(4); len = rd32(raw + pos); pos += 4; NEED(len); pos += len;   /* kdfname */
    NEED(4); len = rd32(raw + pos); pos += 4; NEED(len); pos += len;   /* kdfopts */
    NEED(4); pos += 4;                           /* key count  */
    NEED(4); len = rd32(raw + pos); pos += 4; NEED(len); pos += len;   /* pub blob */
    NEED(4); len = rd32(raw + pos); pos += 4;    /* private section */
    NEED(len);
    NEED(8); pos += 8;                           /* checkint x2 */
    NEED(4); len = rd32(raw + pos); pos += 4;    /* keytype     */
    NEED(len);
    if (len != 11) return -4;                    /* not ed25519 */
    pos += len;
    NEED(4); len = rd32(raw + pos); pos += 4; NEED(len); pos += len;   /* pub again */
    NEED(4); len = rd32(raw + pos); pos += 4;    /* private: seed||pub */
    if (len != 64) return -1;
    NEED(64);
#undef NEED
    {   int r = ssh_key_add(name, raw + pos, pass);
        sk_zero(raw, (int)sizeof raw);
        return r; }
}

/* ---- saved sessions ------------------------------------------------------ */
int ssh_sess_set(const char *name, const char *host, int port,
                 const char *user, const char *key)
{
    int i, free_ = -1;
    store_load();
    for (i = 0; i < SSH_MAXSESS; i++) {
        if (g_db.sess[i].used && sk_eq(g_db.sess[i].name, name)) break;
        if (!g_db.sess[i].used && free_ < 0) free_ = i;
    }
    if (i == SSH_MAXSESS) { if (free_ < 0) return -1; i = free_; }
    sk_set(g_db.sess[i].name, SSH_NAMELEN, name);
    sk_set(g_db.sess[i].host, SSH_HOSTLEN, host);
    sk_set(g_db.sess[i].user, SSH_NAMELEN, user);
    sk_set(g_db.sess[i].key,  SSH_NAMELEN, key ? key : "");
    g_db.sess[i].port = port > 0 ? port : 22;
    g_db.sess[i].used = 1;
    return store_save();
}

int ssh_sess_get(const char *name, char *host, int hcap, int *port,
                 char *user, int ucap, char *key, int kcap)
{
    int i;
    store_load();
    for (i = 0; i < SSH_MAXSESS; i++) {
        if (!g_db.sess[i].used || !sk_eq(g_db.sess[i].name, name)) continue;
        if (host) sk_set(host, hcap, g_db.sess[i].host);
        if (user) sk_set(user, ucap, g_db.sess[i].user);
        if (key)  sk_set(key,  kcap, g_db.sess[i].key);
        if (port) *port = g_db.sess[i].port;
        return 0;
    }
    return -1;
}

int ssh_sess_list(int idx, char *name, int cap)
{
    int i, n = 0;
    store_load();
    for (i = 0; i < SSH_MAXSESS; i++) {
        if (!g_db.sess[i].used) continue;
        if (n++ != idx) continue;
        if (name) sk_set(name, cap, g_db.sess[i].name);
        return 0;
    }
    return -1;
}

int ssh_sess_delete(const char *name)
{
    int i;
    store_load();
    for (i = 0; i < SSH_MAXSESS; i++)
        if (g_db.sess[i].used && sk_eq(g_db.sess[i].name, name)) {
            sk_zero((unsigned char *)&g_db.sess[i], (int)sizeof g_db.sess[i]);
            return store_save();
        }
    return -1;
}

/* ---- known hosts ---------------------------------------------------------
 * This is what turns ssh-b's "the peer holds the key it presented" into "the
 * peer is who it was last time". A MISMATCH is deliberately a different answer
 * from UNKNOWN: the first is the one worth stopping for. */
int ssh_known_check(const char *host, const unsigned char fp[32])
{
    int i, j;
    store_load();
    for (i = 0; i < SSH_MAXHOSTS; i++) {
        if (!g_db.hosts[i].used || !sk_eq(g_db.hosts[i].host, host)) continue;
        for (j = 0; j < 32; j++) if (g_db.hosts[i].fp[j] != fp[j]) return SSH_HOST_MISMATCH;
        return SSH_HOST_KNOWN;
    }
    return SSH_HOST_UNKNOWN;
}

int ssh_known_add(const char *host, const unsigned char fp[32])
{
    int i, free_ = -1;
    store_load();
    for (i = 0; i < SSH_MAXHOSTS; i++) {
        if (g_db.hosts[i].used && sk_eq(g_db.hosts[i].host, host)) break;
        if (!g_db.hosts[i].used && free_ < 0) free_ = i;
    }
    if (i == SSH_MAXHOSTS) { if (free_ < 0) return -1; i = free_; }
    sk_set(g_db.hosts[i].host, SSH_HOSTLEN, host);
    sk_copy(g_db.hosts[i].fp, fp, 32);
    g_db.hosts[i].used = 1;
    return store_save();
}

int ssh_known_forget(const char *host)
{
    int i;
    store_load();
    for (i = 0; i < SSH_MAXHOSTS; i++)
        if (g_db.hosts[i].used && sk_eq(g_db.hosts[i].host, host)) {
            sk_zero((unsigned char *)&g_db.hosts[i], (int)sizeof g_db.hosts[i]);
            return store_save();
        }
    return -1;
}

int ssh_verify_host(int handle, const char *host)
{
    const unsigned char *fp = ssh_host_fingerprint(handle);
    ssh_conn *c = uns_get(handle);
    int r;
    if (!fp) return SSH_HOST_UNKNOWN;
    r = ssh_known_check(host, fp);
    /* Record that the caller consulted known-hosts for this connection. The
     * transport gates auth/channel traffic on this flag (unossh_auth.c), so a
     * caller that forgets to verify cannot reach auth at all. Recording the
     * result is deliberately independent of what the result WAS: a MISMATCH
     * is still the caller's to act on, and the shipped callers close the
     * connection on it before any auth runs. */
    if (c) c->host_verified = 1;
    return r;
}
