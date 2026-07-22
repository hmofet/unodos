/* ===========================================================================
 * UnoDOS/pc64 - compact TCP/IP stack (see net.h).
 * ======================================================================== */
#include "net.h"
#include <string.h>

/* ---- static config: matches QEMU SLIRP (-netdev user) defaults ---------- */
static u8 MYIP[4] = {10, 0, 2, 15};
static u8 GW[4]   = {10, 0, 2, 2};
static u8 MASK[4] = {255, 255, 255, 0};
static u8 DNS[4]  = {10, 0, 2, 3};        /* resolver (SLIRP default; DHCP opt 6) */
static int g_dhcp_ack_len, g_dhcp_had_dns, g_dhcp_had_rtr;   /* last-ACK diagnostics */
int net_dhcp_ack_len(void)  { return g_dhcp_ack_len; }
int net_dhcp_had_dns(void)  { return g_dhcp_had_dns; }
int net_dhcp_had_rtr(void)  { return g_dhcp_had_rtr; }

/* last-net_dns_query diagnostics: queries sent, datagrams that reached our
 * port, txid rejects, negative (an=0) responses. Splits "no reply ever
 * arrived" (TX side / router policy) from "replies arrived but were
 * rejected/negative" (parse / cold-cache) on metal. */
static int g_dns_sent, g_dns_rx, g_dns_badid, g_dns_neg;
int net_dns_sent(void)  { return g_dns_sent; }
int net_dns_rx(void)    { return g_dns_rx; }
int net_dns_badid(void) { return g_dns_badid; }
int net_dns_neg(void)   { return g_dns_neg; }

static uno_nic_t *g_nic;
static u8 g_mac[6];
static u32 g_ticks;

/* ---- byte helpers ------------------------------------------------------- */
static u16 rd16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rd32(const u8 *p)
{ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void wr16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static void wr32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static int ip_eq(const u8 *a, const u8 *b)
{ return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3]; }

/* ones-complement internet checksum */
static u16 cksum(const u8 *p, int n, u32 seed)
{
    u32 s = seed;
    int i;
    for (i = 0; i + 1 < n; i += 2) s += (u32)((p[i] << 8) | p[i + 1]);
    if (i < n) s += (u32)(p[i] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)(~s & 0xFFFF);
}

/* ---- frame scratch ------------------------------------------------------ */
#define FRM 1600
static u8 g_tx[FRM];

/* Link-level frame counters. The AX88179 batch round failed at DHCP with the
 * link up, and the NETLOG had no way to say whether we ever TRANSMITTED or ever
 * RECEIVED anything - so "L2 TX/RX or the server" stayed a three-way guess.
 * These make it a one-line answer: tx>0 rx==0 => our RX path or the cable/server
 * is deaf; tx==0 => our TX path never fired; rx>0 but ip==0 => frames arrive but
 * DHCP replies aren't parsed. Reset each net_init. */
static u32 g_tx_frames, g_rx_frames, g_rx_arp, g_rx_ip;
static int nic_tx(int flen)
{ int r; if (!g_nic || flen <= 0) return -1;
  r = g_nic->send(g_nic->ctx, g_tx, flen);
  if (r >= 0) g_tx_frames++;            /* count sends that left the driver, not attempts */
  return r; }
u32 net_tx_frames(void) { return g_tx_frames; }
u32 net_rx_frames(void) { return g_rx_frames; }
u32 net_rx_arp(void)    { return g_rx_arp; }
u32 net_rx_ip(void)     { return g_rx_ip; }

/* Ethernet: dst[6] src[6] type[2] then payload. Returns total frame length. */
static int eth_build(const u8 dst[6], u16 type, int payload_len)
{
    memcpy(g_tx, dst, 6);
    memcpy(g_tx + 6, g_mac, 6);
    wr16(g_tx + 12, type);
    return 14 + payload_len;
}

/* ======================= ARP ============================================= */
#define ARP_N 8
static struct { u8 ip[4]; u8 mac[6]; int used; } arp_cache[ARP_N];

static void arp_put(const u8 ip[4], const u8 mac[6])
{
    int i, slot = -1;
    for (i = 0; i < ARP_N; i++) {
        if (arp_cache[i].used && ip_eq(arp_cache[i].ip, ip)) { slot = i; break; }
        if (slot < 0 && !arp_cache[i].used) slot = i;
    }
    if (slot < 0) slot = 0;
    memcpy(arp_cache[slot].ip, ip, 4);
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].used = 1;
}
static int arp_get(const u8 ip[4], u8 mac[6])
{
    int i;
    for (i = 0; i < ARP_N; i++)
        if (arp_cache[i].used && ip_eq(arp_cache[i].ip, ip)) {
            memcpy(mac, arp_cache[i].mac, 6); return 1;
        }
    return 0;
}

static void arp_send(int op, const u8 tip[4], const u8 tmac[6])
{
    static const u8 bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    u8 *a = g_tx + 14;
    int flen;
    wr16(a + 0, 1);            /* HW: ethernet */
    wr16(a + 2, 0x0800);       /* proto: IPv4  */
    a[4] = 6; a[5] = 4;
    wr16(a + 6, op);           /* 1 req, 2 reply */
    memcpy(a + 8, g_mac, 6); memcpy(a + 14, MYIP, 4);
    memcpy(a + 18, tmac, 6);  memcpy(a + 24, tip, 4);
    flen = eth_build(op == 1 ? bcast : tmac, 0x0806, 28);
    nic_tx(flen);
}

static void arp_request(const u8 ip[4])
{
    static const u8 zero[6] = {0,0,0,0,0,0};
    arp_send(1, ip, zero);
}

