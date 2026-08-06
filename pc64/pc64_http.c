/* pc64_http - HTTP/1.0 for the browser. See pc64_http.h. */
#include "pc64_http.h"
#include "pc64_cookie.h"
#include "pc64_cache.h"
#include "net.h"
#include "netsock.h"      /* one socket per request in flight */
#include "pc64_native.h"  /* the TSC: the only clock a production build has */
#include "e1000.h"
#include "e1000e.h"       /* Intel e1000e GbE (82574 / I217/I218/I219 LOM) */
#include "igb.h"          /* Intel igb GbE (I210/I211/82576) */
#include "r8169.h"        /* Realtek RTL8168/8111 GbE (the common consumer NIC) */
#include "ax88179.h"      /* USB Ethernet fallback (X1 has no wired NIC) */
#include "rtl8152.h"      /* the other common USB Ethernet family (docks) */
#include "iwlwifi.h"      /* Intel AC/AX WiFi (firmware-driven; WIFI.CFG) */
#include "rtwifi.h"       /* Realtek PCIe WiFi (rtw88/rtw89) */
#include "mrvlwifi.h"     /* Marvell/NXP PCIe WiFi (mwifiex) */
#include "tls.h"          /* tls_open / tls_send / tls_recv (HTTPS) */
#include <string.h>
#include <stdlib.h>

void uno_pc64_delay_ms(int ms);          /* firmware Stall (uefi_main) */

/* ---- shared network bring-up (idempotent) -------------------------------- */
static int g_net_inited;

struct nicent { uno_nic_t *(*n)(void); const unsigned char *(*m)(void); };
/* wired NICs are cheap to probe; the WiFi entries run a full multi-second
 * bring-up ON PROBE, so they are a separate tier reached only if no wired NIC
 * exists at all. */
static const struct nicent g_wired[] = {
    { e1000_nic, e1000_mac },   /* native PCI (82540/82545)      */
    { e1000e_nic, e1000e_mac }, /* Intel e1000e (82574 / I219)   */
    { igb_nic, igb_mac },       /* Intel igb (I210/I211/82576)   */
    { ax88179_nic, ax88179_mac },/* USB Ethernet (ASIX)          */
    { rtl8152_nic, rtl8152_mac },/* USB Ethernet (Realtek dock)  */
    { r8169_nic, r8169_mac },   /* Realtek RTL8168/8111 - LAST: it is under bring-up
                                 * (dead RX until proven), so it must not be chosen as the
                                 * management uplink ahead of a working USB adapter. On a
                                 * Zimaboard (onboard r8169 + ASIX dongle) with no lease from
                                 * the boot eth-test, pc64_net_up() used to bind this and
                                 * strand the URC link on a receive-dead NIC. */
};
static const struct nicent g_wifi[] = {
    { iwl_nic, iwl_mac }, { rtwifi_nic, rtwifi_mac }, { mrvlwifi_nic, mrvlwifi_mac },
};

/* run DHCP on the already-net_init'd nic, waiting for link first. USB ethernet
 * (ax88179/rtl8152) programs the MAC medium from the PHY's negotiated status
 * inside nic->link(), and a wrong medium silently kills RX, so link MUST be up
 * before DHCP. ~3 s covers autoneg. */
static int net_dhcp_after_link(uno_nic_t *nic)
{
    int i;
    if (nic->link) { for (i = 0; i < 600 && !nic->link(nic->ctx); i++) uno_pc64_delay_ms(5); }
    /* net_dhcp_start sends one DISCOVER; net_poll's dhcp_tick retransmits it
     * (and the REQUEST) every ~1.5 s, so a lost OFFER/ACK recovers. Wait ~9 s. */
    net_dhcp_start();
    for (i = 0; i < 1800 && !net_dhcp_done(); i++) { net_poll(); uno_pc64_delay_ms(5); }
    /* Report whether we actually LEASED, not merely that we tried. This used to
     * `return 1` unconditionally, so pc64_net_up() claimed success on a NIC with
     * link but no address: callers sailed past their "no network" branch and
     * failed later at the first DNS lookup instead, which is what made the
     * ZimaBlade's dead link present itself as "DNS lookup failed" with no lease
     * behind it (2026-07-27). */
    return net_dhcp_done();
}

int pc64_net_up(void)
{
    uno_nic_t *nic, *fb = 0;
    const unsigned char *fbmac = 0;
    int i, nw = (int)(sizeof g_wired / sizeof g_wired[0]);
    /* Already bound. Report the LEASE, not just that we bound something: this
     * used to `return net_link() || 1`, i.e. 1 unconditionally, which put the
     * same optimistic answer back for every caller after the first and undid
     * the honest result from net_dhcp_after_link below. Still latched on
     * g_net_inited so we never re-net_init a bound adapter (re-initialising a
     * USB NIC brings its RX up dead - see the note above). */
    if (g_net_inited) return net_dhcp_done();

    /* Reuse a stack that already holds a DHCP lease. The boot eth TEST brings
     * the ASIX up and leases with WORKING RX; re-net_init'ing it here starts a
     * second bring-up whose RX comes up dead (tx>0 rx=0 - the medium/bulk-in
     * re-arm does not survive the re-init), stranding the remote link on a
     * link-up-but-leaseless NIC. Gating on an actual lease (not just link) is
     * safe: the null test NIC used by S-NET-08/19 never leases, so SPECTEST
     * still exercises the full fresh-probe path below. */
    if (net_dhcp_done()) { g_net_inited = 1; return 1; }

    /* Pass 1: a WIRED NIC that reports link up wins. This is the fix for a
     * machine with a cableless onboard Intel port (e.g. the X13 Yoga's I219
     * 0x0d4f, claimed by e1000e) sitting IN FRONT of the USB dongle that
     * actually has the cable - committing to the linkless onboard port left
     * the net stack "bound, no link" and the remote link never dialed. */
    for (i = 0; i < nw; i++) {
        nic = g_wired[i].n(); if (!nic) continue;
        if (!fb) { fb = nic; fbmac = g_wired[i].m(); }   /* first bound = fallback */
        if (nic->link && nic->link(nic->ctx)) {          /* has a cable */
            net_init(nic, g_wired[i].m()); g_net_inited = 1;
            return net_dhcp_after_link(nic);
        }
    }
    /* Pass 2: a wired NIC bound but none had link yet - use the first and wait
     * for autoneg (a cable that just needs a moment). */
    if (fb) { net_init(fb, fbmac); g_net_inited = 1; return net_dhcp_after_link(fb); }
    /* Pass 3: no wired NIC at all - fall back to WiFi (expensive bring-up). */
    for (i = 0; i < (int)(sizeof g_wifi / sizeof g_wifi[0]); i++) {
        nic = g_wifi[i].n();
        if (nic) { net_init(nic, g_wifi[i].m()); g_net_inited = 1; return net_dhcp_after_link(nic); }
    }
    return 0;                                    /* no NIC at all */
}

