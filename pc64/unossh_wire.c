/* ===========================================================================
 * unossh - wire format, packet framing and key exchange arithmetic.
 *
 * Everything in here is a pure function of its arguments: no sockets, no
 * global connection state, no allocation. That is deliberate, because it is
 * the half of SSH that fails silently. A wrong mpint or a length counted in
 * the wrong units does not produce an error - it produces an exchange hash the
 * server computes differently, and the connection dies several messages later
 * with "invalid signature", pointing nowhere near the mistake.
 *
 * Being pure also makes it the half that can be tested on the host, which is
 * what tools/sshwiretest.c does.
 * ======================================================================== */
#include "bearssl_hash.h"
#include "bearssl_ec.h"
#include "unossh.h"

static void sw_copy(unsigned char *d, const unsigned char *s, int n)
{ int i; for (i = 0; i < n; i++) d[i] = s[i]; }

/* ---- writer -------------------------------------------------------------- */
void ssh_buf_init(ssh_buf *b, unsigned char *store, int cap)
{ b->p = store; b->cap = cap; b->len = 0; b->err = 0; }

static unsigned char *sw_room(ssh_buf *b, int n)
{
    unsigned char *at;
    if (b->err || n < 0 || b->len + n > b->cap) { b->err = 1; return 0; }
    at = b->p + b->len;
    b->len += n;
    return at;
}

void ssh_put_u8(ssh_buf *b, unsigned v)
{ unsigned char *a = sw_room(b, 1); if (a) a[0] = (unsigned char)v; }

void ssh_put_u32(ssh_buf *b, unsigned v)
{
    unsigned char *a = sw_room(b, 4);
    if (!a) return;
    a[0] = (unsigned char)(v >> 24); a[1] = (unsigned char)(v >> 16);
    a[2] = (unsigned char)(v >> 8);  a[3] = (unsigned char)v;
}

void ssh_put_raw(ssh_buf *b, const unsigned char *d, int n)
{ unsigned char *a = sw_room(b, n); if (a) sw_copy(a, d, n); }

void ssh_put_str(ssh_buf *b, const unsigned char *d, int n)
{ ssh_put_u32(b, (unsigned)n); ssh_put_raw(b, d, n); }

void ssh_put_cstr(ssh_buf *b, const char *s)
{
    int n = 0;
    while (s[n]) n++;
    ssh_put_str(b, (const unsigned char *)s, n);
}

/* mpint: signed big-endian. Leading zero bytes are not part of the value, and
 * a positive value whose top bit is set needs one zero byte in front so it is
 * not read as negative. Zero is the empty string. */
void ssh_put_mpint(ssh_buf *b, const unsigned char *d, int n)
{
    int i = 0;
    while (i < n && d[i] == 0) i++;          /* strip leading zeros */
    if (i == n) { ssh_put_u32(b, 0); return; }
    if (d[i] & 0x80) {
        ssh_put_u32(b, (unsigned)(n - i + 1));
        ssh_put_u8(b, 0);
    } else {
        ssh_put_u32(b, (unsigned)(n - i));
    }
    ssh_put_raw(b, d + i, n - i);
}

/* ---- reader -------------------------------------------------------------- */
void ssh_rd_init(ssh_rd *r, const unsigned char *d, int n)
{ r->p = d; r->len = n; r->pos = 0; r->err = 0; }

int ssh_rd_left(const ssh_rd *r) { return r->err ? 0 : r->len - r->pos; }

static const unsigned char *sr_take(ssh_rd *r, int n)
{
    const unsigned char *at;
    if (r->err || n < 0 || r->pos + n > r->len) { r->err = 1; return 0; }
    at = r->p + r->pos;
    r->pos += n;
    return at;
}

unsigned ssh_get_u8(ssh_rd *r)
{ const unsigned char *a = sr_take(r, 1); return a ? a[0] : 0u; }

unsigned ssh_get_u32(ssh_rd *r)
{
    const unsigned char *a = sr_take(r, 4);
    if (!a) return 0u;
    return ((unsigned)a[0] << 24) | ((unsigned)a[1] << 16) |
           ((unsigned)a[2] << 8)  | (unsigned)a[3];
}

const unsigned char *ssh_get_str(ssh_rd *r, int *n)
{
    unsigned len = ssh_get_u32(r);
    const unsigned char *a;
    /* a length field is 32 bits on the wire but our buffers are int-sized, so
     * an absurd length has to be refused here rather than overflowing below */
    if (r->err || len > (unsigned)0x7FFFFFF) { r->err = 1; if (n) *n = 0; return 0; }
    a = sr_take(r, (int)len);
    if (n) *n = a ? (int)len : 0;
    return a;
}

/* ---- packet framing ------------------------------------------------------
 * The encrypted region is 4 (length) + 1 (padlen) + payload + padding and has
 * to be a multiple of the block size AND at least 16 bytes, with at least 4
 * bytes of padding. Returns the padding length. */
