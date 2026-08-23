/* ===========================================================================
 * UnoDOS/pc64 - unoxfer's SCP backend, over unossh's exec channel.
 *
 * WHY SCP AND NOT SFTP.  SFTP is the better protocol and it is not available:
 * it needs a "subsystem" channel, and unossh has no ssh_subsystem() yet (five
 * lines beside ssh_exec - channel_request() already has the exact shape -
 * requested 2026-08-22).  SCP needs nothing unossh does not already do: it is
 * a program run over an exec channel, and every byte of its protocol is on the
 * channel's stdin/stdout.  So this backend works TODAY, on the SSH client the
 * OS already ships, and UNOXFER_SFTP sits behind the weak link.
 *
 * The three-part split below is the shape of the thing:
 *   - the CONNECTION (login, host-key policy, re-exec per operation)
 *   - LISTING, which SCP does not do at all, so it is `ls -l` parsed
 *   - the SCP WIRE, which is a tiny, exacting, easily-got-wrong protocol
 *
 * HOST-KEY POLICY IS HERE, deliberately, exactly as sshapp_ui.c argues: the
 * store answers known / unknown / mismatch and the CONSUMER decides.  A
 * MISMATCH is refused outright with UNOXFER_EHOSTKEY - it is the one answer
 * worth stopping a transfer for, and a background job has nobody to ask.
 * ======================================================================== */
#include "unoxfer_int.h"
#include "unossh.h"
#include "pc64_http.h"          /* pc64_net_up */

void *malloc(unsigned long);
void  free(void *);
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
int   snprintf(char *, unsigned long, const char *, ...);
int   net_poll(void);
long  TickCount(void);
int   uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
long  uno_fs_size(int vol, const char *name);

/* unossh's subsystem channel, weak-linked - see unoxfer.c for why the probe is
 * a separate predicate rather than an address test. */
int ssh_subsystem(int handle, const char *name);
int ssh_subsystem_supported(void);

#define SX_CMD 512

typedef struct {
    int  h;                     /* the unossh handle, -1 when disconnected   */
    char host[UNOXFER_HOSTLEN];
    int  port;
    char user[UNOXFER_NAMELEN];
    char key[UNOXFER_NAMELEN];
} sx;

/* ===========================================================================
 * The connection.
 * ======================================================================== */
static int sx_login(unoxfer_client *c, sx *x)
{
    unsigned char seed[32];
    int rc;

    if (x->h >= 0) return UNOXFER_OK;
    if (!pc64_net_up()) return ux_fail(c, UNOXFER_EIO, "no network");

    x->h = ssh_connect(x->host, x->port ? x->port : 22);
    if (x->h < 0) return ux_failf(c, UNOXFER_EIO, "connect %s:%d refused",
                                  x->host, x->port ? x->port : 22);
    if (ssh_handshake(x->h) != 0) {
        ux_failf(c, UNOXFER_EIO, "handshake: %s", ssh_error(x->h));
        ssh_close(x->h); x->h = -1; return UNOXFER_EIO;
    }
    /* Trust on first use, stated rather than implied.  A MISMATCH stops; an
     * UNKNOWN host is recorded and used.  ssh_verify_host does both halves. */
    rc = ssh_verify_host(x->h, x->host);
    if (rc == SSH_HOST_MISMATCH) {
        ux_failf(c, UNOXFER_EHOSTKEY,
                 "HOST KEY MISMATCH for %s - refusing (run `ssh hosts`)", x->host);
        ssh_close(x->h); x->h = -1; return UNOXFER_EHOSTKEY;
    }

    /* Key auth only.  unossh's store holds keys, not passwords, and this
     * subsystem deliberately does not open a second secret store to hold the
     * one thing the first one does not (UNOXFER.md, "what was left out"). */
    if (!x->key[0]) {
        ux_fail(c, UNOXFER_EAUTH,
                "no key named for this site (xfer site … <key>; keys: `ssh keys`)");
        ssh_close(x->h); x->h = -1; return UNOXFER_EAUTH;
    }
    if (ssh_key_load(x->key, "", seed) != 0) {
        ux_failf(c, UNOXFER_EAUTH,
                 "key '%s' is missing or passphrase-guarded (unattended login "
                 "needs an unguarded key)", x->key);
        ssh_close(x->h); x->h = -1; return UNOXFER_EAUTH;
    }
    if (ssh_auth_key(x->h, x->user, seed) != 0) {
        ux_failf(c, UNOXFER_EAUTH, "%s@%s refused the key: %s",
                 x->user, x->host, ssh_error(x->h));
        memset(seed, 0, sizeof seed);
        ssh_close(x->h); x->h = -1; return UNOXFER_EAUTH;
    }
    memset(seed, 0, sizeof seed);
    return UNOXFER_OK;
}

