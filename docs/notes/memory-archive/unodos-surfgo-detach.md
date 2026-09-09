# Surface Laptop Go full detach and Ice Lake xHCI bring-up

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-surfgo-detach.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---

**Branch `surfgo-xhci`, worktree `C:\Users\arin\unodos-surfgo`, plan
`docs/SURFGO-DETACH-PLAN.md`, claim filed in `pc64/UNOAUTOMATE-REQUESTS.md`
(2026-08-23).** Lane = usb stack (`xhci.* usbio.* usbhid.* usbmsc.* usbboot.*`).

**The keyboard question is CLOSED: USB, not SAM.** The firmware's UsbIo list
on the Surface is `04f3:0c5b` x2 (ELAN, HID boot class 03/01: kbd + touchpad),
camera `05c8:03e2`, BT `8087:0026`, and the stick. All on the one Ice Lake-LP
xHCI `8086:34ed`. `docs/SURFACE-KEYBOARD.md`'s SAM sizing is refuted.

**The box DETACHES and then hangs on "starting desktop"** (build
`debug-local-20260823-0326`, master `be1cf7a4`, boots 1+2 on 2026-08-23):
that splash line is painted AFTER `try_detach()` returns and a stranded stick
paints "detach failed" instead, so EBS, the xHCI takeover, the usbmsc LBA-0
read and the HID claim all completed. The hang is in the post-splash window
(`uefi_main.c:1394-1433`): `uno_dbg_on_detach` (IDT/LAPIC) or the FIRST
native `WRITE(10)` (telemetry) over usbmsc. `vm-selftest` was a red herring.
With `DETACH.CFG: off` the desktop is up in 8 s and input works (firmware).

**Why nothing was logged:** the RAM stash at `0x1F00000` survives only a
WARM reset and is flushed as `CRASH\SURFGO\RS###`/`HG###` on the next boot
after an UNCLEAN reset; holding the power button is cold and loses it. The
watchdog is armed only after `try_detach()` returns (blind window), and
`xhci.c` has zero `uno_dbg_log` calls (all 0x402 debugcon, SMM-trapped on
this firmware). No serial, WiFi never ALIVE (F12), so the FRAMEBUFFER + the
stash are the only channels. Phase 0 of the plan = screen tracer + early
watchdog + xhci logs; Phase 1 = USBLEGSUP handoff (NOT implemented anywhere),
PMCSR D0, open the boot path's controller (Ice Lake-U may have a CPU-side
xHCI `8a13` at 00:0d.0 that enumerates before the PCH one), timed deadlines.

**Stick logistics:** the Surface stick is the Verbatim `18a5:0250` (30 GB,
label CRUZER, `D:` on carbon 192.168.2.19); update in place per
[[unodos-stick-update-in-place]], clear `CRASH\SURFGO` before every boot,
archive each result under `~/unodos-metal-reports/surfgo-2026-08-<n>/`.
Newest silicon the xHCI stack had driven before this: Comet Lake-U (Yoga).
F14 (Restart/Shut Down hang on this box) is reopened and matters because the
collection loop needs CF9 to work.

**★ IT DETACHES (2026-08-23, build `debug-local-20260823-2115`).** Desktop on
fully native drivers, `usb-hid: kbd=1 mouse=1`, `msc: up`, `HV: eligible: yes`,
and **the firmware OSK disappeared** - independent proof, since the OSK exists
only while the firmware owns the machine. Report:
`~/unodos-metal-reports/surfgo-2026-08-23-DETACH-OK/`.

**Two bugs stood between us and that, and the SECOND is the one that fixed it:**

**★ 2026-08-23: THE SURFACE HAS TWO xHCI CONTROLLERS AND WE OPENED THE WRONG
ONE, AND IT BRICKED THE LAPTOP.**

```
xhci inv: 00:0d.0 8086:8a13  CPU-side Thunderbolt/USB4
xhci inv: 00:14.0 8086:34ed  PCH - keyboard, touchpad, USB-A  <- THE BOOT PATH'S
```

