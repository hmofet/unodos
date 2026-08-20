# WiFi security: the supplicant contract

Owner of `wifi_wpa.*`, `wifi_sae.*`, `tools/sae_test.*`. Consumed by every WiFi
driver (`iwlwifi.c`, `rtwifi.c`, `mrvlwifi.c`) across the `uno_nic_t` seam.
See [AGENTS.md](../AGENTS.md) §1 for what that ownership means.

Status: `[STABLE]` for WPA2-PSK, `[EXPERIMENTAL]` for WPA3-SAE — the SAE path
is verified end to end on the host and has not yet completed a join against a
real access point. See **Open** at the bottom.

## What this is

Two ways to turn a passphrase into a PMK, and one shared handshake that turns
a PMK into installed CCMP keys.

```
                     WPA2-PSK                        WPA3-SAE
                        |                                |
     passphrase --> PBKDF2-SHA1 --> PMK        passphrase --> SAE exchange --> PMK
                        |            (offline-grindable)     (fresh per join, |
                        |                                     not grindable)  |
                        +--------------> EAPOL-Key 4-way <--------------------+
                                              |
                          pairwise TK + GTK (+ IGTK under PMF)
                                              |
                                   the driver installs them in the NIC
```

The whole of the SAE half lives in `wifi_sae.c`; `wifi_wpa.c` is the 4-way
handshake plus the RSN element negotiation that decides which of the two runs.

## Why WPA3 had to happen

The supplicant advertised exactly one AKM suite, `00-0F-AC-02` (PSK), with RSN
capabilities `0x0000`: no SAE, no management-frame protection. That is not
merely "older" — it is unjoinable on a growing share of ordinary home
networks:

- An access point with **MFPR** set (PMF required) **refuses** an association
  request whose RSN element does not claim MFPC. WPA3-only SSIDs set it.
- A **6 GHz** SSID is WPA3-only by regulation, so an AKM-2-only supplicant is
  never offered a way in at all.
- An Eero Pro 6E — and most 2024-onward consumer hardware — defaults its main
  SSID to WPA3 or WPA2/WPA3 transition mode with PMF capable.

That is exactly the shape of the SKYNET failure recorded during the 2026-08
conformance run: the WPA2-only guest SSID (NimmuNet) joined first time, and
the main SSID could not be joined at all. Nothing in the log said why, because
the driver never parsed the beacon's RSN element — it offered WPA2-PSK to
every network and read the resulting deauthentication as a bad password.

## The negotiation

`wpa_parse_rsn_ie()` + `wpa_parse_rsnxe()` read the beacon; `wpa_pick_akm()`
and `wpa_pick_mfp()` decide; `wpa_build_rsn_ie()` writes what we answer with.

| AP offers | we use | our RSN capabilities |
|---|---|---|
| PSK only | WPA2-PSK | `0x0000` — byte-identical to what shipped before |
| PSK + SAE (transition) | **WPA3-SAE** | MFPC + MFPR, group mgmt BIP-CMAC-128 |
| SAE only | WPA3-SAE | MFPC + MFPR, group mgmt BIP-CMAC-128 |
| 802.1X, or no CCMP | *nothing* — the join is refused with a reason | — |

Two deliberate choices in there:

- **SAE wins on a transition AP.** The passphrase is the same either way, and
  the resulting PMK cannot be ground out of a captured handshake. There is no
  reason to take the weaker of two options the AP is offering.
- **Plain WPA2 keeps PMF off** unless the AP demands it. Claiming MFPC on the
  WPA2 path would change an association request that has been joining networks
  for a year, to gain protection on a link that has no SAE anyway.

`wpa_pick_akm()` returning 0 is a real answer, not a failure to decide: the
join stops and says which AKMs the AP offered. An enterprise (802.1X) network
is unjoinable here and should say so in one line rather than fail in the
4-way.

## SAE, concretely

IEEE 802.11-2020 §12.4, finite cyclic group 19 (NIST P-256) only — the
mandatory-to-implement group; no AP requires anything else for WPA3-Personal.

1. **PWE**, the password element. Two derivations, both implemented:
   - **Hash-to-element (H2E)**, §12.4.4.3.2. SSWU maps two hashed field
     elements onto the curve; the sum is scaled by a MAC-derived value. Fixed
     work, no secret-dependent branching. Used whenever the AP sets the H2E
     bit in its RSN Extension element, and mandatory on 6 GHz.
   - **Hunting-and-pecking (HnP)**, §12.4.4.3.3. Hash, test whether the result
     is a valid abscissa, repeat. The universally supported baseline, and the
     method the Dragonblood papers attacked. We run all 40 iterations
     unconditionally, switching to a dummy password once a candidate is found,
     so the *iteration count* leaks nothing. The residual per-iteration timing
     signal is why H2E is preferred — not a fallback we are happy with.
2. **Commit**: scalar = (rand + mask) mod n, element = −(mask·PWE). Exchanged
   in Authentication frames with algorithm 3, sequence 1.
3. **Confirm**: an HMAC-SHA256 over both commits under the SAE KCK, sequence 2.
4. Out falls the PMK and a PMKID, which go into the ordinary 4-way.

**A wrong passphrase fails at the confirm, never at the commit.** That is the
design — SAE deliberately leaks nothing about the password until both sides
have committed — and the driver's log says so explicitly, because "no confirm
from the AP" would otherwise read as a timeout.

Anti-clogging tokens (status 76) are handled: the AP's token is echoed in an
otherwise **identical** commit. Regenerating the scalar there would restart
the exchange, and some APs answer that with a deauthentication.

### Entropy fails closed