static void sx_drop(sx *x)
{
    if (x->h >= 0) { ssh_close(x->h); x->h = -1; }
}

/* Run one command on a fresh channel.  A channel is one-shot in unossh (the
 * connection carries a single channel at a time), so every operation re-execs;
 * if the channel will not open, the connection is dropped and remade once,
 * which is what a server that timed the session out looks like. */
static int sx_exec(unoxfer_client *c, sx *x, const char *cmd)
{
    int rc = sx_login(c, x);
    if (rc != UNOXFER_OK) return rc;
    if (ssh_exec(x->h, cmd) == 0) return UNOXFER_OK;
    sx_drop(x);
    rc = sx_login(c, x);
    if (rc != UNOXFER_OK) return rc;
    if (ssh_exec(x->h, cmd) == 0) return UNOXFER_OK;
    return ux_failf(c, UNOXFER_EIO, "exec refused: %s", ssh_error(x->h));
}

/* ---- channel I/O ----------------------------------------------------------
 * READ AND DRAIN, ONE POLL AT A TIME.  unossh appends channel data to a 16 KB
 * ring (SSH_RBCAP) and DROPS the overflow silently, while open_session()
 * advertises a 32 KB maximum packet and ssh_poll() will dispatch up to 64
 * packets before the caller reads any.  Interactive use never notices; a bulk
 * transfer loses bytes in the middle of a file.  Filed against unossh
 * (UNOXFER.md, "waiting on other lanes").  Until it is fixed the mitigation is
 * this loop: read after every single poll so the ring stays shallow.  That
 * makes the loss unlikely, not impossible - the fix belongs in unossh. */
static int sx_read(sx *x, unsigned char *buf, int want, int ms, volatile int *cancel)
{
    long t0 = TickCount(), limit = (long)ms * 60 / 1000;
    int got = 0;
    if (limit < 1) limit = 1;
    while (got < want) {
        int n;
        if (cancel && *cancel) return -2;
        n = ssh_read(x->h, buf + got, want - got);
        if (n > 0) { got += n; t0 = TickCount(); continue; }
        if (n < 0) return got ? got : -1;              /* real end of stream */
        net_poll();
        ssh_poll(x->h);
        if (TickCount() - t0 > limit) return got ? got : -1;
    }
    return got;
}

/* One line, up to and including its '\n'. */
static int sx_line(sx *x, char *out, int cap, int ms, volatile int *cancel)
{
    int n = 0;
    while (n < cap - 1) {
        unsigned char ch;
        int r = sx_read(x, &ch, 1, ms, cancel);
        if (r == -2) return -2;
        if (r != 1) { out[n] = 0; return n ? n : -1; }
        if (ch == '\n') { out[n] = 0; return n; }
        out[n++] = (char)ch;
    }
    out[n] = 0;
    return n;
}

static int sx_ack(sx *x)                 /* the protocol's "go on": one zero */
{
    unsigned char z = 0;
    return ssh_write(x->h, &z, 1) == 1 ? 0 : -1;
}