int net_arp_resolve(const u8 ip[4], u8 mac_out[6])
{
    const u8 *target = ip;
    /* off-link -> resolve the gateway instead */
    if (((ip[0]^MYIP[0])&MASK[0]) | ((ip[1]^MYIP[1])&MASK[1]) |
        ((ip[2]^MYIP[2])&MASK[2]) | ((ip[3]^MYIP[3])&MASK[3]))
        target = GW;
    if (arp_get(target, mac_out)) return 1;
    arp_request(target);
    return 0;
}

static void arp_recv(const u8 *a, int len)
{
    if (len < 28) return;
    if (rd16(a + 6) == 1 && ip_eq(a + 24, MYIP))       /* request for us */
        arp_send(2, a + 14, a + 8);
    if (rd16(a + 6) == 2)                                /* a reply */
        arp_put(a + 14, a + 8);
    else if (rd16(a + 6) == 1)
        arp_put(a + 14, a + 8);                          /* learn requester */
}

/* ======================= IPv4 ============================================ */
static u16 g_ipid = 1;

/* build an IPv4 packet into g_tx payload; returns frame length or -1 if the
   destination MAC isn't resolved yet (caller retries after net_poll) */
static int ip_build(const u8 dst[4], u8 proto, const u8 *payload, int plen)
{
    u8 mac[6];
    u8 *ip = g_tx + 14;
    int total = 20 + plen;
    if (!net_arp_resolve(dst, mac)) return -1;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (u16)total);
    wr16(ip + 4, g_ipid++);
    wr16(ip + 6, 0);              /* no fragmentation */
    ip[8] = 64; ip[9] = proto;
    wr16(ip + 10, 0);
    memcpy(ip + 12, MYIP, 4);
    memcpy(ip + 16, dst, 4);
    wr16(ip + 10, cksum(ip, 20, 0));
    memcpy(ip + 20, payload, plen);
    return eth_build(mac, 0x0800, total);
}

/* pseudo-header checksum seed for TCP/UDP */
static u32 pseudo_seed(const u8 src[4], const u8 dst[4], u8 proto, int l4len)
{
    u32 s = 0;
    s += (src[0]<<8)|src[1]; s += (src[2]<<8)|src[3];
    s += (dst[0]<<8)|dst[1]; s += (dst[2]<<8)|dst[3];
    s += proto; s += l4len;
    return s;
}

/* ======================= ICMP =========================================== */
static int g_ping_sent, g_ping_got, g_ping_t0, g_ping_ms;
static u16 g_ping_seq;
static u8 g_ping_dst[4];               /* S-NET-08: COPY, not the caller's ptr -
                                          the post-ARP retry runs a poll later,
                                          by which point a caller's stack IP is
                                          long gone (dangling read) */
static int g_ping_dst_set;

void net_ping(const u8 ip[4])
{
    u8 icmp[8];
    int flen;
    g_ping_dst[0] = ip[0]; g_ping_dst[1] = ip[1];
    g_ping_dst[2] = ip[2]; g_ping_dst[3] = ip[3];
    g_ping_dst_set = 1;
    icmp[0] = 8; icmp[1] = 0;          /* echo request */
    wr16(icmp + 2, 0);
    wr16(icmp + 4, 0x4944);            /* id */
    wr16(icmp + 6, ++g_ping_seq);
    wr16(icmp + 2, cksum(icmp, 8, 0));
    flen = ip_build(ip, 1, icmp, 8);
    if (flen > 0) {
        nic_tx(flen);
        g_ping_sent = 1; g_ping_got = 0; g_ping_t0 = g_ticks;
    } else {
        g_ping_sent = 2;               /* pending ARP: retry in poll */
    }
}
int net_ping_replied(void) { return g_ping_got; }
int net_ping_ms(void) { return g_ping_ms; }

static void icmp_recv(const u8 *ip, const u8 *icmp, int len)
{
    if (len >= 8 && icmp[0] == 0 && rd16(icmp + 4) == 0x4944) {   /* echo reply */
        g_ping_got = 1;
        g_ping_ms = g_ticks - g_ping_t0;
    }
    if (len >= 8 && icmp[0] == 8) {              /* echo request -> reply */
        u8 reply[64];
        int n = len; if (n > 64) n = 64;
        memcpy(reply, icmp, n);
        reply[0] = 0; wr16(reply + 2, 0);
        wr16(reply + 2, cksum(reply, n, 0));
        { int flen = ip_build(ip + 12, 1, reply, n);   /* ip+12 = their src */
          if (flen > 0) nic_tx(flen); }
    }
}

/* ======================= UDP ============================================= */
#define UDP_QN 4
static struct {
    u16 port; int used;
    u8 data[512]; int len; u8 src[4]; u16 sport;
} udp_q[UDP_QN];

static void udp_bind(u16 port)
{
    int i;
    for (i = 0; i < UDP_QN; i++) if (udp_q[i].used && udp_q[i].port == port) return;
    for (i = 0; i < UDP_QN; i++) if (!udp_q[i].used) {
        udp_q[i].used = 1; udp_q[i].port = port; udp_q[i].len = 0; return;
    }
}

int net_udp_send(const u8 dst[4], u16 dport, u16 sport, const void *data, int len)
{
    u8 udp[8 + 512];
    int flen;
    u32 seed;
    if (len > 512) len = 512;
    udp_bind(sport);
    wr16(udp + 0, sport); wr16(udp + 2, dport);
    wr16(udp + 4, (u16)(8 + len)); wr16(udp + 6, 0);
    memcpy(udp + 8, data, len);
    seed = pseudo_seed(MYIP, dst, 17, 8 + len);
    { u16 c = cksum(udp, 8 + len, seed); wr16(udp + 6, c ? c : 0xFFFF); }
    flen = ip_build(dst, 17, udp, 8 + len);
    if (flen < 0) return -1;
    return nic_tx(flen);
}

