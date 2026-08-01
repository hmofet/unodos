/* ===========================================================================
 * unossh - user authentication and the session channel.  RFC 4252, RFC 4254.
 *
 * Sits on the transport in unossh.c: everything here is one more SSH packet
 * exchange, so it needs no crypto of its own beyond ed25519_sign() for the
 * publickey signature.
 *
 * THE SIGNATURE COVERS THE SESSION ID. That is what stops a publickey auth
 * being replayed onto a different connection - the blob signed is the session
 * id followed by the request itself, and the request that goes on the wire is
 * the same bytes minus that leading string. Building the two from one buffer
 * is not a tidiness choice; a client that builds them separately and lets them
 * drift produces signatures the server rejects with no useful diagnosis.
 *
 * Channels are flow-controlled in both directions (RFC 4254 section 5.2). The
 * local window has to be topped up with WINDOW_ADJUST as data is consumed, or
 * the server stops sending after the initial window and the session simply
 * stalls with no error anywhere.
 *
 * Reading is NON-BLOCKING: ssh_poll() drains whatever has arrived into a ring
 * and returns, so the shell's frame loop can drive a live session without the
 * desktop stalling on the network.
 * ======================================================================== */
#include "unossh.h"
#include "ed25519.h"
#include "unossh_int.h"

#define MSG_SERVICE_REQUEST 5
#define MSG_SERVICE_ACCEPT  6
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_BANNER  53
#define MSG_USERAUTH_PK_OK   60
#define MSG_GLOBAL_REQUEST   80
#define MSG_REQUEST_FAILURE  82
#define MSG_CHANNEL_OPEN     90
#define MSG_CHANNEL_OPEN_OK  91
#define MSG_CHANNEL_OPEN_FAIL 92
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA     94
#define MSG_CHANNEL_EXT_DATA 95
#define MSG_CHANNEL_EOF      96
#define MSG_CHANNEL_CLOSE    97
#define MSG_CHANNEL_REQUEST  98
#define MSG_CHANNEL_SUCCESS  99
#define MSG_CHANNEL_FAILURE  100

#define WIN_INIT   (256 * 1024)
#define WIN_TOPUP  (64 * 1024)

static int ua_str_is(const unsigned char *s, int n, const char *want)
{
    int i;
    for (i = 0; i < n; i++) { if (!want[i] || s[i] != (unsigned char)want[i]) return 0; }
    return want[n] == 0;
}

/* ---- service request ----------------------------------------------------- */
static int request_userauth(ssh_conn *c)
{
    unsigned char m[64];
    ssh_buf b;
    ssh_buf_init(&b, m, (int)sizeof m);
    ssh_put_u8(&b, MSG_SERVICE_REQUEST);
    ssh_put_cstr(&b, "ssh-userauth");
    if (b.err || uns_send(c, m, b.len) < 0) return -1;
    for (;;) {
        if (uns_recv(c, 15000) < 0) return -1;
        if (c->paylen < 1) continue;
        if (c->pay[0] == MSG_SERVICE_ACCEPT) return 0;
        if (c->pay[0] == MSG_USERAUTH_BANNER) continue;
        uns_err(c, "server refused ssh-userauth");
        return -1;
    }
}

/* Wait for the verdict on an auth attempt. 0 = in, 1 = rejected, -1 = broken.
 * A BANNER can arrive at any point and is not an answer. */
static int auth_verdict(ssh_conn *c)
{
    for (;;) {
        if (uns_recv(c, 20000) < 0) return -1;
        if (c->paylen < 1) continue;
        if (c->pay[0] == MSG_USERAUTH_BANNER) continue;
        if (c->pay[0] == MSG_USERAUTH_SUCCESS) { c->authed = 1; return 0; }
        if (c->pay[0] == MSG_USERAUTH_FAILURE) {
            ssh_rd r;
            const unsigned char *l;
            int n;
            ssh_rd_init(&r, c->pay, c->paylen);
            ssh_get_u8(&r);
            l = ssh_get_str(&r, &n);
            if (l && n > 0) {
                char msg[96];
                int k = 0, j;
                const char *pre = "rejected; server accepts: ";
                for (j = 0; pre[j] && k < (int)sizeof msg - 1; j++) msg[k++] = pre[j];
                for (j = 0; j < n && k < (int)sizeof msg - 1; j++) msg[k++] = (char)l[j];
                msg[k] = 0;
                uns_err(c, msg);
            } else uns_err(c, "authentication rejected");
            return 1;
        }
        uns_err(c, "unexpected reply during authentication");
        return -1;
    }
}