/* ===========================================================================
 * Shell quoting.
 *
 * The remote path goes through the login shell, so it MUST be quoted or a
 * space, a '$' or a ';' in a filename becomes remote code execution against
 * your own machine.  Single quotes with the standard '\'' escape: inside
 * single quotes the shell expands nothing at all, which is the property
 * wanted.  Returns 0 if it did not fit, and a path that did not fit is refused
 * rather than truncated - a truncated path names a DIFFERENT file.
 * ======================================================================== */
static int sh_quote(char *dst, int cap, const char *s)
{
    int n = 0;
    if (cap < 3) return 0;
    dst[n++] = '\'';
    for (; *s; s++) {
        if (*s == '\'') {
            if (n + 4 >= cap) return 0;
            dst[n++] = '\''; dst[n++] = '\\'; dst[n++] = '\''; dst[n++] = '\'';
        } else {
            if (n + 2 >= cap) return 0;
            dst[n++] = *s;
        }
    }
    dst[n++] = '\'';
    dst[n] = 0;
    return 1;
}

/* ===========================================================================
 * Listing.
 *
 * SCP has no listing at all, so this is `ls` parsed - which is exactly what
 * every SCP-only client has always done, and is stated here rather than
 * hidden because it has a real failure mode: a filename containing a newline
 * produces a row this cannot see.  `-b` would escape those, but not portably
 * (BSD ls has no -b), so the parser instead REFUSES a row it cannot make sense
 * of rather than inventing an entry for it.
 *
 * The format asked for is deliberately the narrow one:
 *     ls -lLA --time-style=+%s
 * giving "type+perms links owner group size mtime name".  GNU coreutils only;
 * a BSD server falls back to the second command, which drops mtime.
 * ======================================================================== */
static int parse_ls(const char *line, unoxfer_ent *e, int have_epoch)
{
    int field = 0;
    const char *p = line;
    char type = *line;

    if (type != '-' && type != 'd' && type != 'l') return 0;   /* not a row */
    memset(e, 0, sizeof *e);
    e->is_dir = (unsigned char)(type == 'd');

    /* walk to the size field (5th), then the name (8th with epoch, else the
     * rest after three date fields) */
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        field++;
        if (field == 6) e->size = e->is_dir ? 0ull : ux_u64(p);
        if (have_epoch && field == 7) e->mtime = (unsigned)ux_u64(p);
        if (field == (have_epoch ? 8 : 9)) {
            /* the rest of the line is the name, spaces and all.  A symlink row
             * carries " -> target"; the name is what precedes it. */
            const char *arrow = 0, *q;
            for (q = p; q[0] && q[1] && q[2]; q++)
                if (q[0] == ' ' && q[1] == '-' && q[2] == '>') { arrow = q; break; }
            {
                int n = arrow ? (int)(arrow - p) : (int)strlen(p);
                if (n <= 0 || n >= (int)sizeof e->name) return 0;
                memcpy(e->name, p, (unsigned long)n);
                e->name[n] = 0;
            }
            /* a symlink's target decides whether it is a directory, and `ls
             * -L` already followed it, so type 'l' here means a BROKEN link.
             * Skip it: a plan that includes an unreadable entry fails later
             * for a reason nobody can act on. */
            return type == 'l' ? 0 : 1;
        }
        while (*p && *p != ' ') p++;
    }
    return 0;
}

static int sx_list(unoxfer_client *c, const char *path,
                   unoxfer_ent *out, int max, int *total)
{
    sx *x = (sx *)c->impl;
    char cmd[SX_CMD], q[UNOXFER_PATHLEN * 2 + 8], line[512];
    int n = 0, wrote = 0, epoch = 1, rc;

    if (!sh_quote(q, (int)sizeof q, path && *path ? path : "/"))
        return ux_fail(c, UNOXFER_EARG, "path too long to quote");
    snprintf(cmd, sizeof cmd, "ls -lLA --time-style=+%%s %s 2>/dev/null "
                              "|| ls -lLA %s", q, q);
    rc = sx_exec(c, x, cmd);
    if (rc != UNOXFER_OK) return rc;

    for (;;) {
        unoxfer_ent e;
        int len = sx_line(x, line, (int)sizeof line, 15000, 0);
        if (len < 0) break;
        if (!len) continue;
        if (line[0] == 't') continue;                  /* "total 48" */
        /* The fallback `ls -lLA` has a three-token date, so the name starts a
         * field later.  Detect it once, from the first row that parses either
         * way, rather than guessing per row. */
        if (!parse_ls(line, &e, epoch)) {
            if (epoch && parse_ls(line, &e, 0)) epoch = 0;
            else continue;
        }
        n++;
        if (wrote < max) out[wrote++] = e;
    }
    if (total) *total = n;
    return wrote;
}