/* ---- boot-time proactive bring-up ---------------------------------------- *
 * pc64_net_up() above is LAZY (first net use) and commits to the first link-up
 * NIC. pc64_net_boot() is the eager boot-path variant: it walks the SAME device
 * tables but treats each device as pass/fail on whether it actually LEASES
 * within a shared time budget, so a link-up-but-receive-dead NIC (the ZimaBlade's
 * onboard Realtek) or a cableless port is tried, skipped, and the next device
 * gets its turn - and the whole thing is bounded, so it can never hang boot. It
 * settles on the first device that leases and leaves the on-demand path intact
 * for everything after. */

#ifdef UNO_DEBUG
void uno_dbg_log(const char *fmt, ...);
#define NETBOOT_LOG(...) uno_dbg_log(__VA_ARGS__)
#else
#define NETBOOT_LOG(...) ((void)0)
#endif

/* Bring one NIC up and return 1 iff it obtains a DHCP lease before `*budget_ms`
 * (approx, decremented as we wait) runs out. All waits are bounded by both a
 * loop cap and the shared budget, so a stuck device just burns its slice and is
 * skipped. Timeouts are tighter than pc64_net_up's since several NICs may be
 * tried at boot. */
static int net_try_lease(uno_nic_t *nic, const unsigned char *mac, int *budget)
{
    int i;
    if (*budget <= 0) return 0;
    net_init(nic, mac);
    /* Wait for link (autoneg) - up to ~3 s, never past the shared budget. The
     * old ~1 s cap was below what gigabit autoneg actually takes (2-5 s is
     * normal), so a perfectly good wired NIC could be declared linkless and
     * skipped. pc64_net_up's net_dhcp_after_link already waits ~3 s; the eager
     * boot path should not be stingier than the lazy one it runs ahead of.
     *
     * And if the link never comes up, return NOW instead of spending the DHCP
     * window broadcasting into a dead PHY - that hands the unused budget to the
     * next device rather than burning it here. */
    if (nic->link) {
        for (i = 0; i < 600 && *budget > 0 && !nic->link(nic->ctx); i++) { uno_pc64_delay_ms(5); *budget -= 5; }
        if (!nic->link(nic->ctx)) return 0;
    }
    /* DHCP - up to ~3 s; net_poll retransmits DISCOVER/REQUEST as it pumps */
    net_dhcp_start();
    for (i = 0; i < 600 && *budget > 0 && !net_dhcp_done(); i++) { net_poll(); uno_pc64_delay_ms(5); *budget -= 5; }
    return net_dhcp_done();
}

int pc64_net_boot(void)
{
    int i, nw = (int)(sizeof g_wired / sizeof g_wired[0]);
    int nf = (int)(sizeof g_wifi  / sizeof g_wifi[0]);
    int budget = 8000;                           /* total ms across all devices */
    uno_nic_t *nic;

    if (g_net_inited || net_dhcp_done()) { g_net_inited = 1; return 1; }

    /* Wired first (cheap to probe). The first device that LEASES wins; a device
     * that links but never leases (dead RX / no server) is skipped. */
    for (i = 0; i < nw && budget > 0; i++) {
        nic = g_wired[i].n();
        if (!nic) continue;
        if (net_try_lease(nic, g_wired[i].m(), &budget)) {
            NETBOOT_LOG("net-boot: wired[%d] leased (rx=%u tx=%u)", i, net_rx_frames(), net_tx_frames());
            g_net_inited = 1; return 1;
        }
        NETBOOT_LOG("net-boot: wired[%d] no lease, skipping (rx=%u tx=%u)", i, net_rx_frames(), net_tx_frames());
    }
    /* Only if no wired device leased: WiFi (its probe is a multi-second bring-up,
     * so it is deliberately last and reached only when nothing wired worked). */
    for (i = 0; i < nf && budget > 0; i++) {
        nic = g_wifi[i].n();
        if (!nic) continue;
        if (net_try_lease(nic, g_wifi[i].m(), &budget)) {
            NETBOOT_LOG("net-boot: wifi[%d] leased", i);
            g_net_inited = 1; return 1;
        }
        NETBOOT_LOG("net-boot: wifi[%d] no lease, skipping", i);
    }
    NETBOOT_LOG("net-boot: no device leased (budget %d ms left)", budget);
    return 0;                                     /* on-demand pc64_net_up may retry later */
}

/* ---- tiny helpers -------------------------------------------------------- */
static void sput_n(char *dst, int cap, const char *src)
{ int i = 0; if (cap <= 0) return; while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; } dst[i] = 0; }

static void set_tls_err(char *status, int statusmax, const char *what, int e)
{
    int i = 0, n = 0; char num[8];
    if (statusmax <= 0) return;
    while (what[i] && i < statusmax-1) { status[i] = what[i]; i++; }
    { const char *sfx = " (BearSSL err "; int k = 0; while (sfx[k] && i < statusmax-1) status[i++] = sfx[k++]; }
    if (e < 0) { if (i < statusmax-1) status[i++]='-'; e = -e; }
    if (!e) { num[n++]='0'; }
    while (e && n < (int)sizeof num) { num[n++]=(char)('0'+e%10); e/=10; }
    while (n && i < statusmax-1) status[i++] = num[--n];
    if (i < statusmax-1) status[i++]=')';
    status[i] = 0;
}

static int is_ipv4(const char *s, unsigned char out[4])
{
    int part = 0, val = 0, digits = 0, i;
    for (i = 0; ; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') { val = val*10 + (c-'0'); digits++; if (val > 255) return 0; }
        else if (c == '.' || c == 0) {
            if (!digits || part > 3) return 0;
            out[part++] = (unsigned char)val; val = 0; digits = 0;
            if (c == 0) break;
        } else return 0;
    }
    return part == 4;
}

/* parse the numeric status code out of "HTTP/1.x NNN reason" */
static int http_status_code(const char *raw)
{
    const char *s = raw;
    while (*s && *s != ' ' && *s != '\r' && *s != '\n') s++;   /* skip HTTP/1.x */
    while (*s == ' ') s++;
    if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9')
        return (s[0]-'0')*100 + (s[1]-'0')*10 + (s[2]-'0');
    return 0;
}

/* find a header value by name (case-insensitive) within the header block of a
 * raw response. Stops at the blank line so it never matches inside the body.
 * Returns 1 and fills `out` (NUL-terminated, trimmed) if found, else 0. */