int ssh_auth_password(int handle, const char *user, const char *pass)
{
    ssh_conn *c = uns_get(handle);
    unsigned char m[512];
    ssh_buf b;
    if (!c) return -1;
    if (!c->authed && request_userauth(c) < 0) return -1;
    ssh_buf_init(&b, m, (int)sizeof m);
    ssh_put_u8(&b, MSG_USERAUTH_REQUEST);
    ssh_put_cstr(&b, user);
    ssh_put_cstr(&b, "ssh-connection");
    ssh_put_cstr(&b, "password");
    ssh_put_u8(&b, 0);
    ssh_put_cstr(&b, pass);
    if (b.err || uns_send(c, m, b.len) < 0) return -1;
    return auth_verdict(c);
}

/* publickey with an Ed25519 key held as its 32-byte seed. */
int ssh_auth_key(int handle, const char *user, const unsigned char seed[32])
{
    ssh_conn *c = uns_get(handle);
    unsigned char pk[32], blob[64], req[768], signed_[832], sig[64], sigblob[128];
    ssh_buf b, kb, sb;
    int reqlen;

    if (!c) return -1;
    if (!c->authed && request_userauth(c) < 0) return -1;

    ed25519_pubkey(pk, seed);
    ssh_buf_init(&kb, blob, (int)sizeof blob);       /* the public key blob */
    ssh_put_cstr(&kb, "ssh-ed25519");
    ssh_put_str(&kb, pk, 32);
    if (kb.err) { uns_err(c, "key blob did not fit"); return -1; }

    /* The request, built ONCE. The bytes signed are the session id followed by
     * exactly these bytes, and the bytes sent are exactly these bytes - so the
     * two cannot drift apart. */
    ssh_buf_init(&b, req, (int)sizeof req);
    ssh_put_u8(&b, MSG_USERAUTH_REQUEST);
    ssh_put_cstr(&b, user);
    ssh_put_cstr(&b, "ssh-connection");
    ssh_put_cstr(&b, "publickey");
    ssh_put_u8(&b, 1);                               /* with a signature */
    ssh_put_cstr(&b, "ssh-ed25519");
    ssh_put_str(&b, blob, kb.len);
    if (b.err) { uns_err(c, "auth request did not fit"); return -1; }
    reqlen = b.len;

    ssh_buf_init(&sb, signed_, (int)sizeof signed_);
    ssh_put_str(&sb, c->sess_id, 32);
    ssh_put_raw(&sb, req, reqlen);
    if (sb.err) { uns_err(c, "signed blob did not fit"); return -1; }

    ed25519_sign(sig, signed_, sb.len, pk, seed);

    ssh_buf_init(&sb, sigblob, (int)sizeof sigblob);
    ssh_put_cstr(&sb, "ssh-ed25519");
    ssh_put_str(&sb, sig, 64);
    if (sb.err) { uns_err(c, "signature blob did not fit"); return -1; }

    {   unsigned char full[900];
        ssh_buf fb;
        ssh_buf_init(&fb, full, (int)sizeof full);
        ssh_put_raw(&fb, req, reqlen);
        ssh_put_str(&fb, sigblob, sb.len);
        if (fb.err || uns_send(c, full, fb.len) < 0) return -1;
    }
    return auth_verdict(c);
}

/* ---- the session channel ------------------------------------------------- */
static void ring_put(ssh_conn *c, const unsigned char *d, int n)
{
    int i;
    if (c->rb_off > 0 && c->rb_len + n > SSH_RBCAP) {   /* compact first */
        for (i = 0; i < c->rb_len - c->rb_off; i++) c->rb[i] = c->rb[c->rb_off + i];
        c->rb_len -= c->rb_off;
        c->rb_off = 0;
    }
    for (i = 0; i < n && c->rb_len < SSH_RBCAP; i++) c->rb[c->rb_len++] = d[i];
}