static long long sx_size(unoxfer_client *c, const char *rpath)
{
    sx *x = (sx *)c->impl;
    char cmd[SX_CMD], q[UNOXFER_PATHLEN * 2 + 8], line[64];
    int rc;
    if (!sh_quote(q, (int)sizeof q, rpath)) return UNOXFER_EARG;
    /* GNU stat, then BSD stat, then wc: three spellings of one question, and
     * a server that answers none of them is one whose sizes we do without. */
    snprintf(cmd, sizeof cmd,
             "stat -c %%s %s 2>/dev/null || stat -f %%z %s 2>/dev/null || wc -c < %s",
             q, q, q);
    rc = sx_exec(c, x, cmd);
    if (rc != UNOXFER_OK) return rc;
    if (sx_line(x, line, (int)sizeof line, 15000, 0) <= 0) return UNOXFER_ENOENT;
    return (long long)ux_u64(line);
}

/* ===========================================================================
 * The SCP wire.
 *
 * Sink mode (`scp -f <path>`, we RECEIVE):
 *     -> \0                        we are ready
 *     <- C0644 <size> <name>\n     a file header
 *     -> \0
 *     <- <size> bytes, then \0     the payload and its status byte
 *     -> \0
 *   ...and \x01 / \x02 lead an advisory / fatal message instead of a header.
 *
 * Source mode (`scp -t <path>`, we SEND) is the mirror image.
 *
 * The exacting part is the ORDER of the acknowledgements: an ack sent one step
 * early makes the server send the payload before we have a place to put it,
 * and an ack forgotten makes it wait forever.  That is why this reads as a
 * flat sequence rather than a loop - each step is one line of code because
 * each step is one message.
 * ======================================================================== */
static int scp_err(unoxfer_client *c, sx *x, int lead)
{
    char msg[256];
    sx_line(x, msg, (int)sizeof msg, 5000, 0);
    return ux_failf(c, lead == 2 ? UNOXFER_ENOENT : UNOXFER_EPERM,
                    "scp: %s", msg[0] ? msg : "refused");
}

