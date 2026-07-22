/* ===========================================================================
 * UNOAUTOMATE remote channel - see unoauto_remote.h.
 *
 * A cooperative, non-blocking TCP client that dials the dev PC and speaks the
 * URC line protocol: it streams LOG lines out (via an unoauto sink), executes
 * inbound commands against the same DRIVE/PROBE/TEST surface the Python
 * `unoauto` module uses, and hands MSG payloads to Python consumers.
 * ======================================================================== */
#include "unoauto.h"
#include "unoauto_remote.h"
#include "net.h"            /* u8/u16, net_tcp_*, net_poll */
#include "pc64_http.h"      /* pc64_net_up */

#ifdef UNO_DEBUG

/* ---- freestanding libc + debug kernel symbols (no public header) --------- */
void        *memcpy(void *, const void *, unsigned long);
void        *memmove(void *, const void *, unsigned long);
unsigned long strlen(const char *);
void  uno_pc64_inject_key(int scan, int uni, int ctrl);
void  uno_pc64_inject_pointer(int x, int y, int btn);
int   pc64_shell_app_count(void);
int   pc64_shell_launch(int a);
void  pc64_shell_close_top(void);
void  uno_pc64_shutdown(void);
unsigned long long uno_dbg_uptime_ms(void);
int   pc64_stress_cfg_value(const char *key, char *buf, int cap);
int   pc64_shell_py_exec(const char *src, char *out, int cap);   /* pc64_uui.c */

/* ---- tiny string builder (avoids snprintf; see the S-LIBC-06 history) ---- */
typedef struct { char *p; int cap, len; } SB;
static void sb_init(SB *b, char *buf, int cap) { b->p = buf; b->cap = cap; b->len = 0; }
static void sb_c(SB *b, char c)   { if (b->len < b->cap - 1) b->p[b->len++] = c; }
static void sb_s(SB *b, const char *s) { while (*s) sb_c(b, *s++); }
static void sb_i(SB *b, long v)
{
    char t[24]; int n = 0; unsigned long u;
    if (v < 0) { sb_c(b, '-'); u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) { sb_c(b, '0'); return; }
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) sb_c(b, t[--n]);
}

/* ---- link state ---------------------------------------------------------- */
enum { RS_OFF = 0, RS_CONNECTING, RS_UP, RS_DOWN };
static int      g_state;
static u8       g_ip[4];
static u16      g_port;
static int      g_sink = -1;
static unsigned g_tick;
static unsigned g_deadline;         /* connect timeout / retry-at (in ticks)  */
static int      g_pending_off;      /* shut down once the TX queue drains      */

/* outbound byte queue (linear, compacted on flush) */
static char     g_tx[8192];
static int      g_txlen;
static unsigned g_tx_dropped;

/* inbound line assembly */
static char     g_rx[2048];
static int      g_rxlen;

/* inbound MSG queue handed to Python via unoauto_remote_recv */
#define INQN 8
#define INQL 256
static char     g_inq[INQN][INQL];
static int      g_in_head, g_in_tail;

/* ---- outbound framing ---------------------------------------------------- */
static void tx_putn(const char *s, int n)
{
    if (n <= 0) return;
    if (g_txlen + n > (int)sizeof g_tx) { g_tx_dropped += (unsigned)n; return; }
    memcpy(g_tx + g_txlen, s, (unsigned long)n);
    g_txlen += n;
}

static const char *chan_name(UnoAutoChan ch)
{
    switch (ch) {
    case UA_CH_KERNEL:  return "KERNEL";
    case UA_CH_NET:     return "NET";
    case UA_CH_UI:      return "UI";
    case UA_CH_STORAGE: return "STORAGE";
    case UA_CH_TEST:    return "TEST";
    case UA_CH_SCRIPT:  return "SCRIPT";
    default:            return "?";
    }
}

/* the LOG spine: every channel line becomes a `LOG <chan> <text>` frame while
 * the link is up.  Registered as an unoauto sink so producers never know. */
