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
    g_nic->send(g_nic->ctx, g_tx, flen);
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
static const u8 *g_ping_dst;

void net_ping(const u8 ip[4])
{
    u8 icmp[8];
    int flen;
    g_ping_dst = ip;                   /* remembered for the post-ARP retry */
    icmp[0] = 8; icmp[1] = 0;          /* echo request */
    wr16(icmp + 2, 0);
    wr16(icmp + 4, 0x4944);            /* id */
    wr16(icmp + 6, ++g_ping_seq);
    wr16(icmp + 2, cksum(icmp, 8, 0));
    flen = ip_build(ip, 1, icmp, 8);
    if (flen > 0) {
        g_nic->send(g_nic->ctx, g_tx, flen);
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
          if (flen > 0) g_nic->send(g_nic->ctx, g_tx, flen); }
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
    return g_nic->send(g_nic->ctx, g_tx, flen);
}

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

/* ======================= TCP (one connection) =========================== */
static struct {
    int   state;
    u8    dst[4];
    u16   dport, sport;
    u32   snd_nxt, snd_una, rcv_nxt;
    int   retx_at;               /* tick to retransmit the unacked control/data */
    u8    pend[512]; int pend_len;   /* the single outstanding data segment */
    u8    rxq[2048]; int rxq_len;
} tcp;

static void tcp_out(u8 flags, const u8 *data, int dlen)
{
    u8 seg[20 + 512];
    int flen;
    u32 seed;
    memset(seg, 0, 20);
    wr16(seg + 0, tcp.sport); wr16(seg + 2, tcp.dport);
    wr32(seg + 4, tcp.snd_nxt);
    wr32(seg + 8, tcp.rcv_nxt);
    seg[12] = 0x50;              /* data offset 5 words */
    seg[13] = flags;
    wr16(seg + 14, 4096);       /* window */
    wr16(seg + 16, 0);          /* checksum (filled below) */
    if (dlen > 0) memcpy(seg + 20, data, dlen);
    seed = pseudo_seed(MYIP, tcp.dst, 6, 20 + dlen);
    wr16(seg + 16, cksum(seg, 20 + dlen, seed));
    flen = ip_build(tcp.dst, 6, seg, 20 + dlen);
    if (flen > 0) g_nic->send(g_nic->ctx, g_tx, flen);
}

int net_tcp_connect(const u8 dst[4], u16 dport)
{
    memcpy(tcp.dst, dst, 4);
    tcp.dport = dport;
    tcp.sport = (u16)(40000 + (g_ticks & 0x3FF));
    tcp.snd_nxt = 0x1000;
    tcp.snd_una = tcp.snd_nxt;
    tcp.rcv_nxt = 0;
    tcp.state = TCP_SYN_SENT;
    tcp.pend_len = 0; tcp.rxq_len = 0;
    tcp_out(0x02, 0, 0);        /* SYN */
    tcp.snd_nxt++;              /* SYN consumes a sequence number */
    tcp.retx_at = g_ticks + 30;
    return 0;
}

int net_tcp_state(void) { return tcp.state; }

int net_tcp_send(const void *data, int len)
{
    if (tcp.state != TCP_ESTABLISHED || tcp.pend_len) return -1;   /* one in flight */
    if (len > 512) len = 512;
    memcpy(tcp.pend, data, len);
    tcp.pend_len = len;
    tcp_out(0x18, tcp.pend, len);      /* PSH|ACK */
    tcp.snd_nxt += len;
    tcp.retx_at = g_ticks + 30;
    return len;
}

int net_tcp_recv(void *buf, int cap)
{
    int n = tcp.rxq_len;
    if (n <= 0) return 0;
    if (n > cap) n = cap;
    memcpy(buf, tcp.rxq, n);
    if (n < tcp.rxq_len) {
        memmove(tcp.rxq, tcp.rxq + n, tcp.rxq_len - n);
        tcp.rxq_len -= n;
    } else tcp.rxq_len = 0;
    return n;
}

void net_tcp_close(void)
{
    if (tcp.state == TCP_ESTABLISHED) {
        tcp_out(0x11, 0, 0);           /* FIN|ACK */
        tcp.snd_nxt++;
        tcp.state = TCP_FIN_WAIT;
    }
}