static int sx_get(unoxfer_client *c, const char *rpath, long long off,
                  int vol, const char *lpath, unoxfer_prog *p)
{
    sx *x = (sx *)c->impl;
    char cmd[SX_CMD], q[UNOXFER_PATHLEN * 2 + 8], hdr[320];
    unsigned char *buf, lead, status;
    long long size = 0, capn = 0, got = 0;
    int rc, i;
    volatile int *cancel = p ? &p->cancel : 0;

    /* SCP cannot resume: it is a whole-file protocol with no offset in its
     * grammar.  The engine knows that from UNOXFER_CAP_RESUME and restarts the
     * file; this assert-by-refusal is here so a future caller cannot quietly
     * get a corrupt file by asking anyway. */
    if (off != 0) return ux_fail(c, UNOXFER_EUNSUP, "scp cannot resume a transfer");

    if (!sh_quote(q, (int)sizeof q, rpath))
        return ux_fail(c, UNOXFER_EARG, "path too long to quote");
    snprintf(cmd, sizeof cmd, "scp -f %s", q);
    rc = sx_exec(c, x, cmd);
    if (rc != UNOXFER_OK) return rc;

    if (sx_ack(x) < 0) return ux_fail(c, UNOXFER_EIO, "channel closed");

    if (sx_read(x, &lead, 1, 20000, cancel) != 1)
        return ux_fail(c, UNOXFER_EIO, "no response from scp");
    if (lead == 1 || lead == 2) return scp_err(c, x, lead);
    if (lead == 'D')
        return ux_fail(c, UNOXFER_EARG,
                       "that is a directory (the engine walks directories itself)");
    if (lead != 'C')
        return ux_failf(c, UNOXFER_EIO, "unexpected scp record '%c'", lead);

    if (sx_line(x, hdr, (int)sizeof hdr, 20000, cancel) <= 0)
        return ux_fail(c, UNOXFER_EIO, "truncated scp header");
    /* "0644 12345 name" - skip the mode, take the size */
    for (i = 0; hdr[i] && hdr[i] != ' '; i++) { }
    size = (long long)ux_u64(hdr + i);
    if (p) { p->total = (unsigned long long)size; p->done = 0;
             ux_cpy(p->file, (int)sizeof p->file, unoxfer_basename(rpath)); }

    buf = ux_stage_get(size, &capn);
    if (!buf) return ux_fail(c, UNOXFER_EIO, "the staging buffer is busy");
    if (size > capn) {
        ux_stage_put();
        return ux_failf(c, UNOXFER_ETOOBIG,
                        "%s is %lld bytes, over the %lld byte staging cap "
                        "(unofs append not linked)", unoxfer_basename(rpath),
                        size, capn);
    }
    if (sx_ack(x) < 0) { ux_stage_put(); return ux_fail(c, UNOXFER_EIO, "channel closed"); }

    while (got < size) {
        int want = (int)(size - got > 32768 ? 32768 : size - got);
        int n = sx_read(x, buf + got, want, 30000, cancel);
        if (n == -2) { ux_stage_put(); sx_drop(x); return ux_fail(c, UNOXFER_ECANCEL, "cancelled"); }
        if (n <= 0) { ux_stage_put(); sx_drop(x);
                      return ux_failf(c, UNOXFER_EIO, "truncated at %lld of %lld bytes",
                                      got, size); }
        got += n;
        if (p) p->done = (unsigned long long)got;
    }
    if (sx_read(x, &status, 1, 20000, cancel) != 1 || status != 0) {
        ux_stage_put();
        return ux_fail(c, UNOXFER_EIO, "scp reported a failure after the data");
    }
    sx_ack(x);

    rc = ux_commit_file(vol, lpath, buf, (long)got);
    ux_stage_put();
    if (rc != UNOXFER_OK) return ux_failf(c, rc, "write failed: %s", lpath);
    return UNOXFER_OK;
}