/* The nth (0-based) occurrence of a header. Set-Cookie is the reason this
 * exists: a response commonly carries several, and a "find the header"
 * helper that stops at the first would quietly drop all but one. */
static int http_header_nth(const char *raw, int rawlen, const char *name,
                           int nth, char *out, int outmax)
{
    int nl = (int)strlen(name), seen = 0;
    const char *e = raw + rawlen, *ln = raw;
    while (ln < e && *ln != '\n') ln++;               /* skip the status line */
    if (ln < e) ln++;
    for (; ln < e; ) {
        const char *le = ln; while (le < e && *le != '\n') le++;
        {   int ll = (int)(le - ln), k, m = 1;
            if (ll == 0 || (ll == 1 && ln[0] == '\r')) break;    /* end of headers */
            if (ll > nl && ln[nl] == ':') {
                for (k = 0; k < nl; k++) {
                    char a = ln[k], b = name[k];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { m = 0; break; }
                }
                if (m && seen++ == nth) {
                    const char *v = ln + nl + 1, *ve = le; int o = 0;
                    while (v < ve && (*v == ' ' || *v == '\t')) v++;
                    while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                    while (v < ve && o < outmax - 1) out[o++] = *v++;
                    out[o] = 0;
                    return 1;
                }
            } }
        ln = le + 1;
    }
    return 0;
}