`find_xhci()` took the first in bus/dev/fn order, so it brought up the EMPTY
Thunderbolt controller. **The failure does not announce itself: bring-up
SUCCEEDS, the controller runs, it enumerates nothing.** Fixed (`bb7615b2`) by
latching the boot device path's PCI dev/fn in usbboot's `evaluate()` (pre-EBS,
so production gets it too) and preferring it.

**The second bug is the one that did the damage.** `vol_carries_system()`
accepts `EFI\BOOT\BOOTX64.EFI` with an `MZ` - the removable-media fallback
path EVERY UEFI OS installs to. With the stick gone, SDHCI brought up the
INTERNAL eMMC, its **Windows ESP** satisfied the test,
`uno_fat_native_eligible()` said "not stranded", and UnoDOS wrote telemetry
onto Windows's boot partition through an SDHCI driver never run on metal. The
write hung; **the Surface now boots neither USB nor its own eMMC.** Guard
added (`035a0c88`): on a USB boot only `uno_usbmsc_boot_bound()` counts, and a
stranded machine writes NOTHING. The real fix (match volume IDENTITY by BPB
serial, not a filename any OS provides) is filed as a request to unofs.

**★ THE SECOND BUG, AND THE ACTUAL FIX: THE PORT SCAN LOOKED BEFORE ANYTHING
COULD HAVE APPEARED.** With the right controller open it still reported
`ports=0 devs=0` from silicon with a keyboard wired to it. `xhci_bringup`
powered each port and sampled its connect bit a few thousand SPIN ITERATIONS
later, ONCE. Real ports read `pls=7` (Polling, mid link-training); at a real
120 ms settle all four were up. **This is the same "deadlines are durations,
not spin counts" bug USB.md records for TRANSFERS in July, still living in the
port path a month later - when a codebase fixes a bug class, grep for the
class.** Also implemented (never existed before): the **xHCI 1.2 s7.1.1
BIOS-to-OS handoff**. On this machine the BIOS did NOT hold the ownership
semaphore but DID have three SMI status bits latched (`e0010000 -> 00010000`).
Hygiene, not the blocker - the timing was.

**Lesson worth more than the code: a gate that asks "is there a bootable OS
here" is not asking "is this MY volume", and on a dual-boot machine the
difference is someone else's disk.** Also: `sdhci.c` waits are iteration
counts, not durations, so first contact with new silicon can spin forever.

**Still open on this box:** F14 - **Shut Down hangs even DETACHED**, where the
path is ACPI S5 and not the firmware ResetSystem that was fixed in `53d746ed`;
so a hang here cannot self-report via a warm reset and the RAM stash is lost to
every forced power-off. The post-detach env block costs **~26 s** because it
re-benchmarks the framebuffer and post-detach `blt bench` is unavailable, so it
measures the direct path against uncached VRAM. AX201 still never ALIVEs (F12),
so the appliance has no network here yet.

## WiFi on the Surface, 2026-08-24: JOINED (over WPA2-PSK)

**The scan killer was the DMA arena.** `arena_init_lowmem()` asks the firmware
for memory under 4 GB because the boot ROM and RX ring address it with 32 bits;
its fallback is `g_arena_static`, a **.bss array**, and .bss is wherever the
firmware loaded the kernel - on this box `image_base 0x140000000`. A detached
machine never gets the good allocation (no boot services left), so the arena
landed at 0x1_43add000 and **the card went ALIVE, read its MAC, and received
NOTHING**: `rb_total=0 mpdu_seen=0 aps=0`, which reads from the desk as "the
card works but will not join". Fixed by reserving pre-EBS beside
`uno_modload_reserve`/`uno_vmm_reserve`. After it: 24 APs, 8 networks.

