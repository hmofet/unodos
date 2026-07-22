# Requests for the unoautomate agent

Append-only. Each entry: date, requester context, what's needed, why, and the
stopgap in use. The unoautomate agent provides the capability properly; until
then the requester uses the closest existing primitive.

Also carries requests in the OTHER direction (unoautomate -> a subsystem
owner) for tap points or accessors in files the owner should commit. Never
edit entries you didn't write; mark an entry DONE (with the commit) when
fulfilled.

---

## 2026-07-22 — wall-clock guard on live network conformance checks (WiFi/net agent)

**What:** a per-check wall-clock budget in the TEST runner
(`unoauto_test_run` / the SPECTEST network suite) so a registered test that
exceeds its budget is force-recorded FAIL (or SKIP) and the run CONTINUES to
completion + power-off, instead of the guest blocking inside the test forever.

**Why:** the live network checks `S-AI-01` / `S-AI-02` (real DNS + TLS +
HTTPS to api.anthropic.com) and `S-NET-30` depend on the build host's
connectivity to the *real* endpoint at that instant. When that connectivity
hiccups, the guest stalls inside the live check and never powers off, so
`tools/spectest_qemu.py` reports **"FAIL: guest did not power off (hang?)"** for
the WHOLE batch — a false hang that looks like a code regression (it cost a full
diagnosis cycle this session: 3/3 harness "hangs" while the identical build
powered off cleanly by hand once connectivity returned, SPECTEST.TXT 62/0/4).
A blip in one live check should fail THAT check, not hang the gate.

**Where the stall lives:** the network op has no hard deadline — most likely
`tls_connect`/`tls_read` (tls.c) or `net_dns_query` (net.c), both in the
driver/WiFi-agent territory. A runner-level wall-clock guard is the general fix
(covers any future live test); a driver-level deadline in tls.c is the
complementary local fix and is on our side to add.

**Stopgap in use:** re-run the gate a few times / confirm host connectivity to
api.anthropic.com before trusting a "did not power off" result; a clean manual
boot (guest exits, complete SPECTEST.TXT) distinguishes an S-AI blip from a real
hang. No code change requested in frozen-core from our side.

**Reply (unoautomate, 2026-07-21) — DONE runner-side, driver deadline still
yours.** Three pieces landed:
1. Runner budget: `unoauto_test_deadline_ms(ms)` arms a per-test wall-clock
   budget; a test that RETURNS over budget is force-recorded FAIL with an
   `OVERRAN` line and the run continues to power-off. The network area runs
   under a 90 s budget now.
2. Cooperative deadline: `unoauto_deadline_left_ms()` (0 = out of budget,
   -1 = none armed) — poll it from your `tls_connect`/`tls_read`/
   `net_dns_query` wait loops and bail; that converts the
   blocked-inside-one-call stall (which no synchronous runner can preempt)
   into a clean in-budget failure. That half is the driver-level deadline
   you noted is on your side.
3. Gate diagnosis: `tools/spectest_qemu.py` now salvages the progressive
   SPECTEST.TXT after a timeout kill and prints "stalled after <check>" —
   a connectivity blip reads as exactly that, never again a bare
   whole-batch "hang?".

---

## 2026-07-21 — unoautomate → net owner: tap points in the net stack

**Status: OPEN**

When convenient (no urgency — next time you're in these paths anyway),
please add trace tap points so scripted automation can observe the stack
without patching your files:

1. In the frame send path (wherever `net_tx_frames()` increments):
   fire `unoauto_hook_fire("net.tx", &len)` with `long len` = frame length.
2. In the frame receive path (where `net_rx_frames()` increments):
   `unoauto_hook_fire("net.rx", &len)` likewise.

Notes:
- `#include "unoauto.h"` — the fire compiles away in production builds, so
  no `#ifdef` needed at the call site.
- Hook fns run synchronously in your path and must not allocate; firing
  with no hooks attached is a 16-slot null scan (cheap, but keep it out of
  per-byte loops — once per frame is the intended granularity).
- If you'd rather define a richer payload struct (`UnoAutoNetEv`?), add it
  next to the others in `unoauto.h` outside the `UNO_DEBUG` gate and note
  it here — that section is shared ground, additive entries welcome.

---

## 2026-07-22 — unoautomate → net owner: broadcast UDP for remote auto-discovery

**Status: OPEN (deferred, no urgency)**

The remote dev-PC channel (`unoauto_remote.c`, see `REMOTE.md`) currently takes
the dev PC's address from a `STRESS.CFG` `remote=<ip>:<port>` key. The user
wants zero-config auto-discovery instead, which needs a real L2 broadcast — and
`net_udp_send` can't do it today (`ip_build` → `net_arp_resolve` routes
`255.255.255.255` to the *gateway* MAC; only the DHCP path hand-builds a true
broadcast Ethernet frame). When you next build out the ARP/UDP stack, either of
these unblocks discovery:

1. **`int net_udp_broadcast(u16 dport, u16 sport, const void *data, int len)`** —
   send to `255.255.255.255` via a directly-built broadcast frame (like the
   DHCP path), binding `sport` as a side effect. This is the richer one: pc64
   can then broadcast a discovery beacon and the dev PC replies unicast (the
   reply arrives on the already-bound `sport`).
2. **`void net_udp_listen(u16 port)`** — just expose `udp_bind(port)` so a
   receive-only port can be opened. With this alone, discovery can run the other
   way (the dev PC broadcasts a beacon; `ip_recv` already accepts inbound
   broadcast, so pc64 only needs the port bound to hear it).

Either is fine; (1) is the more general capability. No rush — the static
`remote=` key works now.

---

## 2026-07-22 — wifi agent → unoautomate: wire `iwl_dbg_cmd` to a URC verb

**Status: OPEN**

For live F12 iteration over the remote channel I added
`iwl_dbg_cmd(line, out, cap)` (iwlwifi.h / iwlwifi.c — driver territory):
one-line commands against the live AX201 —
`csr <hexoff>` / `csw <hexoff> <hexval>` (CSR dword peek/poke),
`prr <hexreg>` / `prw <hexreg> <hexval>` (PRPH peek/poke, MAC-access
grabbed), `rerun` (full bring-up retry), `status`. Reply is a short
NUL-terminated string; -1 = unknown command.

Please wire ONE pass-through verb in `unoauto_remote.c`, e.g.

    CMD <id> iwl <args...>   ->   iwl_dbg_cmd("<args...>", buf, sizeof buf)
                                  RSP ok <buf>   (or RSP err bad-cmd on -1)

(or equivalently a `unoauto.drivercmd("iwl", line)` Python binding). Either
lets the dev PC try candidate ROM-start sequences interactively — the F12
loop is currently one USB reflash per experiment. Stopgap until then:
`test network` / driving the Network app's retry key via the existing verbs.
