# UnoDOS pc64 - networking + TLS security review (2026-07-20)

## CRITICAL

### C1 - WPA2 EAPOL: attacker `kdlen` overflows the 256-byte GTK-unwrap stack buffer
**Confirmed.** `pc64/wifi_wpa.c` - `aes_unwrap()` (L102-124) called from
`wpa_sm_rx_eapol()` msg-3/4 (L307) and group-rekey (L323), both writing into
`u8 kd[256]` (L299, L320).

- `kdlen = be16(frame + EK_KDLEN)` (L279) is a raw 16-bit frame field.
- Only size gate is `EK_KDATA + kdlen > len` (L280). **No check against
  `sizeof kd` (256).**
- `aes_unwrap` writes `n*8` bytes where `n = kdlen/8 - 1`. Overflow at
  `kdlen >= 272`. `mic_ok()` rejects `len > 512` (L243), so max reachable
  `kdlen = 408` ⇒ `n=50` ⇒ **400 bytes into `kd[256]` = 144-byte stack overflow**,
  plaintext fully attacker-controlled (AP holds KEK = `ptk+16`, derived from PSK).

Scenario: a malicious / evil-twin AP that knows the WPA2-PSK completes msg 1/2,
then sends msg 3/4 (or rekey 1/2) with Key-Data-Length 408 and a valid MIC →
`aes_unwrap` smashes the saved return address → code execution on the station.
MIC-gated (AP-authenticated), still CRITICAL.

Reachability: only `mrvlwifi.c:395` passes the correct 802.1X offset (`frame+22`),
so live on the Marvell path (metal-pending). `iwlwifi.c:743` and `rtwifi.c:451/462`
pass the raw 802.11 MPDU pointer → `wpa_sm_rx_eapol` returns 0 at L276 and never
unwraps - which is *also a functional bug* (4-way handshake can't complete on those
two drivers). Fix in the library:
```c
/* msg-3 and rekey branches, before aes_unwrap: */
if (kdlen > (int)sizeof kd) { sm->state = WPA_ST_FAILED; return -1; }
```
Ideally also give `aes_unwrap` an output-capacity arg.

## HIGH / MED (suspected - WiFi RX, metal-pending)

**H1 - `mrvlwifi.c:383-397` `rx_one()` trusts device `pkt_off`/`pkt_len` without
bounding to `rl`.** `frame = pd + pkt_off` (arbitrary), `pkt_len` (to 65535) flows
into `handle_eapol(frame+22, pkt_len-22)`. → OOB read up to 64 KB; removes the
`<1600` clamp, worsening C1 on Marvell. Fix: `pkt_off + pkt_len <= rl - INTF_HDR`
and a `pkt_len <= 1600` ceiling.

**H2 - `iwlwifi.c:707-748` `rx_process_rb()` trusts descriptor `mpdu_len` (`fl`)
independent of `plen`.** Guard never checks `(frame-rb)+fl <= cap`. An oversized
`mlen` reads past the packet into the next `g_rb[]` row (info-leak via
`wifi_to_eth` memcpy). Fix: `if ((int)(frame-rb)+fl > cap) continue;`.

**M1 - `tls.c:50-60` entropy fallback.** No-RDRAND path seeds the TLS PRNG from a
`rdtsc`-seeded LCG. Predictable ClientHello random / ECDHE. Should hard-fail
instead of shipping a demo PRNG.

## LOW / informational

**L1** - `net.c` reads L4 header fields before minimum-length checks (`udp_recv`
L247, `tcp_recv` L352-356, `icmp_recv`, `dhcp_opt`). Stray reads stay inside the
static `rx[1600]` buffer → not a true OOB today; fragile defense-in-depth debt.
`ip_recv` already clamps correctly (L512).

**L2** - DNS resolver uses a fixed txn id (`0x5153`, L566) + fixed source port →
easy off-path spoofing (design). Name parser is bounded and does **not** follow
compression pointers, so no decompression loop/OOB.

## Verified CORRECT

- **TLS verification enforced; no skip-verify path; no embedded private key.**
  `tls_connect_ca` (L166-189) wires BearSSL x509-minimal to `uno_tls_tas` trust
  anchors, sets validity time, passes SNI (real hostname from `pc64_http.c:106`) →
  chain + name + date all checked. `tls_ca.c` holds genuine DER roots (ISRG Root
  X1 confirmed). Pinned path uses `br_x509_knownkey` over a **public** P-256 point.
- **WPA MIC verification present** (`mic_ok` HMAC-SHA1/KCK, constant-length
  compare); ANonce re-checked; `find_gtk` bounds GTK to `gtk[32]`. Only hole is C1.
- **Wired/USB NIC RX all clamp to `cap`**: e1000 L198, e1000e L218, igb L218,
  r8169 L227; `ax88179` re-validates `frame+flen <= g_rx_len` (L154/163).
- **HTTP client memory-safe** (`pc64_http.c`): `raw[49152]` clamps every append;
  Content-Length not trusted.
- **Browser parsers bounded** (`pc64_browser.c`): tag stack `st[24]` guarded; word/
  line buffers length-checked; `js_expand` clamps to `DOC_MAX`.