/* Limited broadcast (255.255.255.255). ARP cannot resolve a broadcast IP, so
 * build the Ethernet + IP + UDP frame directly with a broadcast dst MAC - the
 * same technique the DHCP path uses. Source is MYIP (0.0.0.0 before a lease).
 * Binds `sport` so a peer's unicast reply is queued for net_udp_recv. */
int net_udp_broadcast(u16 dport, u16 sport, const void *data, int len)
{
    static const u8 bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const u8 bcast_ip[4]  = {255,255,255,255};
    u8 *ip = g_tx + 14, *udp = ip + 20;
    int total, flen;
    u32 seed;
    if (len < 0) len = 0;
    if (len > 512) len = 512;
    udp_bind(sport);
    wr16(udp + 0, sport); wr16(udp + 2, dport);
    wr16(udp + 4, (u16)(8 + len)); wr16(udp + 6, 0);
    memcpy(udp + 8, data, len);
    seed = pseudo_seed(MYIP, bcast_ip, 17, 8 + len);
    { u16 c = cksum(udp, 8 + len, seed); wr16(udp + 6, c ? c : 0xFFFF); }
    ip[0] = 0x45; ip[1] = 0; wr16(ip + 2, (u16)(28 + len));
    wr16(ip + 4, g_ipid++); wr16(ip + 6, 0); ip[8] = 64; ip[9] = 17;
    wr16(ip + 10, 0); memcpy(ip + 12, MYIP, 4); memcpy(ip + 16, bcast_ip, 4);
    wr16(ip + 10, cksum(ip, 20, 0));
    total = 28 + len;
    flen = eth_build(bcast_mac, 0x0800, total);
    return nic_tx(flen);
}

void net_udp_listen(u16 port) { udp_bind(port); }

int net_udp_recv(u16 sport, void *buf, int cap, u8 src[4], u16 *src_port)
{
    int i;
    for (i = 0; i < UDP_QN; i++)
        if (udp_q[i].used && udp_q[i].port == sport && udp_q[i].len > 0) {
            int n = udp_q[i].len; if (n > cap) n = cap;
            memcpy(buf, udp_q[i].data, n);
            if (src) memcpy(src, udp_q[i].src, 4);
            if (src_port) *src_port = udp_q[i].sport;
            udp_q[i].len = 0;
            return n;
        }
    return 0;
}

static void udp_recv(const u8 *ip, const u8 *udp, int len)
{
    u16 dport = rd16(udp + 2);
    int i, dlen = len - 8;
    void dhcp_input(const u8 *udp, int len);
    if (dport == 68) { dhcp_input(udp, len); return; }
    if (dlen < 0) return;
    for (i = 0; i < UDP_QN; i++)
        if (udp_q[i].used && udp_q[i].port == dport) {
            int n = dlen; if (n > 512) n = 512;
            memcpy(udp_q[i].data, udp + 8, n);
            udp_q[i].len = n;
            udp_q[i].sport = rd16(udp + 0);
            memcpy(udp_q[i].src, ip + 12, 4);
            return;
        }
}

/* ======================= TCP / sockets ================================== *
 * A small socket table (netsock.h): NSOCK slots, each an SOCK_UDP port or an
 * SOCK_TCP connection - active, passive (TCP_LISTEN), or a child accepted from
 * a listener. The per-connection TCP engine is the metal-tested single-
 * connection logic (S-NET-15 partial-overlap accounting, window advertisement,
 * retransmit) generalized to run on any slot. UDP slots reuse the udp_q store.
 * The legacy net_tcp_* API (net.h) wraps a single reserved slot, so tls.c /
 * pc64_http / unoauto_remote / the .UNO app ABI are unchanged. */
#include "netsock.h"

typedef struct {
    int   type;                  /* SOCK_NONE / SOCK_TCP / SOCK_UDP */
    int   state;                 /* TCP_* (SOCK_TCP); 1 if bound (SOCK_UDP) */
    u8    raddr[4];              /* remote peer IP */
    u16   lport, rport;          /* local, remote ports */
    u32   snd_nxt, snd_una, rcv_nxt;
    int   retx_at;               /* tick to retransmit the unacked control/data */
    u8    pend[512]; int pend_len;   /* the single outstanding data segment */
    /* 8 KB rx queue per connection: a CA-validated TLS handshake's certificate
     * flight is ~4-5 KB arriving back-to-back; a smaller queue overflows the
     * tail (S-NET-15) and the stream comes out corrupt. */
    u8    rxq[8192]; int rxq_len;
    int   accept_owner;          /* SYN_RCVD child: index of the LISTEN socket */
} sock_t;

static sock_t sk[NSOCK];
static int    g_legacy = -1;     /* reserved slot backing the legacy net_tcp_* API */

/* ephemeral local-port generator (40000-60000; avoids well-known + DNS 5300) */
static u16 g_eport = 40000;
static u16 eport_next(void)
{ g_eport++; if (g_eport < 40001u || g_eport > 60000u) g_eport = 40001; return g_eport; }

static int sock_alloc(int type)
{
    int i;
    for (i = 0; i < NSOCK; i++) if (!sk[i].type) {
        memset(&sk[i], 0, sizeof sk[i]);
        sk[i].type  = type;
        sk[i].state = (type == SOCK_TCP) ? TCP_CLOSED : 0;
        return i;
    }
    return -1;
}
static int sk_valid(int s) { return s >= 0 && s < NSOCK && sk[s].type; }

