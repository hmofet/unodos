# unonet — networking + the HEADLESS server (Phase 13, §6/§10)

Networking is **absent by default** — a queried capability (`NET`). A per-link driver
publishes the `nic` service (`send`/`recv`/`link`) into the registry (§7); a
`HEADLESS+NET` build (zero display surfaces) is a **server by composition**, not new
mechanism. `sh unonet/build.sh` proves on host: a loopback nic is published and bound
through `unobus`, a packet round-trips `send`→`recv`, empty `recv` returns 0 (no UB),
and the server composition holds (nic bound, no framebuffer). Real links (DC BBA,
PS3 GigE, USB-Ethernet) are the hardware tail.
