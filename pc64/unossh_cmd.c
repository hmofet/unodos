/* ===========================================================================
 * unossh - the automation verb.
 *
 * ONE entry point, `ssh_dbg_cmd`, the same shape as r8169_dbg_cmd and
 * uno_hw_wdt_cmd. unoautomate lands a weak stub and a four-line dispatch
 * clause once; after that everything here is invisible to it, because the
 * sub-verb grammar and the output format are ours. That is the pattern
 * HARNESS-POLICY documents and the reason this file needs no edit anywhere
 * else in the tree.
 *
 * The point of the verb: the harness that already commands THIS machine can
 * log into others and command those. Which is why the core is headless - it
 * has to work on a box with no desktop drawn.
 *
 * DESIGNED AROUND THE 8 KB TX BUFFER, not around it. unoautomate's g_tx is
 * 8192 bytes and tx_putn DROPS SILENTLY past it, while the output of a remote
 * command is unbounded. So `ssh run` returns an id and a length, and `ssh get
 * <id> <off>` hands back one bounded slice at a time - exactly the idiom
 * `readsec` and `screen read` already use. Nothing here asks unoautomate for
 * new streaming machinery.
 * ======================================================================== */
#include "unossh.h"

#define OUTCAP   32768           /* one command's captured output */
#define SLICEMAX 1024            /* per `get`, comfortably under g_tx */

static char g_out[OUTCAP];
static int  g_outlen, g_id, g_conn = -1;

