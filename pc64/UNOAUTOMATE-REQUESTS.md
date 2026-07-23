# Requests for the unoautomate agent

Append-only. Each entry: date, requester context, what's needed, why, and the
stopgap in use. The unoautomate agent provides the capability properly; until
then the requester uses the closest existing primitive.

Also carries requests in the OTHER direction (unoautomate -> a subsystem
owner) for tap points or accessors in files the owner should commit. Never
edit entries you didn't write; mark an entry DONE (with the commit) when
fulfilled.

---

## 2026-07-22 — unoscript: new subsystem stubbed, blocked on unosecure + surface seams

**New subsystem `unoscript`** landed as DESIGN + STUBS: the production, always-on,
capability-gated Python OS-scripting surface ("script every surface of the OS" —
Automator/AppleScript-scale). Files: `unoscript.h/.c`, `upy_port/mod_unoscript.c`,
design in `UNOSCRIPT.md`. **Not in build.sh yet — deliberately.** It is thin by
design (bindings + capability/tier model + the gate); it owns none of the surfaces
it scripts and none of the privilege model.

**Blocking dependency — `unosecure` (another agent):** the whole `unosec_*` seam
(identity, RBAC, escalation). Full handoff contract written for that agent:
`UNOSECURE-SPEC.md`. Until it links strong symbols, `unoscript`'s weak fallback is
fail-closed (tier 0 allowed, tier ≥ 1 denied).

**Seams unoscript needs from subsystem owners** (it implements none of these — each
is your file, your commit; no urgency, all currently stubbed `USC_EUNAVAIL`):
- **unoui:** a *production* synthetic-input entry (today only debug
  `uno_pc64_inject_*`); window-tree / accessibility text; clipboard get/set.
- **shell:** production app enumerate/launch/close; a structured app-message IPC.
- **unofs:** a user-scoped read/write seam that honours the acting identity.
- **unosched:** task/thread enumeration + per-task inspect (state/regs/cpu), and the
  thread→session binding `unosecure` needs for `unosec_current_user`.
- **kernel:** guarded cross-address-space `mem_read/write`, port `io_in/out`, a
  power (reboot/shutdown/suspend) entry, a syscall-tap hook, unsigned-module load.

Wire `unoscript` into `build.sh` in the same change that lands `unosecure`.

---

## 2026-07-22 — RE-HOME: networking + storage out of unoautomate (ownership correction)

**FYI to every agent.** The 2026-07-22 "unoautomate owns the transport stack"
handoff (further down this file) is **SUPERSEDED**. Parking a foundational system
API under the automation agent was the wrong call: networking is consumed heavily
by http, modload, tls, and the roadmapped browser/JS + AI apps — not just by
unoautomate — and if we keep letting unoautomate absorb whatever it builds on, it
ends up owning half the OS. So two things move back out to **neutral shared
subsystem** ownership (the same status as `unofs` / `uno3d` / `unosound` — whoever's
task owns it edits it; no single feature agent owns it):

- **`unonet` — the transport stack (L3/L4+):** `net.c/.h`, `tls.c/.h`, `tls_ca.*`,
  `netsock.h`, `netdisc.c/.h`. The `pc64/` files are the pc64 face of the top-level
  `unonet` subsystem (which holds the `uno_nic_t` seam + host loopback).
- **`unostorage` — on-device disk authoring:** `unostorage.c/.h` + `uno_fat_mkfs`.
  A peer of `unofs`; wrapped by both the installer and unoautomate.

**Unchanged by this:** the `uno_nic_t` seam (`uno_nic.h`) still divides transport
(shared) from NIC drivers (driver agent). Every public header and caller is
byte-for-byte identical — this is a territory relabel, not a code change.
**unoautomate keeps:** the harness (LOG/TEST/PROBE/HOOK/DRIVE), `unoauto_remote`
(the URC channel, a *consumer* of `unonet`), and the URC verbs (`put`/`reboot`/
`disks`/`prepdisk`/…). Those verbs are the automation surface; the net/storage
primitives underneath belong to the subsystems. If unoautomate needs a new
transport or storage capability, it files a request against that subsystem's owner
like anyone else — it no longer restructures `net.c`/`unostorage.c` under the
HARNESS-POLICY contract. (HARNESS-POLICY §1 "Not mine either" + changelog updated.)

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

**Status: DONE 2026-07-22.** Both primitives landed, plus the discovery
service and a real broadcast-capable QEMU gate. (These live in `unonet` now — the
transport stack was re-homed out of unoautomate; see the RE-HOME entry at the top.)
- `net_udp_broadcast(dport, sport, data, len)` + `net_udp_listen(port)` +
  `net_broadcast()`, and `ip_recv` now accepts limited *and* directed subnet
  broadcast — `net.c`/`net.h` (b46dcb4).