static int http_header(const char *raw, int rawlen, const char *name, char *out, int outmax)
{
    int nl = (int)strlen(name);
    const char *e = raw + rawlen, *ln = raw;
    while (ln < e && *ln != '\n') ln++;               /* skip the status line */
    if (ln < e) ln++;
    for (; ln < e; ) {
        const char *le = ln; while (le < e && *le != '\n') le++;
        int ll = (int)(le - ln), k, m = 1;
        if (ll == 0 || (ll == 1 && ln[0] == '\r')) break;   /* end of headers */
        if (ll > nl && ln[nl] == ':') {
            for (k = 0; k < nl; k++) {
                char a = ln[k], b = name[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { m = 0; break; }
            }
            if (m) {
                const char *v = ln + nl + 1, *ve = le; int o = 0;
                while (v < ve && (*v == ' ' || *v == '\t')) v++;
                while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                while (v < ve && o < outmax-1) out[o++] = *v++;
                out[o] = 0;
                return 1;
            }
        }
        ln = le + 1;
    }
    return 0;
}

/* ---- response framing ------------------------------------------------------
 * Where does a response END? Until now the answer was "when the server hangs
 * up", which works only because every request says Connection: close, and
 * costs a wait for the close (or a 3 s idle timeout) even when the server
 * already stated the length. Framing the body properly is what makes the
 * response finish the moment its last byte lands - and it is the hard
 * prerequisite for keep-alive, where the close never comes at all.
 *
 * Two framings matter: Content-Length, and Transfer-Encoding: chunked. A
 * response with neither still falls back to read-until-close, which remains
 * correct for HTTP/1.0.
 */

/* Offset of the body (past the blank line), or -1 if the headers are not all
 * here yet. */
static int hdr_split(const char *raw, int rn)
{
    int i;
    for (i = 0; i + 1 < rn; i++) {
        if (i + 3 < rn && raw[i]=='\r' && raw[i+1]=='\n' && raw[i+2]=='\r' && raw[i+3]=='\n')
            return i + 4;
        if (raw[i]=='\n' && raw[i+1]=='\n') return i + 2;
    }
    return -1;
}

/* De-chunk in place. Returns the decoded length, or -1 if the terminating
 * zero-size chunk has not arrived yet (so the caller keeps reading). */
static int dechunk(char *body, int len)
{
    int in = 0, out = 0;
    for (;;) {
        long sz = 0;
        int digits = 0;
        while (in < len && (body[in]=='\r' || body[in]=='\n')) in++;   /* CRLF between chunks */
        while (in < len) {                                            /* hex size */
            char c = body[in];
            int v = (c>='0'&&c<='9') ? c-'0' :
                    (c>='a'&&c<='f') ? c-'a'+10 :
                    (c>='A'&&c<='F') ? c-'A'+10 : -1;
            if (v < 0) break;
            sz = sz * 16 + v;
            in++; digits++;
        }
        if (!digits) return -1;                       /* size line incomplete */
        while (in < len && body[in] != '\n') in++;    /* skip any chunk-ext   */
        if (in >= len) return -1;
        in++;                                         /* past the LF          */
        if (sz == 0) { body[out] = 0; return out; }   /* the terminator       */
        if (in + sz > len) return -1;                 /* chunk not all here   */
        memmove(body + out, body + in, (size_t)sz);
        out += (int)sz;
        in += (int)sz;
    }
}

/* ---- progressive delivery -------------------------------------------------
 * A page used to appear only when its last byte landed. The transport now
 * offers the body AS IT ARRIVES, throttled, so the browser can draw what it
 * has. The transport does not know what a document is - it hands over bytes
 * and lets the embedder decide what they are worth. */
static pc64_http_progress_fn g_progress;

void pc64_http_on_progress(pc64_http_progress_fn fn) { g_progress = fn; }

/* ---- a millisecond clock a PRODUCTION build has ---------------------------
 * uno_dbg_uptime_ms() is debug-only, and a transport whose timeouts exist only
 * in the debug build has no timeouts. The TSC is what the shell's animation
 * clock and the browser's page clock already use. When it is uncalibrated the
 * fallback counts CALLS rather than milliseconds: coarser, but a deadline that
 * still ends beats one that never fires. */
static unsigned http_ms(void)
{
    static unsigned long long t0;
    static unsigned tick;
    unsigned long long per_ms = uno_native_tsc_per_us() * 1000ull, now;
    if (!per_ms) return ++tick;
    now = uno_native_rdtsc();
    if (!t0) t0 = now;
    return (unsigned)((now - t0) / per_ms);
}

/* ---- connections ----------------------------------------------------------
 * A page is a document plus its images and stylesheets, and each of those used
 * to cost a fresh DNS + TCP + TLS round to the SAME server. Holding connections
 * open across them is the single biggest win available in the fetch path, and
 * the framing work above is what makes it possible: on a persistent connection
 * the server never closes, so the only way to know a response ended is its own
 * Content-Length or chunked terminator.
 *
 * A POOL, not one slot. One slot was right while the browser could only fetch
 * one thing at a time; now that several requests are in flight, one slot would
 * mean the second request closes the first's connection and neither is reused.
 *
 * The failure that matters: a server may drop an idle connection at any moment,
 * and it looks identical to a healthy one until the write or the first read
 * fails. So a REUSED connection that fails is retried once on a fresh one, and
 * only a fresh connection's failure is reported. Without that retry, keep-alive
 * turns a working browser into one that intermittently fails for no visible
 * reason. */
#define KA_N   4
#define DNS_N  8
/* The receive buffer GROWS. It used to be a flat 49152, which is where
 * https://google.com went blank: www.google.com answers 1,384 bytes of headers
 * and an 82,760-byte body whose <body> does not open until body offset 62,883,
 * so a 48 KB response was a document consisting entirely of <head> - scripts
 * and one stylesheet - and a parser handed that renders nothing. The status
 * line still said 200 OK, because the fetch really had succeeded.
 *
 * A flat buffer big enough for that page would be paid by every 300-byte icon
 * on it, and pc64_fetch runs FETCH_PAR of those at once, so the size is the
 * response's, not the constant's: start small, double on demand, stop at
 * RAW_MAX. 1 MB is not a new opinion about how big a page may be - it is what
 * pc64_fetch already allows per subresource (FETCH_ONE_MAX), and this is the
 * transport that feeds it. */
#define RAW_START (16u * 1024u)
#define RAW_MAX   (1u << 20)
/* The browser's share of the NSOCK (16) socket table. The rest of the machine
 * needs sockets too - the URC link, the child its listener accepts, discovery -
 * and on a box driven only over URC, a browser that crowded those out would
 * take the machine with it. In-flight requests are already capped (one
 * document plus pc64_fetch's FETCH_PAR), so this is really a cap on how many
 * IDLE pooled connections may sit around alongside them; over budget, the
 * oldest idle one is given up. */
#define HTTP_MAX_CONNS 8

typedef struct {
    /* An EXPLICIT occupied flag, not "is sock >= 0": socket id 0 is a
     * perfectly good socket, so a zeroed pool slot reads as occupied and
     * every connection gets closed instead of kept - which is keep-alive
     * silently doing nothing, and a conn_close() on somebody else's socket 0.
     * tools/netverify_urc.py caught exactly that, second page load. */
    int       used;
    int       sock;                 /* -1 unless plain    */
    tls_conn *tls;                  /* 0 unless secure    */
    int       secure, port;
    char      host[128];
} httpconn;

static httpconn g_pool[KA_N];
static int      g_conn_open;        /* pooled + in flight */

static void conn_clear(httpconn *c)
{ memset(c, 0, sizeof *c); c->sock = -1; }

static int conn_live(const httpconn *c) { return c->used; }

static void conn_close(httpconn *c)
{
    if (!c->used) { conn_clear(c); return; }
    if (c->tls) tls_free(c->tls); else if (c->sock >= 0) net_sock_close(c->sock);
    g_conn_open--;
    conn_clear(c);
}

/* Is a POOLED connection still worth handing out? A dead one costs the caller
 * a whole request before it finds out, so ask before, not after. */
static int conn_healthy(httpconn *c)
{
    if (!conn_live(c)) return 0;
    if (c->tls) return tls_poll(c->tls) == TLS_READY;
    return net_sock_state(c->sock) == TCP_ESTABLISHED;
}

static int pool_take(const char *host, int port, int secure, httpconn *out)
{
    int i;
    for (i = 0; i < KA_N; i++) {
        httpconn *p = &g_pool[i];
        if (!conn_live(p) || p->secure != secure || p->port != port) continue;
        if (strcmp(p->host, host)) continue;
        if (!conn_healthy(p)) { conn_close(p); continue; }
        *out = *p;
        conn_clear(p);
        return 1;
    }
    return 0;
}

/* Give up idle connections until a new one fits inside the budget. Only
 * POOLED ones can be given up - an in-flight request owns its socket - so
 * this cannot starve a request that is already running. */
static void budget_make_room(void)
{
    while (g_conn_open >= HTTP_MAX_CONNS) {
        int i;
        for (i = 0; i < KA_N; i++)
            if (conn_live(&g_pool[i])) { conn_close(&g_pool[i]); break; }
        if (i == KA_N) return;              /* nothing idle left to give up */
    }
}

static void pool_put(httpconn *c)
{
    int i;
    if (!conn_live(c)) return;
    for (i = 0; i < KA_N; i++)
        if (!conn_live(&g_pool[i])) { g_pool[i] = *c; conn_clear(c); return; }
    conn_close(c);                      /* pool full: the newest one loses */
}

/* ---- the resolver's memory ------------------------------------------------
 * net_dns_query() is synchronous, so a page whose four subresources live on
 * one host used to pay four blocking round trips to learn the same address.
 * Page-lifetime, dropped by pc64_http_disconnect() along with the connections,
 * which is what a network reconfiguration invalidates. Deliberately not a TTL
 * cache: this is a browser session's worth of memory, not a resolver. */
static struct { char host[128]; unsigned char ip[4]; } g_dns[DNS_N];
static int g_dnsn;

static int resolve_host(const char *host, unsigned char ip[4])
{
    int i;
    if (is_ipv4(host, ip)) return 1;
    for (i = 0; i < g_dnsn; i++)
        if (!strcmp(g_dns[i].host, host)) { memcpy(ip, g_dns[i].ip, 4); return 1; }
    if (!net_dns_query(host, ip)) return 0;
    if (g_dnsn < DNS_N) {
        sput_n(g_dns[g_dnsn].host, sizeof g_dns[g_dnsn].host, host);
        memcpy(g_dns[g_dnsn].ip, ip, 4);
        g_dnsn++;
    }
    return 1;
}

void pc64_http_disconnect(void)
{
    int i;
    for (i = 0; i < KA_N; i++) conn_close(&g_pool[i]);
    g_dnsn = 0;
}

int pc64_http_conns(void) { return g_conn_open; }

/* ---- a request in flight --------------------------------------------------
 * One state machine, one implementation of what an HTTP request is. The
 * blocking pc64_http_get/request are this plus a wait loop; the browser's
 * subresource fetcher is several of these plus one wait loop.
 */
enum { HS_CONNECT = 0, HS_SEND, HS_RECV, HS_FINISH, HS_END };

/* deadlines, in ms of no progress. A plain TCP open is either answered or it
 * is not; a TLS open also has to carry a ~4-5 KB certificate flight, and this
 * transport moves one 512-byte segment per round trip. */
#define CONNECT_MS_PLAIN 3000
#define CONNECT_MS_TLS   8000
#define SEND_MS          8000
#define IDLE_MS          3000
#define MAX_HOPS         6

struct http_req {
    int   st, finished, rc;
    int   hop, want_progress;
    char  url0[512];                /* the ORIGINAL url - what the cache keys */
    char  cur[512];                 /* this hop's url                         */
    char *post;                     /* copied; NULL for a GET                 */
    /* per hop */
    char  host[128], path[512];
    unsigned char ip[4];
    int   secure, port, reused, retried;
    httpconn c;
    char  req[2048]; int reqn, reqoff;
    char *raw; int rn, rcap, capped;
    int   split, chunked, done, next_report;
    long  clen;
    unsigned t0;
    int   body_off, body_len;
    char  status[192];
    pc64_cache_ctl ctl;
};

/* Room in raw[] for more bytes, growing it if there is none. Always leaves one
 * byte spare, because everything downstream NUL-terminates at raw[rn].
 *
 * Returns 0 when the response has outgrown RAW_MAX (or the heap said no), and
 * sets `capped`. That flag is the whole point: the old loop stopped at the cap
 * down the SAME path as a complete response, so a truncated page reported
 * "200 OK" and rendered whatever half a document renders as. A reader has no
 * way to tell that from a site that is genuinely blank. */
static int raw_room(http_req *r)
{
    int room = r->rcap - 1 - r->rn;
    char *bigger;
    int want;
    if (room > 0) return room;
    if (r->rcap >= (int)RAW_MAX) { r->capped = 1; return 0; }
    want = r->rcap * 2;
    if (want > (int)RAW_MAX) want = (int)RAW_MAX;
    bigger = (char *)realloc(r->raw, (size_t)want);
    if (!bigger) { r->capped = 1; return 0; }   /* keep what already arrived */
    r->raw = bigger;
    r->rcap = want;
    return r->rcap - 1 - r->rn;
}

/* Grow to hold `want` bytes up front, for a caller that already knows the size
 * (the cache hit below). 1 on success. */
static int raw_reserve(http_req *r, int want)
{
    char *bigger;
    if (want > (int)RAW_MAX) want = (int)RAW_MAX;
    if (r->rcap >= want) return 1;
    bigger = (char *)realloc(r->raw, (size_t)want);
    if (!bigger) return 0;
    r->raw = bigger;
    r->rcap = want;
    return 1;
}

static int req_fail(http_req *r, int rc, const char *why)
{
    if (why) sput_n(r->status, sizeof r->status, why);
    conn_close(&r->c);
    r->rc = rc;
    r->st = HS_END;
    r->finished = 1;
    return 1;
}

/* Parse this hop's URL, get a connection (pooled if one matches, fresh
 * otherwise) and build the request bytes. `no_reuse` forces a fresh
 * connection - that is the keep-alive retry. */
static int hop_start(http_req *r, int no_reuse)
{
    const char *p = r->cur;
    int hn = 0, i;

    r->secure = 0; r->port = 80; r->reused = 0;
    r->reqn = r->reqoff = 0;
    r->rn = 0; r->split = -1; r->chunked = 0; r->done = 0; r->next_report = 0;
    r->capped = 0;                  /* per hop: a redirect starts clean */
    r->clen = -1;

    if (!strncmp(p, "https://", 8)) { r->secure = 1; r->port = 443; p += 8; }
    else if (!strncmp(p, "http://", 7)) p += 7;

    while (*p && *p != '/' && *p != ':' && hn < (int)sizeof(r->host)-1) r->host[hn++] = *p++;
    r->host[hn] = 0;
    if (*p == ':') { p++; r->port = 0; while (*p >= '0' && *p <= '9') r->port = r->port*10 + (*p++ - '0'); }
    if (*p != '/') { r->path[0] = '/'; i = 1; } else { i = 0; }
    {   int j = i; while (*p && j < (int)sizeof(r->path)-1) r->path[j++] = *p++; r->path[j] = 0; }
    if (r->path[0] == 0) { r->path[0] = '/'; r->path[1] = 0; }
    if (r->host[0] == 0) return req_fail(r, -2, "Empty host");

    /* now that pc64_net_up() reports the lease rather than just the bind, this
     * branch also catches "NIC up, link up, but no address", so say so - the
     * old wording blamed a missing NIC for what was usually a missing lease */
    if (!pc64_net_up())
        return req_fail(r, -3, "No network (no NIC, no link, or no DHCP lease - check the cable, or WIFI.CFG + firmware)");

    /* The one blocking step left, and the only one: net_dns_query is
     * synchronous by contract (net.h). Memoised per host, so a page's whole
     * subresource set pays it once. */
    if (!resolve_host(r->host, r->ip)) return req_fail(r, -4, "DNS lookup failed");

    if (!no_reuse && pool_take(r->host, r->port, r->secure, &r->c)) {
        r->reused = 1;
        r->st = HS_SEND;
    } else if (r->secure) {
        budget_make_room();
        r->c.tls = tls_open(r->ip, (unsigned short)r->port, r->host, TLS_TRUST_CA);
        if (!r->c.tls) {
            /* A refusal for want of entropy never reached BearSSL, so there is
             * no BR_ERR_* to quote and "TLS connect failed (BearSSL err 0)"
             * would point the reader at the wrong layer. Name the real cause. */
            if (tls_open_error() == TLS_ENOENTROPY)
                return req_fail(r, -5, "TLS refused: no entropy source on this machine");
            return req_fail(r, -5, "TLS connect failed (no socket or no memory)");
        }
        r->c.used = 1; r->c.sock = -1; r->c.secure = 1; r->c.port = r->port;
        sput_n(r->c.host, sizeof r->c.host, r->host);
        g_conn_open++;
        r->st = HS_CONNECT;
    } else {
        int s;
        budget_make_room();
        s = net_socket(SOCK_TCP);
        if (s < 0) return req_fail(r, -5, "TCP connect failed (no socket free)");
        if (net_connect(s, r->ip, (unsigned short)r->port) != 0) {
            net_sock_close(s);
            return req_fail(r, -5, "TCP connect failed");
        }
        r->c.used = 1; r->c.sock = s; r->c.tls = 0; r->c.secure = 0; r->c.port = r->port;
        sput_n(r->c.host, sizeof r->c.host, r->host);
        g_conn_open++;
        r->st = HS_CONNECT;
    }

    /* the request bytes */
    {   char ck[768];
        int rn = 0;
        const char *a = r->post ? "POST " : "GET ";
        const char *b = " HTTP/1.0\r\nHost: ";
        /* Connection: keep-alive, NOT close. The framing above is what lets a
         * response end without a close, and asking for one anyway is how the
         * pool ends up empty against every server that honours the header.
         * The test server did not, so nothing in the tree noticed. A server
         * that will not persist answers "Connection: close" and the finish
         * step below drops the connection, which is the correct fallback. */
        const char *c = "\r\nUser-Agent: UnoDOS-pc64\r\nConnection: keep-alive\r\n"
                        "Accept: text/html,text/markdown,text/plain\r\n";
        /* every append is bounds-checked against sizeof(req): host/path come
           from the address bar AND from links in untrusted pages, so a crafted
           long URL must not overflow this buffer. */
        #define REQ_PUT(s) do { int l=(int)strlen(s); \
            if (rn+l >= (int)sizeof(r->req)) { return req_fail(r, -8, "URL too long"); } \
            memcpy(r->req+rn,(s),l); rn+=l; } while (0)
        REQ_PUT(a); REQ_PUT(r->path); REQ_PUT(b); REQ_PUT(r->host); REQ_PUT(c);
        /* stored cookies for this origin. The jar decides what matches; the
         * transport only has to hand it enough to decide with - host, path AND
         * the scheme, since a Secure cookie must never leave over plain HTTP. */
        if (pc64_cookie_header(r->host, r->path, r->secure, ck, sizeof ck) > 0) {
            REQ_PUT("Cookie: "); REQ_PUT(ck); REQ_PUT("\r\n");
        }
        if (r->post) {
            char num[24];
            int k = 0, v = (int)strlen(r->post);
            REQ_PUT("Content-Type: application/x-www-form-urlencoded\r\n");
            REQ_PUT("Content-Length: ");
            if (!v) num[k++] = '0';
            {   char tmp[16]; int t = 0, vv = v;
                while (vv) { tmp[t++] = (char)('0' + vv % 10); vv /= 10; }
                while (t) num[k++] = tmp[--t]; }
            num[k] = 0;
            REQ_PUT(num);
            REQ_PUT("\r\n");
        }
        REQ_PUT("\r\n");                            /* end of headers */
        if (r->post) REQ_PUT(r->post);
        #undef REQ_PUT
        r->reqn = rn;
    }
    r->t0 = http_ms();
    return 0;
}