/* ---- tiny text helpers (no libc in this build) --------------------------- */
static int sc_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int sc_eqn(const char *a, const char *b, int n)
{ int i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

/* copy one whitespace-delimited word out of `s`, advancing it */
static int sc_word(const char **s, char *out, int cap)
{
    int n = 0;
    while (**s == ' ' || **s == '\t') (*s)++;
    while (**s && **s != ' ' && **s != '\t' && n < cap - 1) out[n++] = *(*s)++;
    out[n] = 0;
    return n;
}

static int sc_num(const char *s)
{ int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

typedef struct { char *p; int cap, len; } sc_buf;
static void sc_put(sc_buf *b, const char *s)
{ while (*s && b->len < b->cap - 1) b->p[b->len++] = *s++; b->p[b->len] = 0; }
static void sc_putn(sc_buf *b, int v)
{
    char t[12];
    int i = 0, j;
    if (v < 0) { sc_put(b, "-"); v = -v; }
    if (!v) { sc_put(b, "0"); return; }
    while (v && i < 11) { t[i++] = (char)('0' + v % 10); v /= 10; }
    for (j = i - 1; j >= 0; j--) { if (b->len < b->cap - 1) b->p[b->len++] = t[j]; }
    b->p[b->len] = 0;
}

/* ---- open a connection from a saved session, or user@host ---------------- */
static int open_target(const char *target, sc_buf *b)
{
    char host[SSH_HOSTLEN], user[SSH_NAMELEN], keyn[SSH_NAMELEN];
    unsigned char seed[32];
    int port = 22, h, i;

    if (ssh_sess_get(target, host, sizeof host, &port, user, sizeof user,
                     keyn, sizeof keyn) != 0) {
        /* not a saved session: accept user@host, no key -> no way to auth */
        for (i = 0; target[i] && target[i] != '@'; i++) ;
        if (!target[i]) { sc_put(b, "no such session: "); sc_put(b, target); return -1; }
        { int k; for (k = 0; k < i && k < (int)sizeof user - 1; k++) user[k] = target[k];
          user[k] = 0; }
        { int k = 0; const char *hp = target + i + 1;
          while (hp[k] && k < (int)sizeof host - 1) { host[k] = hp[k]; k++; }
          host[k] = 0; }
        keyn[0] = 0;
    }
    if (!keyn[0]) { sc_put(b, "session has no key; add one with sessadd"); return -1; }
    if (ssh_key_load(keyn, "", seed) != 0) {
        sc_put(b, "key '"); sc_put(b, keyn);
        sc_put(b, "' needs a passphrase, which the verb cannot ask for");
        return -1;
    }
    h = ssh_connect(host, port);
    if (h < 0) { sc_put(b, "connect: "); sc_put(b, ssh_error(h)); return -1; }
    if (ssh_handshake(h) != 0) {
        sc_put(b, "handshake: "); sc_put(b, ssh_error(h)); ssh_close(h); return -1;
    }
    /* known-hosts is advisory here and reported, never silently ignored */
    switch (ssh_verify_host(h, host)) {
    case SSH_HOST_MISMATCH:
        sc_put(b, "HOST KEY MISMATCH for "); sc_put(b, host);
        ssh_close(h); return -1;
    case SSH_HOST_UNKNOWN:
        ssh_known_add(host, ssh_host_fingerprint(h));
        break;
    default: break;
    }
    if (ssh_auth_key(h, user, seed) != 0) {
        sc_put(b, "auth: "); sc_put(b, ssh_error(h)); ssh_close(h); return -1;
    }
    return h;
}

/* ---- the verb ------------------------------------------------------------ */
int ssh_dbg_cmd(const char *line, char *out, int cap)
{
    sc_buf b;
    char verb[24];
    const char *p = line ? line : "";

    b.p = out; b.cap = cap; b.len = 0;
    if (cap > 0) out[0] = 0;
    sc_word(&p, verb, (int)sizeof verb);

    if (!verb[0] || sc_eqn(verb, "help", 5)) {
        sc_put(&b, "keys | keygen <n> | keypub <n> | keyrm <n> | "
                   "sess | sessadd <n> <host> <port> <user> <key> | sessrm <n> | "
                   "hosts | run <sess|user@host> <cmd...> | get <id> <off> | close");
        return b.len;
    }

    if (sc_eqn(verb, "keys", 5)) {
        char name[SSH_NAMELEN];
        int i, guarded, n = 0;
        for (i = 0; ssh_key_list(i, name, sizeof name, 0, &guarded) == 0; i++) {
            if (n++) sc_put(&b, "\n");
            sc_put(&b, name);
            sc_put(&b, guarded ? " (passphrase)" : " (unprotected)");
        }
        if (!n) sc_put(&b, "no keys");
        if (!ssh_store_persistent()) sc_put(&b, "\nWARNING: store is on the RAM disk");
        return b.len;
    }
    if (sc_eqn(verb, "keygen", 7)) {
        char name[SSH_NAMELEN];
        if (!sc_word(&p, name, sizeof name)) { sc_put(&b, "keygen <name>"); return b.len; }
        if (ssh_key_generate(name, "") != 0) { sc_put(&b, "generate failed"); return -1; }
        sc_put(&b, "generated "); sc_put(&b, name);
        return b.len;
    }
    if (sc_eqn(verb, "keypub", 7)) {
        char name[SSH_NAMELEN];
        if (!sc_word(&p, name, sizeof name)) { sc_put(&b, "keypub <name>"); return b.len; }
        if (ssh_key_export_pub(name, out, cap) <= 0) { sc_put(&b, "no such key"); return -1; }
        return sc_len(out);
    }
    if (sc_eqn(verb, "keyrm", 6)) {
        char name[SSH_NAMELEN];
        sc_word(&p, name, sizeof name);
        if (ssh_key_delete(name) != 0) { sc_put(&b, "no such key"); return -1; }
        sc_put(&b, "deleted");
        return b.len;
    }

    if (sc_eqn(verb, "sess", 5)) {
        char name[SSH_NAMELEN], host[SSH_HOSTLEN], user[SSH_NAMELEN], keyn[SSH_NAMELEN];
        int i, port, n = 0;
        for (i = 0; ssh_sess_list(i, name, sizeof name) == 0; i++) {
            ssh_sess_get(name, host, sizeof host, &port, user, sizeof user, keyn, sizeof keyn);
            if (n++) sc_put(&b, "\n");
            sc_put(&b, name); sc_put(&b, " "); sc_put(&b, user); sc_put(&b, "@");
            sc_put(&b, host); sc_put(&b, ":"); sc_putn(&b, port);
            sc_put(&b, " key="); sc_put(&b, keyn[0] ? keyn : "-");
        }
        if (!n) sc_put(&b, "no sessions");
        return b.len;
    }
    if (sc_eqn(verb, "sessadd", 8)) {
        char name[SSH_NAMELEN], host[SSH_HOSTLEN], port[8], user[SSH_NAMELEN], keyn[SSH_NAMELEN];
        sc_word(&p, name, sizeof name); sc_word(&p, host, sizeof host);
        sc_word(&p, port, sizeof port); sc_word(&p, user, sizeof user);
        sc_word(&p, keyn, sizeof keyn);
        if (!name[0] || !host[0]) { sc_put(&b, "sessadd <name> <host> <port> <user> <key>"); return b.len; }
        if (ssh_sess_set(name, host, sc_num(port), user, keyn) != 0)
            { sc_put(&b, "no room"); return -1; }
        sc_put(&b, "saved "); sc_put(&b, name);
        return b.len;
    }
    if (sc_eqn(verb, "sessrm", 7)) {
        char name[SSH_NAMELEN];
        sc_word(&p, name, sizeof name);
        if (ssh_sess_delete(name) != 0) { sc_put(&b, "no such session"); return -1; }
        sc_put(&b, "deleted");
        return b.len;
    }
    if (sc_eqn(verb, "hosts", 6)) {
        sc_put(&b, ssh_store_persistent() ? "store: native volume"
                                          : "store: RAM DISK (not persistent)");
        return b.len;
    }

    /* run: capture the whole output here, hand it back in slices */
    if (sc_eqn(verb, "run", 4)) {
        char target[SSH_HOSTLEN];
        int h, guard;
        if (!sc_word(&p, target, sizeof target)) { sc_put(&b, "run <target> <cmd...>"); return b.len; }
        while (*p == ' ') p++;
        if (!*p) { sc_put(&b, "run <target> <cmd...>"); return b.len; }
        h = open_target(target, &b);
        if (h < 0) return -1;
        g_outlen = 0;
        if (ssh_exec(h, p) != 0) {
            sc_put(&b, "exec: "); sc_put(&b, ssh_error(h)); ssh_close(h); return -1;
        }
        for (guard = 0; guard < 60000; guard++) {
            int n;
            ssh_poll(h);
            n = ssh_read(h, g_out + g_outlen, OUTCAP - g_outlen);
            if (n > 0) { g_outlen += n; continue; }
            if (!ssh_channel_open(h)) break;
        }
        g_id++;
        b.len = 0;
        sc_put(&b, "id="); sc_putn(&b, g_id);
        sc_put(&b, " len="); sc_putn(&b, g_outlen);
        sc_put(&b, " exit="); sc_putn(&b, ssh_exit_status(h));
        ssh_close(h);
        return b.len;
    }
    /* get: ONE bounded slice. The caller loops until off >= len. */
    if (sc_eqn(verb, "get", 4)) {
        char idtxt[12], offtxt[12];
        int id, off, n, i;
        sc_word(&p, idtxt, sizeof idtxt);
        sc_word(&p, offtxt, sizeof offtxt);
        id = sc_num(idtxt); off = sc_num(offtxt);
        if (id != g_id) { sc_put(&b, "stale id"); return -1; }
        if (off < 0 || off > g_outlen) { sc_put(&b, "bad offset"); return -1; }
        n = g_outlen - off;
        if (n > SLICEMAX) n = SLICEMAX;
        if (n > cap - 1) n = cap - 1;
        for (i = 0; i < n; i++) out[i] = g_out[off + i];
        out[n] = 0;
        return n;
    }
    if (sc_eqn(verb, "close", 6)) {
        if (g_conn >= 0) { ssh_close(g_conn); g_conn = -1; }
        g_outlen = 0;
        sc_put(&b, "closed");
        return b.len;
    }

    sc_put(&b, "bad-cmd (try: help)");
    return -1;
}