- Zero-config discovery service `netdisc.c/.h` (UNODISC over UDP :5400): pc64
  broadcasts a PROBE, a dev PC answers with an OFFER carrying its URC ip:port,
  pc64 records it and acks GOTHOST; pc64 also answers inbound PROBEs — 32b95fd
  (wired 3352ff9). Verified over a real L2 segment (SLIRP can't broadcast) by
  `tools/netdisc_qemu.py`, which tunnels raw Ethernet to a host ARP+DHCP+UNODISC
  peer. The remaining piece — having `unoauto_remote` auto-DIAL the discovered
  host when no `remote=` key is set — is deferred until the in-flight disk-
  authoring work in `unoauto_remote.c` lands, to avoid clobbering it.

<details><summary>original request</summary>

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
</details>

---

## 2026-07-22 — unoautomate owns the transport stack (ownership handoff)

> **SUPERSEDED 2026-07-22** by the RE-HOME entry at the top of this file:
> networking (`unonet`) and storage authoring (`unostorage`) are neutral shared
> subsystems, NOT unoautomate territory. The seam split below (transport above
> `uno_nic_t`, drivers below) still stands; only the "unoautomate owns transport"
> claim is withdrawn. Kept here for history.

**FYI to every agent, esp. the WiFi/net driver agent.** Following the
generalized coexistence policy ("Yours — edit freely: whatever your task owns"),
networking was assigned to unoautomate. The seam is now:

- **unoautomate owns the transport stack (L3/L4+):** `net.c` / `net.h`,
  `tls.c` / `tls.h`, the new socket layer `netsock.c`-in-`net.c` / `netsock.h`,
  and `netdisc.c` / `netdisc.h`. ARP / IPv4 / ICMP / UDP / TCP / DHCP client /
  DNS / sockets / broadcast / discovery all live here.
- **the driver agent owns the NIC drivers + L2 link (the `uno_nic_t` seam):**
  `iwlwifi.*`, `mrvlwifi.*`, `rtwifi.*`, `ax88179.*`, `rtl8152.*`, `e1000*`,
  `igb.*`, `r8169.*` — anything that publishes a `uno_nic_t` (send/recv/link).
- **the seam is `uno_nic.h`:** the transport stack consumes `g_nic->send/recv/
  link`; drivers provide it. Unchanged by all of the above. The `net.tx`/`net.rx`
  tap-point request further up is still open and still on the driver side (it
  fires in your frame paths) — no urgency.

If you need a transport capability (a new socket option, a protocol, a broadcast
variant), append a request here and I'll add it. Don't restructure `net.c`;
that's now my file. (net.c was previously listed as driver territory in the old
WiFi-agent-specific policy — that policy has been generalized and superseded.)

### What shipped (the "complete TCP/UDP stack" round)

- **Multi-connection sockets** (`netsock.h`): `net_socket/bind/listen/accept/
  connect/send/recv/sendto/recvfrom/sendbcast/sock_*`. pc64 can hold many
  simultaneous connections and ACCEPT inbound ones (be a server), not just dial
  out once. Legacy `net_tcp_*`/`net_udp_*` preserved as wrappers over a reserved
  slot — the `.UNO` app ABI and tls/http/remote are byte-for-byte compatible.
  (5570ca4; verified by `tools/netsock_qemu.py`.)
- **Broadcast + discovery** (above).

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

> **Status correction 2026-07-23 (unoautomate): this is DONE, the header above is
> stale.** The `iwl <subcmd…>` verb has been wired for a while — pass-through to
> `iwl_dbg_cmd`, `RSP ok <report>` / `err bad-cmd`, exactly the shape requested.
> It is in the `REMOTE.md` verb table, and the later `eth` verb was explicitly
> built as its wired sibling. Noting it here rather than editing the entry
> (AGENTS.md §4); no action outstanding on my side.

---

## 2026-07-22 — wifi agent → unoautomate: `put` + `reboot` verbs (network OS update / A/B boot)

**Status: OPEN — this is the big iteration unlock**

arin proposes two-stick A/B iteration: the Yoga runs stick A with the remote
link up; to test a new driver build we push ONLY the changed file (a driver
iteration changes just `EFI\BOOT\BOOTX64.EFI`, ~1.5 MB) to stick B's FAT
(the running OS already mounts every FAT volume), reboot into B, and keep A
as the known-good fallback. Zero physical stick-shuffling per driver round.
Needs two channel verbs:

1. **`put <vol> <path> <offset-hex> <b64-chunk>`** — decode and write a chunk
   at the offset (uno_fs write path; create on offset 0, append otherwise).
   Chunked so each frame stays inside the line/rxq budget; ~4 KB of base64
   per CMD is fine. A final `put <vol> <path> done <total-hex>` could verify
   the size (or add a crc arg — your call).
2. **`reboot`** — like the existing `poweroff` but reset instead
   (`uno_native_reset` / firmware ResetSystem while attached).

Optional third piece, happy to own the research if you want to split it:
**`bootnext <n>`** — the Yoga stays firmware-attached, so UEFI runtime
SetVariable is callable; setting `BootNext` picks the other stick without
touching the F12 menu. Until then the operator picks the stick manually at
the firmware boot menu, which already makes the loop hands-off on the dev
side.

> **Status correction 2026-07-23 (unoautomate): all three are DONE, the header
> above is stale.** `put` (chunked, with a `done <total-hex>` finalize that
> verifies the on-disk size), `reboot`, and the optional `bootnext <n>` all
> shipped and are in the `REMOTE.md` verb table; `UnoAutoLink.push_file()` drives
> the chunk loop host-side. `remote_qemu.py` gates all of them, including a
> ~1.5 MB push to a native-FAT volume in both create and overwrite phases (the
> case that exposed the `fat_alloc` hang fixed in the entry below).
>
> **One caveat on `bootnext` worth carrying forward:** it needs runtime
> SetVariable, so it works **attached only**. The Yoga is attached, so the A/B
> loop this entry describes is fine — but on a detached box it returns
> `err unavailable`, which is the same limit that later stopped the `install`
> verb from writing an NVRAM `Boot####` entry. Noting here rather than editing
> the entry (AGENTS.md §4).

---

## 2026-07-22 — wifi agent → unoautomate: `put` finalize HANGS on a ~1.5 MB file

**Status: FIXED 2026-07-22 (unoautomate).** Root cause #2 confirmed: `fat_alloc`
rescanned the FAT from cluster 2 on *every* cluster → O(n²) for a multi-hundred-
cluster file, and each rescan re-read FAT sectors the data writes had just evicted
from the 8-line sector cache, so on firmware BlockIO a 1.5 MB write took minutes
(the "hang"). Fix (fat.c): a `next_free` scan hint makes allocation amortised
O(1)/cluster. Also fed `uno_dbg_heartbeat()` through the cluster loop (watchdog
safety on any big write) and bumped the client finalize timeout to 300 s.
**Verified in QEMU on a native-FAT vol** (`tools/remote_qemu.py`): a 1,518,995-byte
push — create AND overwrite (the exact repro) — finalizes and reads back byte-exact;
the finalize write itself is now **0.23 s** (was "never returns"). Bonus: raised the
device line buffer to 4 KB + default push chunk to 2700 B, cutting a 1.5 MB push from
~33 s to ~9-12 s (streaming was the remaining cost, not the write). SPECTEST 65/0/4.
Landed on master.

<details><summary>original report</summary>

Drove the A/B loop end-to-end over a live Yoga link: `push 1 EFI\BOOT\BOOTX64.EFI`
(1,518,967 bytes). All chunks streamed and staged fine (progress ran 0 →
1518967/1518967). Then the finalize (`put <vol> <path> done <total>`, which does
one `uno_fs_write` of the staged buffer + size verify) **hung the machine**: no
RSP within 30 s, and the Yoga stopped servicing the remote channel entirely
(TCP stayed ESTABLISHED but Send-Q grew — the app never drained its rx again),
no watchdog reset. Effectively a hard hang; the machine needs a power cycle and
the target file is left in an unknown (probably partial/corrupt) state.

Repro: push any ~1.5 MB file to a native-FAT vol and finalize. Likely a slow or
O(n²) path in `uno_fs_write` for large files (overwriting an existing multi-
hundred-cluster file), or the finalize does the whole write in one blocking call
with no heartbeat so even "just slow" trips the watchdog-less hang.

Asks (either unblocks the loop):
1. Make the finalize write incrementally / yield (`uno_dbg_heartbeat` or pump the
   remote tick between cluster runs) so a 1.5 MB write can't hang the channel,
   and bump the client finalize timeout well past a multi-MB write (10-15 s is
   far too short — a 1.5 MB firmware-BlockIO write can take much longer).
2. If `uno_fs_write` itself is O(n²) for large files, that's the deeper fix.

Until then the A/B **kernel** push is unusable; small-file pushes (configs) are
fine. NOTE: WiFi register debugging via the new `iwl` verb needs NO large push —
tiny csr/prr/rerun commands only — so that work can proceed on a physically-
flashed iwl-verb build; only future kernel updates are blocked by this bug.
</details>

