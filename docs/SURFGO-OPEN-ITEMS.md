# Surface Laptop Go: what is done, and the four things that are not

Written 2026-08-24 at the end of the detach session. Everything described as
done is on `origin/master` and was proven on the machine, not in QEMU.

## Done, and not worth re-testing

The Surface Laptop Go **detaches from its firmware** and runs UnoDOS entirely
on native drivers: the ELAN keyboard, the touchpad, the boot stick, and
`HV: eligible: yes`. The firmware's on-screen keyboard disappears, which is the
independent confirmation - it exists only while the firmware owns the machine.

Two bugs stood in the way, both landed:

- `find_xhci()` opened the FIRST xHCI in bus/dev/fn order. This machine has
  two - `00:0d.0 8086:8a13` (CPU-side Thunderbolt) sorts before
  `00:14.0 8086:34ed` (the PCH controller carrying the keyboard, touchpad and
  the USB-A port). It now opens the one the boot device path names.
- the root-port scan powered a port and sampled its connect bit a few thousand
  SPIN ITERATIONS later, once. Real ports read `pls=7` (mid link-training) and
  need a real interval. **This was the same "deadlines are durations, not spin
  counts" bug `USB.md` records for TRANSFERS a month earlier, still living in
  the port path.**

WiFi went from dead to **joined**: the AX201 loads firmware, goes ALIVE, scans
(24 APs), authenticates, associates, completes the 4-way and installs keys.

**2026-08-25: and now to NETWORKED.** `dhcp=LEASE ip=...` on metal, over
WPA2-PSK, on this machine. Item 2 below is closed; the receive ring was
starving after exactly one lap. Item 1 (SAE) and item 3 (retarget) are still
open, and item 4 is still a sizing decision rather than a bug. So this document
is now three things, not four.

## HANDOFF, 2026-08-26 (branch `surfgo-grp`, NOT landed)

A long session on `iwlwifi`. Read this before re-deriving anything; several
plausible theories are already dead and the runs that killed them are named.

### Landed and proven on metal

- **`grp=0` is CLOSED** - see the section under item 2. NimmuNet is the guest
  SSID with client isolation on. `ACCEPT_GRP` works.
- **Rejoin no longer reloads the firmware.** `iwl_join_ssid()` re-points the
  live contexts (`leave_bss()` + the retry path) instead of `device_stop()` +
  reload. **The firmware reload is retained as a fallback**, so the worst case
  is the old behaviour, never worse.
- **A rejoin has completed end to end with no reload** (2026-08-26 11:08 and
  again 12:39): `retry assoc -> N`, `retry 4-way COMPLETE`, lease, no
  `MVM init` line at all.
- **`retarget_ap()` re-points after a SUCCESSFUL association**, which item 3
  says cannot happen. `leave_bss()`'s `MAC_CONFIG assoc=0` is the likely reason.
  Item 3 is not closed - this wants its own verification.
- Four UI faults fixed (Control Panel + unoui): per-network passwords, the
  reveal eye's hit box, a scan that froze the desktop for five seconds, and a
  text view that never scrolled back. `unoui/tools/edit_test.sh` is green at 69
  assertions, including cases that would have failed before.

### Still broken, with the exact evidence

**A. The second firmware load of a boot asserts the UMAC.** Fully characterised
and reproducible byte for byte:

```
MVM init: the fw ASSERTED at "INIT_COMPLETE wait" / "INIT_EXTENDED_CFG"
UMAC error_id=201010a3 (ADVANCED_SYSASSERT) cmd=001c0c00 data1=3 data2=1 data3=deadbeef
LMAC error_id=00000071 (NMI_INTERRUPT_UMAC_FATAL)
```

`cmd_header` is `{u8 cmd; u8 group_id; __le16 seq}`, so `001c0c00` = group
`0x0c` cmd `0x00` = **NVM_ACCESS_COMPLETE** (`GRP_REGNVM` is `0xc` in
`iwlwifi.h`). Everything after is a dead radio - which is what "after a failed
join even scanning fails" is.

**The one asymmetry worth starting from:** reloading a firmware that has ALREADY
asserted works fine; reloading a HEALTHY one asserts. So whatever poisons the
second load is left behind by stopping a *running* firmware. Ruled out already:
NVM_GET_INFO sent twice (now once per boot, as upstream), a firmware killed
mid-flight (the upstream fw reset handshake is implemented and ACKs in 2 ms),
the command sequence (`IWL_INIT_NVM` is enum position 1, order matches
`iwl_run_unified_mvm_ucode()`), the command path (the fw's own `cmd_header`
proves it received the command), a partial DRAM map, and the APM stop.