/* "the connection I reused was dead - try again on a fresh one". Never
 * reported to a caller; this consumes it. */
static int hop_retry(http_req *r)
{
    conn_close(&r->c);
    r->retried = 1;
    return hop_start(r, 1);
}

static void step_connect(http_req *r)
{
    if (r->secure) {
        int p = tls_poll(r->c.tls);
        if (p < 0) { char m[160]; set_tls_err(m, sizeof m, "TLS connect failed", tls_conn_error(r->c.tls));
                     req_fail(r, -5, m); return; }
        if (p == TLS_READY) { r->st = HS_SEND; r->t0 = http_ms(); return; }
        if (p == TLS_EOF) { req_fail(r, -5, "TLS connect failed (peer closed)"); return; }
        if (http_ms() - r->t0 > CONNECT_MS_TLS) req_fail(r, -6, "TLS handshake timed out");
    } else {
        int s = net_sock_state(r->c.sock);
        if (s == TCP_ESTABLISHED) { r->st = HS_SEND; r->t0 = http_ms(); return; }
        if (s == TCP_DONE || s == TCP_CLOSED) { req_fail(r, -5, "TCP connect failed"); return; }
        if (http_ms() - r->t0 > CONNECT_MS_PLAIN) req_fail(r, -6, "Connection timed out");
    }
}