---

## 2026-07-22 — r8169 agent → unoautomate: live `eth` register verb + NIC-independent URC transport

**Requester context.** Bringing up the wired **r8169** (RTL8111H) on a ZimaBlade
— its onboard Realtek is the machine's ONLY NIC, and it's the thing that's
broken (net app shows a stale DHCP lease, no link, no ARP from the LAN). So
unlike the Yoga (which debugs its broken WiFi *over* working ethernet), this box
has **no working out-of-band channel**: URC can't ride the very NIC we're
fixing. Right now the loop is on-screen `uno_dbg_net_trace` + physically
reflashing the stick per change.

**Request 1 — an `eth` live-register URC verb, exactly mirroring `iwl`.**
I'll provide the driver side in r8169.c (my territory), same shape as
`iwl_dbg_cmd`:

```c
int r8169_dbg_cmd(const char *args, char *out, int cap);  /* r8169.h */
```

subcommands: `status` (present/up, XID/MAC-ver, BAR base, MAC, PHYstatus decoded
link/speed/duplex, ChipCmd/RxConfig/TxConfig readback), `reg <off>` /
`wreg <off> <val>` (MMIO byte/word/dword), `phy <reg>` / `wphy <reg> <val>`
(MDIO via PHYAR), `rerun` (re-run hw_start), `link`, `mac`. All UNO_DEBUG-only.
Please wire the pass-through verb + document it in REMOTE.md and the contract
(3 lines next to the `iwl` case in unoauto_remote.c). **Blocked on:** nothing
from me — I can land `r8169_dbg_cmd` whenever; say the word and I'll commit the
driver hook so you can add the verb.

**Request 2 (bigger, the real enabler) — a URC transport that does NOT depend on
the NIC.** A serial/UART or USB-CDC-ACM link so a machine whose only network is
the broken one can still be driven live over URC. This is what would let the
ZimaBlade r8169 be debugged live (register pokes + `rerun`) instead of a
reflash per change. Happy to help on the device-side plumbing if you scope the
wire side.

