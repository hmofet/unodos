/* ===========================================================================
 * unolog.c - the UnoDOS system log.  Contract: UNOLOG.md.
 * ======================================================================== */
#include "unolog.h"
#include "pc64_native.h"     /* rdtsc + tsc_per_us (the only production clock)
                              * and uno_native_rtc_read for wall time         */
#include "fat.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- sizes ----------------------------------------------------------------
 * Both are static rather than malloc'd because unolog() is documented to work
 * before anything else does - including the heap - and a log that cannot
 * record the early boot is missing the part people actually want.
 *
 * The FILE image is held in memory and written WHOLE. The FAT layer has no
 * append (fat.h: read, write, read_at, no write_at), so the alternative is a
 * read-modify-write of the file on every flush. Holding the image also makes
 * rotation a memcpy instead of a second file read. */
#define UNOLOG_RING      192              /* records kept in memory          */
#define UNOLOG_FILE_MAX  (64 * 1024)      /* \LOGS\SYSTEM.LOG before rotation */
#define UNOLOG_FLUSH_MS  10000            /* periodic write-out              */
#define SYSLOG_PORT      514

static unolog_rec  g_ring[UNOLOG_RING];
static unsigned long g_next;              /* seq of the NEXT record written   */
static unsigned long g_dropped, g_sent, g_recvd;

static int g_level        = LOG_NOTICE;
static int g_remote_level = LOG_WARNING;
static char g_remote_host[64];
static int  g_remote_port = SYSLOG_PORT;
static unsigned char g_remote_ip[4];
static int  g_remote_resolved;
static int  g_listen;

static char g_file[UNOLOG_FILE_MAX];
static int  g_filen;                      /* bytes used in g_file             */
static int  g_dirty;                      /* records added since the last write */
static int  g_vol = -1;                   /* volume holding \LOGS             */
static unsigned long g_last_flush;
static int  g_inited;

/* Re-entrancy: the flush path and the syslog path both log on failure, and a
 * log call from inside a flush would recurse into the flush. One flag, checked
 * in unolog(), turns that into a dropped line instead of a stack overflow. */
static int g_in_log;

static const char *kSev[UNOLOG_NSEV] = {
    "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"
};
static const char *kFac[UNOLOG_NFAC] = {
    "kernel", "net", "storage", "browser", "ui", "security", "app", "remote"
};
/* local facility -> syslog facility number. 16..23 are "local0..local7",
 * which is exactly what a private set of facilities is meant to use; kernel
 * maps to 0 because a collector showing it as kern.* is telling the truth. */
static const int kSyslogFac[UNOLOG_NFAC] = { 0, 16, 17, 18, 19, 4, 20, 21 };

const char *unolog_sev_name(int s)
{ return (s >= 0 && s < UNOLOG_NSEV) ? kSev[s] : "?"; }
const char *unolog_fac_name(int f)
{ return (f >= 0 && f < UNOLOG_NFAC) ? kFac[f] : "?"; }

/* ---- clocks ---------------------------------------------------------------
 * Uptime from the TSC, the same way pc64_http does: uno_dbg_uptime_ms() is the
 * debug harness's and compiles away in production, which is precisely the
 * build this subsystem exists for. */
static unsigned long ulog_ms(void)
{
    static unsigned long long t0;
    static unsigned long tick;
    unsigned long long per_ms = uno_native_tsc_per_us() * 1000ull, now;
    if (!per_ms) return ++tick;          /* uncalibrated: monotonic counter */
    now = uno_native_rdtsc();
    if (!t0) t0 = now;
    return (unsigned long)((now - t0) / per_ms);
}