static int sx_put(unoxfer_client *c, int vol, const char *lpath,
                  const char *rpath, unoxfer_prog *p)
{
    sx *x = (sx *)c->impl;
    char cmd[SX_CMD], q[UNOXFER_PATHLEN * 2 + 8], hdr[128];
    unsigned char *buf, status;
    long long capn = 0, sent = 0;
    long sz = uno_fs_size(vol, lpath), n;
    int rc;
    volatile int *cancel = p ? &p->cancel : 0;

    if (sz < 0) return ux_failf(c, UNOXFER_ENOENT, "no such local file: %s", lpath);
    buf = ux_stage_get(sz, &capn);
    if (!buf) return ux_fail(c, UNOXFER_EIO, "the staging buffer is busy");
    if (sz > capn) { ux_stage_put();
        return ux_failf(c, UNOXFER_ETOOBIG, "%s is %ld bytes, over the %lld byte "
                        "staging cap", lpath, sz, capn); }
    n = uno_fs_read(vol, lpath, buf, sz);
    if (n < 0) { ux_stage_put(); return ux_failf(c, UNOXFER_EIO, "read failed: %s", lpath); }

    if (!sh_quote(q, (int)sizeof q, rpath)) {
        ux_stage_put(); return ux_fail(c, UNOXFER_EARG, "path too long to quote");
    }
    snprintf(cmd, sizeof cmd, "scp -t %s", q);
    rc = sx_exec(c, x, cmd);
    if (rc != UNOXFER_OK) { ux_stage_put(); return rc; }

    if (sx_read(x, &status, 1, 20000, cancel) != 1 || status != 0) {
        ux_stage_put();
        return status ? scp_err(c, x, status)
                      : ux_fail(c, UNOXFER_EIO, "scp did not answer");
    }
    /* Mode 0644 flat: this OS has no POSIX permission model to carry across,
     * and inventing one from FAT's read-only bit would be a lie with
     * consequences on the far end. */
    snprintf(hdr, sizeof hdr, "C0644 %ld %s\n", n, unoxfer_basename(rpath));
    if (ssh_write(x->h, hdr, (int)strlen(hdr)) < 0) {
        ux_stage_put(); return ux_fail(c, UNOXFER_EIO, "channel closed");
    }
    if (sx_read(x, &status, 1, 20000, cancel) != 1 || status != 0) {
        ux_stage_put();
        return status ? scp_err(c, x, status)
                      : ux_fail(c, UNOXFER_EIO, "scp refused the header");
    }
    if (p) { p->total = (unsigned long long)n; p->done = 0;
             ux_cpy(p->file, (int)sizeof p->file, unoxfer_basename(lpath)); }

    while (sent < n) {
        int want = (int)(n - sent > 16384 ? 16384 : n - sent), w;
        if (cancel && *cancel) { ux_stage_put(); sx_drop(x);
                                 return ux_fail(c, UNOXFER_ECANCEL, "cancelled"); }
        w = ssh_write(x->h, buf + sent, want);
        if (w < 0) { ux_stage_put(); sx_drop(x);
                     return ux_fail(c, UNOXFER_EIO, "channel closed mid-file"); }
        if (w == 0) { net_poll(); ssh_poll(x->h); continue; }  /* window shut */
        sent += w;
        if (p) p->done = (unsigned long long)sent;
    }
    ux_stage_put();
    if (sx_ack(x) < 0) return ux_fail(c, UNOXFER_EIO, "channel closed");
    if (sx_read(x, &status, 1, 20000, cancel) != 1 || status != 0)
        return ux_fail(c, UNOXFER_EIO, "the server rejected the file");
    return UNOXFER_OK;
}

static int sx_mkdir(unoxfer_client *c, const char *path)
{
    sx *x = (sx *)c->impl;
    char cmd[SX_CMD], q[UNOXFER_PATHLEN * 2 + 8], line[128];
    int rc;
    if (!sh_quote(q, (int)sizeof q, path)) return ux_fail(c, UNOXFER_EARG, "path too long");
    snprintf(cmd, sizeof cmd, "mkdir -p %s && echo XOK", q);
    rc = sx_exec(c, x, cmd);
    if (rc != UNOXFER_OK) return rc;
    if (sx_line(x, line, (int)sizeof line, 15000, 0) > 0 && ux_eq(line, "XOK"))
        return UNOXFER_OK;
    return ux_failf(c, UNOXFER_EPERM, "mkdir failed: %s", path);
}

/* Delete is a single file or an EMPTY directory - never `rm -rf`.  A transfer
 * client that can be told to recursively delete a remote path over an
 * automation link is one typo away from being the worst thing on the network,
 * and nothing in this subsystem needs it. */
static int sx_del(unoxfer_client *c, const char *path)
{
    sx *x = (sx *)c->impl;
    char cmd[SX_CMD], q[UNOXFER_PATHLEN * 2 + 8], line[128];
    int rc;
    if (!sh_quote(q, (int)sizeof q, path)) return ux_fail(c, UNOXFER_EARG, "path too long");
    snprintf(cmd, sizeof cmd, "rm -f %s 2>/dev/null || rmdir %s; echo XOK", q, q);
    rc = sx_exec(c, x, cmd);
    if (rc != UNOXFER_OK) return rc;
    if (sx_line(x, line, (int)sizeof line, 15000, 0) > 0 && ux_eq(line, "XOK"))
        return UNOXFER_OK;
    return ux_failf(c, UNOXFER_EPERM, "delete failed: %s", path);
}

