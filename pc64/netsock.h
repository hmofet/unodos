/* ===========================================================================
 * UnoDOS/pc64 - netsock: a multi-connection socket layer over net.c.
 *
 * The base stack (net.h) carries one TCP connection and a handful of UDP
 * ports - enough to dial out once. netsock adds a small BSD-ish socket table
 * so the system can hold MANY connections at once and, for TCP, ACCEPT inbound
 * ones (listen/accept) - i.e. be a server, not just a client. This is what lets
 * unoautomate serve several dev-PC links, and the discovery service run
 * alongside a live remote channel.
 *
 * It is implemented inside net.c (it shares the L2/L3 send path and the
 * metal-tested TCP engine), and declared here as the public surface. The
 * legacy net_tcp_* / net_udp_* API in net.h still works unchanged - those are
 * thin wrappers over a reserved socket, so tls.c / pc64_http / unoauto_remote
 * and the .UNO app ABI are untouched.
 *
 * All calls are non-blocking; drive progress with net_poll() and poll state.
 * IPs are u8[4]; ports are host-order u16.
 * ======================================================================== */
#ifndef PC64_NETSOCK_H
#define PC64_NETSOCK_H
#include "net.h"

enum { SOCK_NONE = 0, SOCK_TCP, SOCK_UDP };

/* How many sockets exist at once (listeners + connections + UDP ports). */
#ifndef NSOCK
#define NSOCK 12
#endif

/* Create a socket of SOCK_TCP or SOCK_UDP. Returns a socket id [0,NSOCK) or -1
 * if the table is full / the type is bad. A fresh socket is unbound + closed. */
int  net_socket(int type);

/* Bind a local port (0 = leave ephemeral, chosen at connect). For UDP this also
 * opens the receive queue for that port (like net_udp_listen). Returns 0/-1. */
int  net_bind(int s, unsigned short port);

/* TCP passive open: mark a bound socket as a listener. Inbound SYNs to its port
 * become child sockets delivered by net_accept(). Returns 0/-1. */
int  net_listen(int s);

/* TCP: return a newly-ESTABLISHED child socket id from a listener's backlog,
 * or -1 if none is pending. The child is an independent connected socket. */
int  net_accept(int s);

/* TCP active open to dst:dport (non-blocking). Poll net_sock_state() until it
 * reads TCP_ESTABLISHED (or TCP_DONE/CLOSED on failure). Returns 0/-1. */
int  net_connect(int s, const unsigned char dst[4], unsigned short dport);

/* TCP connection state (one of net.h's TCP_* enums, plus TCP_LISTEN /
 * TCP_SYN_RCVD). For a UDP socket: 1 if bound, else 0. */
int  net_sock_state(int s);

/* Stream I/O (TCP). send returns bytes queued (one segment in flight at a time,
 * -1 if busy/closed); recv returns bytes copied (0 if none). */
int  net_send(int s, const void *data, int len);
int  net_recv(int s, void *buf, int cap);

/* Datagram I/O (UDP). sendto uses the socket's bound port as the source port;
 * recvfrom fills src/sport of the sender (either may be NULL). */
int  net_sendto(int s, const unsigned char dst[4], unsigned short dport,
                const void *data, int len);
int  net_recvfrom(int s, void *buf, int cap,
                  unsigned char src[4], unsigned short *sport);
/* UDP broadcast (255.255.255.255) from the socket's bound source port. */
int  net_sendbcast(int s, unsigned short dport, const void *data, int len);

/* The connected peer of a TCP socket (e.g. one returned by net_accept). Fills
 * ip/port (either may be NULL). Returns 0/-1. */
int  net_sock_peer(int s, unsigned char ip[4], unsigned short *port);

/* Local bound port of a socket (0 if unbound). */
unsigned short net_sock_port(int s);

/* Close a socket: TCP sends FIN and tears down; UDP just frees the slot. */
void net_sock_close(int s);

/* Count of sockets currently in use (diagnostic / probe). */
int  net_sock_count(void);

#endif
