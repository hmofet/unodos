# unonet, roadmap

Plan for the `unonet` seam and for the networking improvements the target
mission needs. **Read the ownership note first, it changes who does what.**

## Ownership (per the networking contract, not negotiable here)

> **Correction (2026-07-28).** The "unoautomate owns the transport stack"
> handoff this section was written against was **superseded the same day** by
> the RE-HOME entry at the top of `pc64/UNOAUTOMATE-REQUESTS.md`: `unonet`
> (net.c / tls.* / netsock / netdisc) and `unostorage` are **neutral shared
> subsystems**: whoever's task owns them edits them, no single feature agent
> owns them (AGENTS.md §1). So items 1-3 below are not "requests to
> unoautomate"; they are unonet-lane work for whoever holds the lane. The
> seam split (transport above `uno_nic_t`, drivers below) is unchanged and
> still correct. Item 4 is settled by the same entry: the files stay in
> `pc64/`, under shared ownership, and `unonet/` remains the seam home.

`pc64/NETSTACK.md` + the 2026-07-22 handoff in `pc64/UNOAUTOMATE-REQUESTS.md`
set the whole coexistence contract for networking:

```
  apps / unoauto_remote / netdisc / tls / http      <- consumers
  ──────────────────────────────────────────────
  netsock.h / net.c / netdisc.c / tls.*             <- TRANSPORT STACK
                                                       (unoautomate owns)
  ──────────────────────────────────────────────
  uno_nic_t { send, recv, link }   <- THE SEAM      <- driver / L2 side
  ──────────────────────────────────────────────
  iwlwifi · ax88179 · e1000 · r8169 · ...           <- NIC drivers
```

- **`unonet/` is the seam's home** (`uno_nic.h`, host loopback, build-proof
  `unonet_test.c`), the driver/L2 side of the line. That is what this roadmap
  owns and may edit freely.
- **The transport stack, `net.c/.h`, `tls.*`, `netsock.h`, `netdisc.*`: is
  unoautomate's.** Per policy §1/§2, changes there are **requests filed in
  `pc64/UNOAUTOMATE-REQUESTS.md`**, not edits made from here.

The earlier draft of this file assumed unonet would absorb the transport stack.
That premise contradicts the contract above and has been dropped, see item 4.

## Target mission (decided)

**LAN workstation + LAN server-by-composition.** Trusted, low-loss links. Not a
general Internet-facing stack. This scoping removes most WAN-grade TCP depth -
see "Explicitly out of scope."

## What's already good enough, do not spend here

- **TCP core**: active/passive open, `TCP_LISTEN`/accept, retransmit, window
  advertisement, S-NET-15 overlap accounting. Sufficient on a clean LAN.
- **TLS**: BearSSL, real crypto: CA-chain validation (`tls_connect_ca`) +
  pinned-key mode (`tls_connect`), SNI. The RNG feeding it was the weak spot; item 1 closed it.
- **Discovery + remote channel**: `netdisc` zero-config + the URC link, both
  QEMU-verified. A NIC-independent UART transport is requested (r8169 Request 2,
  OPEN), the right robustness move for a box you can't reach over the NIC.
- **The seam**: `net_init(uno_nic_t *nic, ...)` runs the whole stack over the
  `uno_nic_t` contract; every driver publishes one. Stack is already
  port-independent by construction.

## The work, 4 items, correctly attributed

### 1. RNG / entropy  (security, highest value), **DONE 2026-07-28**

Both asks landed, in the unonet lane. The collector moved into its own file
`pc64/tls_entropy.{c,h}` so the health test is testable without QEMU:

- **(a) fail closed**: `tls_connect`/`tls_connect_ca` now check the source
  *before opening a socket* and return `TLS_ENOENTROPY` when none qualifies.
  Nothing is ever injected from a seed we cannot defend; the TSC-LCG is gone.