**★ SAE AUTHENTICATES AND ITS PMK IS STILL WRONG.** With the join pinned to a
good BSS: `SAE: ACCEPTED - PMK established`, `assoc -> 1 (AID 1)`, we send 2/4,
and the AP replies **DEAUTH reason 2** - after 2/4 that means the MIC did not
verify, and the MIC comes from the PTK from the PMK. **The control is decisive:
WPA2-PSK against the SAME AP completes the identical 4-way and installs keys.**
So the bug is in `sae_prepare()`/`wpa_arm()` - the PMK handed to
`wpa_sm_init`, or the PMKID/RSNE in 2/4 - and NOT in the radio, association,
supplicant or firmware, all of which are now proven on this hardware.

**A DONE supplicant froze the machine.** After a deauth the handler cleared
`g_wpa_active` but the supplicant object kept `WPA_ST_DONE`, and `handle_eapol`
never checked the flag - so a retried 1/4 got `reply 0` (no 2/4 ever sent), the
AP deauthed, and we re-accepted 3/4 and re-declared success. 21 times, every
200 ms, forever; the join runs ON THE SHELL MAIN LOOP so the desktop froze.

**The mesh has a documented-bad BSS**: `e8:d3:eb:47:4e:cf` is the loudest on
NimmuNet and deauths every completed handshake (reason 7). `bssid=` in
**DEBUG.CFG** pins the join past it - no rebuild, no credentials.
`retarget_ap()` also fails to re-point after a successful association, so BSS
rotation is unreliable; and a third join attempt asserts the firmware, after
which every scan returns rb_total=0 until reboot.

## Traps worth more than the fixes

- **`__attribute__((weak))` DOES NOT RESOLVE PREDICTABLY ON PE/COFF.** mingw
  emits the weak definition as a REAL local symbol plus a default resolution
  (`T .weak.unolog_tap.dbg_vec0` + `w unolog_tap` in one object, `T unolog_tap`
  in another), and which one a call site binds to depends on LINK ORDER. The
  system log captured every kernel line in one build and none in the next from
  source changes touching neither file. **AGENTS.md recommends weak symbols as
  the seam pattern; on this target use a registered function pointer instead.**
- **`uno_fs_*` and `uno_fat_*` are DIFFERENT INDEX SPACES**, mapped by
  `uno_fs_fat_index()`. unolog cached its volume forever and a detach RENUMBERS
  them (the stick is vol 1 before EBS and vol 2 after), so its file sink pointed
  at another disk.
- **The production build overwrites `build/esp`.** Running `UNO_DEBUG=1
  ./build.sh` then `./build.sh` and staging afterwards ships a PRODUCTION image
  that has no `uno_dbg_log` at all - two metal runs were spent on empty logs
  because of it. **Stage only from a verified debug build; check BUILD.TXT.**
- **A hang loses the log**: unolog flushes from the shell main loop, which the
  join blocks. Only `BOOTLOG.TXT`'s kernel ring (rewritten later) survived.
- **Linux leaves the partition flagged read-only** after `mount -o ro` or an
  unclean pull: `blockdev --setrw /dev/sdX1` AFTER unmounting, or the next
  write silently fails.
- **F14 is characterised, not fixable here**: on this machine BOTH the
  firmware's `ResetSystem(Shutdown)` AND `uacpi_enter_sleep_state(S5)` never
  return. It reports honestly now; powering off needs a SAM driver.

See [[unodos-chromium-in-unovirt]] (the goal), [[unodos-detach-usb-phase-a]],
[[unodos-launch-conformance-2026-08]].

## 2026-08-25: NETWORKED. `dhcp=LEASE ip=...` on metal, over WPA2-PSK.

**The bug was the RX ring, and it survived exactly ONE LAP.** `rx_restock()`
advanced the free-list write index and rewrote no descriptors, on the reasoning
that the list is a static identity mapping (slot i -> rb i, vid i+1) so
re-advertising an index refills it. **The hardware CONSUMES a free descriptor**,
so re-advertising an eaten slot hands it back whatever the eat left behind:
`rb=2047` against `RXQ_N` 2048, then the receive path stops dead with the
association still up and every transmit still ACKed. `iwl_pcie_rxmq_restock()`
writes `bd[write] = page_dma | vid` every time, and this is why.