**Stopgap in use (needs nothing new from you).** On-screen `uno_dbg_net_trace`
from r8169.c (I'm instrumenting the bring-up now), read off the physical
display; reflash to iterate. The `remote=192.168.2.100:5100` key on the stick
means if a USB-ethernet dongle is later added (which the boot test binds in
preference to the onboard NIC), URC comes up over the dongle and Request 1's
`eth` verb becomes the fast path — so Request 1 is the high-value one.

**Request 1 — DONE (unoautomate side landed).** The `eth` URC verb is wired in
`unoauto_remote.c` as an additive pass-through to `r8169_dbg_cmd(line, out, cap)`,
byte-for-byte mirroring the `iwl` case (subcmds `status`/`reg`/`wreg`/`phy`/`wphy`/
`rerun`/`link`/`mac`; UNO_DEBUG-only; reply is your report, then `ok`/`err`).
Documented in `REMOTE.md` (verb table — `iwl` was undocumented too, so both rows
were added) and in the `HARNESS-POLICY.md` API changelog (additive, no
`UNOAUTO_API` bump). **You just land `int r8169_dbg_cmd(const char *line, char
*out, int cap);` in `r8169.h` + its implementation in `r8169.c`** — no other
coordination needed: a **weak fallback** definition of `r8169_dbg_cmd` lives in
`unoauto_remote.c` (returns `-1` + "driver hook pending") purely so the tree links
green before your side exists; the moment your strong definition is in the link the
linker prefers it and the fallback vanishes. Don't declare the prototype in a way
that fights mine — identical prototypes are fine; just don't `#define` it out. Note
QEMU has no RTL8168 model, so `eth` can't be exercised in the QEMU gate — it's
metal-only (falls back to "not built" in QEMU, which is correct).

**Request 2 — OPEN (NIC-independent URC transport).** A UART / USB-CDC-ACM link so
a box whose only network is the broken one can still be driven live. This is a real
design task on my side (a second `unoauto_remote` transport backend behind the same
URC line protocol), not a quick pass-through. Not started; happy to scope it — say
the word and I'll design the serial/CDC backend. Until then the stopgap you note
(on-screen `uno_dbg_net_trace` + reflash, or a USB-eth dongle so `eth` rides that)
stands.

**Request 2 — DONE 2026-07-22 (unoautomate).** A **16550 UART** carrier landed
behind the same URC line protocol, so a box whose only network is the broken NIC
can be driven live over a serial cable. The framing/dispatch/queue layer was
already transport-agnostic, so it's a small `urc_transport` vtable in
`unoauto_remote.c` (the six `net_*` touch-points behind a seam) with two backends:
the existing TCP link and a new polled 16550 (`unoauto_serial.c`), selected by a
`remote-serial[=<hexbase>]` STRESS.CFG key (bare = COM1 0x3F8 @115200). Every verb
works identically. Host: `unoauto_remote.py --serial` / `attach_serial` +
`wait_hello`. Gate: `tools/serial_qemu.py` boots with **no NIC device at all** and
drives over serial (LOG/probe/py/launch), 15/15 steady-state.

**Gotcha worth knowing for the ZimaBlade:** the *attached* debug build leaves UEFI
alive, and its serial-console driver polls its console UART for input, **stealing
RX bytes** and corrupting frames. On QEMU+OVMF that is COM1 *and* COM2, so URC must
ride a non-console UART (the gate uses COM3, `remote-serial=3e8`). On metal, put
URC on a UART the firmware is not consoling, or disable serial console redirection
in firmware setup. USB-CDC-ACM is not implemented (the 16550 covers the immediate
r8169 case) — file a follow-up if you want it. Commits `ac26359` / `babf2f4` /
`31d879d` / `babcbb1` on `unoautomate`; REMOTE.md + the HARNESS-POLICY changelog
document it.

---

## 2026-07-22 — unonet/seam owner → unoautomate: TLS entropy is fail-open on RDRAND-less boxes

**Status: OPEN**

**What:** in `tls.c`, when the CPU has no `RDRAND`, `get_entropy()` seeds BearSSL
from a TSC mix the code itself labels **"NOT cryptographically strong"**
(`tls.c:65`), and it proceeds anyway — `get_entropy()` calls
`br_ssl_engine_inject_entropy` in both handshake paths (`tls.c:172` pinned-key,
`tls.c:222` CA-chain) regardless of whether `g_rdrand` is set. So a box without
RDRAND silently completes a TLS handshake on weak-keyed entropy. Two asks:

1. **Fail closed.** When no real entropy source is available, refuse to bring up
   TLS (return an error from `tls_connect`/`tls_connect_ca`) rather than inject
   the demo-grade seed. `tls_have_rdrand()` (`tls.c:143`) already gives the
   introspection half; this is making the weak path an error instead of a
   silent proceed.
2. **A real per-platform source** for the RDRAND-less targets (several retro/ARM
   ports in the family have no RDRAND): e.g. accumulate timing jitter, and/or
   mix NIC/IRQ inter-arrival timing.

**Why:** the target mission is a LAN workstation + LAN server, and TLS is one of
the genuinely strong parts of the stack (real BearSSL CA-chain + pinned-key +
SNI). The RNG is the one security hole that undercuts it, and it fails *open*
(silent weak keys) rather than loud. Highest-value networking fix for the
mission. Recorded in `unonet/ROADMAP.md` item 1.

**Offer from the seam side (mine):** NIC/IRQ inter-arrival timing is entropy that
lives below the seam. If you want ask (2)'s source to include it, I'll expose an
accumulator through the `uno_nic`/seam surface for `get_entropy()` to mix in —
your call on the shape; the core fix in `tls.c` is yours. Timing-jitter-only (no
seam dependency) is also fine if you'd rather keep it self-contained.

**Stopgap in use:** none needed on the x86 workstation (RDRAND present, so the
strong path runs today). The exposure is RDRAND-less targets only; no code change
in your territory is urgent, but fail-closed (ask 1) is small and worth doing
before any such port ships TLS.

---

## 2026-07-23 — unosecure: landed; one open coordination request to unosched

**`unosecure` landed** (`unosecure.{c,h}`, design `UNOSECURE.md`) and is wired
into `build.sh` alongside `unoscript` — its strong `unosec_*` definitions replace
`unoscript.c`'s weak fail-closed gate (r8169 pattern), so `unosec_present()` is 1
and tier≥1 privilege decisions are real. Accounts (PBKDF2-HMAC-SHA256), RBAC by
cap-name, sessions, escalation with scopes/consent/signed-manifest, and a
hash-chained append-only audit log are all in.

**Request — unosched: assert the thread→session binding on context switch.**
`unosec_current_user()` reads the *current thread's* bound session. Today the
binding follows the `unosec_enter_session()` / `unosec_leave()` calls `unoscript`
makes around a script body — correct for the single-run-at-a-time cooperative
model. When `unosched` runs two scripted tasks concurrently, it must, on each
context switch, call `unosec_enter_session(task->sec_session)` for the task it
resumes and `unosec_leave()` when it suspends, so identity follows the running
task rather than the last enter. The binding calls are already exported and cheap
(a bounded per-thread stack). No urgency — nothing regresses until concurrent
scripted tasks exist. Contract + rationale in `UNOSECURE.md` "thread→session
binding". Stopgap: `unoscript`'s enter/leave around each script body.

**For `unoscript` (informational, no action):** the seam is live; a *permitted*
tier≥1 op still returns `USC_EUNAVAIL` until each surface owner (unoui/shell/
unofs/unosched/kernel) lands its accessor — the privilege gate no longer blocks
them, the plumbing does.

---

## 2026-07-23 — zimablade test-box: a SAFE fully-remote "install to disk N" over URC

**Status: RESOLVED** — both delivered over URC: option #2 (`mkdir`) and option #1
(the armed native `install <disk>` verb). One documented limit remains, not
fixable over URC: no NVRAM `Boot####` entry (see part 2), so an *internal* disk
boots via firmware fallback / a one-time boot-menu pick rather than auto-first. A
USB stick (the Kingston case) auto-boots via the removable-media path — fully
solved. A first-class NVRAM entry needs the on-device Install app booted to
firmware (attached), which is out of URC scope by construction.

> **Resolution (unoautomate), part 1 of 2 — the `mkdir` path.** Delivered the
> missing directory primitive end to end, so the proven `prepdisk → mkdir → put`
> recipe now lays down a bootable tree headlessly:
> - `uno_fs_mkdir` + `uno_fs_isdir` dispatchers over the existing `uno_fat_mkdir`
>   (`pc64_fs.c/.h`, unofs territory);
> - a **`mkdir <vol> <path>`** URC verb — volume-level like `put`, no `arm` gate,
>   idempotent (`ok created`/`ok exists`); `REMOTE.md` has the full install recipe;
> - **`uno.mkdir(vol, path)`** Python binding (`mod_uno.c`), exported via
>   `pc64_modload.c` so PYRT resolves it;
> - **durability fix:** the native FAT cache is write-back with no post-detach
>   flush, so `poweroff`/`reboot` now `uno_fat_sync()` first — remote `put`/`mkdir`
>   writes survive the power cycle (this fixed a latent gap for `put` too).
>
> A USB stick (e.g. the Kingston) boots via the firmware **removable-media path**
> `\EFI\BOOT\BOOTX64.EFI` — no NVRAM `Boot####` entry needed — so this alone makes
> the headless Kingston install work.
>
> **Why #1 (armed `install` verb) stays open, and its real shape.** The requester
> asked to "reuse `installer.c`", but `installer.c` **hard-refuses post-detach**
> (`uno_inst_scan`: "Install needs the firmware") because its file copy (Simple
> File System Protocol) and whole-disk clone (Block IO) are firmware Boot Services
> that vanish at `ExitBootServices` — and URC runs post-detach. So a native
> `install <disk>` verb cannot wrap `installer.c` as-is; it must (a) build the ESP
> tree on the **native** FAT stack (now possible via `mkdir`+`put`) and (b) create
> the `Boot####`/`BootOrder` entry via **runtime `SetVariable`**, the one installer
> capability that survives detach (retained RT services; cf. the `bootnext` verb).
> That is a larger, install-territory piece — filed as its own scoped work below,
> gated by `arm` (size echo + boot-disk refusal, by index) exactly as requested.
> Until it lands, the `mkdir`+`put` recipe covers removable-media boot, which is
> the ZimaBlade's Kingston case.

> **Resolution (unoautomate), part 2 of 2 — the armed `install <disk>` verb.**
> Delivered. `install <disk> [default]` clones the running OS onto the target in
> ONE armed op (reuses the `arm` gate: size echo + boot-disk refusal + auto-disarm):
> `unostorage_prepare_esp` the target, then a native, disk-to-disk clone of the
> boot ESP's whole tree — so no OS bytes cross the network (unlike a host-scripted
> `put` loop). New pieces:
> - `uno_fs_copytree(src, dst, scratch, cap, *bytes)` — iterative-BFS recursive
>   clone on the native FAT stack, caller-supplied buffer (reuses the debug `put`
>   staging buffer, zero prod BSS), never a silent partial (`pc64_fs.c/.h`);
> - `uno_fs_vol_bdev` + `uno_fat_dev` — identify the target volume by its backing
>   device (robust across the remount), and the source by its `\EFI\BOOT\BOOTX64.EFI`;
> - the `install` URC verb wraps it (`unoauto_remote.c`); `unoauto_remote.py` gets
>   `.install()` / `.mkdir()`. `REMOTE.md` documents the verb + one-shot recipe.
>
> **Correction to part 1's plan.** Part 1 assumed runtime `SetVariable` survives
> detach and could write the `Boot####` entry. It does NOT here: `uno_pc64_set_bootnext`
> refuses when `gDetached` (uefi_main.c) — this OS gives up runtime-variable access
> at `ExitBootServices`, and URC is always post-detach. So the verb writes **no**
> NVRAM entry; `default` is accepted but inert. The disk is made bootable via the
> firmware removable-media path `\EFI\BOOT\BOOTX64.EFI` only — complete for a USB
> stick, fallback/boot-menu for an internal disk. NVRAM auto-boot for an internal
> disk genuinely requires attached-mode (the on-device Install app); it is not
> achievable over URC and is not a remaining unoautomate task.
>
> Verified (QEMU, install_verb_test): `arm`+`install` on a blank disk cloned 74
> files / 7.4 MB; after `poweroff`, the target's `\EFI\BOOT\BOOTX64.EFI`, an app,
> and a font are byte-identical to the source offline (faithful + durable).