static void step_send(http_req *r)
{
    while (r->reqoff < r->reqn) {
        int want = r->reqn - r->reqoff, n;
        if (r->secure) {
            n = tls_send(r->c.tls, r->req + r->reqoff, want);
            if (n < 0) {
                /* on a reused connection this is almost always "the server
                 * dropped it while idle", not a real error */
                if (r->reused && !r->retried) { hop_retry(r); return; }
                { char m[160]; set_tls_err(m, sizeof m, "TLS write failed", tls_conn_error(r->c.tls));
                  req_fail(r, -7, m); }
                return;
            }
        } else {
            n = net_send(r->c.sock, r->req + r->reqoff, want);
            if (n < 0) {
                if (net_sock_state(r->c.sock) != TCP_ESTABLISHED) {
                    if (r->reused && !r->retried) { hop_retry(r); return; }
                    req_fail(r, -7, "Connection lost while sending");
                    return;
                }
                n = 0;                              /* a segment is in flight */
            }
        }
        if (n == 0) break;                          /* would block: come back */
        r->reqoff += n;
        r->t0 = http_ms();
    }
    if (r->reqoff >= r->reqn) { r->st = HS_RECV; r->t0 = http_ms(); }
    else if (http_ms() - r->t0 > SEND_MS) req_fail(r, -7, "Request send timed out");
}

/* Update the framing state from whatever is in raw[] now. Split out because
 * both the arrival path and the finish path need it to agree. */
static void frame_update(http_req *r)
{
    if (r->split < 0) {
        r->split = hdr_split(r->raw, r->rn);
        if (r->split >= 0) {
            char v[64];
            r->raw[r->rn] = 0;
            if (http_header(r->raw, r->split, "content-length", v, sizeof v)) {
                long q = 0; const char *p2 = v;
                while (*p2 >= '0' && *p2 <= '9') q = q * 10 + (*p2++ - '0');
                r->clen = q;
            }
            if (http_header(r->raw, r->split, "transfer-encoding", v, sizeof v)) {
                int k; for (k = 0; v[k]; k++) if (v[k]>='A'&&v[k]<='Z') v[k] += 32;
                if (strstr(v, "chunked")) r->chunked = 1;
            }
        }
    }
    if (r->split >= 0 && !r->done) {
        if (r->chunked) {
            /* peek: dechunk on a COPY would cost a second buffer, so probe for
             * the terminator instead and decode once, at the finish */
            int i;
            for (i = r->split; i + 4 < r->rn; i++)
                if (r->raw[i]=='\n' && r->raw[i+1]=='0' &&
                    (r->raw[i+2]=='\r' || r->raw[i+2]=='\n')) { r->done = 1; break; }
        } else if (r->clen >= 0 && (long)(r->rn - r->split) >= r->clen) r->done = 1;
    }
}