**It took SEVEN metal runs to reach that bug**, because three other faults each
killed the link first and each looked like the answer while it was the last one
standing: a scan table full at 24 silently dropping every further BSSID; a
`bssid=` pin announced-but-not-honoured and then honoured-but-not-SSID-checked
(a NimmuNet BSSID aimed a SKYNET join at the wrong radio); and the ABANDONED
attempt's EAPOL tearing down the association that replaced it. **When a bug
needs a link that LIVES in order to be visible, every fault above it has to fall
first, and each will impersonate the answer on the way.**

Two lessons that generalise past WiFi:

- **A snapshot cannot see a stall.** A verdict drawn from the SHAPE of one
  reading - which things arrived and which did not - is, on a frozen counter,
  the last instant before the stop preserved. It read two unicast frames that
  arrived BEFORE the freeze and confidently announced "unicast arrives and
  group-addressed traffic never does". **Test for movement between samples
  before interpreting any shape.**
- **A diagnostic that never prints its own question.** Eight runs about "no DHCP
  lease" and not one line said whether there was a lease; the first success had
  to be inferred from `tx` having stopped climbing.

**Levers now in DEBUG.CFG, no rebuild and no credentials:** `bssid=` (pins one
BSS, and now only applies to a BSS carrying the SSID being joined) and `akm=psk`
/ `akm=sae` (pins the AKM, no fallback) - the latter is what takes the SAE bug
out of the path so anything else can be measured at all.

**STILL OPEN and it is the ORIGINAL suspicion after all:** `grp=0` - not one
group-addressed frame in 113 from the AP over ten seconds, with `drop=0`, so
they never arrive rather than failing to decrypt. The lease landed only because
that server UNICAST its OFFER; ARP and every broadcast protocol will not be.

**Two environmental facts that cost runs:** RSSI is not a constant (the BSS that
held 100 s read -25 dBm; later runs on the same BSSID read -57..-66 and got
kicked), and **a run of failed handshakes gets the client shunned** - `assoc ->
-4097` from a BSS that had accepted PSK twenty minutes earlier. Rest the APs a
few minutes between runs.

## 2026-08-26: the second-load assert is THE COMMAND RING, and the proof was already on disk

**`grp=0` is CLOSED** (NimmuNet is the guest SSID with client isolation on;
`ACCEPT_GRP` works), and rejoin no longer reloads the firmware - it re-points the
live contexts, with the reload kept as a fallback. That fallback is what runs
into this:

**The second firmware load of a boot comes up ALIVE and then UMAC
ADVANCED_SYSASSERTs, because `g_cmd_wr` was never reset across a load.** The
firmware's own error table decided it, from two runs already recorded:
`cmd=001c0c00` and `cmd=00190c00`. The header is `{u8 cmd; u8 group; __le16
seq}`, so both are NVM_ACCESS_COMPLETE at **seq 28 and seq 25** - and `send_cmd()`
sets seq to the ring slot, while that command is the SECOND one
`mvm_init_unified()` sends into a fresh firmware: **slot 1, every time**. A
deterministic command cannot be at slot 25 in one run and 28 in another unless
the index was inherited. A just-loaded firmware reads the queue from slot 0
(upstream re-zeroes both pointers in `iwl_txq_init()` on every `start_fw`), so
the doorbell then hands it slots 0..25 - **the last 32 commands of the previous
session** (MAC_CONFIG, LINK_CONFIG, SEC_KEY, SCD_QUEUE) with no contexts to run
them against. It also explains the asymmetry that had no explanation: reloading
an ALREADY-ASSERTED firmware works because that session stopped early, so the
replayed slots are early init commands the fresh firmware expects anyway.

**`rx_alloc_lists()` has wiped the receive side per bring-up ever since a second
bring-up read the FIRST one's ALIVE out of RB 0. This was the same bug in the
other ring.** Third time in this lane that an instance was fixed and the class
was never swept for (after "deadlines are durations, not spin counts" outliving
its own fix by a month in the port path). **When this codebase fixes a bug
class, grep for the class.**