- **(b) a real source for RDRAND-less boxes**: CPU timing jitter: rdtsc
  deltas of a data-dependent walk over a 64 KB (larger-than-L1) working set,
  conditioned through BearSSL's SHA-256, credited at 1 bit/sample. It counts
  only after an online health test (repetition run, change rate, distinct
  low-bit values), so a frozen / step-locked / too-coarse clock yields NO
  source rather than a repeatable seed. RDRAND is likewise only trusted after
  an *actual* success, not merely a CPUID bit.
- The seam-side offer (NIC/IRQ inter-arrival timing) was **not** needed: the
  frame counters are folded in as uncredited diversity, so there is no new
  `uno_nic` surface to maintain. That offer can stay closed.
- Gates: `tools/tls_entropy_test.sh` (6 synthetic-CPU scenarios incl. the
  three dead-clock cases) + SPECTEST **S-TLS-10/11**. Contract: SPEC.md
  S-TLS-06/10/11.

<details><summary>original item</summary>

`tls.c` seeds BearSSL from a TSC-mix the code itself flags **"NOT
cryptographically strong"** (`pc64/tls.c:65`) whenever `RDRAND` is absent, and it
proceeds fail-open: `get_entropy()` injects the seed via
`br_ssl_engine_inject_entropy` regardless of `g_rdrand` (`tls.c:172`,
`tls.c:222`). Any RDRAND-less box (incl. retro/ARM ports) silently gets weak TLS
keys. `tls.c` is unoautomate's file → **filed as a request** (2026-07-22) asking
for: (a) fail-closed when no real entropy source is present, and (b) a real
per-platform source for RDRAND-less targets. `tls_have_rdrand()` (`tls.c:143`)
already exists for the introspection half.

*Possible seam-side contribution (mine):* NIC/IRQ inter-arrival timing is an
entropy source that lives on the driver/seam side; if unoautomate wants it, I can
expose an accumulator through the `uno_nic`/seam surface for `get_entropy()` to
mix in. Offered in the request; the core fix stays unoautomate's.
</details>

### 2. Multi-client server loop, REQUEST → unoautomate

`net_accept` (`pc64/net.c:472`) hands over one `TCP_ESTABLISHED` child per call -
no listen backlog, no concurrent servicing. The server half of the mission (and
the deferred "accept several dev PCs at once", NETSTACK.md §Deferred) needs a
real backlog, an N-client connection table, per-conn buffering, and a poll loop.
`net.c` is unoautomate's → request. NETSTACK.md already flags the
`unoauto_remote` multi-inbound work as deferred behind it.

### 3. TCP teardown completeness (reuse safety), REQUEST → unoautomate

State enum lacks `TIME_WAIT / CLOSE_WAIT / LAST_ACK / CLOSING`; close goes
`FIN_WAIT` → `TCP_CLOSED` (`pc64/net.c:574`). No `TIME_WAIT` → a reused 4-tuple
can accept a stale segment on a long-lived server. `net.c` is unoautomate's →
request. Bounded, well-understood.

### 4. The "lift", NOT scheduled; contract-level decision required

Relocating `net.c`/`tls.*`/`netsock`/`netdisc` into `unonet/` would move
unoautomate's owned files and overturn the networking contract above. It is
**not a task this roadmap can execute or file as a routine request.** It only
happens if the user decides to re-open the networking ownership split and the
unoautomate agent agrees. Until then the transport stack stays in `pc64/` under
unoautomate, and `unonet/` remains the seam home. Open question for the user, not
a scheduled item.

## Sequence

Item **1 is done** (2026-07-28). Items 3 then 2 remain, and per the correction
at the top they are ordinary unonet-lane slices, not requests filed elsewhere.
Item 4 is settled: the transport files stay in `pc64/` under shared ownership.

## Explicitly out of scope (LAN mission)

Dropped because the target is trusted low-loss LANs, not the open Internet:
congestion control (cwnd/slow-start), fast-retransmit / dup-ACK / SACK, RTT/RTO
estimation, IP fragment reassembly, IPv6, multi default-route, DNS
caching/CNAME/multi-A, TLS-terminating server. Revisit only if the mission
changes to Internet-facing.