static void step_recv(http_req *r)
{
    int eof = 0;

    for (;;) {
        char tmp[1460];
        int n, room = raw_room(r);
        if (room <= 0) break;
        if (room > (int)sizeof tmp) room = (int)sizeof tmp;
        n = r->secure ? tls_recv(r->c.tls, tmp, room)
                      : net_recv(r->c.sock, tmp, room);
        if (n < 0) { eof = 1; break; }              /* TLS closed or failed */
        if (n == 0) {
            if (!r->secure) {
                int s = net_sock_state(r->c.sock);
                if (s == TCP_DONE || s == TCP_CLOSED || s == TCP_FIN_WAIT) eof = 1;
            }
            break;
        }
        memcpy(r->raw + r->rn, tmp, (size_t)n);
        r->rn += n;
        r->t0 = http_ms();
    }

    frame_update(r);

    /* Offer what has arrived, at a GEOMETRIC interval: ~6 KB at first, then a
     * quarter of the body so far. Chunked bodies are not offered - they are
     * still encoded at this point, and handing the embedder chunk-size lines to
     * render would be worse than making it wait.
     *
     * The interval used to be a flat 6 KB, which was fine only because the
     * buffer stopped at 48 KB: eight reports, eight repaints. An embedder
     * re-parses and re-renders the whole partial document on each one, so a
     * flat interval is quadratic in the page size, and against the 1 MB the
     * transport now accepts that is ~170 full re-renders of a document that
     * keeps getting longer - the big pages this change exists to allow would
     * arrive, and crawl. Growing the interval keeps the early feedback (the
     * first screenful is what the reader is waiting for) and makes the total
     * about two dozen reports whatever the size. */
    if (r->want_progress && g_progress && r->split >= 0 && !r->chunked &&
        r->rn - r->split > r->next_report) {
        int have = r->rn - r->split, step = have / 4;
        r->raw[r->rn] = 0;
        g_progress(r->raw + r->split, have, r->clen);
        if (step < 6144) step = 6144;
        r->next_report = have + step;
    }

    if (r->done || eof || r->capped) { r->st = HS_FINISH; return; }
    /* An idle timeout is not a failure: the old loop broke out and used
     * whatever had arrived, which is what read-until-close means when the
     * close never comes. */
    if (http_ms() - r->t0 > IDLE_MS) r->st = HS_FINISH;
}

/* Resolve a Location into an absolute URL, into r->cur. Handles absolute
 * Locations, root-relative ("/path"), and http<->https upgrades (google.com ->
 * www.google.com, apex -> www, http -> https all land here). */
static void redirect_to(http_req *r, const char *loc)
{
    char next[512];
    int o = 0, redirmax = (int)sizeof next;
    #define RPUT(s) do { const char *q=(s); while (*q && o<redirmax-1) next[o++]=*q++; } while (0)
    if (!strncmp(loc,"http://",7) || !strncmp(loc,"https://",8)) {
        RPUT(loc);
    } else {
        char pnum[8]; int v = r->port, k = 0, defport = r->secure ? 443 : 80;
        RPUT(r->secure ? "https://" : "http://");
        RPUT(r->host);
        if (v != defport) { RPUT(":"); if(!v)pnum[k++]='0'; while(v){pnum[k++]=(char)('0'+v%10);v/=10;}
                            while (k && o<redirmax-1) next[o++]=pnum[--k]; }
        if (loc[0] != '/') RPUT("/");
        RPUT(loc);
    }
    #undef RPUT
    next[o] = 0;
    sput_n(r->cur, sizeof r->cur, next);
}

static void step_finish(http_req *r)
{
    /* Keep the connection ONLY when the reply was framed (so we know where it
     * ended) and neither side asked to close. Anything else and we are guessing
     * about a shared socket, which is how a browser starts reading one page's
     * bytes as the next page's body. */
    {   char cv[64];
        int keep = (r->split >= 0) && (r->chunked || r->clen >= 0) && r->done;
        if (keep && http_header(r->raw, r->split, "connection", cv, sizeof cv)) {
            int k; for (k = 0; cv[k]; k++) if (cv[k]>='A'&&cv[k]<='Z') cv[k] += 32;
            if (strstr(cv, "close")) keep = 0;
        }
        if (keep) pool_put(&r->c); else conn_close(&r->c);
    }
    /* a reused connection that produced NOTHING was dead: retry once */
    if (r->rn == 0 && r->reused && !r->retried) { hop_retry(r); return; }
    r->raw[r->rn] = 0;

    /* chunked bodies are decoded in place, so everything downstream - the
     * header/body split below, the cache, the parser - sees a plain body */
    if (r->split >= 0 && r->chunked) {
        int dl = dechunk(r->raw + r->split, r->rn - r->split);
        if (dl >= 0) { r->rn = r->split + dl; r->raw[r->rn] = 0; }
    }

    /* status line */
    {   int j = 0; const char *s = r->raw;
        while (*s && *s != '\r' && *s != '\n' && j < (int)sizeof r->status - 1) r->status[j++] = *s++;
        r->status[j] = 0;
        if (!j) sput_n(r->status, sizeof r->status, "No response");
        /* SAY SO when the answer did not fit. Truncation used to be reported as
         * "200 OK", which is true of the exchange and useless to the reader:
         * the page is short or blank and nothing on screen distinguishes that
         * from a site that really is. The status band is the browser's one line
         * of feedback, so the fact goes there. */
        if (r->capped) {
            char kb[16]; int k = 0, v = r->rn / 1024;
            char tmp[8]; int t = 0;
            if (!v) kb[k++] = '0';
            while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
            while (t) kb[k++] = tmp[--t];
            kb[k] = 0;
            if (j + 20 + k < (int)sizeof r->status) {
                const char *a = " - TRUNCATED at ", *b = " KB";
                memcpy(r->status + j, a, strlen(a)); j += (int)strlen(a);
                memcpy(r->status + j, kb, (size_t)k); j += k;
                memcpy(r->status + j, b, strlen(b)); j += (int)strlen(b);
                r->status[j] = 0;
            }
        }
    }

    /* Set-Cookie, every occurrence. This runs BEFORE the redirect branch below
     * on purpose: a sign-in almost always answers 302 + Set-Cookie, and taking
     * the redirect first would drop the very cookie the redirect delivers. */
    {   char cv[768]; int occ;
        for (occ = 0; occ < 8; occ++) {
            if (!http_header_nth(r->raw, r->rn, "set-cookie", occ, cv, sizeof cv)) break;
            pc64_cookie_set(r->host, r->path, cv);
        } }

    /* Cache-Control, for the cache decision. Parsed here because this is the
     * only place with the headers in hand - the body is all that survives. */
    {   char cc[160];
        r->ctl.no_store = 0;
        r->ctl.max_age = -1;
        if (http_status_code(r->raw) != 200) r->ctl.no_store = 1;   /* only 200 */
        if (http_header(r->raw, r->rn, "cache-control", cc, sizeof cc)) {
            int k;
            for (k = 0; cc[k]; k++) if (cc[k] >= 'A' && cc[k] <= 'Z') cc[k] += 32;
            if (strstr(cc, "no-store") || strstr(cc, "no-cache") ||
                strstr(cc, "private")) r->ctl.no_store = 1;
            {   const char *m = strstr(cc, "max-age");
                if (m) { long v = 0; int any = 0;
                         m += 7; while (*m == ' ' || *m == '=') m++;
                         while (*m >= '0' && *m <= '9') { v = v*10 + (*m++ - '0'); any = 1; }
                         if (any) r->ctl.max_age = v; } }
        }
    }

    /* follow a redirect */
    {   int code = http_status_code(r->raw); char loc[512];
        if (code >= 300 && code < 400 &&
            http_header(r->raw, r->rn, "location", loc, sizeof loc) && loc[0]) {
            if (++r->hop >= MAX_HOPS) { req_fail(r, -9, "Too many redirects"); return; }
            redirect_to(r, loc);
            /* a redirect after a POST is followed as a GET, which is what
             * browsers do */
            if (r->post) { free(r->post); r->post = 0; }
            r->retried = 0;
            hop_start(r, 0);
            return;
        } }

    /* split headers from body at the blank line */
    {   const char *bp = r->raw, *e = r->raw + r->rn, *sp = 0;
        for (; bp + 3 < e; bp++) {
            if (bp[0]=='\r'&&bp[1]=='\n'&&bp[2]=='\r'&&bp[3]=='\n') { sp = bp+4; break; }
            if (bp[0]=='\n'&&bp[1]=='\n') { sp = bp+2; break; }
        }
        if (!sp) sp = r->raw;                       /* no headers found - show all */
        r->body_off = (int)(sp - r->raw);
        r->body_len = r->rn - r->body_off;
        if (r->body_len < 0) r->body_len = 0;
    }
    /* cache under the ORIGINAL url, not the last hop: that is what the next
     * visit will ask for. Never a POST - replaying one from a cache is how a
     * browser double-submits an order. Never a TRUNCATED body either: it is not
     * the resource, and storing it would serve the half-page from memory for the
     * whole TTL, so a reload could not even recover by chance. */
    if (r->body_len > 0 && !r->post && !r->capped)
        pc64_cache_put(r->url0, r->raw + r->body_off, r->body_len, r->status, &r->ctl);
    r->rc = r->body_len;
    r->st = HS_END;
    r->finished = 1;
}

