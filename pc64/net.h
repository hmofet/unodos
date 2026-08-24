/* ===========================================================================
 * UnoDOS/pc64 - a compact TCP/IP stack over the uno_nic_t link service.
 *
 * Ethernet / ARP / IPv4 / ICMP-echo / UDP / a minimal single-connection TCP.
 * "Basic" by intent (no fragmentation, one TCP connection, a handful of UDP
 * ports, no TLS) - enough to ARP, ping, DHCP, resolve DNS, and run a TCP or
 * UDP echo. Big-endian on the wire; IPs are u8[4] throughout.
 * ======================================================================== */
#ifndef PC64_NET_H
#define PC64_NET_H
#include "uno_nic.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

enum { TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT, TCP_DONE,
       /* appended for the netsock multi-connection layer (netsock.h): a passive
        * server socket, and a half-open connection accepted from one. */
       TCP_LISTEN, TCP_SYN_RCVD };

void net_init(uno_nic_t *nic, const u8 mac[6]);

/* The link net_init was given, or NULL.  Added for the appliance bridge
 * (unovirt M3), which puts a guest's frames on the same wire; every other
 * consumer wants the calls above instead. */
uno_nic_t *net_nic(void);
void net_poll(void);                 /* pump once; call often */
int  net_link(void);
const u8 *net_ip(void);              /* our current IPv4 (u8[4]) */
const u8 *net_gw(void);              /* gateway (static or DHCP opt 3)  */
const u8 *net_dns(void);             /* resolver (static or DHCP opt 6) */
int  net_dhcp_ack_len(void);         /* last DHCP ACK option-area length (diag) */
int  net_dhcp_had_dns(void);         /* 1 if the ACK carried a usable opt 6 (DNS) */
int  net_dhcp_had_rtr(void);         /* 1 if the ACK carried opt 3 (router) */

/* DNS: resolve an A record. 1 = out[4] set, 0 = failed/timeout. Synchronous
 * (pumps net_poll internally); call after the link + DHCP are up. */
int  net_dns_query(const char *host, u8 out[4]);   /* BLOCKS - see below */

/* The same lookup, non-blocking, for a caller with a frame to draw.
 *
 * net_dns_begin:  1 = answered from the cache, `out` is filled and nothing is
 *                     in flight;  0 = started, poll it;  -1 = unusable name;
 *                    -2 = another lookup is already in flight (one at a time).
 * net_dns_poll:   0 = still going;  1 = resolved into `out`;  -1 = gave up.
 *                 Does NOT pump the stack - call net_poll() yourself, exactly
 *                 as with tls_poll, so one loop can drive everything.
 * net_dns_abort:  drop an in-flight lookup (a cancelled request).
 *
 * net_dns_query is these plus a wait, so there is one implementation. */
int  net_dns_begin(const char *host, u8 out[4]);
int  net_dns_poll(u8 out[4]);
void net_dns_abort(void);
int  net_dns_sent(void);             /* last-query diag: queries transmitted */
int  net_dns_rx(void);               /* last-query diag: datagrams reaching our port */
int  net_dns_badid(void);            /* last-query diag: txid rejects */
int  net_dns_neg(void);              /* last-query diag: negative (an=0) responses */

/* ARP */
int  net_arp_resolve(const u8 ip[4], u8 mac_out[6]);   /* 1=known (else kicks req) */

/* ICMP echo */
void net_ping(const u8 ip[4]);
int  net_ping_replied(void);         /* 1 once a matching reply arrived */
int  net_ping_ms(void);              /* poll-count round trip of the last ping */

/* UDP */
int  net_udp_send(const u8 dst[4], u16 dport, u16 sport,
                  const void *data, int len);
int  net_udp_recv(u16 sport, void *buf, int cap, u8 src[4], u16 *src_port);
/* Send a datagram to the limited broadcast address 255.255.255.255 via a
 * directly-built broadcast Ethernet frame (ARP can't resolve a broadcast IP).
 * Binds `sport` as a side effect so a unicast reply is queued for recv. */
int  net_udp_broadcast(u16 dport, u16 sport, const void *data, int len);
/* Open a receive-only UDP port (bind without sending). Inbound datagrams to
 * `port` - including broadcast - are then queued for net_udp_recv(port,...). */
void net_udp_listen(u16 port);
/* Our directed subnet broadcast address (network | ~mask), u8[4]. Some links
 * prefer this over 255.255.255.255; discovery uses the limited form by default. */
const u8 *net_broadcast(void);

/* TCP (one active connection) */
int  net_tcp_connect(const u8 dst[4], u16 dport);
int  net_tcp_state(void);
int  net_tcp_send(const void *data, int len);
int  net_tcp_recv(void *buf, int cap);
void net_tcp_close(void);

/* DHCP (optional; static config works without it) */
void net_dhcp_start(void);
int  net_dhcp_done(void);            /* 1 once a lease is bound */

/* Link-level frame counters since the last net_init - lets the net test say
 * which side of a DHCP failure is at fault (did we transmit? did anything
 * arrive? were any of them IP?). */
u32  net_tx_frames(void);
u32  net_rx_frames(void);
u32  net_rx_arp(void);
u32  net_rx_ip(void);

/* Negotiated link speed in Mbps (0 = unknown / not reported by the driver).
 * A NIC driver publishes it from the PHY via net_set_link_speed_mbps(); the
 * shell's LAN-chip tooltip reads it. */
int  net_link_speed_mbps(void);
void net_set_link_speed_mbps(int mbps);

/* Promiscuous reception, for the appliance bridge only (unovdev_net.c): the
 * guest keeps its own MAC, so the link has to accept a frame that is not
 * addressed to us. On while a guest runs, off the rest of the time - it is
 * not free on a slow machine. A NIC driver that can do it registers its own
 * setter; one that cannot never calls net_set_promisc_fn and net_set_promisc
 * is then a no-op. */
void net_set_promisc_fn(void (*fn)(int on));
void net_set_promisc(int on);
int  net_promisc(void);

#endif