Fix = `tx_queues_reset()` beside `rx_hw_init()` in `bringup_to_alive()`: both
host indices, the descriptors AND the payload buffers, plus `g_data_qid`. It
traces the index it reset from. **LANDED ON MASTER 2026-08-26** (`5749122a`,
merge gate green: both builds, every host gate, SPECTEST 87/0 in QEMU). The
`surfgo-fwreload` branch and the `unodos-surfgo` worktree are both deleted -
make a fresh worktree off `origin/master` to pick this lane back up.

**★ CLOSED ON METAL, 2026-08-26 14:08.** Four forced reloads in one boot,
inherited indices **24, 21, 29, 21** - every one non-zero, the predicted cause
reading back off the machine - and every one went ALIVE, `INIT_COMPLETE
arrived`, 4-way complete, `dhcp=LEASE`, alternating SKYNET (192.168.2.52) and
NimmuNet (192.168.10.21). `csr2808=00000000` throughout and **the string
"assert" does not appear anywhere in the run's logs.** Before this the FIRST
reload took the radio down until a reboot.

**New DEBUG.CFG lever: `fwreload`** - a rejoin takes the firmware-reload path
instead of re-pointing. Without it the second load can only be reached by first
making a re-point fail, which puts the experiment downstream of a failure.
Levers are now read by `iwl_join_ssid()` as well as `find_and_join()`.

**Both sticks are staged at `debug-local-20260826-1753`** (2026-08-26), verified
byte-identical to the build bar DEBUG.CFG: the **Verbatim** (`/dev/sdb1` on
devbuntu, label CRUZER) for the Surface, with
`nostress/noshutdown/bssid=e8:d3:eb:47:4e:c6/akm=psk/fwreload` and `CRASH\SURFGO`
cleared; the **SanDisk Cruzer Glide** (`/dev/sdc1`, label UNODOS) for the
ZimaBlade, DEBUG.CFG preserved verbatim. What the run must show: `cmd ring reset:
host index was N` reading NON-ZERO on the second load, and a rejoin under
`fwreload` that does not assert.

**devbuntu's write-blocker needs `blockdev --setrw` on BOTH the disk and the
partition** - setting it on `/dev/sdX1` alone still mounts read-only, and the
failure looks like a stuck ro flag rather than a policy. See
[[devbuntu-image-writing]], which says so and which I did not follow.

## ★ THERE HAS NEVER BEEN A BOOT AUTO-JOIN, AND THE LOG SAID SO EVERY BOOT

Same run, found from the operator's side ("no auto join, I had to manually
connect"): the box boots, the radio comes up, a remembered network is in range,
and nothing joins until the Network app is opened by hand - which joins on the
first try.

```
14:08:12  wifi: rejoining the last network "SKYNET" (psk_len=24)
14:08:17  wifi: ALIVE reached in the NORMAL path - MVM/join sequence gated off
14:08:17  == net test done: WIFI FAIL (bring-up stopped) ==
```

**`g_mvm_arm` gates everything past ALIVE - MVM init, scan, auth, assoc, the
4-way - and the only thing that ever set it was `radio_up()`, the GUI's entry
point.** So the boot path announced its intent and stopped one step short of it,
on every boot this driver has ever had. The gate is from July, when nothing past
ALIVE had run and running it inline wedged the rig; it has been the ordinary GUI
path for a month. Now a bring-up that HAS credentials arms it, one that does not
still stops at ALIVE, and `iwl mvm` still drives it by hand.

**The lesson is the one this lane keeps paying for: a diagnostic line that names
a fault is not the same as anyone reading it as one.** Two boots printed "WIFI
FAIL (bring-up stopped)" and it was filed as the machine's normal boot.

Stick state: the **Verbatim** carries `debug-local-20260826-1814` (the boot-join
arm), staged and verified 2026-08-26, `CRASH\SURFGO` + `LOGS\` cleared. The
**SanDisk Cruzer** went to the ZimaBlade at `debug-local-20260826-1753`, which
is the same code WITHOUT that arm. Salvaged runs on devbuntu:
`~/surfgo-run-20260826-140958-fwreload` is the one that closed the assert.