/* ===========================================================================
 * open / close, shared by both SSH-based backends.
 * ======================================================================== */
static int sx_open(unoxfer_client *c, const unoxfer_site *s)
{
    sx *x = (sx *)malloc(sizeof *x);
    if (!x) return ux_fail(c, UNOXFER_EIO, "out of memory");
    memset(x, 0, sizeof *x);
    x->h = -1;
    ux_cpy(x->host, (int)sizeof x->host, s->host);
    ux_cpy(x->user, (int)sizeof x->user, s->user[0] ? s->user : "root");
    ux_cpy(x->key,  (int)sizeof x->key,  s->key);
    x->port = s->port ? s->port : 22;

    /* A site may name one of unossh's SAVED SESSIONS instead of spelling the
     * host out.  Reusing that store rather than duplicating it is the whole
     * reason unoxfer has no credential store of its own. */
    if (!x->host[0] && s->name[0]) {
        char h[SSH_HOSTLEN], u[SSH_NAMELEN], k[SSH_NAMELEN];
        int port = 22;
        if (ssh_sess_get(s->name, h, (int)sizeof h, &port,
                         u, (int)sizeof u, k, (int)sizeof k) == 0) {
            ux_cpy(x->host, (int)sizeof x->host, h);
            ux_cpy(x->user, (int)sizeof x->user, u);
            ux_cpy(x->key,  (int)sizeof x->key,  k);
            x->port = port ? port : 22;
        }
    }
    if (!x->host[0]) { free(x); return ux_fail(c, UNOXFER_EARG, "no host"); }
    c->impl = x;
    /* Connect eagerly: an open() that succeeds and a first list() that fails
     * with "auth refused" is a worse experience than failing where the user
     * asked to connect. */
    return sx_login(c, x);
}

static void sx_close(unoxfer_client *c)
{
    sx *x = (sx *)c->impl;
    if (!x) return;
    sx_drop(x);
    free(x);
    c->impl = 0;
}

const unoxfer_backend unoxfer_be_scp = {
    "scp",
    /* No UNOXFER_CAP_RESUME: SCP has no byte offset in its grammar, and a
     * backend that claims resume it cannot do hands back corrupt files. */
    UNOXFER_CAP_LIST | UNOXFER_CAP_GET | UNOXFER_CAP_PUT |
    UNOXFER_CAP_MKDIR | UNOXFER_CAP_DELETE | UNOXFER_CAP_SIZE,
    sx_open, sx_close, sx_list, sx_size, sx_get, sx_put, sx_mkdir, sx_del
};

/* ---- SFTP ----------------------------------------------------------------
 * Present, registered, and honest: unoxfer_proto_ready() answers 0 for it
 * until unossh lands ssh_subsystem(), so unoxfer_open() refuses with a reason
 * and this open() is not reached.  The row exists so the protocol has an
 * identity, a port and a name in the UI today, and so the day the subsystem
 * channel lands is a day of writing packet code and not of plumbing. */
static int sf_open(unoxfer_client *c, const unoxfer_site *s)
{
    (void)s;
    return ux_fail(c, UNOXFER_EUNSUP,
                   "SFTP needs unossh's ssh_subsystem() (requested 2026-08-22); "
                   "use scp:// to the same host meanwhile");
}

const unoxfer_backend unoxfer_be_sftp = {
    "sftp",
    UNOXFER_CAP_LIST | UNOXFER_CAP_GET | UNOXFER_CAP_PUT | UNOXFER_CAP_MKDIR |
    UNOXFER_CAP_DELETE | UNOXFER_CAP_SIZE | UNOXFER_CAP_RESUME,
    sf_open, 0, 0, 0, 0, 0, 0, 0
};