/* One packet's worth of channel bookkeeping. 0 = handled. */
static int channel_dispatch(ssh_conn *c)
{
    ssh_rd r;
    unsigned t;
    if (c->paylen < 1) return 0;
    t = c->pay[0];
    ssh_rd_init(&r, c->pay, c->paylen);
    ssh_get_u8(&r);

    switch (t) {
    case MSG_CHANNEL_DATA: {
        const unsigned char *d;
        int n;
        ssh_get_u32(&r);
        d = ssh_get_str(&r, &n);
        if (d && n > 0) {
            ring_put(c, d, n);
            /* Top the window back up. Without this the server sends exactly
             * one window's worth and then stops, which looks like a hang and
             * reports nothing anywhere. */
            c->win_in -= (unsigned)n;
            if (c->win_in < WIN_INIT / 2) {
                unsigned char m[16];
                ssh_buf b;
                ssh_buf_init(&b, m, (int)sizeof m);
                ssh_put_u8(&b, MSG_CHANNEL_WINDOW_ADJUST);
                ssh_put_u32(&b, c->ch_remote);
                ssh_put_u32(&b, WIN_TOPUP);
                if (!b.err && uns_send(c, m, b.len) == 0) c->win_in += WIN_TOPUP;
            }
        }
        return 0; }
    case MSG_CHANNEL_EXT_DATA: {
        const unsigned char *d;
        int n;
        ssh_get_u32(&r); ssh_get_u32(&r);          /* channel, data type */
        d = ssh_get_str(&r, &n);
        if (d && n > 0) { ring_put(c, d, n); c->win_in -= (unsigned)n; }
        return 0; }
    case MSG_CHANNEL_WINDOW_ADJUST:
        ssh_get_u32(&r);
        c->win_out += ssh_get_u32(&r);
        return 0;
    case MSG_CHANNEL_EOF:
        c->ch_eof = 1;
        return 0;
    case MSG_CHANNEL_CLOSE:
        c->ch_eof = 1;
        c->ch_state = 0;
        return 0;
    case MSG_CHANNEL_REQUEST: {
        const unsigned char *w;
        int n;
        ssh_get_u32(&r);
        w = ssh_get_str(&r, &n);
        if (w && ua_str_is(w, n, "exit-status")) {
            ssh_get_u8(&r);                         /* want_reply, always 0 */
            c->ch_exit = (int)ssh_get_u32(&r);
        }
        return 0; }
    case MSG_GLOBAL_REQUEST: {
        const unsigned char *w;
        int n;
        w = ssh_get_str(&r, &n);
        (void)w;
        if (ssh_get_u8(&r)) {                       /* want_reply */
            unsigned char m[4];
            m[0] = MSG_REQUEST_FAILURE;
            uns_send(c, m, 1);
        }
        return 0; }
    default:
        return 0;
    }
}

static int open_session(ssh_conn *c)
{
    unsigned char m[64];
    ssh_buf b;
    if (!c->authed) { uns_err(c, "not authenticated"); return -1; }
    c->ch_local = 0;
    c->win_in = WIN_INIT;
    ssh_buf_init(&b, m, (int)sizeof m);
    ssh_put_u8(&b, MSG_CHANNEL_OPEN);
    ssh_put_cstr(&b, "session");
    ssh_put_u32(&b, c->ch_local);
    ssh_put_u32(&b, WIN_INIT);
    ssh_put_u32(&b, 32768);
    if (b.err || uns_send(c, m, b.len) < 0) return -1;

    for (;;) {
        ssh_rd r;
        if (uns_recv(c, 20000) < 0) return -1;
        if (c->paylen < 1) continue;
        if (c->pay[0] == MSG_CHANNEL_OPEN_FAIL) { uns_err(c, "channel open refused"); return -1; }
        if (c->pay[0] != MSG_CHANNEL_OPEN_OK) { channel_dispatch(c); continue; }
        ssh_rd_init(&r, c->pay, c->paylen);
        ssh_get_u8(&r);
        ssh_get_u32(&r);                     /* our channel, echoed back */
        c->ch_remote = ssh_get_u32(&r);
        c->win_out = ssh_get_u32(&r);
        c->maxpkt_out = ssh_get_u32(&r);
        if (r.err) { uns_err(c, "malformed channel open reply"); return -1; }
        if (c->maxpkt_out > 32768) c->maxpkt_out = 32768;
        c->ch_state = 1;
        c->ch_exit = -1;
        c->ch_eof = 0;
        c->rb_off = c->rb_len = 0;
        return 0;
    }
}