/* emit one segment for connection c */
static void tcp_out(sock_t *c, u8 flags, const u8 *data, int dlen)
{
    u8 seg[20 + 512];
    int flen;
    u32 seed;
    memset(seg, 0, 20);
    wr16(seg + 0, c->lport); wr16(seg + 2, c->rport);
    wr32(seg + 4, c->snd_nxt);
    wr32(seg + 8, c->rcv_nxt);
    seg[12] = 0x50;              /* data offset 5 words */
    seg[13] = flags;
    /* advertise the space we can actually store (S-NET-23) */
    wr16(seg + 14, (u16)((int)sizeof c->rxq - c->rxq_len));
    wr16(seg + 16, 0);          /* checksum (filled below) */
    if (dlen > 0) memcpy(seg + 20, data, dlen);
    seed = pseudo_seed(MYIP, c->raddr, 6, 20 + dlen);
    wr16(seg + 16, cksum(seg, 20 + dlen, seed));
    flen = ip_build(c->raddr, 6, seg, 20 + dlen);
    if (flen > 0) nic_tx(flen);
}

/* ---- socket API (netsock.h) -------------------------------------------- */
int net_socket(int type)
{
    if (type != SOCK_TCP && type != SOCK_UDP) return -1;
    return sock_alloc(type);
}

int net_bind(int s, unsigned short port)
{
    if (!sk_valid(s)) return -1;
    sk[s].lport = port;
    if (sk[s].type == SOCK_UDP) { if (port) udp_bind(port); sk[s].state = 1; }
    return 0;
}

int net_listen(int s)
{
    if (!sk_valid(s) || sk[s].type != SOCK_TCP || !sk[s].lport) return -1;
    sk[s].state = TCP_LISTEN;
    return 0;
}

int net_connect(int s, const unsigned char dst[4], unsigned short dport)
{
    sock_t *c;
    if (!sk_valid(s) || sk[s].type != SOCK_TCP) return -1;
    c = &sk[s];
    memcpy(c->raddr, dst, 4);
    c->rport = dport;
    if (!c->lport) c->lport = eport_next();
    c->snd_nxt = 0x1000; c->snd_una = c->snd_nxt; c->rcv_nxt = 0;
    c->state = TCP_SYN_SENT;
    c->pend_len = 0; c->rxq_len = 0;
    tcp_out(c, 0x02, 0, 0);        /* SYN */
    c->snd_nxt++;                  /* SYN consumes a sequence number */
    c->retx_at = g_ticks + 30;
    return 0;
}

int net_sock_state(int s) { return sk_valid(s) ? sk[s].state : TCP_CLOSED; }

int net_send(int s, const void *data, int len)
{
    sock_t *c;
    if (!sk_valid(s) || sk[s].type != SOCK_TCP) return -1;
    c = &sk[s];
    if (c->state != TCP_ESTABLISHED || c->pend_len) return -1;   /* one in flight */
    if (len > 512) len = 512;
    memcpy(c->pend, data, len);
    c->pend_len = len;
    tcp_out(c, 0x18, c->pend, len);      /* PSH|ACK */
    c->snd_nxt += len;
    c->retx_at = g_ticks + 30;
    return len;
}

int net_recv(int s, void *buf, int cap)
{
    sock_t *c;
    int n, free_before;
    if (!sk_valid(s) || sk[s].type != SOCK_TCP) return 0;
    c = &sk[s];
    n = c->rxq_len;
    free_before = (int)sizeof c->rxq - c->rxq_len;
    if (n <= 0) return 0;
    if (n > cap) n = cap;
    memcpy(buf, c->rxq, n);
    if (n < c->rxq_len) { memmove(c->rxq, c->rxq + n, c->rxq_len - n); c->rxq_len -= n; }
    else c->rxq_len = 0;
    /* reopen a collapsed window so the peer leaves zero-window probing */
    if (free_before < 1460 && c->state == TCP_ESTABLISHED) tcp_out(c, 0x10, 0, 0);
    return n;
}

int net_accept(int s)
{
    sock_t *l;
    int child, i;
    if (!sk_valid(s) || sk[s].type != SOCK_TCP || sk[s].state != TCP_LISTEN) return -1;
    l = &sk[s];
    /* the listener stores ready child ids in its rxq bytes? no - scan for a
     * child that is ESTABLISHED and owned by this listener, hand it over once. */
    for (child = 0; child < NSOCK; child++) {
        sock_t *ch = &sk[child];
        if (ch->type == SOCK_TCP && ch->state == TCP_ESTABLISHED &&
            ch->accept_owner == s + 1) {           /* +1: 0 means "no owner" */
            ch->accept_owner = 0;                  /* detach: now an app socket */
            return child;
        }
    }
    (void)i; (void)l;
    return -1;
}

int net_sendto(int s, const unsigned char dst[4], unsigned short dport,
               const void *data, int len)
{
    if (!sk_valid(s) || sk[s].type != SOCK_UDP) return -1;
    if (!sk[s].lport) sk[s].lport = eport_next();
    return net_udp_send(dst, dport, sk[s].lport, data, len);
}

int net_recvfrom(int s, void *buf, int cap, unsigned char src[4], unsigned short *sport)
{
    if (!sk_valid(s) || sk[s].type != SOCK_UDP) return 0;
    return net_udp_recv(sk[s].lport, buf, cap, src, sport);
}

int net_sendbcast(int s, unsigned short dport, const void *data, int len)
{
    if (!sk_valid(s) || sk[s].type != SOCK_UDP) return -1;
    if (!sk[s].lport) sk[s].lport = eport_next();
    return net_udp_broadcast(dport, sk[s].lport, data, len);
}