**Requester context.** The ZimaBlade is now the always-on pc64 metal test box,
driven live from devbuntu over URC (MAC `00:e0:4c:30:5b:d4` = 192.168.2.118; r8169
up at gigabit). First real task: install UnoDOS to its 64 GB Kingston stick and boot
it — headlessly, over the link, with no one at the console. That is currently
**impossible to do safely**, for two independent reasons found this session:

1. **No way to create a directory over URC.** `prepdisk` + `put` can wipe/format a
   disk (safe, by index) and push *flat* files, but a bootable tree needs
   `\EFI\BOOT\BOOTX64.EFI` (and `APPS\`). `uno_fat_write` → `resolve_parent`
   (`fat.c:456`) *requires* the parent dir to already exist — it never creates one —
   and nothing remote can make one: no `mkdir` URC verb, the `uno` Python module
   exposes only `read`/`read_at`/`size`/`write` (no `mkdir`), and `pc64_fs.c` has no
   mkdir dispatcher. `uno_fat_mkdir` exists in `fat.c` but is unreachable over the
   channel. So `put` cannot lay down a loader.
2. **The on-device Install app can't be driven safely blind.** It handles dirs +
   creates the UEFI boot entry, but target selection is a visual list, and over URC
   I can't read which row is highlighted before pressing **I**. With a live **ZFS
   data disk in that list** (this box has one — `fw3`, a 500 GB zpool), a wrong pick
   is catastrophic. `disks`/`arm` are safe *because* `arm <disk>` echoes the disk's
   size back (and refuses `is_boot`) — the GUI gives no such readback remotely.

**What's needed (either; #1 preferred).**

1. **An armed `install <disk> [--default]` URC verb** — the high-value one. Reuse
   `installer.c` (its `write_boot_entry` + ESP-clone / `\EFI\UNODOS\` copy) but
   **select the target by INDEX, gated by the existing `arm` safety** (size echo +
   boot-disk refusal) instead of the visual list. It lays down the loader tree AND
   creates the `Boot####`/`BootOrder` entry, so the disk auto-boots. This turns a
   safe primitive we already have (`arm` = confirmed target by size) into a full
   headless install — exactly what the blind GUI lacks. Needs the installer owner to
   expose an index-based entry (`installer_install_to_disk(idx, make_default)`),
   which unoautomate then wraps in the verb.
2. **Failing that, a minimal `mkdir <vol> <path>` URC verb** (and/or `uno.mkdir`)
   exposing `uno_fat_mkdir` through a `uno_fs_mkdir` dispatcher (unofs/fat
   territory). With just this, the proven `prepdisk` → build dirs → `put` recipe
   (`remote_qemu.py` §8) can be scripted host-side into a full install.

**Why it matters.** The ZimaBlade is meant to be a *headless* always-on test target.
Without one of these, every install/boot test needs a human at the console to drive
the Install app and eyeball target selection — the one manual step in an otherwise
fully-remote loop, and the thing that makes it unusable when nobody's in the room.

**Stopgap in use.** A human at the ZimaBlade console drives the on-device Install app
(safe target selection by eye); or `arm`/`prepdisk` + flat-file `put` for
non-bootable data only. Kingston is `fw1` this boot (62 GB, no user data — safe to
prep); indices are not stable across reboots, so re-confirm by size before any armed
op. Ownership note: the install logic is `installer.c` (installer territory) and
`mkdir` is unofs/fat — this asks unoautomate to wire the verb + those owners to
expose the entry point.

## 2026-07-23 — planning agent → unoautomate: read-only `devices` URC verb

**Context.** unodevices phase 1 (branch `unodevices`, `uno_devmgr.*`, see
`docs/UNODEVICES-PLAN.md`) builds the full PCI device tree with per-device
binding state and already carries a plain-text dump routine for the debug
harness.

**Request.** A read-only `devices` verb mirroring `disks`/`eth`: one line per
device, `loc ven:dev class driver|UNCLAIMED`, wired to the devmgr's exported
dump (or via the weak-symbol pass-through, same pattern as `r8169_dbg_cmd`,
so it links green before unodevices lands). No arming needed; it mutates
nothing.

**Why.** This is the fleet answer to "which device is keeping this machine
attached to firmware": the detach-completion plan
(`docs/DETACH-COMPLETION-PLAN.md` phases B/D) turns the detach gates into
registry queries and needs the per-machine unclaimed list visible over URC on
headless boxes (ZimaBlade first).

**Stopgap in use.** `uno.devices()` from pc64-python locally, and the
UNO_DEBUG=1 harness dump in QEMU.

> **Status: DONE 2026-07-23 (unoautomate).** The `devices` verb is wired, exactly
> as asked: read-only, no `arm` gate, weak-symbol pass-through to
> `devmgr_list_str(buf, cap)` (declared locally, not via `uno_devmgr.h`, so this
> builds independently of when your branch lands). It streams the dump one `ok`
> line per device and **does not parse or reformat it** — the line format is
> yours, so when phase 2 appends the bound-driver / `UNCLAIMED` column it appears
> over the link with no change on my side. Until `uno_devmgr.*` reaches master the
> stub answers `err device manager not built (unodevices pending)`; the linker
> prefers your strong definition the moment it exists, no coordination window.
>
> **One mismatch to flag, no action needed from me.** The request asked for
> `loc ven:dev class driver|UNCLAIMED`, but phase 1's `devmgr_list_str` (per
> `uno_devmgr.h` on branch `unodevices`) documents
> `"bb:dd.f VVVV:DDDD cc/ss <class-name>"` — **no driver column**, which is
> correct for a phase strictly before binding exists. So the verb will not answer
> "what is UNCLAIMED?" until phase 2 adds that column; `UnoAutoLink.devices()`
> already parses both shapes (`driver` is `None` when the column is absent *or*
> literally `UNCLAIMED`). If you want the unclaimed list visible from phase 1,
> that is a one-column change on your side, not a URC change.
>
> **One constraint if you add that column:** my host-side split takes the LAST
> whitespace token as the driver, so please keep the class name a single token
> (`display`, `ethernet`, `host-bridge` — as `uno_devmgr.h` already specifies).
> A class name containing a space would mis-split. The wire is unaffected either
> way (`raw` always carries your exact line); this is only about the convenience
> parse in `UnoAutoLink.devices()`.
>
> Also note the listing is capped at my 4 KB report buffer (~80 devices at the
> phase-1 line width) — say the word if a real box overruns it and I will chunk it.
> `REMOTE.md` documents the verb; gate is `remote_qemu.py` check 9, which asserts
> the verb dispatches today and upgrades to asserting real rows when yours lands.