static void tcp_recv(const u8 *ip, const u8 *seg, int len)
{
    u8 flags;
    u32 seq, ack;
    int hl, dlen;
    if (tcp.state == TCP_CLOSED) return;
    if (rd16(seg + 0) != tcp.dport || rd16(seg + 2) != tcp.sport) return;
    flags = seg[13];
    seq = rd32(seg + 4);
    ack = rd32(seg + 8);
    hl = (seg[12] >> 4) * 4;
    dlen = len - hl;

    if (flags & 0x04) { tcp.state = TCP_DONE; return; }   /* RST */

    if (tcp.state == TCP_SYN_SENT) {
        if ((flags & 0x12) == 0x12) {           /* SYN|ACK */
            tcp.rcv_nxt = seq + 1;
            tcp.snd_una = ack;
            tcp.state = TCP_ESTABLISHED;
            tcp_out(0x10, 0, 0);                /* ACK */
        }
        return;
    }

    if (ack > tcp.snd_una) {                     /* our data/FIN acked */
        tcp.snd_una = ack;
        if (tcp.pend_len && tcp.snd_una >= tcp.snd_nxt) tcp.pend_len = 0;
    }

    if (dlen > 0 && seq == tcp.rcv_nxt) {        /* in-order data */
        int room = (int)sizeof tcp.rxq - tcp.rxq_len;
        int n = dlen; if (n > room) n = room;
        memcpy(tcp.rxq + tcp.rxq_len, seg + hl, n);
        tcp.rxq_len += n;
        tcp.rcv_nxt += dlen;
        tcp_out(0x10, 0, 0);                     /* ACK the data */
    }

    if (flags & 0x01) {                          /* FIN */
        tcp.rcv_nxt++;
        tcp_out(0x10, 0, 0);
        tcp.state = TCP_DONE;
    }
}

static void tcp_tick(void)
{
    if ((tcp.state == TCP_SYN_SENT || tcp.pend_len) &&
        (int)(g_ticks - tcp.retx_at) >= 0) {
        if (tcp.state == TCP_SYN_SENT) {
            tcp.snd_nxt = tcp.snd_una;
            tcp_out(0x02, 0, 0); tcp.snd_nxt++;
        } else {
            tcp.snd_nxt = tcp.snd_una + tcp.pend_len;    /* keep seq consistent */
            tcp.snd_nxt -= tcp.pend_len;
            tcp_out(0x18, tcp.pend, tcp.pend_len);
            tcp.snd_nxt += tcp.pend_len;
        }
        tcp.retx_at = g_ticks + 30;
    }
}

/* ======================= DHCP =========================================== */
static int g_dhcp_state;          /* 0 idle, 1 discover sent, 2 request sent, 3 bound */
static u8  g_dhcp_xid[4] = {0x55, 0x4E, 0x4F, 0x21};
static u8  g_dhcp_srv[4], g_dhcp_offer[4];

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
    pkt[olen++] = 55; pkt[olen++] = 2; pkt[olen++] = 1; pkt[olen++] = 3;  /* params */
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
        g_nic->send(g_nic->ctx, g_tx, flen);
        udp_bind(68);
    }
}

void net_dhcp_start(void)
{
    udp_bind(68);
    g_dhcp_state = 1;
    dhcp_send(1, 0, 0);            /* DISCOVER */
}
int net_dhcp_done(void) { return g_dhcp_state == 3; }

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
        g_dhcp_state = 3;
    }
}

/* ======================= dispatch ======================================= */
static void ip_recv(const u8 *ip, int len)
{
    int hl = (ip[0] & 0x0F) * 4;
    u8 proto = ip[9];
    int plen = rd16(ip + 2) - hl;
    if (len < hl || plen < 0) return;
    if (!ip_eq(ip + 16, MYIP) && !(ip[16]==255&&ip[17]==255)) {
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
    memset(&tcp, 0, sizeof tcp);
    g_dhcp_state = 0;
    g_ticks = 0;
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
    for (tries = 0; tries < 4; tries++) {       /* (re)send, then wait */
        int waited;
        net_udp_send(DNS, 53, sport, q, qn);
        for (waited = 0; waited < 120; waited++) {
            u8 r[512]; u8 src[4]; u16 sp; int n, i, qd, an;
            net_poll(); uno_pc64_delay_ms(8);
            n = net_udp_recv(sport, r, sizeof r, src, &sp);
            if (n < 12) continue;
            qd = (r[4]<<8)|r[5]; an = (r[6]<<8)|r[7];
            if (an < 1) return 0;
            i = 12;
            for (; qd > 0; qd--) {               /* skip questions */
                while (i < n && r[i]) { if ((r[i]&0xC0)==0xC0){ i++; break; } i += r[i]+1; }
                i++; i += 4;                     /* null + QTYPE + QCLASS */
            }
            for (; an > 0 && i + 12 <= n; an--) { /* answers */
                u16 type, rdl;
                if ((r[i]&0xC0)==0xC0) i += 2;   /* compressed name */
                else { while (i < n && r[i]) i += r[i]+1; i++; }
                type = (r[i]<<8)|r[i+1];
                rdl  = (r[i+8]<<8)|r[i+9];
                i += 10;
                if (type == 1 && rdl == 4 && i + 4 <= n) { memcpy(out, r + i, 4); return 1; }
                i += rdl;
            }
            return 0;
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
        type = rd16(rx + 12);
        if (type == 0x0806) arp_recv(rx + 14, n - 14);
        else if (type == 0x0800) ip_recv(rx + 14, n - 14);
    }
    /* deferred ping once ARP resolves */
    if (g_ping_sent == 2 && g_ping_dst) net_ping(g_ping_dst);
    tcp_tick();
}