static int channel_request(ssh_conn *c, const char *what, const char *arg)
{
    unsigned char m[1024];
    ssh_buf b;
    ssh_buf_init(&b, m, (int)sizeof m);
    ssh_put_u8(&b, MSG_CHANNEL_REQUEST);
    ssh_put_u32(&b, c->ch_remote);
    ssh_put_cstr(&b, what);
    ssh_put_u8(&b, 1);                       /* want_reply */
    if (arg) ssh_put_cstr(&b, arg);
    if (b.err || uns_send(c, m, b.len) < 0) return -1;
    for (;;) {
        if (uns_recv(c, 20000) < 0) return -1;
        if (c->paylen < 1) continue;
        if (c->pay[0] == MSG_CHANNEL_SUCCESS) return 0;
        if (c->pay[0] == MSG_CHANNEL_FAILURE) { uns_err(c, "channel request refused"); return -1; }
        channel_dispatch(c);
    }
}

int ssh_exec(int handle, const char *cmd)
{
    ssh_conn *c = uns_get(handle);
    if (!c) return -1;
    if (open_session(c) < 0) return -1;
    return channel_request(c, "exec", cmd);
}

int ssh_shell(int handle)
{
    ssh_conn *c = uns_get(handle);
    if (!c) return -1;
    if (open_session(c) < 0) return -1;
    /* a pty is best-effort: a server that refuses one can still run a shell */
    channel_request(c, "pty-req-skip", 0);
    return channel_request(c, "shell", 0);
}

int ssh_poll(int handle)
{
    ssh_conn *c = uns_get(handle);
    int guard;
    if (!c) return -1;
    for (guard = 0; guard < 64; guard++) {
        if (uns_recv(c, 0) < 0) break;       /* nothing waiting, or closed */
        channel_dispatch(c);
    }
    return (c->ch_state == 1 || c->rb_len > c->rb_off) ? 1 : 0;
}

int ssh_read(int handle, void *buf, int cap)
{
    ssh_conn *c = uns_get(handle);
    unsigned char *out = (unsigned char *)buf;
    int n = 0;
    if (!c) return -1;
    while (n < cap && c->rb_off < c->rb_len) out[n++] = c->rb[c->rb_off++];
    if (c->rb_off == c->rb_len) c->rb_off = c->rb_len = 0;
    if (n == 0 && c->ch_state != 1 && c->ch_eof) return -1;   /* real EOF */
    return n;
}

int ssh_write(int handle, const void *buf, int n)
{
    ssh_conn *c = uns_get(handle);
    const unsigned char *in = (const unsigned char *)buf;
    int sent = 0;
    if (!c || c->ch_state != 1) return -1;
    while (sent < n) {
        unsigned char m[SSH_RBCAP / 4 + 64];
        ssh_buf b;
        int chunk = n - sent;
        if ((unsigned)chunk > c->win_out) chunk = (int)c->win_out;
        if ((unsigned)chunk > c->maxpkt_out) chunk = (int)c->maxpkt_out;
        if (chunk > (int)sizeof m - 64) chunk = (int)sizeof m - 64;
        if (chunk <= 0) break;               /* the remote window is shut */
        ssh_buf_init(&b, m, (int)sizeof m);
        ssh_put_u8(&b, MSG_CHANNEL_DATA);
        ssh_put_u32(&b, c->ch_remote);
        ssh_put_str(&b, in + sent, chunk);
        if (b.err || uns_send(c, m, b.len) < 0) return sent ? sent : -1;
        c->win_out -= (unsigned)chunk;
        sent += chunk;
    }
    return sent;
}

int ssh_exit_status(int handle)
{ ssh_conn *c = uns_get(handle); return c ? c->ch_exit : -1; }

int ssh_channel_open(int handle)
{ ssh_conn *c = uns_get(handle); return c ? (c->ch_state == 1) : 0; }