http_req *pc64_http_begin(const char *url, const char *post)
{
    http_req *r = (http_req *)malloc(sizeof *r);
    if (!r) return 0;
    memset(r, 0, sizeof *r);
    conn_clear(&r->c);
    r->raw = (char *)malloc(RAW_START);
    if (!r->raw) { free(r); return 0; }
    r->rcap = (int)RAW_START;
    r->split = -1;
    r->clen = -1;
    r->ctl.max_age = -1;
    if (post) {
        size_t n = strlen(post) + 1;
        r->post = (char *)malloc(n);
        if (!r->post) { free(r->raw); free(r); return 0; }
        memcpy(r->post, post, n);
    }
    sput_n(r->url0, sizeof r->url0, url ? url : "");
    sput_n(r->cur,  sizeof r->cur,  url ? url : "");

    /* The cache lives HERE, not in the blocking wrapper, so every request path
     * gets it. It used to sit in pc64_http_request, which was fine while every
     * caller blocked - but the subresource queue now calls begin() directly,
     * and a page's second visit would have re-fetched every image it already
     * had. A fresh copy short-circuits the whole DNS + TCP + TLS round, which
     * on this box is seconds rather than milliseconds. NEVER for a POST: that
     * asks the server to CHANGE something, and replaying one from a cache is
     * how a browser double-submits an order. */
    if (!r->post) {
        /* Ask how big the entry is and grow to fit FIRST. raw[] no longer
         * starts at the maximum, so handing the cache its own capacity would
         * hand back a cache hit truncated to 16 KB - which is the bug this
         * change exists to remove, reintroduced on the fast path. */
        int have = pc64_cache_len(r->url0);
        int n = -1;
        /* And if it will not fit, do not take a SHORT copy - go to the network
         * instead. A truncated cache hit is the worst of both: wrong, and fast
         * enough that nothing ever looks at it twice. */
        if (have >= 0 && raw_reserve(r, have + 1))
            n = pc64_cache_get(r->url0, r->raw, r->rcap, r->status, sizeof r->status);
        if (n >= 0) {
            r->body_off = 0; r->body_len = n; r->rc = n;
            r->st = HS_END; r->finished = 1;
            return r;
        }
    }
    hop_start(r, 0);                 /* sets HS_* or finishes with an error */
    return r;
}

void pc64_http_req_progress(http_req *r, int on) { if (r) r->want_progress = on; }

int pc64_http_poll(http_req *r)
{
    if (!r) return 1;
    if (r->finished) return 1;
    switch (r->st) {
    case HS_CONNECT: step_connect(r); break;
    case HS_SEND:    step_send(r);    break;
    case HS_RECV:    step_recv(r);    break;
    case HS_FINISH:  step_finish(r);  break;
    default:         r->finished = 1; break;
    }
    return r->finished;
}

int pc64_http_take(http_req *r, char *body, int bodymax, char *status, int statusmax)
{
    if (bodymax > 0) body[0] = 0;
    if (statusmax > 0) status[0] = 0;
    if (!r) return -2;
    if (statusmax > 0) sput_n(status, statusmax, r->status);
    if (r->rc < 0) return r->rc;
    {   int n = r->body_len;
        if (n > bodymax - 1) n = bodymax - 1;
        if (n < 0) n = 0;
        if (bodymax > 0) { memcpy(body, r->raw + r->body_off, (size_t)n); body[n] = 0; }
        return n;
    }
}

int pc64_http_len(http_req *r)
{ return r ? (r->rc < 0 ? r->rc : r->body_len) : -2; }

int pc64_http_wait(http_req *r)
{
    if (!r) return 0;
    while (!pc64_http_poll(r)) { net_poll(); uno_pc64_delay_ms(2); }
    return 1;
}

void pc64_http_free(http_req *r)
{
    if (!r) return;
    conn_close(&r->c);               /* a cancelled request never pools its own */
    free(r->raw);
    free(r->post);
    free(r);
}

/* ---- the blocking entry points -------------------------------------------- */
int pc64_http_request(const char *url, const char *post,
                      char *body, int bodymax, char *status, int statusmax)
{
    http_req *r;
    int rc;

    if (bodymax > 0) body[0] = 0;
    if (statusmax > 0) status[0] = 0;
    r = pc64_http_begin(url, post);      /* the cache check is inside */
    if (!r) { if (statusmax > 0) sput_n(status, statusmax, "Out of memory"); return -2; }
    pc64_http_req_progress(r, 1);
    pc64_http_wait(r);
    rc = pc64_http_take(r, body, bodymax, status, statusmax);
    pc64_http_free(r);
    return rc;
}

int pc64_http_get(const char *url, char *body, int bodymax, char *status, int statusmax)
{ return pc64_http_request(url, 0, body, bodymax, status, statusmax); }