SAE's security is entirely in `rand` and `mask`. `sae_init()` draws them from
`tls_entropy_get()` and returns `SAE_ENOENTROPY` when no source qualifies; the
driver refuses the join and says why. This is the same contract `tls.c` holds
itself to, for the same reason: a loud refusal beats a quiet weak key. The
host gate drives that case explicitly.

## Key descriptor versions — the one silent-wrong-answer trap

The 4-way handshake is *shared*, but two of its algorithms are chosen by the
negotiated AKM (802.11-2020 Table 12-10), and getting the pairing wrong does
not fail loudly. It fails as a bad MIC and a deauthentication, which is
indistinguishable from a wrong password.

| AKM | version | PTK derivation | EAPOL MIC |
|---|---|---|---|
| `00-0F-AC-02` PSK | 2 | PRF-SHA1 | HMAC-SHA1 |
| `00-0F-AC-08` SAE | 0 | KDF-HMAC-SHA256 | AES-128-CMAC |

Three ways the SHA-256 KDF differs from the SHA-1 PRF, each of which produces
a plausible-looking wrong key rather than an error: the counter comes **first**
and is **little-endian**, the label is **not** NUL-terminated, and the output
length **in bits** is appended. `wpa_kdf_sha256()` is the only implementation;
do not open-code it.

The Key Descriptor Version field on a received frame is echoed back but never
acted on — the AKM is the authority. The old code rejected version 0 outright,
which is precisely what an SAE access point sends.

## Management frame protection (802.11w)

Required by WPA3, so `wpa_build_rsn_ie()` emits MFPC + MFPR and a group
management cipher suite (BIP-CMAC-128) whenever SAE is chosen, and
`mld_sta_cfg()` keeps the firmware's MFP flag set for the life of the
association rather than dropping it at authorization.

One positional trap worth knowing about, because the RSN element has no tags:
the group management cipher may only *follow* a PMKID count. With PMF on and
no PMKID to advertise we still emit a count of zero — omitting it makes the AP
read the BIP suite's `00-0F` as a PMKID count of 0xAC00 and reject the
association.

The IGTK arrives in message 3 and is parsed and stored, but **not installed
into the NIC by default** — see Open below.

## Testing

`sh tools/sae_test.sh` — seconds, no QEMU, no radio. Wired into
`tools/gate.sh`, for the same reason the TLS gates are: SPECTEST's network area
runs a NULL NIC and can never reach a supplicant.

Three kinds of check, in increasing order of what they can catch:

1. **Self-consistency** — two supplicant instances complete the exchange and
   must land on the same PMK. This catches state-machine and framing bugs and
   nothing else: an arithmetic layer that is wrong the same way on both sides
   agrees with itself perfectly.
2. **Structural** — the PWE is on the curve, every one of the 272 bits of a
   confirm is covered by its MAC, a reflected commit is refused, an
   out-of-range scalar is refused, a wrong passphrase is rejected at the
   confirm and not before, an Eero-shaped transition beacon resolves to SAE,
   a WPA2-only beacon still resolves to the unchanged WPA2 path.
3. **Cross-implementation** — `tools/sae_test.py` reimplements both PWE
   derivations and the KDF from the 802.11 text on Python bignums, sharing no
   line of code with the C, and diffs them.

Check 3 is the one that matters most and is easiest to skip. `wifi_sae.c`
carries a hand-written Montgomery arithmetic layer (BearSSL exposes point
multiplication but not the coordinate-field operations SAE needs — modular
square roots, quadratic-residue tests, inverses), and a hand-rolled bignum is
exactly the kind of code that is self-consistently wrong. Checks 1 and 2
cannot see that. Do not delete the Python.

Every Montgomery constant in `wifi_sae.c` is *derived at first use* from p,
rather than transcribed. A mistyped `R^2 mod p` produces arithmetic that is
wrong only for some inputs, which is worse than arithmetic that is always
wrong.

## Open

- **The SAE path has never completed a join against a real access point.** It
  is correct against an independent model of the specification and against
  itself; that is not the same as interoperating with an Eero. The next step is
  a metal run against SKYNET on the AX201, watching the NET log.
- **The IGTK is held but not installed** into the firmware. Installing it needs
  a `SEC_KEY_CMD` shape for a BIP key that is untested on this firmware, and a
  command this firmware dislikes does not fail — it SYSASSERTs, and only a
  reboot recovers. `iwl igtk 1` turns the install on for the next join so it
  can be tried deliberately. Until it is on, an *unprotected* broadcast
  deauthentication is still acted on — no worse than the WPA2 path today, but
  it is the protection PMF exists to add.
- **Only `iwlwifi.c` speaks SAE.** `rtwifi.c` and `mrvlwifi.c` still advertise
  WPA2-PSK, because their scan/auth/assoc paths are `[metal gap]` stubs that do
  not send pre-association Authentication frames at all. They advertise the AKM
  they can actually complete rather than one they would fail halfway through.
- **Group 19 only.** Groups 20 (P-384) and 21 (P-521) are not implemented; a
  commit naming one is refused with `SAE_EGROUP`. No WPA3-Personal AP requires
  them.
- **SAE-PK and SAE-EXT-KEY (AKM 24) are not implemented.** The RSN parser
  recognises AKM 24 so it can be reported, but `wpa_pick_akm()` will not select
  it.

## Debug verbs

- `iwl sec` — what security each scanned BSS offers, and what we would
  negotiate with it. Answers "why will this network not join" without a
  capture; an SSID whose row reads `-> none` is one we cannot speak to.
- `iwl igtk <0|1>` — install the BIP key on the next PMF join (default off).