static long long civil_days(int y, int m, int d)
{
    long long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Unix seconds, or 0 when the machine has no usable RTC. 0 is a value callers
 * must handle rather than a failure: RFC 5424 lets a sender write "-" for the
 * timestamp, and a missing time is something a collector can merge around. A
 * plausible wrong time is not. */
static long long ulog_wall(void)
{
    int y, mo, d, h, mi, s;
    /* rtc_read returns 1 on SUCCESS (pc64_native.c: `return 1` at the end,
     * `return 0` when the UIP spin times out). Every other caller in the tree
     * has this inverted - see the request filed against qjs_port.c - so it is
     * worth being explicit rather than terse here. */
    if (uno_native_rtc_read(&y, &mo, &d, &h, &mi, &s) != 1) return 0;
    return civil_days(y, mo, d) * 86400ll + h * 3600ll + mi * 60ll + s;
}

static void wall_parts(long long w, int *y, int *mo, int *d,
                       int *h, int *mi, int *s)
{
    long long days = w / 86400, rem = w % 86400;
    long long era, doe, yoe, yy, doy, mp;
    if (rem < 0) { rem += 86400; days--; }
    *h = (int)(rem / 3600); *mi = (int)((rem / 60) % 60); *s = (int)(rem % 60);
    days += 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    yy  = yoe + era * 400;
    doy = doe - (365*yoe + yoe/4 - yoe/100);
    mp  = (5*doy + 2)/153;
    *d  = (int)(doy - (153*mp+2)/5 + 1);
    *mo = (int)(mp + (mp < 10 ? 3 : -9));
    *y  = (int)(yy + (*mo <= 2));
}

/* ---- formatting ---------------------------------------------------------- */
int unolog_format(const unolog_rec *r, char *buf, int cap)
{
    int n;
    if (!r || !buf || cap <= 0) return 0;
    if (r->wall) {
        int y, mo, d, h, mi, s;
        wall_parts(r->wall, &y, &mo, &d, &h, &mi, &s);
        n = snprintf(buf, (size_t)cap, "%04d-%02d-%02d %02d:%02d:%02d %-7s %-8s %s%s%s%s",
                     y, mo, d, h, mi, s, unolog_sev_name(r->sev),
                     unolog_fac_name(r->fac),
                     r->src[0] ? "[" : "", r->src, r->src[0] ? "] " : "",
                     r->text);
    } else {
        /* No clock: uptime, and say that is what it is. A bare number that
         * might be either would make two machines' logs unmergeable. */
        n = snprintf(buf, (size_t)cap, "+%6lu.%03lus %-7s %-8s %s%s%s%s",
                     r->ms / 1000, r->ms % 1000, unolog_sev_name(r->sev),
                     unolog_fac_name(r->fac),
                     r->src[0] ? "[" : "", r->src, r->src[0] ? "] " : "",
                     r->text);
    }
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    return n;
}

/* ---- the ring ------------------------------------------------------------ */
unsigned long unolog_next(void) { return g_next; }
unsigned long unolog_first(void)
{ return g_next > UNOLOG_RING ? g_next - UNOLOG_RING : 0; }

int unolog_get(unsigned long seq, unolog_rec *out)
{
    if (!out || seq >= g_next || seq < unolog_first()) return 0;
    *out = g_ring[seq % UNOLOG_RING];
    return 1;
}

unsigned long unolog_dropped(void)  { return g_dropped; }
unsigned long unolog_sent(void)     { return g_sent; }
unsigned long unolog_received(void) { return g_recvd; }

/* ---- the file sink -------------------------------------------------------
 * Appends into the in-memory image; the image reaches the disk in flush(). */
static void file_append(const unolog_rec *r)
{
    char line[256];
    int n = unolog_format(r, line, (int)sizeof line - 2);
    if (n <= 0) return;
    line[n++] = '\n';
    if (g_filen + n > UNOLOG_FILE_MAX) {
        /* Rotate. One generation only: this is an OS that may be living on a
         * 32 GB stick, and the log anyone wants is nearly always the current
         * one. Losing SYSTEM.1 beats filling the volume. */
        if (g_vol >= 0) {
            uno_fat_write(g_vol, "LOGS\\SYSTEM.1", (const unsigned char *)g_file,
                          (long)g_filen);
            uno_fat_sync();
        }
        g_filen = 0;
    }
    memcpy(g_file + g_filen, line, (size_t)n);
    g_filen += n;
    g_dirty++;
}

/* Find (or make) a volume that will hold \LOGS. Cached; -1 until one exists,
 * which is the normal state for the first moments of boot. */
static int log_vol(void)
{
    int v, nv;
    if (g_vol >= 0) return g_vol;
    nv = uno_fat_volumes();
    for (v = 0; v < nv; v++) {
        uno_fat_entry e[64];
        int i, n = uno_fat_list_ex(v, "", e, 64), have = 0;
        if (n < 0) continue;
        for (i = 0; i < n; i++)
            if (e[i].is_dir && !strcmp(e[i].name, "LOGS")) { have = 1; break; }
        if (!have && !uno_fat_mkdir(v, "LOGS")) continue;   /* read-only, skip */
        g_vol = v;
        return v;
    }
    return -1;
}

int unolog_flush(void)
{
    int wrote;
    if (!g_dirty) return 0;
    if (log_vol() < 0) return 0;          /* no storage yet - keep buffering */
    if (!uno_fat_write(g_vol, "LOGS\\SYSTEM.LOG",
                       (const unsigned char *)g_file, (long)g_filen))
        return 0;
    uno_fat_sync();
    wrote = g_dirty;
    g_dirty = 0;
    g_last_flush = ulog_ms();
    return wrote;
}

/* ---- syslog: the wire ----------------------------------------------------- */
int  net_dhcp_done(void);
int  net_udp_send(const unsigned char dst[4], unsigned short dport,
                  unsigned short sport, const void *data, int len);
int  net_udp_recv(unsigned short sport, void *buf, int cap,
                  unsigned char src[4], unsigned short *src_port);
void net_udp_listen(unsigned short port);
int  net_dns_query(const char *name, unsigned char ip[4]);

static int parse_ipv4(const char *s, unsigned char ip[4])
{
    int i, v, any;
    for (i = 0; i < 4; i++) {
        v = 0; any = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
        if (!any || v > 255) return 0;
        ip[i] = (unsigned char)v;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    return *s == 0;
}

/* RFC 5424. PRI = facility*8 + severity. A machine with no RTC writes "-" for
 * the timestamp, which the RFC allows - see the note on ulog_wall(). */
static int syslog_frame(const unolog_rec *r, char *buf, int cap)
{
    int pri = kSyslogFac[(r->fac >= 0 && r->fac < UNOLOG_NFAC) ? r->fac : 0] * 8
              + (r->sev & 7);
    char ts[32];
    if (r->wall) {
        int y, mo, d, h, mi, s;
        wall_parts(r->wall, &y, &mo, &d, &h, &mi, &s);
        snprintf(ts, sizeof ts, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, h, mi, s);
    } else {
        ts[0] = '-'; ts[1] = 0;
    }
    return snprintf(buf, (size_t)cap, "<%d>1 %s %s unodos - %s - %s",
                    pri, ts, "unodos", unolog_fac_name(r->fac), r->text);
}

static void syslog_emit(const unolog_rec *r)
{
    char pkt[320];
    int n;
    if (!g_remote_host[0]) return;
    if (r->sev > g_remote_level) return;
    /* Never re-forward what arrived from the network: two boxes each listening
     * and sending to the other would amplify one line forever. */
    if (r->fac == LF_REMOTE) return;
    if (!net_dhcp_done()) return;         /* no lease: nothing to send on */
    if (!g_remote_resolved) {
        if (parse_ipv4(g_remote_host, g_remote_ip)) g_remote_resolved = 1;
        else if (net_dns_query(g_remote_host, g_remote_ip)) g_remote_resolved = 1;
        else return;                      /* try again on the next record */
    }
    n = syslog_frame(r, pkt, (int)sizeof pkt);
    if (n <= 0) return;
    if (n > (int)sizeof pkt - 1) n = (int)sizeof pkt - 1;
    if (net_udp_send(g_remote_ip, (unsigned short)g_remote_port,
                     SYSLOG_PORT, pkt, n) >= 0) g_sent++;
}

/* ---- writing a record ---------------------------------------------------- */
void unolog(int sev, int fac, const char *fmt, ...)
{
    unolog_rec *r;
    va_list ap;
    int n;

    if (sev < 0) sev = 0;
    if (sev > LOG_DEBUG) sev = LOG_DEBUG;
    if (fac < 0 || fac >= UNOLOG_NFAC) fac = LF_KERNEL;
    if (sev > g_level) { g_dropped++; return; }
    if (g_in_log) { g_dropped++; return; }   /* a flush that logs; see above */
    g_in_log = 1;

    r = &g_ring[g_next % UNOLOG_RING];
    r->seq  = g_next;
    r->sev  = sev;
    r->fac  = fac;
    r->ms   = ulog_ms();
    r->wall = ulog_wall();
    r->src[0] = 0;
    va_start(ap, fmt);
    n = vsnprintf(r->text, sizeof r->text, fmt, ap);
    va_end(ap);
    if (n < 0) r->text[0] = 0;
    g_next++;

    file_append(r);
    syslog_emit(r);
    /* An error is the line that explains a machine about to lose the ability
     * to write lines, so it does not wait for the periodic flush. */
    if (sev <= LOG_ERR) unolog_flush();
    g_in_log = 0;
}

void unolog_tap(int sev, const char *line)
{
    if (!line || !*line) return;
    unolog(sev, LF_KERNEL, "%s", line);
}

/* ---- being a collector ---------------------------------------------------- */

/* Accept RFC 5424 (<PRI>1 ...) and RFC 3164 (<PRI>Mmm dd hh:mm:ss host ...),
 * because real senders emit both, and fall back to filing the raw text rather
 * than dropping a message we could not parse. A log is the wrong place to be
 * fussy about format: an unparsed line still tells you something, and a
 * discarded one cannot. */
static void collector_file(const char *pkt, int len, const unsigned char src[4])
{
    int sev = LOG_NOTICE, i = 0, pri = 0;
    unolog_rec *r;
    char txt[192];
    int t = 0;

    if (len > 0 && pkt[0] == '<') {
        i = 1;
        while (i < len && pkt[i] >= '0' && pkt[i] <= '9') pri = pri*10 + (pkt[i++]-'0');
        if (i < len && pkt[i] == '>') { i++; sev = pri & 7; }
        else { i = 0; sev = LOG_NOTICE; }
    }
    /* Skip the version + timestamp + host preamble when it looks like one; if
     * it does not, keep everything from here - see the note above. */
    if (i < len && pkt[i] == '1' && i + 1 < len && pkt[i+1] == ' ') {
        int sp = 0;
        i += 2;
        while (i < len && sp < 4) { if (pkt[i] == ' ') sp++; i++; }
    }
    while (i < len && t < (int)sizeof txt - 1) {
        char c = pkt[i++];
        if (c == '\r' || c == '\n') break;
        txt[t++] = c;
    }
    txt[t] = 0;
    if (!t) return;

    if (sev > g_level) { g_dropped++; return; }
    r = &g_ring[g_next % UNOLOG_RING];
    r->seq = g_next; r->sev = sev; r->fac = LF_REMOTE;
    r->ms = ulog_ms(); r->wall = ulog_wall();
    snprintf(r->src, sizeof r->src, "%u.%u.%u.%u", src[0], src[1], src[2], src[3]);
    memcpy(r->text, txt, (size_t)t + 1);
    g_next++;
    g_recvd++;
    file_append(r);
    /* deliberately NOT syslog_emit: LF_REMOTE is never forwarded */
}

int unolog_set_listen(int on)
{
    if (on && !g_listen) { net_udp_listen(SYSLOG_PORT); g_listen = 1; }
    else if (!on) g_listen = 0;      /* the port stays open; we stop draining */
    return g_listen;
}
int unolog_listening(void) { return g_listen; }

/* ---- settings ------------------------------------------------------------- */
int unolog_level(void)        { return g_level; }
int unolog_remote_level(void) { return g_remote_level; }
void unolog_set_level(int s)
{ if (s >= 0 && s <= LOG_DEBUG) g_level = s; }
void unolog_set_remote_level(int s)
{ if (s >= 0 && s <= LOG_DEBUG) g_remote_level = s; }

const char *unolog_remote_host(void) { return g_remote_host; }
int unolog_remote_port(void)         { return g_remote_port; }

int unolog_set_remote(const char *host, int port)
{
    g_remote_resolved = 0;
    if (!host || !*host) { g_remote_host[0] = 0; return 1; }
    if (strlen(host) >= sizeof g_remote_host) return 0;
    strcpy(g_remote_host, host);
    g_remote_port = port > 0 ? port : SYSLOG_PORT;
    return 1;
}

/* ---- \LOGS\LOG.CFG -------------------------------------------------------
 * unolog's own parser. pc64_stress_cfg_* belongs to the debug harness and does
 * not exist in a production build - which is the build this is for. Read
 * WHOLE, unlike DEBUG.CFG, whose 512-byte read has twice now silently eaten
 * keys that sat past the cutoff. */
static int cfg_num(const char *v) { int n = 0; while (*v >= '0' && *v <= '9') n = n*10 + (*v++ - '0'); return n; }

static void cfg_apply(char *buf, long len)
{
    long i = 0;
    while (i < len) {
        char *line = buf + i, *eq;
        long j = i;
        while (j < len && buf[j] != '\n' && buf[j] != '\r') j++;
        buf[j] = 0;
        i = j + 1;
        while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || !*line) continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, "level"))             unolog_set_level(cfg_num(eq));
        else if (!strcmp(line, "remote_level")) unolog_set_remote_level(cfg_num(eq));
        else if (!strcmp(line, "listen"))       unolog_set_listen(cfg_num(eq) != 0);
        else if (!strcmp(line, "remote")) {
            char host[64]; int p = 0, k = 0;
            while (eq[k] && eq[k] != ':' && k < (int)sizeof host - 1) { host[k] = eq[k]; k++; }
            host[k] = 0;
            if (eq[k] == ':') p = cfg_num(eq + k + 1);
            unolog_set_remote(host, p);
        }
    }
}

