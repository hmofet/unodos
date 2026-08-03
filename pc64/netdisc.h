/* ===========================================================================
 * UnoDOS/pc64 - netdisc: zero-config LAN discovery over UDP broadcast.
 *
 * The remote channel (REMOTE.md) normally learns the dev PC's address from a
 * static DEBUG.CFG `remote=<ip>:<port>` key. netdisc removes that: pc64
 * BROADCASTs a discovery PROBE on the LAN; a listening dev PC replies with an
 * OFFER carrying its URC listener ip:port; pc64 records it (and can then dial
 * it with no prior configuration). It also ANSWERS inbound PROBEs with an OFFER
 * describing itself, so a host-side tool can enumerate the UnoDOS boxes present.
 *
 * Wire protocol (UDP :5400, single ASCII datagram, space-separated):
 *   UNODISC 1 PROBE   <role> <name> <api>
 *   UNODISC 1 OFFER   <role> <name> <api> <ip> <port>
 *   UNODISC 1 GOTHOST <ip> <port>                 (pc64's ack of a host OFFER)
 * role is "pc64" or "host". Built on the M1 broadcast + M2 socket primitives.
 *
 * Ships in production, like the remote channel it serves. A debug build arms it
 * from the DEBUG.CFG `discover`/`listen` flags; a production build only ever
 * arms it as a side effect of arming the channel (unoauto_gate.h), so a machine
 * nobody armed neither advertises itself nor answers a scan.
 *
 * LISTEN mode. When the URC channel runs as a SERVER (the `listen` DEBUG.CFG
 * key - the host dials INTO the box instead of the box dialing out), the box
 * calls netdisc_listen(port) to arm netdisc as a RESPONDER ONLY: it does not
 * broadcast PROBEs (it is not looking for a host), it just answers a host's
 * PROBE with `OFFER pc64 <name> <api> <ip> <port>` carrying its URC listen port,
 * so a scanning client can enumerate the boxes it can dial in to. (In the
 * dial-out `discover` path the self-OFFER advertises port 0 - nothing to dial.)
 * ======================================================================== */
#ifndef PC64_NETDISC_H
#define PC64_NETDISC_H
#include "net.h"

void netdisc_boot(void);              /* arm from the DEBUG.CFG `discover` flag */
void netdisc_listen(unsigned short port);  /* arm responder-only, advertise a URC listen port */
void netdisc_tick(void);              /* pump: probe + service inbound; each frame */
int  netdisc_active(void);            /* 1 if armed */
int  netdisc_have_host(void);         /* 1 once a host OFFER has been recorded */
const u8 *netdisc_host_ip(void);      /* discovered host URC IP (u8[4]) */
unsigned short netdisc_host_port(void);   /* discovered host URC port */

#endif /* PC64_NETDISC_H */