int net_sock_peer(int s, unsigned char ip[4], unsigned short *port)
{
    if (!sk_valid(s) || sk[s].type != SOCK_TCP) return -1;
    if (ip) memcpy(ip, sk[s].raddr, 4);
    if (port) *port = sk[s].rport;
    return 0;
}

unsigned short net_sock_port(int s) { return sk_valid(s) ? sk[s].lport : 0; }
int net_sock_count(void) { int i, n = 0; for (i = 0; i < NSOCK; i++) if (sk[i].type) n++; return n; }

void net_sock_close(int s)
{
    sock_t *c;
    if (!sk_valid(s)) return;
    c = &sk[s];
    if (c->type == SOCK_TCP && c->state == TCP_ESTABLISHED) {
        tcp_out(c, 0x11, 0, 0);           /* FIN|ACK */
        c->snd_nxt++;
    }
    c->type = SOCK_NONE;                   /* free the slot */
    c->state = TCP_CLOSED;
}

/* ---- legacy single-connection API (net.h) over the reserved slot -------- */
static sock_t *legacy(void)
{
    if (g_legacy < 0) g_legacy = sock_alloc(SOCK_TCP);
    return (g_legacy >= 0) ? &sk[g_legacy] : 0;
}

int net_tcp_connect(const u8 dst[4], u16 dport)
{
    sock_t *c = legacy();
    if (!c) return -1;
    c->type = SOCK_TCP;
    memcpy(c->raddr, dst, 4);
    c->rport = dport;
    c->lport = eport_next();
    c->snd_nxt = 0x1000; c->snd_una = c->snd_nxt; c->rcv_nxt = 0;
    c->state = TCP_SYN_SENT;
    c->pend_len = 0; c->rxq_len = 0;
    c->accept_owner = 0;
    tcp_out(c, 0x02, 0, 0);        /* SYN */
    c->snd_nxt++;
    c->retx_at = g_ticks + 30;
    return 0;
}

int net_tcp_state(void) { return (g_legacy >= 0) ? sk[g_legacy].state : TCP_CLOSED; }
int net_tcp_send(const void *data, int len) { return (g_legacy >= 0) ? net_send(g_legacy, data, len) : -1; }
int net_tcp_recv(void *buf, int cap)        { return (g_legacy >= 0) ? net_recv(g_legacy, buf, cap) : 0; }

void net_tcp_close(void)
{
    sock_t *c;
    if (g_legacy < 0) return;
    c = &sk[g_legacy];
    if (c->state == TCP_ESTABLISHED) {
        tcp_out(c, 0x11, 0, 0);           /* FIN|ACK */
        c->snd_nxt++;
        c->state = TCP_FIN_WAIT;          /* keep the slot: peer's FIN/ACK still drains */
    }
}

/* ---- per-connection receive + timers ----------------------------------- */
static sock_t *conn_find(const u8 *ip, u16 their_src, u16 their_dst)
{
    int i;
    for (i = 0; i < NSOCK; i++) {
        sock_t *c = &sk[i];
        if (c->type != SOCK_TCP) continue;
        if (c->state == TCP_LISTEN || c->state == TCP_CLOSED) continue;
        if (c->lport == their_dst && c->rport == their_src && ip_eq(c->raddr, ip + 12))
            return c;
    }
    return 0;
}
static sock_t *listener_find(u16 dport)
{
    int i;
    for (i = 0; i < NSOCK; i++)
        if (sk[i].type == SOCK_TCP && sk[i].state == TCP_LISTEN && sk[i].lport == dport)
            return &sk[i];
    return 0;
}

static void tcp_recv(const u8 *ip, const u8 *seg, int len)
{
    u16 sp = rd16(seg + 0), dp = rd16(seg + 2);
    u8  flags = seg[13];
    u32 seq = rd32(seg + 4), ack = rd32(seg + 8);
    int hl = (seg[12] >> 4) * 4;
    int dlen = len - hl;
    sock_t *c = conn_find(ip, sp, dp);

    if (!c) {
        /* No live connection for this 4-tuple. A bare SYN to a LISTEN port
         * opens a child connection (passive open -> SYN_RCVD). */
        if ((flags & 0x12) == 0x02) {
            sock_t *l = listener_find(dp);
            int cid;
            if (!l) return;
            cid = sock_alloc(SOCK_TCP);
            if (cid < 0) return;                   /* table full: drop, peer retries */
            c = &sk[cid];
            c->lport = dp; c->rport = sp;
            memcpy(c->raddr, ip + 12, 4);
            c->rcv_nxt = seq + 1;
            c->snd_nxt = 0x2000; c->snd_una = c->snd_nxt;
            c->state = TCP_SYN_RCVD;
            c->accept_owner = (int)(l - sk) + 1;   /* +1 so 0 = "no owner" */
            c->retx_at = g_ticks + 30;
            tcp_out(c, 0x12, 0, 0);                /* SYN|ACK */
            c->snd_nxt++;
        }
        return;
    }

    if (flags & 0x04) { c->state = TCP_DONE; return; }   /* RST */

    if (c->state == TCP_SYN_SENT) {
        if ((flags & 0x12) == 0x12) {           /* SYN|ACK */
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->state = TCP_ESTABLISHED;
            tcp_out(c, 0x10, 0, 0);             /* ACK */
        }
        return;
    }

    if (c->state == TCP_SYN_RCVD) {             /* ACK completing our SYN|ACK */
        if (flags & 0x10) {
            c->snd_una = ack;
            c->state = TCP_ESTABLISHED;         /* now visible to net_accept */
        } else return;
        /* fall through: this ACK may piggyback the peer's first data */
    }

    if (ack > c->snd_una) {                      /* our data/FIN acked */
        c->snd_una = ack;
        if (c->pend_len && c->snd_una >= c->snd_nxt) c->pend_len = 0;
    }

    if (dlen > 0) {
        u32 diff = c->rcv_nxt - seq;             /* 0 = in-order; (0,dlen) = overlap */
        if (diff < (u32)dlen) {
            int off  = (int)diff;
            int room = (int)sizeof c->rxq - c->rxq_len;
            int n    = dlen - off; if (n > room) n = room;
            memcpy(c->rxq + c->rxq_len, seg + hl + off, n);
            c->rxq_len += n;
            /* advance rcv_nxt only past bytes actually STORED (S-NET-15) */
            c->rcv_nxt += (u32)n;
            tcp_out(c, 0x10, 0, 0);              /* ACK what fits */
        } else if (diff >= (u32)dlen && diff < 0x40000000u) {
            tcp_out(c, 0x10, 0, 0);              /* stale duplicate: re-ACK */
        }
        /* else: a future segment - drop, the peer resends */
    }

    /* FIN counts only at exactly rcv_nxt (after all its data was accepted) */
    if ((flags & 0x01) && seq + (u32)dlen == c->rcv_nxt && c->state != TCP_DONE) {
        c->rcv_nxt++;
        tcp_out(c, 0x10, 0, 0);
        c->state = TCP_DONE;
    }
}