### A, DIAGNOSED 2026-08-26 (branch `surfgo-fwreload`, fix built, not yet on metal)

**It is the command ring, and the proof was already in the two error tables
above.** `cmd_header` is `{u8 cmd; u8 group; __le16 seq}`, so `001c0c00` and
`00190c00` are the same command - NVM_ACCESS_COMPLETE - with **sequence numbers
28 and 25**. `send_cmd()` sets seq to the ring slot, and on a firmware that has
just loaded, NVM_ACCESS_COMPLETE is the SECOND command `mvm_init_unified()`
sends: slot 1, every time. A deterministic command cannot be at slot 25 in one
run and slot 28 in another unless the index was inherited from the session
before it - and `g_cmd_wr` only ever increments. Nothing reset it across a load.

A freshly loaded firmware reads the command queue from slot 0 (upstream
re-zeroes both pointers in `iwl_txq_init()` on every `start_fw`). So the first
command after a reload is written at slot 25 and the doorbell tells a firmware
whose read pointer is 0 that slots 0..25 are commands to run. The ring is 32
slots and a join spends far more than 32 commands, so those slots hold **the
last 32 commands of the previous session** - MAC_CONFIG, LINK_CONFIG, SEC_KEY,
SCD_QUEUE - replayed into a firmware with no contexts.

**The asymmetry falls out of it.** Reloading an already-asserted firmware works
because that session stopped early, so the replayed slots are the early init
commands the fresh firmware expects anyway.

`tx_queues_reset()` (in `bringup_to_alive()`, beside `rx_hw_init()`) zeroes both
host indices and wipes the descriptors and the payload buffers. It traces the
index it reset from, so the next Surface run says outright whether a second load
inherited one - **that trace line is what a metal run has to confirm**, together
with a rejoin whose reload no longer asserts.

The receive side has wiped itself per bring-up ever since a second bring-up read
the FIRST one's ALIVE out of RB 0. This was the same bug in the other ring: the
instance was fixed and the class was never swept for. Third time in this lane,
after "deadlines are durations, not spin counts" outliving its own fix by a
month in the port path.

**B. The re-point path is not yet reliable.** Two remaining failure modes, both
from the 12:39 boot:

- the re-point scan sometimes returns `complete=no(timeout) aps=0` outright
- a re-point that associates can fail its 4-way (`retry assoc -> 4` then
  `retry 4-way did NOT complete after 4000 ms`)

Both fall back to the reload, which then hits (A). Deafness itself IS fixed:
that scan read `aps=3` against a healthy 32 before `leave_bss()` deactivated the
link, and `aps=25` after.

### Theories that are DEAD - do not re-derive them

Each fitted the evidence and each was disproved by a run, not by argument:

1. **DTIM / beacon timing** for `grp=0`. The fw ACCEPTS the post-association
   link tune (`dtim_interval=200`, no assert) and `grp` did not move.
2. **`ACCEPT_GRP` being the wrong bit.** It is `BIT(2)` in the new MAC config
   API, checked against upstream `fw/api/mac-cfg.h`; it was always being sent.