static void remote_sink(UnoAutoChan ch, const char *line, void *user)
{
    char f[600]; SB b; (void)user;
    if (g_state != RS_UP) return;
    sb_init(&b, f, sizeof f);
    sb_s(&b, "LOG "); sb_s(&b, chan_name(ch)); sb_c(&b, ' '); sb_s(&b, line);
    sb_c(&b, '\n');
    tx_putn(f, b.len);
}

/* one `RSP <id> <status> [text]` frame (id echoed verbatim). */
static void rsp(const char *id, const char *status, const char *text)
{
    char f[600]; SB b;
    sb_init(&b, f, sizeof f);
    sb_s(&b, "RSP "); sb_s(&b, id); sb_c(&b, ' '); sb_s(&b, status);
    if (text && *text) { sb_c(&b, ' '); sb_s(&b, text); }
    sb_c(&b, '\n');
    tx_putn(f, b.len);
}

int unoauto_remote_send(const char *type, const char *text)
{
    char f[600]; SB b; int before = g_txlen;
    if (g_state != RS_UP) return -1;
    sb_init(&b, f, sizeof f);
    sb_s(&b, type ? type : "MSG");
    if (text && *text) { sb_c(&b, ' '); sb_s(&b, text); }
    sb_c(&b, '\n');
    tx_putn(f, b.len);
    return g_txlen > before ? b.len : -1;
}

/* ---- inbound MSG queue --------------------------------------------------- */
static void inq_push(const char *s)
{
    int i = 0;
    int nxt = (g_in_head + 1) % INQN;
    if (nxt == g_in_tail) g_in_tail = (g_in_tail + 1) % INQN;  /* drop oldest */
    while (s[i] && i < INQL - 1) { g_inq[g_in_head][i] = s[i]; i++; }
    g_inq[g_in_head][i] = 0;
    g_in_head = nxt;
}

int unoauto_remote_recv(char *buf, int cap)
{
    int i = 0; const char *s;
    if (g_in_tail == g_in_head || cap <= 0) { if (cap > 0) buf[0] = 0; return 0; }
    s = g_inq[g_in_tail];
    while (s[i] && i < cap - 1) { buf[i] = s[i]; i++; }
    buf[i] = 0;
    g_in_tail = (g_in_tail + 1) % INQN;
    return i;
}