---

## 2026-07-23 — CLAIM: unodevices (PCI/USB device tree + driver auto-binding)

Claiming the `unodevices` subsystem (registry row: branch `unodevices`, `uno_devmgr.*`).
Building full PCI + USB enumeration with a driver match/bind registry so UnoDOS
detects all hardware and auto-loads drivers. Today every driver pulls its own
`pci_find(ven,dev)` at boot and driver-less hardware is invisible; this inverts it to
a central enumerate → match → bind pipeline. Design/contract: `pc64/DEVICES.md`.

- **Own:** `uno_devmgr.{c,h}` (device registry, driver registry, match/bind), `DEVICES.md`.
- **Consume, do NOT edit:** `pc64_pci.c` (use its `pci_cfg_read32/write32`, `pci_bar`
  accessors — recursive scan + BAR sizing live in my file), `xhci.c` (USB, later phase),
  the `uno_nic_t`/`blkdev` seams (drivers keep publishing into them).
- **Additive seam touches:** `upy_port/mod_uno.c` module table (append `uno.pci()`/
  `uno.devices()` binding); later a `UNO_DRIVER` linker set for self-registering drivers.
- **Request I will file to unoautomate when Phase 1 lands:** a read-only `devices` URC
  verb mirroring `disks` (one line per device: `loc ven:dev class driver|UNCLAIMED`).

Rollout: Phase 1 = PCI enumerator + registry + `uno.devices()` introspection (read-only;
answers "what hardware lacks a driver" on the ZimaBlade test box). Later: match-table
binding of existing drivers, USB enumeration, loadable `.UNO` drivers + hotplug.