static void tcp_tick(void)
{
    int i;
    for (i = 0; i < NSOCK; i++) {
        sock_t *c = &sk[i];
        if (c->type != SOCK_TCP) continue;
        if ((c->state == TCP_SYN_SENT || c->state == TCP_SYN_RCVD || c->pend_len) &&
            (int)(g_ticks - c->retx_at) >= 0) {
            if (c->state == TCP_SYN_SENT) {
                c->snd_nxt = c->snd_una;
                tcp_out(c, 0x02, 0, 0); c->snd_nxt = c->snd_una + 1;
            } else if (c->state == TCP_SYN_RCVD) {
                c->snd_nxt = c->snd_una;
                tcp_out(c, 0x12, 0, 0); c->snd_nxt = c->snd_una + 1;
            } else {
                c->snd_nxt = c->snd_una;                 /* retransmit from the unacked start */
                tcp_out(c, 0x18, c->pend, c->pend_len);
                c->snd_nxt = c->snd_una + c->pend_len;
            }
            c->retx_at = g_ticks + 30;
        }
    }
}

/* ======================= DHCP =========================================== */
static int g_dhcp_state;          /* 0 idle, 1 discover sent, 2 request sent, 3 bound */
static u8  g_dhcp_xid[4] = {0x55, 0x4E, 0x4F, 0x21};
static u8  g_dhcp_srv[4], g_dhcp_offer[4];
static u32 g_dhcp_tx_tick;        /* g_ticks at the last DISCOVER/REQUEST send */

static void dhcp_send(int type, const u8 *reqip, const u8 *srvip)
{
    static const u8 bcast_ip[4] = {255,255,255,255};
    static const u8 zero_ip[4] = {0,0,0,0};
    u8 pkt[300];
    int i = 0, olen;
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 1; pkt[1] = 1; pkt[2] = 6;         /* BOOTREQUEST, ethernet, hlen 6 */
    memcpy(pkt + 4, g_dhcp_xid, 4);
    memcpy(pkt + 28, g_mac, 6);                 /* chaddr */
    wr32(pkt + 236, 0x63825363);                /* magic cookie */
    olen = 240;
    pkt[olen++] = 53; pkt[olen++] = 1; pkt[olen++] = (u8)type;   /* msg type */
    if (reqip) { pkt[olen++] = 50; pkt[olen++] = 4; memcpy(pkt+olen, reqip, 4); olen += 4; }
    if (srvip) { pkt[olen++] = 54; pkt[olen++] = 4; memcpy(pkt+olen, srvip, 4); olen += 4; }
    /* param request list: subnet, router, DNS. Option 6 was missing here -
     * RFC-compliant servers only return requested options, so real routers
     * omitted the DNS server and the resolver stayed at the SLIRP default
     * (Yoga metal run: ack len=300 opt3=1 opt6=0). */
    pkt[olen++] = 55; pkt[olen++] = 3; pkt[olen++] = 1; pkt[olen++] = 3; pkt[olen++] = 6;
    pkt[olen++] = 255;                          /* end */
    (void)zero_ip;
    /* UDP 68 -> 67, broadcast (source 0.0.0.0). Build a broadcast Ethernet
       frame directly since ARP can't resolve 255.255.255.255. */
    {
        static const u8 bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        u8 *ip = g_tx + 14, *udp;
        u8 zero[4] = {0,0,0,0};
        int total, flen;
        udp = ip + 20;
        wr16(udp + 0, 68); wr16(udp + 2, 67);
        wr16(udp + 4, (u16)(8 + olen)); wr16(udp + 6, 0);
        memcpy(udp + 8, pkt, olen);
        ip[0] = 0x45; ip[1] = 0; wr16(ip + 2, (u16)(28 + olen));
        wr16(ip + 4, g_ipid++); wr16(ip + 6, 0); ip[8] = 64; ip[9] = 17;
        wr16(ip + 10, 0); memcpy(ip + 12, zero, 4); memcpy(ip + 16, bcast_ip, 4);
        wr16(ip + 10, cksum(ip, 20, 0));
        total = 28 + olen;
        flen = eth_build(bcast_mac, 0x0800, total);
        nic_tx(flen);
        udp_bind(68);
    }
    g_dhcp_tx_tick = g_ticks;                   /* for the retransmit timer */
}