3. **Repeated session-protection requests** (item 3's stated trigger). A guard
   that declines the second request never fired ONCE and the fw asserted anyway,
   on the first request of its join.
4. **A channel change under a live link.** Both BSSes were on channel 1 and no
   `PHY_CONTEXT` modify was issued.
5. **A malformed `SCD_QUEUE remove`** (`qid was -1`). Real, fixed - and it still
   asserted afterwards.
6. **A stale BSSID in the link context.** `mld_link_cfg()` never writes
   `ref_bssid_addr` at all, on any join, including every successful one.

### What actually worked, twice, when reasoning did not

**Ask the hardware.** The second-load assert survived three builds of
upstream-derived fixes and was named in ONE run by dumping the fw's own error
tables at the moment of the assert. The re-point failure survived four theories
and was explained by an AP count that had been in the log for hours.

Both diagnostics were unreachable by construction before this session - `iwl
fwerr` arrives over URC, and URC needs the network that just died. That is the
third instance of that shape here, after `g_rx_data_log` and `iwl mld a`. **If a
diagnostic can only be reached by typing a verb, it does not exist on the
machine that needs it.** Both now fire automatically.

### Practical notes

- `WIFINETS.CFG` is rewritten most-recent-first by `saved_remember()` on every
  successful join, and it is NOT in the ESP tarball, so it survives updates.
  Whichever network was joined last becomes the next boot's target - set it
  deliberately before a run or the experiment changes under you.
- Salvaged run logs are on devbuntu in `~/surfgo-run-*`; the last is
  `~/surfgo-run-125304-linkdown`.
- The branch builds clean both `UNO_DEBUG=0` and `UNO_DEBUG=1`. It has NOT been
  rebased onto a moved `master` or landed.

## 1. SAE's PMK is wrong  ·  the one with the most evidence

**Symptom.** WPA3-SAE authenticates and the 4-way then dies:

```
SAE: ACCEPTED - PMK established, PMKID e36b16f7...
join: assoc -> 1 (>=0 AID)
join: associated with "NimmuNet" aid=1 - pumping RX for the 4-way
EAPOL in (121 bytes, ki=0088 1/4) -> state 1, reply 143
DEAUTH from 30:29:2b:70:4f:cf reason=2
```

Deauthentication reason 2 arriving immediately after message 2/4 means the AP
could not verify our MIC. The MIC comes from the PTK, the PTK from the PMK, and
for SAE the PMK is the one thing authentication produced.

**The control, in the same log file.** Under WPA2-PSK, against the same AP, the
identical 4-way COMPLETES and installs keys. So PSK's PMK is right and the SAE
one is not. That is why the machine can join at all today: `find_and_join()`
falls back to PSK when a 4-way fails after a successful association.

**Where to look.** `sae_prepare()` / `wpa_arm()` in `pc64/iwlwifi.c`:
the 32-byte PMK copied out of `g_sae.pmk` into `wpa_sm_init()`, and the PMKID /
RSNE that goes into message 2/4 (`g_rsn_ie`, 44 bytes under SAE vs the PSK
case's shorter element). `tools/sae_test.*` is a host gate for the SAE maths
and it passes, so suspect the handover into the supplicant rather than the
exchange itself.

**Ruled out by this session, on this hardware:** the radio, the firmware image,
scanning, authentication, association, the supplicant state machine, and the
4-way message flow. Do not re-derive those.

## 2. No DHCP lease after a successful join  ·  **CLOSED 2026-08-25**

**Symptom.** The join completes (PSK), keys are installed, the station is
authorized - and `apps/network.c` polls `net_dhcp_done()` for ~9 s
(`apps/network.c`, the `net_init(nic, iwl_mac()); net_dhcp_start();` block) and
gives up with "Joined, but no DHCP lease yet." Nothing retries afterwards, so
the UI text is not an ongoing attempt.

**The lead worth taking first.** A DHCP OFFER is normally a BROADCAST, and
broadcast is decrypted with the GTK rather than the pairwise key. The handshake
does install one (`SEC_KEY idx=1 mcast=1 flags=4a`), but a group key that is
installed-and-wrong looks exactly like this: association holds, unicast works,
every broadcast reply is discarded in silence. This driver has a matching note
in its own history - *"the RX descriptor has been saying all along whether the
firmware decrypted, and nobody read it"* - so read it.

**What is not yet known** and what the next log should answer: whether DHCP
DISCOVER frames leave the NIC at all, and whether anything arrives and is
dropped. `iwl netres` reports `[tx=.. rx=.. arp=.. ip=..]`; on the X1 Carbon a
working lease reads `dhcp=LEASE ip=... ping=3/3` (`pc64/WIFI-F12-HANDOFF.md`
round 25).

### 2026-08-24, branch `surfgo-dhcp`: half of that is now known, and it moves the suspect

Read out of `CRASH/SURFGO` and `LOGS/SYSTEM.LOG` from the 14:40 boot, before
touching any code.

- **DISCOVER frames do leave the NIC.** `TX q=1 flen=309` every ~1.5 s for 32
  seconds, and 309 is exactly what this build assembles (24 header + 8 SNAP +
  277 of IP/UDP/BOOTP). Every one of them is ACKed - `ack_fail=0`. That half of
  the open question is closed.
- **Nothing whatsoever comes back.** Not a DHCP OFFER, not an ARP, not a
  neighbour's broadcast, not one data frame from any BSS on the channel, for
  the ~100 seconds between the keys going in and the shutdown. The per-frame
  `RXDATA` trace is armed to 16 frames at key install, spent 2 of them on the
  AP's re-sent EAPOL 1/4 and 3/4, and **printed nothing for the remaining 14
  slots**. On a 2.4 GHz mesh channel that is not a quiet minute.
- So the GTK is not yet the leading suspect, because **a wrong group key still
  receives frames**. `prot` would climb with `dec` at zero; it read `prot=0`.
  What this looks like instead is a receive path that goes deaf when CCMP comes
  up - or an AP that associates us and forwards nothing.
- **The one reading we had was taken 2.5 s in, and its verdict came from
  beacons.** `from_ap` counts every frame the AP transmits, beacons included,
  so "traffic flows BOTH ways - the link is fine and DHCP itself is the
  problem" was drawn from an AP announcing itself. The diagnosis now runs four
  rounds (2.5/9/20/40 s) and prints the receive funnel whole - `rb`, `mpdu`,
  `from_ap`, `bcn`, `uni`, `grp` - because which step stops naming a different
  subsystem is the entire point.
- **The retarget on the run that reached DHCP was clean** (`STA_REMOVE` then a
  fresh `STA_CONFIG`, not a re-point), and the association that succeeded
  negotiated `mfp=0`. Neither the stale-station fault nor PMF is in this.

### 2026-08-25: CLOSED. The receive ring survived exactly one lap.

**`wifi: post-join diag: dhcp=LEASE ip=...`, on metal, on the Surface Laptop
Go.** It was not the GTK, not the DTIM wake and not DHCP.

The run that answered it (2026-08-24 23:57, WPA2-PSK on `30:29:2b:70:4f:cf`)
came after seven that could not, because the diagnosis fired **once, 2.5 s in**,
and the whole question lives in the thirty seconds after that. Four rounds, and
the numbers stood still:

```
+2s   rb=2047 mpdu=371 | from_ap=7 bcn=2 uni=2 grp=0
+6s   rb=2047 mpdu=371 | from_ap=7 bcn=2 uni=2 grp=0
+10s  rb=2047 mpdu=371 | from_ap=7 bcn=2 uni=2 grp=0
+25s  rb=2047 mpdu=371 | from_ap=7 bcn=2 uni=2 grp=0
```

Not one number moved in twenty-three seconds while `tx` climbed 2 → 16 and every
DISCOVER was ACKed. The receive path did not lose broadcast and did not
mis-decrypt anything: it **stopped, whole**. And 2047 is `RXQ_N - 1` - one lap of
the ring, exactly.

`rx_restock()` rewrote nothing. Its comment said why: the free list is a static
identity mapping, slot i → rb i → vid i+1, so re-advertising an index was
believed to be the same as refilling it. `iwl_pcie_rxmq_restock()` writes
`bd[write] = page_dma | vid` on every restock instead, and this is the reason -
**the hardware CONSUMES a free descriptor**, so re-advertising a slot it has
already eaten hands it back whatever the eat left behind. One lap is exactly how
long a static free list can survive that.

With the descriptors rewritten, the same machine: `closed` advancing, `read`
tracking it, `zeros=0/2048`, and DISCOVER → OFFER → REQUEST → **lease**.

**Why nobody saw it for seven runs, which is the transferable part.** Every
earlier failure killed the link before 2048 RBs had gone by, so the ring never
reached its lap. It took a join that LIVED - `akm=psk` to keep WPA3-SAE (item 1)
out of the path, an SSID-aware `bssid=` pin, and a filter for the abandoned
attempt's EAPOL - before the bug underneath could even be reached. **Three
separate faults each had to be fixed before the real one became visible, and
each of them looked like the answer while it was the last one standing.**

Two lessons worth more than the bug:

- **A snapshot cannot see a stall.** Every rung of the verdict ladder reasoned
  about the SHAPE of one reading - which frames arrived and which did not - and
  on a frozen counter the shape is just the last instant before the stop,
  preserved. It read two unicast frames that arrived *before* the freeze and
  announced "unicast arrives and group-addressed traffic never does". The
  ladder now tests for movement between rounds first.
- **The diagnosis never printed its own question.** Eight runs about "no DHCP
  lease" and not one line said whether there was a lease; the first success had
  to be argued from `tx` having stopped climbing. `net_dhcp_done()` and
  `net_ip()` are public in `net.h` and it prints them now.

### `grp=0`, which this close left standing - **ALSO CLOSED, 2026-08-25**

It read: *"Not one group-addressed frame in 113 from the AP over ten seconds,
with `drop=0` - so they are not arriving at all rather than failing to decrypt.
ARP and every broadcast protocol will not be so lucky."* True, and not a fault.

**NimmuNet is the guest network and has CLIENT ISOLATION ON.** Client isolation
is exactly a refusal to forward traffic between clients, group-addressed traffic
included. Every `grp=0` in this lane was measured on a NimmuNet BSS, and the
first time the same measurement ran against SKYNET it was non-zero:

```
SKYNET e8:d3:eb:47:4e:c6, filter=04 (ACCEPT_GRP, no promisc)
+2s  from_ap=43 bcn=28 uni=6 grp=6 fgrp=0  foreign=0
+6s  from_ap=78 bcn=62 uni=6 grp=7 fgrp=0  foreign=0
```

`foreign=0` proves promisc was off, so `ACCEPT_GRP` alone is delivering
group-addressed data. `grp` moving 6 -> 7 proves it is a live reading and not a
frozen one.

**What it cost, and the transferable part.** A whole branch went into this - a
DTIM hypothesis, a post-association `LINK_CONFIG` tune, a `fgrp` control that
turned out unable to discriminate, and two firmware theories - because every
measurement was taken against ONE SSID and nobody varied it. The `bssid=` pin
existed the whole time and made that control a config-file edit with no rebuild
and no credentials. **When a reading is identical every time, vary the thing you
have been holding fixed before theorising about the thing you have been
changing.**

Worth keeping rather than rounding off: promisc delivered noticeably MORE of the
same AP's group traffic than `ACCEPT_GRP` did in the same window - 18 by +2 s
against 6. So the normal filter may still narrow WHICH group frames reach the
host. That is a far smaller question than this one was, and nothing is blocked
on it.

Landed on the way, all on `surfgo-grp`: the post-association link tune (the fw
ACCEPTS it - `dtim_interval=200`, no assert - it simply was not the problem),
`rxpromisc` as a DEBUG.CFG switch, an EAPOL length fix (this fw leaves four
bytes on under promisc and the supplicant was handed all of them, which killed
every 4-way in that mode), and honest before/after assert attribution in the
tune.

## 3. `retarget_ap()` cannot re-point after a successful association

**Symptom.** `join: could not re-point the contexts at the next BSS`, on the
third attempt, after an earlier attempt had gone all the way to association
(AID granted) on a different BSS. The BSS rotation is the whole reason the
retry loop exists, so when retarget fails the loop cannot do its job.

Related and probably the same area: **the third join attempt asserts the
firmware** (`session-prot asserted the fw on the retry`), after which every
scan returns `rb_total=0` and only a reboot recovers. Repeated
session-protection requests are the trigger.

### 2026-08-26: the stated trigger is WRONG, and this matters before anyone plans around it

"Repeated session-protection requests are the trigger" does not survive a test.
A guard was added that declines to send a second SESSION_PROTECTION_CMD while a
live one still has time in it - which is exactly what upstream's
`iwl_mvm_schedule_session_protection()` does - and on metal it **never fired
once**: every request in the boot was more than 300 ms after the one before. The
firmware still asserted, on a session-prot that was the FIRST of its join:

```
retarget: fresh TX queue -> qid=1 csr2808=00000000
session-prot: MAC_CONF 0x5 len=24 capa54=1 id=0 mld=1
join: session-prot asserted the fw on the retry
```

So the assert is about the COMMAND or the state it is sent in, not about how
many of them have gone out. The guard stays on the branch, because declining to
send what upstream declines to send is right on its own terms, but it is not a
fix for this and is not recorded as one.

Also from that run, and still open: after a rejoin that FULLY SUCCEEDED on the
re-point path, the firmware asserted again with nothing in the log between - the
next rejoin opens with `left the old BSS (MAC_CONFIG assoc=0) csr2808=02000000`,
already dead. That happened during ordinary post-join life, not during a join,
and no instrument watches there yet.

**Context you will want.** NimmuNet's `e8:d3:eb:47:4e:cf` is the loudest BSS on
the air and deauthenticates every completed handshake with reason 7. It is
already described in `iwlwifi.c` two screens above the join loop. `bssid=` in
**DEBUG.CFG** pins the join past it - no rebuild and no credentials needed,
which makes it the cheapest way to take the mesh out of an experiment.

### 2026-08-24, branch `surfgo-dhcp`: the pin was not working, and had not been

The cheapest lever in this document was broken, which is worth knowing before
anyone plans an experiment around it. On the 14:40 boot, two consecutive lines:

```
wifi: join: bssid= pins the first attempt to 30:29:2b:70:4f:cf
wifi: join: try 1/3 "NimmuNet" bssid e8:d3:eb:47:4e:cf chan 1 ...
```

It joined the exact BSS the pin exists to route around. Three faults, all fixed
on `surfgo-dhcp`:

- **`scan: aps=24` against a 24-entry table** is a table that was FULL, and a
  full table used to drop every further BSSID unrecorded, first come first
  served. The pinned BSS was simply never in the results. It now evicts -
  never a BSS carrying our SSID, never the pinned one - and `SCAN_AP_MAX` is 32,
  the real ceiling (`scan_pick_nth`'s `taken` is one bit per entry).
- **`read_bssid_override()` ran AFTER the scan**, so throughout it `g_pin_on`
  was 0 and nothing knew which BSSID to protect. It reads config files off
  mounted volumes and needs nothing from the radio; it is now first.
- **The log announced a pin nobody had checked was honourable.** It now says
  whether the pin is in effect, and names the scan's AP count when it is not.

## 4. F14: this machine cannot power itself off

**Characterised, not fixable in the power lane.** On a detached Surface Laptop
Go BOTH terminal mechanisms hang:

- the firmware's `ResetSystem(EfiResetShutdown)` never returns (the log ends at
  `unolog stopping`, the first line `power_down()` writes)
- `uacpi_enter_sleep_state(S5)` never returns either (the log ends at
  `acpi: S5 prepared, entering`)

Neither ignores the request and returns; both swallow the thread. `power_down()`
now tries S5 first, the firmware last, and draws "The firmware refused every
method we have" BEFORE the first terminal attempt, so the machine reports
honestly instead of freezing on a blank splash.

**What would actually close it** is the Surface Aggregator Module's own
power-off path: a framed protocol over a UART (SYN, header, CRC16, per-command
sequence numbers and ACKs), request/response plumbing, and it must work
post-EBS. `docs/SURFACE-KEYBOARD.md` sized that transport for a different
purpose and the sizing still applies. **That is a deliberate decision to make,
not something to slip into another session** - and note the keyboard half of
that document is now REFUTED: this machine's input is USB HID (`04f3:0c5b`,
two boot interfaces on `00:14.0`), not SAM.

## Traps this session paid for, which will otherwise be paid again

- **`__attribute__((weak))` does not resolve predictably on PE/COFF.** mingw
  emits the weak definition as a real local symbol plus a default resolution
  (`T .weak.unolog_tap.dbg_vec0` and `w unolog_tap` in one object, `T
  unolog_tap` in another) and which one a call site binds to depends on LINK
  ORDER. The system log captured every kernel line in one build and none in the
  next, from source changes touching neither file. **AGENTS.md section 2
  recommends weak symbols as the seam pattern; on this target prefer a
  registered function pointer** (`uno_dbg_set_log_tap`, `cef678ec`).
- **`uno_fs_*` and `uno_fat_*` are different index spaces**, mapped by
  `uno_fs_fat_index()`. A detach RENUMBERS volumes - the boot stick is volume 1
  before ExitBootServices and volume 2 after - so a cached index does not miss,
  it addresses a DIFFERENT DISK.
- **The production build overwrites `build/esp`.** Running `UNO_DEBUG=1
  ./build.sh` and then `./build.sh` and staging afterwards ships a PRODUCTION
  image, which has no `uno_dbg_log` at all. Two metal runs were spent on empty
  logs. Build production FIRST and debug LAST, and check `BUILD.TXT` at flash
  time.
- **A hang loses the system log.** unolog flushes from the shell's main loop,
  and a join blocks that loop; `find_and_join()` now flushes per attempt. When
  it still goes missing, `CRASH\<MACHINE>\BOOTLOG.TXT` carries the kernel ring
  and is rewritten periodically, so it survives what SYSTEM.LOG does not.
- **Linux leaves a partition flagged read-only** after an `-o ro` mount or an
  unclean pull. `blockdev --setrw /dev/sdX1` AFTER unmounting, or every write
  fails silently.
- **A gate that asks "is there a bootable OS here" is not asking "is this MY
  volume".** `vol_carries_system()` accepted another OS's ESP and UnoDOS wrote
  telemetry onto a Windows boot partition, which cost that installation. The
  USB case is guarded now (`uno_usbmsc_boot_bound()`); the AHCI/NVMe/SDHCI arms
  are still filed as a request to unofs in `pc64/UNOAUTOMATE-REQUESTS.md`.
