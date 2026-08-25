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

**Still open, and it is the original suspicion after all:** `grp=0`. Not one
group-addressed frame in 113 from the AP over ten seconds, with `drop=0` - so
they are not arriving at all rather than failing to decrypt. The lease landed
because this server unicast its OFFER; ARP and every broadcast protocol will
not be so lucky. That is the next thing in this lane and the GTK / `ACCEPT_GRP`
path is where to start.

## 3. `retarget_ap()` cannot re-point after a successful association

**Symptom.** `join: could not re-point the contexts at the next BSS`, on the
third attempt, after an earlier attempt had gone all the way to association
(AID granted) on a different BSS. The BSS rotation is the whole reason the
retry loop exists, so when retarget fails the loop cannot do its job.

Related and probably the same area: **the third join attempt asserts the
firmware** (`session-prot asserted the fw on the retry`), after which every
scan returns `rb_total=0` and only a reboot recovers. Repeated
session-protection requests are the trigger.

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
