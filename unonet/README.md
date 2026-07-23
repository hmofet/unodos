# unonet — networking + the HEADLESS server (Phase 13, §6/§10)

Networking is **absent by default** — a queried capability (`NET`). A per-link driver
publishes the `nic` service (`send`/`recv`/`link`) into the registry (§7); a
`HEADLESS+NET` build (zero display surfaces) is a **server by composition**, not new
mechanism. `sh unonet/build.sh` proves on host: a loopback nic is published and bound
through `unobus`, a packet round-trips `send`→`recv`, empty `recv` returns 0 (no UB),
and the server composition holds (nic bound, no framebuffer). Real links (DC BBA,
PS3 GigE, USB-Ethernet) are the hardware tail.

## Transport stack

Above the `uno_nic_t` seam sits the **transport stack** — Ethernet / ARP / IPv4 /
ICMP / UDP / TCP / DHCP / DNS, a multi-connection socket layer, broadcast, and
zero-config discovery. The pc64 implementation lives in `pc64/net.c/.h`,
`pc64/tls.*`, `pc64/netsock.h`, `pc64/netdisc.*` — see `pc64/NETSTACK.md`. It is a
**neutral shared system API** (whoever's task owns it edits it), consumed by http,
module load, tls, the browser/JS + AI apps, and unoautomate's remote channel.
Drivers below the seam belong to the driver agent; the transport stack above it is
`unonet`'s. (Was briefly owned by unoautomate; re-homed here 2026-07-22.)
