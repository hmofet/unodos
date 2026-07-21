# pc64 — next iteration: "Networking to first lease"

Handoff for the next session. The debug/test campaign (`pc64-debug-stress`) has
landed on **master** and the branch is retired. `master`'s default build is now
the **production** OS; opt into the harness with `UNO_DEBUG=1 ./build.sh`.

Read first: `pc64/METAL-FINDINGS.md` (F1–F15), `pc64/SPEC.md` (contracts +
fix status), `pc64/DEBUG.md` (how to run the harness + the STRESS.CFG keys).

## Why networking

Everything in pc64 works on metal *except* networking, and networking is the
keystone: it blocks the Studio AI assistant and the LAN/WiFi conformance checks
(all currently `SKIP`-pending-networking in SPECTEST). The most tractable win is
the **AX88179 USB ethernet**: it already enumerates, resets, reads a valid MAC,
and brings the link up — only **DHCP fails** (Yoga round: link up in 1 ms, no
lease in 12 s). This session added link-level frame counters so the *next* boot
localizes that failure instead of guessing.

## Phase 1 — Localize the eth DHCP failure  (metal, one boot)

1. Reconfigure a stick with the flasher's **"Reconfigure tests (no erase)"**
   button (Developer options → Stress Test off, Network = Ethernet only,
   Conformance on). Or flash fresh.
2. Boot it on a laptop with the AX88179 adapter; collect
   `CRASH\<machine>\NETLOG.TXT`.
3. The counter line on the DHCP-fail decides the next move deterministically:
   - `tx==0`            → our TX path never fired: `ax_send` / the xHCI (or
     UsbIo) bulk-out. Check the 8-byte TX header + the bulk-out call.
   - `tx>0 rx==0`       → nothing came back: RX parse, the cable, or a silent
     DHCP server. Sanity-check against a known-good DHCP LAN.
   - `rx>0 ip==0`       → frames arrive but none are IP: the RX aggregation
     trailer / per-packet descriptor offset / the 2-byte IP-align pad in
     `ax_recv` (leading suspect).
   - `rx>0 ip>0`        → we saw IP but no lease: DHCP offer/option parsing.

## Phase 2 — Fix the localized side in `ax88179.c`

Leading suspect is `ax_recv`: the per-packet descriptor array at the tail of the
bulk-in transfer, `pkt_len`/`step` math, and the 2-byte alignment pad. A raw-RX
hex dump path is already available for tuning. **QEMU has no AX88179**, so this
is metal-iterative — the counters + the dump are the instrument. Secondary
suspect: the TX header (`ax_send`, the `0x80008000` pad-bit case).

## Phase 3 — Bring up the rest of the stack on the real link

Once a lease lands, `ip_suite` (pc64_nettest.c) already runs ping → DNS. Extend
to a **TLS handshake** to a known host (BearSSL is linked) to prove end-to-end
reachability including certs.

## Phase 4 — Convert the networking SKIPs to real conformance tests  ✅ BUILT

DONE (2026-07-21): `test_netstub()` is now `test_netlive()` in
`pc64_spectest.c` — real link-gated checks that PASS/FAIL on a connected
machine and SKIP with a named reason otherwise:
`S-NET-30` (DHCP lease + TCP round-trip; the stub's "S-NET-20" label collided
with SPEC.md's RX-drain contract, hence the renumber), `S-WIFI-20` (live join
to a lease; FAILs with `iwl_status_str` until the MLME tail lands),
`S-AI-01/02` (DNS → CA-validated TLS to api.anthropic.com, then a full HTTPS
request→response through it — a 401 + JSON body is the expected proof, no key
needed). `tools/spectest_qemu.py` now boots with a SLIRP e1000 so S-NET-30 and
S-AI-01/02 run for real in the QEMU regression (needs the build host online).
A link-up-but-no-lease machine now shows a FAIL with the frame counters — the
Phase 1 localization — instead of a SKIP.

Building these immediately caught (and fixed) three real transport bugs the
stack had never been driven through before — all three would have bitten the
Browser/AI on metal too:
- `net.c` S-NET-15/23: fixed 4096 window over a 2048 rxq, and rcv_nxt ACKing
  bytes that missed the queue — every >2 KB TLS certificate flight arrived
  corrupt. Window is now the rxq's real free space (rxq grown to 8 KB), only
  stored bytes are ACKed, overlap-trimmed retransmits recover the tail.
- `tls.c low_read`: when one poll batch delivered the last data segment AND
  the FIN together (Connection: close servers), the state check threw away
  the just-arrived bytes and reported EOF.
- `tls.c tls_close`: `br_sslio_close()` loops forever against a torn-down
  connection when the peer's close_notify was already received (ssl_io.c's
  RFC 5246 carve-out skips the engine-fail) — 20 s spin, watchdog reset.
  Replaced with a bounded polite close. Phase 3's "TLS handshake to a known
  host" is hereby proven end-to-end in QEMU (S-AI-01/02); metal awaits the
  Phase 1/2 eth fix.

## Phase 5 — WiFi (F12), same method, with a card present

The `wait_alive` autopsy is in place (per-family register signatures). Iterate
the **AX201 gen2 kick** and the **AX210 stuck-SW_RESET / TOP handshake** until
firmware reaches ALIVE, then run Phases 3–4 over WiFi. Needs a machine with the
card in hand (the Latitude has an AX210).

## Fold-in quick metal reads (same batch, near-free)

- **MacBook (F9):** it hangs with no telemetry; the F15 splash now prints the
  last bring-up stage in white. Read that line off the screen to locate the hang.
- **Surface (F14):** confirm it now physically powers off (ACPI S5 fallback).

## Still metal-blocked (need the hardware to iterate, not fixable blind)

F4 I2C-HID *binding* (ACPI enum finds the device; the I2C transfer to the pad
still fails), F6/F8 detach on Surface/X1 (gated by F4), and the WiFi/eth items
above.

## Success criteria

A DHCP lease + successful ping/DNS on at least one laptop over the AX88179; the
eth-related SKIPs converted to passing checks; the MacBook last-stage line and
the Surface power-off outcome logged.

## Test workflow reminders

- Debug/QEMU: `UNO_DEBUG=1 ./build.sh` then `python3 tools/spectest_qemu.py`
  (real-FAT; vvfat corrupts multi-cluster writes; boots with a SLIRP e1000 so
  the live network checks run - needs the build host online). Current
  baseline: **62 PASS / 0 FAIL / 4 SKIP**, clean.
- STRESS.CFG control keys (`nostress` off-switch, `passes=1` bounded run +
  self power-off): `UNO_DEBUG=1 UNO_DBGCON=1 ./build.sh` then
  `python3 tools/stresscfg_qemu.py`.
- After any OS change, rebuild + publish the flasher:
  `flash/deploy-to-share.ps1` → `\\behemoth\unreplicated\unodos\pc64`.
- Reading returned sticks: devbuntu has the USB write-blocker; always cross-check
  `cpu:`/`fb_base` in BOOTENV before attributing a report to a machine.