/* ---- token helpers ------------------------------------------------------- */
/* next whitespace-delimited token: NUL-terminates it in place, advances *s. */
static char *tok(char **s)
{
    char *p = *s, *start;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *s = p; return 0; }
    start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p = 0; p++; }
    *s = p;
    return start;
}
static void skip_ws(char **s) { while (**s == ' ' || **s == '\t') (*s)++; }
static long atol_(const char *s)
{
    long v = 0; int neg = 0;
    if (!s) return 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
/* 0 when equal (like strcmp==0), tolerant of NULL a. */
static int strcmp_(const char *a, const char *b)
{
    if (!a) return 1;
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ---- command dispatch ---------------------------------------------------- */
static UnoAutoProbeEnt g_pe[64];
static char            g_report[4096];

static void do_probe(const char *id)
{
    int n = unoauto_probe(g_pe, 64), i;
    for (i = 0; i < n; i++) {
        /* fields: kind state v1 v2 name - name LAST so it may contain spaces */
        char f[256]; SB b;
        sb_init(&b, f, sizeof f);
        sb_i(&b, g_pe[i].kind);  sb_c(&b, ' ');
        sb_i(&b, g_pe[i].state); sb_c(&b, ' ');
        sb_i(&b, (long)g_pe[i].v1); sb_c(&b, ' ');
        sb_i(&b, (long)g_pe[i].v2); sb_c(&b, ' ');
        sb_s(&b, g_pe[i].name ? g_pe[i].name : "?");
        f[b.len] = 0;
        rsp(id, "ok", f);
    }
    rsp(id, "end", 0);
}

static void do_test(const char *id, char *suite)
{
    char *p = g_report; int rc;
    if (suite && !*suite) suite = 0;
    rc = unoauto_test_run(suite, 0, g_report, (int)sizeof g_report);
    /* stream the report line by line */
    while (*p) {
        char *nl = p;
        while (*nl && *nl != '\n') nl++;
        { char save = *nl; *nl = 0; rsp(id, "ok", p); *nl = save; }
        if (!*nl) break;
        p = nl + 1;
    }
    { char t[32]; SB b; sb_init(&b, t, sizeof t); sb_s(&b, "rc="); sb_i(&b, rc); t[b.len] = 0;
      rsp(id, rc == 0 ? "ok" : "err", t); }
    rsp(id, "end", 0);
}

/* execute `verb args...` (id echoed on every RSP). args is the remainder. */
static void dispatch_cmd(const char *id, char *verb, char *args)
{
    if (!verb) { rsp(id, "err", "empty"); rsp(id, "end", 0); return; }

    if (!strcmp_(verb, "probe")) { do_probe(id); return; }

    if (!strcmp_(verb, "log")) {
        unoauto_log(UA_CH_SCRIPT, "%s", args ? args : "");
        rsp(id, "ok", "logged"); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "key")) {
        char *a1 = tok(&args), *a2 = tok(&args), *a3 = tok(&args);
        uno_pc64_inject_key((int)atol_(a1), (int)atol_(a2), a3 ? (int)atol_(a3) : 0);
        rsp(id, "ok", 0); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "pointer")) {
        char *a1 = tok(&args), *a2 = tok(&args), *a3 = tok(&args);
        uno_pc64_inject_pointer((int)atol_(a1), (int)atol_(a2), (int)atol_(a3));
        rsp(id, "ok", 0); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "apps")) {
        char t[16]; SB b; sb_init(&b, t, sizeof t); sb_i(&b, pc64_shell_app_count()); t[b.len] = 0;
        rsp(id, "ok", t); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "launch")) {
        int ok = pc64_shell_launch((int)atol_(tok(&args)));
        rsp(id, ok ? "ok" : "err", ok ? "launched" : "no-app"); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "close")) {
        pc64_shell_close_top(); rsp(id, "ok", 0); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "uptime")) {
        char t[24]; SB b; sb_init(&b, t, sizeof t); sb_i(&b, (long)uno_dbg_uptime_ms()); t[b.len] = 0;
        rsp(id, "ok", t); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "poweroff")) {
        g_pending_off = 1; rsp(id, "ok", "bye"); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "test")) {
        do_test(id, tok(&args)); return;
    }
    if (!strcmp_(verb, "py")) {
        int rc = pc64_shell_py_exec(args ? args : "", g_report, (int)sizeof g_report);
        char *p = g_report;
        while (*p) {                              /* stream captured output */
            char *nl = p; while (*nl && *nl != '\n') nl++;
            { char save = *nl; *nl = 0; rsp(id, rc == 0 ? "ok" : "err", p); *nl = save; }
            if (!*nl) break;
            p = nl + 1;
        }
        if (!g_report[0]) rsp(id, rc == 0 ? "ok" : "err", rc == 0 ? "" : "error");
        rsp(id, "end", 0); return;
    }
    rsp(id, "err", "unknown-verb"); rsp(id, "end", 0);
}

static void dispatch_line(char *line)
{
    char *type = tok(&line);
    if (!type) return;
    if (!strcmp_(type, "CMD")) {
        char *id = tok(&line);
        char *verb = tok(&line);
        skip_ws(&line);
        if (!id) return;
        dispatch_cmd(id, verb, line);
    } else if (!strcmp_(type, "MSG")) {
        skip_ws(&line);
        inq_push(line);
    } else if (!strcmp_(type, "HELLO")) {
        /* peer greeting; nothing required */
    } else if (!strcmp_(type, "BYE")) {
        unoauto_remote_stop();
    }
    /* RSP frames (responses to pc64-initiated CMDs) are surfaced to scripts
     * as MSG-style inbound lines too, so a script can correlate them. */
    else if (!strcmp_(type, "RSP")) { skip_ws(&line); inq_push(line); }
}

