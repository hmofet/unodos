# pc64 network stack (NETSTACK.md)

The transport stack that sits above the NIC drivers — Ethernet / ARP / IPv4 /
ICMP / UDP / TCP / DHCP-client / DNS, plus a multi-connection **socket layer**,
**broadcast**, and **zero-config discovery**. This is the pc64 implementation of
the **`unonet`** system subsystem — a **neutral shared system API**, on the same
footing as `unofs` / `uno3d` / `unosound`. It is **not** owned by any single
feature agent: whoever's task needs to evolve the transport stack edits it, and
its many consumers (`unoauto_remote`, `pc64_http`, `pc64_modload`, tls, and the
roadmapped browser/JS + AI apps) build against its public headers. Re-homed out
of unoautomate on 2026-07-22 — see the note in `UNOAUTOMATE-REQUESTS.md`.

## Layers & ownership

```
  apps / unoauto_remote / netdisc / tls / http        <- consumers
  ────────────────────────────────────────────────
  netsock.h  socket table (net_socket/bind/listen/     \
             accept/connect/send/recv/sendto/...)        |  transport stack
  net.c      ARP · IPv4 · ICMP · UDP · TCP · DHCP · DNS   |  (unonet — shared)
  netdisc.c  UNODISC discovery over UDP broadcast        /
  ────────────────────────────────────────────────
  uno_nic_t  { send, recv, link }   <- THE SEAM         <- driver agent owns
  ────────────────────────────────────────────────
  iwlwifi · ax88179 · e1000 · igb · r8169 · ...         <- NIC drivers
```

The stack consumes only `g_nic->send/recv/link` (`uno_nic.h`). Everything above
that line is the shared `unonet` transport stack; the NIC drivers below it are
the driver agent's. This is the whole coexistence contract for networking.
unoautomate is one consumer among many (its `unoauto_remote` URC link dials out
over these sockets) — it does **not** own the layer.

## Sockets (`netsock.h`)

A fixed table of `NSOCK` (12) slots, each an `SOCK_TCP` connection or an
`SOCK_UDP` port. BSD-ish, non-blocking — drive with `net_poll()` and poll state.

| call | effect |
|------|--------|
| `net_socket(type)` | allocate a socket, returns id or -1 |
| `net_bind(s, port)` | set local port (UDP also opens its recv queue) |
| `net_listen(s)` | TCP passive open (accepts inbound SYNs) |
| `net_accept(s)` | pop a connected child from a listener, or -1 |
| `net_connect(s, ip, port)` | TCP active open (non-blocking) |
| `net_send/recv(s, ...)` | TCP stream I/O (one segment in flight) |
| `net_sendto/recvfrom(s, ...)` | UDP datagram I/O |
| `net_sendbcast(s, dport, ...)` | UDP to 255.255.255.255 |
| `net_sock_state/peer/port/count`, `net_sock_close` | introspect / tear down |

**TCP engine.** The per-connection state machine is the metal-tested single-
connection logic (partial-overlap accounting from S-NET-15, window
advertisement from S-NET-23, control/data retransmit) generalized to run on any
slot. RX demux is by full 4-tuple; a bare SYN to a `TCP_LISTEN` port spawns a
`TCP_SYN_RCVD` child that `net_accept()` hands back once established.

**Legacy compatibility.** The old single-connection `net_tcp_*` / `net_udp_*`
API (net.h) is preserved as thin wrappers over one reserved socket slot, so the
`.UNO` app ABI (kernel exports) and `tls.c` / `pc64_http.c` / `unoauto_remote.c`
are byte-for-byte compatible. New code should prefer the socket API.

## Broadcast (`net.h`)

- `net_udp_broadcast(dport, sport, data, len)` builds an Ethernet+IP+UDP frame
  with a broadcast dst MAC directly (ARP can't resolve a broadcast IP), the same
  technique the DHCP path uses; it binds `sport` so a unicast reply is queued.
- `net_udp_listen(port)` opens a receive-only port.
- `ip_recv` accepts the limited broadcast `255.255.255.255` **and** a directed
  subnet broadcast (host bits all-ones on our network); `net_broadcast()`
  returns the latter.

## Discovery (`netdisc.h`, UNODISC over UDP :5400)

Removes the static `remote=<ip>:<port>` requirement. Armed by a `discover`
STRESS.CFG flag (debug tier). Single ASCII datagrams, space-separated:

```
UNODISC 1 PROBE   <role> <name> <api>
UNODISC 1 OFFER   <role> <name> <api> <ip> <port>
UNODISC 1 GOTHOST <ip> <port>
```

`role` is `pc64` or `host`. pc64 broadcasts a PROBE until a host answers with an
OFFER carrying its URC ip:port; pc64 records it (`netdisc_host_ip/port`) and
acks GOTHOST. pc64 also answers inbound PROBEs with an OFFER describing itself,
so a host-side tool can enumerate the UnoDOS boxes on the LAN.

## Test harnesses

- `tools/netsock_qemu.py` — multi-connection + listen/accept over SLIRP +
  hostfwd, driven by the debug `nst` URC verb. Proves N live sockets, two
  outbound ESTABLISHED, and one accepted inbound, all beside the remote link.
- `tools/netdisc_qemu.py` — discovery over a **real L2 segment** (SLIRP never
  forwards broadcast): QEMU's `socket` netdev tunnels raw Ethernet to a host
  peer implementing ARP + a minimal DHCP server + UNODISC. Proves the guest
  leases, broadcasts a PROBE, records a host OFFER, and answers a host PROBE.
- `tools/remote_qemu.py` — the existing remote-channel gate, unchanged/green.

## Deferred: remote-channel integration

Wiring `unoauto_remote` to (a) auto-dial the discovered host when no `remote=`
key is set, and (b) run on its own socket / accept multiple inbound dev-PC
links (so the Browser/AI apps and the link coexist), is deferred until the
in-flight raw-disk-authoring edits to `unoauto_remote.c` land, to avoid
clobbering them. The socket + discovery primitives it needs are all in place.
