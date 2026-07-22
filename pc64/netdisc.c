/* ===========================================================================
 * UnoDOS/pc64 - netdisc: zero-config LAN discovery (see netdisc.h).
 * ======================================================================== */
#include "netdisc.h"
#ifdef UNO_DEBUG
#include "netsock.h"
#include "unoauto.h"        /* unoauto_log / UA_CH_NET */
#include "uno_debug.h"      /* pc64_stress_cfg_flag / _value */
#include <string.h>

#define DISC_PORT   5400
#define DISC_MAGIC  "UNODISC"

extern int pc64_net_up(void);
extern int unoauto_remote_active(void);

static int  g_active, g_up, g_have_host;
static int  g_udp = -1;              /* UDP socket bound to DISC_PORT */
static u8   g_host_ip[4];
static u16  g_host_port;
static u32  g_tick, g_next_probe;
static char g_name[24] = "unodos-pc64";

/* ---- tiny string helpers (no libc strtok dependency) ------------------- */
static int ap_s(char *b, int cap, int o, const char *s)
{ while (*s && o < cap - 1) b[o++] = *s++; return o; }
static int ap_i(char *b, int cap, int o, unsigned v)
{ char t[12]; int n = 0;
  if (!v) { if (o < cap - 1) b[o++] = '0'; return o; }
  while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
  while (n && o < cap - 1) b[o++] = t[--n];
  return o; }
static int ap_ip(char *b, int cap, int o, const u8 ip[4])
{ int i; for (i = 0; i < 4; i++) { if (i) o = ap_s(b, cap, o, "."); o = ap_i(b, cap, o, ip[i]); } return o; }

static unsigned atoi_(const char *s)
{ unsigned v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); s++; } return v; }

static int parse_ip(const char *s, u8 ip[4])
{
    int i, v;
    for (i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        if (v > 255) return 0;
        ip[i] = (u8)v;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    return 1;
}

/* split in place into up to `max` NUL-terminated tokens on spaces/newlines */
static int split(char *s, char **t, int max)
{
    int n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\n' || *s == '\r') s++;
        if (!*s || n >= max) break;
        t[n++] = s;
        while (*s && *s != ' ' && *s != '\n' && *s != '\r') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

/* ---- beacon formatters -------------------------------------------------- */
static int fmt_probe(char *b, int cap)
{ int o = 0; o = ap_s(b, cap, o, "UNODISC 1 PROBE pc64 "); o = ap_s(b, cap, o, g_name);
  o = ap_s(b, cap, o, " 1"); return o; }
static int fmt_offer_self(char *b, int cap)
{ int o = 0; o = ap_s(b, cap, o, "UNODISC 1 OFFER pc64 "); o = ap_s(b, cap, o, g_name);
  o = ap_s(b, cap, o, " 1 "); o = ap_ip(b, cap, o, net_ip()); o = ap_s(b, cap, o, " 0"); return o; }
static int fmt_gothost(char *b, int cap, const u8 ip[4], u16 port)
{ int o = 0; o = ap_s(b, cap, o, "UNODISC 1 GOTHOST "); o = ap_ip(b, cap, o, ip);
  o = ap_s(b, cap, o, " "); o = ap_i(b, cap, o, port); return o; }

/* ---- inbound handling --------------------------------------------------- */
static void handle_disc(char *s, const u8 src[4], u16 sp)
{
    char *t[8];
    int nt = split(s, t, 8);
    if (nt < 3) return;
    if (strcmp(t[0], DISC_MAGIC) != 0) return;          /* not ours */
    /* t[1] = version (accept "1"); t[2] = type */
    if (!strcmp(t[2], "OFFER") && nt >= 8) {
        if (!strcmp(t[3], "host")) {                    /* a dev PC's URC offer */
            u8 ip[4]; u16 port;
            if (parse_ip(t[6], ip) && (port = (u16)atoi_(t[7])) != 0) {
                memcpy(g_host_ip, ip, 4); g_host_port = port; g_have_host = 1;
                unoauto_log(UA_CH_NET, "disc: host %d.%d.%d.%d:%d",
                            ip[0], ip[1], ip[2], ip[3], port);
                { char b[64]; int l = fmt_gothost(b, (int)sizeof b, ip, port);
                  net_sendto(g_udp, src, sp, b, l); }   /* confirm to the offerer */
            }
        }
    } else if (!strcmp(t[2], "PROBE")) {                /* someone is looking */
        char b[96]; int l = fmt_offer_self(b, (int)sizeof b);
        net_sendto(g_udp, src, sp, b, l);               /* announce ourselves */
    }
}

/* ---- public ------------------------------------------------------------- */
void netdisc_boot(void)
{
    char v[24];
    if (pc64_stress_cfg_flag("discover") <= 0) return;
    g_active = 1;
    if (pc64_stress_cfg_value("name", v, (int)sizeof v) > 0) {
        int i; for (i = 0; i < (int)sizeof g_name - 1 && v[i]; i++) g_name[i] = v[i];
        g_name[i] = 0;
    }
}

void netdisc_tick(void)
{
    u8 buf[256], src[4]; u16 sp;
    int n;
    if (!g_active) return;
    g_tick++;
    if (!g_up) {
        if (!pc64_net_up()) return;                     /* keep trying until the NIC is up */
        g_udp = net_socket(SOCK_UDP);
        if (g_udp < 0) return;
        net_bind(g_udp, DISC_PORT);
        g_up = 1; g_next_probe = g_tick;                /* probe immediately */
    }
    if (!unoauto_remote_active()) net_poll();           /* pump RX if the link isn't */
    /* broadcast a PROBE periodically until a host answers */
    if (!g_have_host && (int)(g_tick - g_next_probe) >= 0) {
        char b[96]; int l = fmt_probe(b, (int)sizeof b);
        net_sendbcast(g_udp, DISC_PORT, b, l);
        g_next_probe = g_tick + 100;
    }
    /* service inbound discovery datagrams */
    while ((n = net_recvfrom(g_udp, buf, (int)sizeof buf - 1, src, &sp)) > 0) {
        buf[n] = 0;
        handle_disc((char *)buf, src, sp);
    }
}

int  netdisc_active(void)          { return g_active; }
int  netdisc_have_host(void)       { return g_have_host; }
const u8 *netdisc_host_ip(void)    { return g_host_ip; }
unsigned short netdisc_host_port(void) { return g_host_port; }

#endif /* UNO_DEBUG */