static void cfg_load(void)
{
    static char buf[1024];
    long n;
    if (log_vol() < 0) return;
    n = uno_fat_read(g_vol, "LOGS\\LOG.CFG", (unsigned char *)buf, (long)sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    cfg_apply(buf, n);
}

int unolog_save_cfg(void)
{
    char buf[256];
    int n;
    if (log_vol() < 0) return 0;
    n = snprintf(buf, sizeof buf,
                 "# unolog settings - see pc64/UNOLOG.md\r\n"
                 "level=%d\r\nremote_level=%d\r\nremote=%s%s%d\r\nlisten=%d\r\n",
                 g_level, g_remote_level,
                 g_remote_host, g_remote_host[0] ? ":" : "",
                 g_remote_host[0] ? g_remote_port : 0, g_listen ? 1 : 0);
    if (n <= 0) return 0;
    if (!uno_fat_write(g_vol, "LOGS\\LOG.CFG", (const unsigned char *)buf, n)) return 0;
    uno_fat_sync();
    return 1;
}

/* ---- lifecycle ------------------------------------------------------------ */
void unolog_init(void)
{
    if (g_inited) return;
    g_inited = 1;
    cfg_load();                 /* a no-op if storage is not up yet; retried in tick */
    ulog_notice(LF_KERNEL, "unolog started: level=%s remote_level=%s",
                unolog_sev_name(g_level), unolog_sev_name(g_remote_level));
}

void unolog_tick(void)
{
    unsigned long now;
    if (!g_inited) return;

    /* The config may not have been readable at init - storage comes up later
     * than the first log line, by design - so try once more when a volume
     * appears. Exactly once: re-reading it every tick would fight the viewer. */
    if (g_vol < 0 && log_vol() >= 0) cfg_load();

    if (g_listen) {
        char pkt[512];
        unsigned char src[4];
        unsigned short sp;
        int n, guard = 8;         /* bounded: a flood must not own the frame */
        while (guard-- > 0 &&
               (n = net_udp_recv(SYSLOG_PORT, pkt, (int)sizeof pkt, src, &sp)) > 0)
            collector_file(pkt, n, src);
    }

    now = ulog_ms();
    if (g_dirty && (now - g_last_flush) >= UNOLOG_FLUSH_MS) unolog_flush();
}

void unolog_shutdown(void)
{
    if (!g_inited) return;
    ulog_notice(LF_KERNEL, "unolog stopping: %lu records, %lu dropped, "
                "%lu sent, %lu received", g_next, g_dropped, g_sent, g_recvd);
    unolog_flush();
}