/* ---- pump ---------------------------------------------------------------- */
static void flush_tx(void)
{
    int n, r;
    if (g_txlen <= 0) return;
    n = g_txlen; if (n > 512) n = 512;
    r = net_tcp_send(g_tx, n);           /* one segment in flight at a time */
    if (r > 0) {
        g_txlen -= r;
        if (g_txlen > 0) memmove(g_tx, g_tx + r, (unsigned long)g_txlen);
    }
}

static void drain_rx(void)
{
    unsigned char buf[512];
    int n, i;
    while ((n = net_tcp_recv(buf, (int)sizeof buf)) > 0) {
        for (i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') { g_rx[g_rxlen] = 0; dispatch_line(g_rx); g_rxlen = 0; }
            else if (g_rxlen < (int)sizeof g_rx - 1) g_rx[g_rxlen++] = c;
            /* else: overline - drop chars until the next '\n' */
        }
    }
}

static void start_connect(void)
{
    g_txlen = 0; g_rxlen = 0;
    if (!pc64_net_up()) { g_state = RS_DOWN; g_deadline = g_tick + 300; return; }
    net_tcp_connect(g_ip, g_port);
    g_state = RS_CONNECTING;
    g_deadline = g_tick + 600;           /* ~10 s handshake window */
}

void unoauto_remote_boot(void)
{
    char v[64]; char *p; int oct, i;
    if (g_state != RS_OFF) return;                 /* armed once */
    if (pc64_stress_cfg_value("remote", v, (int)sizeof v) <= 0) return;   /* not set */
    /* parse a.b.c.d:port */
    p = v;
    for (i = 0; i < 4; i++) {
        oct = (int)atol_(p);
        g_ip[i] = (u8)oct;
        while (*p && *p != '.' && *p != ':') p++;
        if (i < 3) { if (*p != '.') { goto bad; } p++; }
    }
    if (*p != ':') goto bad;
    p++;
    g_port = (u16)atol_(p);
    if (!g_port) goto bad;
    if (g_sink < 0)
        g_sink = unoauto_sink_add((1u << UA_CH_COUNT) - 1, remote_sink, 0);
    unoauto_log(UA_CH_SCRIPT, "remote: dialing %d.%d.%d.%d:%d",
                g_ip[0], g_ip[1], g_ip[2], g_ip[3], g_port);
    start_connect();
    return;
bad:
    unoauto_log(UA_CH_SCRIPT, "remote: bad address '%s' in STRESS.CFG", v);
}

void unoauto_remote_tick(void)
{
    int st;
    if (g_state == RS_OFF) return;
    g_tick++;
    net_poll();
    st = net_tcp_state();
    switch (g_state) {
    case RS_CONNECTING:
        if (st == TCP_ESTABLISHED) {
            g_state = RS_UP;
            unoauto_remote_send("HELLO", "pc64 1");
            /* now that the sink is live, announce the link on the SCRIPT
             * channel - this line flows straight back out as a LOG frame,
             * seeding the remote-log stream. */
            unoauto_log(UA_CH_SCRIPT, "remote: link up");
        } else if (st == TCP_CLOSED || st == TCP_DONE || g_tick > g_deadline) {
            net_tcp_close();
            g_state = RS_DOWN; g_deadline = g_tick + 300;   /* retry ~5 s */
        }
        break;
    case RS_UP:
        drain_rx();
        flush_tx();
        if (st != TCP_ESTABLISHED) {
            g_state = RS_DOWN; g_deadline = g_tick + 300;
        } else if (g_pending_off && g_txlen == 0) {
            uno_pc64_shutdown();
        }
        break;
    case RS_DOWN:
        if (g_tick >= g_deadline) start_connect();
        break;
    default: break;
    }
}

int unoauto_remote_active(void) { return g_state == RS_UP; }

void unoauto_remote_stop(void)
{
    if (g_state == RS_UP) { unoauto_remote_send("BYE", 0); flush_tx(); }
    net_tcp_close();
    g_state = RS_OFF;
    g_txlen = 0; g_rxlen = 0;
}

#endif /* UNO_DEBUG */