int ssh_pad_len(int payload_len, int blocksize)
{
    int unit = blocksize < 8 ? 8 : blocksize;
    int base = 5 + payload_len;
    int pad = unit - (base % unit);
    if (pad < SSH_PKT_MIN_PAD) pad += unit;
    while (base + pad < 16) pad += unit;
    return pad;
}

/* ---- X25519 --------------------------------------------------------------
 * BearSSL's contract is asymmetric and easy to get backwards: the POINT is
 * little-endian (RFC 7748's own encoding, byteswapped internally) while the
 * SCALAR is big-endian, and it clamps the scalar itself. These wrappers speak
 * RFC 7748 little-endian on every argument so no caller has to know. */
static const br_ec_impl *sw_ec(void)
{
    const br_ec_impl *ec = br_ec_get_default();
    if (ec && (ec->supported_curves & ((unsigned)1 << BR_EC_curve25519)))
        return ec;
    return &br_ec_c25519_i31;
}

static void sw_rev32(unsigned char d[32], const unsigned char s[32])
{ int i; for (i = 0; i < 32; i++) d[i] = s[31 - i]; }

int ssh_x25519_base(unsigned char pub[32], const unsigned char sec[32])
{
    const br_ec_impl *ec = sw_ec();
    unsigned char be[32];
    size_t n;
    sw_rev32(be, sec);                       /* scalar -> big-endian */
    n = ec->mulgen(pub, be, 32, BR_EC_curve25519);
    return n == 32;
}

int ssh_x25519(unsigned char out[32], const unsigned char sec[32],
               const unsigned char peer[32])
{
    const br_ec_impl *ec = sw_ec();
    unsigned char be[32];
    unsigned r;
    sw_rev32(be, sec);
    sw_copy(out, peer, 32);                  /* point stays little-endian */
    r = ec->mul(out, 32, be, 32, BR_EC_curve25519);
    return r != 0;
}

/* ---- exchange hash and key derivation ------------------------------------ */
static void sw_hash_str(br_sha256_context *c, const unsigned char *d, int n)
{
    unsigned char hdr[4];
    hdr[0] = (unsigned char)((unsigned)n >> 24);
    hdr[1] = (unsigned char)((unsigned)n >> 16);
    hdr[2] = (unsigned char)((unsigned)n >> 8);
    hdr[3] = (unsigned char)n;
    br_sha256_update(c, hdr, 4);
    if (n) br_sha256_update(c, d, (size_t)n);
}

static void sw_hash_cstr(br_sha256_context *c, const char *s)
{
    int n = 0;
    while (s[n]) n++;
    sw_hash_str(c, (const unsigned char *)s, n);
}

void ssh_exchange_hash(unsigned char out[32], const ssh_exch *e)
{
    br_sha256_context c;
    unsigned char mp[512];
    ssh_buf b;

    br_sha256_init(&c);
    sw_hash_cstr(&c, e->v_c);
    sw_hash_cstr(&c, e->v_s);
    sw_hash_str(&c, e->i_c, e->i_c_len);
    sw_hash_str(&c, e->i_s, e->i_s_len);
    sw_hash_str(&c, e->k_s, e->k_s_len);
    sw_hash_str(&c, e->q_c, 32);
    sw_hash_str(&c, e->q_s, 32);
    /* K is an mpint, not a string - the one field that is different, and the
     * one that most often makes a first implementation's hash disagree */
    ssh_buf_init(&b, mp, (int)sizeof mp);
    ssh_put_mpint(&b, e->k, e->k_len);
    if (!b.err) br_sha256_update(&c, mp, (size_t)b.len);
    br_sha256_out(&c, out);
}

void ssh_derive_key(unsigned char *out, int outlen, unsigned char letter,
                    const unsigned char *k, int k_len,
                    const unsigned char h[32],
                    const unsigned char session_id[32])
{
    br_sha256_context c;
    unsigned char mp[512], block[32];
    ssh_buf b;
    int done = 0;

    ssh_buf_init(&b, mp, (int)sizeof mp);
    ssh_put_mpint(&b, k, k_len);
    if (b.err) return;

    br_sha256_init(&c);
    br_sha256_update(&c, mp, (size_t)b.len);
    br_sha256_update(&c, h, 32);
    br_sha256_update(&c, &letter, 1);
    br_sha256_update(&c, session_id, 32);
    br_sha256_out(&c, block);

    while (done < outlen) {
        int n = outlen - done;
        if (n > 32) n = 32;
        sw_copy(out + done, block, n);
        done += n;
        if (done < outlen) {                 /* K1 || K2 || ... */
            br_sha256_init(&c);
            br_sha256_update(&c, mp, (size_t)b.len);
            br_sha256_update(&c, h, 32);
            br_sha256_update(&c, out, (size_t)done);
            br_sha256_out(&c, block);
        }
    }
}