void net_dhcp_start(void)
{
    udp_bind(68);
    g_dhcp_state = 1;
    dhcp_send(1, 0, 0);            /* DISCOVER */
}
int net_dhcp_done(void) { return g_dhcp_state == 3; }

/* RFC-style retransmission, driven from net_poll. A single lost OFFER (state 1)
 * or ACK (state 2) - common on a marginal link or a dropped ax88179 RX frame -
 * otherwise stalls the whole lease forever, because the client only ever sent
 * one DISCOVER. Resend the current stage every ~1.5 s (net_poll is called at
 * ~200 Hz in the bring-up loops). This is the root-cause fix for the Yoga's
 * "tx=1 rx=1 ip=0 -> no lease": the OFFER was lost and never re-requested. */
static void dhcp_tick(void)
{
    if (g_dhcp_state != 1 && g_dhcp_state != 2) return;
    if ((u32)(g_ticks - g_dhcp_tx_tick) < 300) return;
    if (g_dhcp_state == 1) dhcp_send(1, 0, 0);                     /* re-DISCOVER */
    else                   dhcp_send(3, g_dhcp_offer, g_dhcp_srv); /* re-REQUEST  */
}

static const u8 *dhcp_opt(const u8 *o, int len, u8 want, int *olen)
{
    int i = 240;
    while (i + 1 < len) {
        u8 t = o[i], l;
        if (t == 255) break;
        if (t == 0) { i++; continue; }
        l = o[i + 1];
        if (t == want) { *olen = l; return o + i + 2; }
        i += 2 + l;
    }
    return 0;
}

void dhcp_input(const u8 *udp, int len)
{
    const u8 *boot = udp + 8;
    int blen = len - 8, ol;
    const u8 *mt, *srv;
    if (blen < 240 || boot[0] != 2) return;     /* BOOTREPLY */
    if (memcmp(boot + 4, g_dhcp_xid, 4)) return;
    mt = dhcp_opt(boot, blen, 53, &ol);
    if (!mt) return;
    if (mt[0] == 2 && g_dhcp_state == 1) {       /* OFFER */
        memcpy(g_dhcp_offer, boot + 16, 4);      /* yiaddr */
        srv = dhcp_opt(boot, blen, 54, &ol);
        if (srv) memcpy(g_dhcp_srv, srv, 4);
        g_dhcp_state = 2;
        dhcp_send(3, g_dhcp_offer, g_dhcp_srv);  /* REQUEST */
    } else if (mt[0] == 5 && g_dhcp_state == 2) { /* ACK */
        const u8 *rtr, *dns;
        memcpy(MYIP, boot + 16, 4);
        rtr = dhcp_opt(boot, blen, 3, &ol);  if (rtr && ol >= 4) memcpy(GW,  rtr, 4);
        dns = dhcp_opt(boot, blen, 6, &ol);  if (dns && ol >= 4) memcpy(DNS, dns, 4);
        /* diag: on real hardware the DNS came back as the SLIRP default 10.0.2.3,
         * i.e. option 6 wasn't applied. Record the ACK length + whether opts 3/6
         * were present so a metal run says "router omitted opt 6" vs "our RX
         * truncated the ACK before it" (a short blen = the AX88179 RX cut it). */
        g_dhcp_ack_len = blen;
        g_dhcp_had_dns = (dns && ol >= 4) ? 1 : 0;
        g_dhcp_had_rtr = (rtr) ? 1 : 0;
        /* no DNS option in the ACK -> the gateway is the best real-world
         * resolver guess (home routers run a forwarder); never keep the
         * SLIRP default on a real lease. */
        if (!g_dhcp_had_dns && g_dhcp_had_rtr) memcpy(DNS, GW, 4);
        g_dhcp_state = 3;
    }
}

/* ======================= dispatch ======================================= */
/* A destination we should accept: our unicast IP, the limited broadcast
 * 255.255.255.255, or a directed subnet broadcast (host bits all-ones on our
 * own network). Lets broadcast discovery/beacons reach a bound udp port. */
static int ip_is_bcast(const u8 d[4])
{
    int i, all = 1, sub = 1;
    for (i = 0; i < 4; i++) {
        if (d[i] != 255) all = 0;
        if ((u8)(d[i] | MASK[i]) != 0xFF) sub = 0;              /* a host bit is 0 */
        if ((d[i] & MASK[i]) != (MYIP[i] & MASK[i])) sub = 0;   /* different network */
    }
    return all || sub;
}

static u8 g_bcast[4];
const u8 *net_broadcast(void)
{
    int i;
    for (i = 0; i < 4; i++) g_bcast[i] = (u8)(MYIP[i] | ~MASK[i]);
    return g_bcast;
}

static void ip_recv(const u8 *ip, int len)
{
    int hl = (ip[0] & 0x0F) * 4;
    u8 proto = ip[9];
    int tot = rd16(ip + 2);         /* IP total length - attacker-controlled */
    int plen;
    if (hl < 20 || len < hl) return;
    /* clamp the claimed total length to what we actually received: a short
       frame whose header claims a huge length must never drive an L4 read past
       the capture buffer (remote OOB read / info-leak). */
    if (tot > len) tot = len;
    plen = tot - hl;
    if (plen < 0) return;
    if (!ip_eq(ip + 16, MYIP) && !ip_is_bcast(ip + 16)) {
        /* not for us (and not broadcast) - but accept during DHCP (no IP yet) */
        if (g_dhcp_state && g_dhcp_state < 3) { /* allow */ }
        else return;
    }
    if (proto == 1) icmp_recv(ip, ip + hl, plen);
    else if (proto == 17) udp_recv(ip, ip + hl, plen);
    else if (proto == 6)  tcp_recv(ip, ip + hl, plen);
}

