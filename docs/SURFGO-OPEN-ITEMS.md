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

## 2. No DHCP lease after a successful join

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