void net_init(uno_nic_t *nic, const u8 mac[6])
{
    g_nic = nic;
    memcpy(g_mac, mac, 6);
    memset(arp_cache, 0, sizeof arp_cache);
    memset(udp_q, 0, sizeof udp_q);
    memset(sk, 0, sizeof sk);
    g_legacy = -1;
    g_dhcp_state = 0;
    g_ticks = 0;
    g_tx_frames = g_rx_frames = g_rx_arp = g_rx_ip = 0;
}

int net_link(void) { return g_nic ? g_nic->link(g_nic->ctx) : 0; }
const u8 *net_ip(void) { return MYIP; }
const u8 *net_gw(void) { return GW; }
const u8 *net_dns(void) { return DNS; }

/* ---- DNS (A-record resolver over UDP/53) --------------------------------- *
 * Synchronous + bounded: builds a query, then pumps net_poll while stalling a
 * few ms per turn until the answer arrives or the budget runs out. The browser
 * fetch is a discrete action, so blocking the UI for a beat is acceptable. */
void uno_pc64_delay_ms(int ms);          /* firmware Stall (from uefi_main) */

static int dns_name(u8 *o, const char *host)   /* encode "a.b.c" as labels */
{
    int n = 0; const char *p = host;
    while (*p) {
        const char *s = p; int len = 0;
        while (*p && *p != '.') { p++; len++; }
        if (len > 63) return -1;
        o[n++] = (u8)len;
        while (s < p) o[n++] = (u8)*s++;
        if (*p == '.') p++;
    }
    o[n++] = 0;                                /* root label */
    return n;
}

int net_dns_query(const char *host, u8 out[4])
{
    u8 q[300]; int qn = 0, ql, tries;
    const u16 sport = 5300;
    /* header: id, flags(RD), qd=1 */
    q[0]=0x51; q[1]=0x53; q[2]=0x01; q[3]=0x00;
    q[4]=0; q[5]=1; q[6]=0; q[7]=0; q[8]=0; q[9]=0; q[10]=0; q[11]=0;
    qn = 12;
    ql = dns_name(q + qn, host); if (ql < 0) return 0; qn += ql;
    q[qn++]=0; q[qn++]=1;                       /* QTYPE  A  */
    q[qn++]=0; q[qn++]=1;                       /* QCLASS IN */
    g_dns_sent = g_dns_rx = g_dns_badid = g_dns_neg = 0;
    for (tries = 0; tries < 4; tries++) {       /* (re)send, then wait */
        int waited;
        /* back off before a retry: a home-router forwarder that answered the
         * previous try negatively (cold cache, recursion still running
         * upstream) needs a beat before it can answer positively. */
        if (tries) uno_pc64_delay_ms(300);
        net_udp_send(DNS, 53, sport, q, qn);
        g_dns_sent++;
        for (waited = 0; waited < 120; waited++) {
            u8 r[512]; u8 src[4]; u16 sp; int n, i, qd, an;
            net_poll(); uno_pc64_delay_ms(8);
            n = net_udp_recv(sport, r, sizeof r, src, &sp);
            if (n < 12) continue;
            g_dns_rx++;
            if (r[0] != 0x51 || r[1] != 0x53) { g_dns_badid++; continue; }
                                                          /* S-NET-19: match our
                                                             transaction id, else
                                                             a stray/forged reply
                                                             is accepted as ours */
            qd = (r[4]<<8)|r[5]; an = (r[6]<<8)|r[7];
            /* a no-answer response (SERVFAIL/NXDOMAIN/empty) used to hard-fail
             * the WHOLE query here, killing the remaining retries. Cold-cache
             * forwarders commonly answer SERVFAIL instantly while upstream
             * recursion completes - break to the next try instead. (QEMU never
             * showed this: SLIRP proxies to the host's warm resolver.) */
            if (an < 1) { g_dns_neg++; break; }
            i = 12;
            for (; qd > 0; qd--) {               /* skip questions */
                while (i < n && r[i]) { if ((r[i]&0xC0)==0xC0){ i++; break; } i += r[i]+1; }
                i++; i += 4;                     /* null + QTYPE + QCLASS */
            }
            for (; an > 0 && i + 12 <= n; an--) { /* answers */
                u16 type, rdl;
                if ((r[i]&0xC0)==0xC0) i += 2;   /* compressed name */
                else { while (i < n && r[i]) i += r[i]+1; i++; }
                if (i + 10 > n) { an = 0; break; } /* name walk ran off the buffer */
                type = (r[i]<<8)|r[i+1];
                rdl  = (r[i+8]<<8)|r[i+9];
                i += 10;
                if (type == 1 && rdl == 4 && i + 4 <= n) { memcpy(out, r + i, 4); return 1; }
                i += rdl;
            }
            break;                               /* parsed but no A record: retry */
        }
    }
    return 0;
}

void net_poll(void)
{
    static u8 rx[FRM];
    int n;
    if (!g_nic) return;
    g_ticks++;
    while ((n = g_nic->recv(g_nic->ctx, rx, FRM)) > 0) {
        u16 type;
        if (n < 14) continue;
        g_rx_frames++;
        type = rd16(rx + 12);
        if (type == 0x0806) { g_rx_arp++; arp_recv(rx + 14, n - 14); }
        else if (type == 0x0800) { g_rx_ip++; ip_recv(rx + 14, n - 14); }
    }
    /* deferred ping once ARP resolves */
    if (g_ping_sent == 2 && g_ping_dst_set) net_ping(g_ping_dst);
    dhcp_tick();                    /* retransmit a lost DISCOVER/REQUEST */
    tcp_tick();
}
