# pc64 WiFi (Intel AX201 iwlwifi) and USB Ethernet drivers

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-wifi-usbeth-drivers.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---

Added 2026-07-20 to UnoDOS pc64 (all publish the `uno_nic_t` link service, so
the existing net.c stack + the probe chain in pc64_http.c `pc64_net_up` picks
them up: e1000 → ax88179 → **rtl8152** → **iwlwifi**):

- **rtl8152.c/.h** — Realtek RTL8152/8153/8155/8156 USB Ethernet (docks/dongles),
  the sibling of ax88179.c; OCP-register-over-control-transfer + bulk frames,
  per-version init, matches Realtek + common dock VIDs then confirms via the
  version register. Uses new async bulk-IN in xhci.c (`uno_usb_bulk_in_arm`/
  `_poll` + an event router) so recv never blocks. COMPILES CLEAN, inert when
  UNO_XHCI is off (the shipped default) — so the e1000 QEMU regression passes.
- **wifi_wpa.c/.h** — reusable WPA2-PSK supplicant on in-tree BearSSL (PBKDF2/PRF
  key derivation, EAPOL 4-way handshake, RFC-3394 AES key-unwrap for the GTK).
  Host-testable, hardware-independent.
- **iwlwifi.c/.h** (~1400 lines) — Intel AC (7260..9560) + AX (AX200/AX201/AX210)
  PCIe WiFi: full transport (CSR/APM/reset, gen1 legacy fw DMA + gen2 context-info
  + gen3 v2/IML/PNVM, TFD/TFH queues, RB rings, all polled), .ucode TLV parser,
  MVM command layer (post-alive init, PHY/MAC/binding/quota/ADD_STA/scan/assoc),
  eth↔802.11, keys installed for hw CCMP. Built from Linux-source-distilled
  register/struct references (kept in the session scratchpad at the time).

**Key non-obvious facts:**
- **HARDWARE-PENDING / not runtime-verified.** QEMU has no Intel-WiFi model, so
  iwlwifi is verified only to be *inert* when no supported card is on PCI. Real
  bring-up (primary target: X1 Carbon Gen 8 = AX201, a gen2 Qu/QuZ part) and the
  firmware-version command-struct variance are the metal tail. rtl8152 likewise
  needs a real dongle on real xHCI.
- **Firmware is NOT in the repo** (Intel/Realtek licence). User copies Intel
  `.ucode` (+ `.pnvm` for AX210) from linux-firmware onto the ESP under
  `FIRMWARE\` in 8.3 names; `WIFI.CFG` (ssid=/psk=) holds creds. Mapping table +
  format are in pc64/NETWORK.md. WPA2-PSK/CCMP only.
- Built with `UNO_STUDIO=0` (build.sh had another session's STUDIO WIP that
  references apps/ucc.c / ucc_x64.c which don't exist yet, so the default target
  won't build without that flag).

Follow-up (same day) - readout + firmware pipeline + flasher DEPLOYED:
- **Network app WiFi readout**: apps/network.c has an "Intel WiFi" section
  (iwl_present + iwl_status_str) + a "Connect WiFi" button (key W); exports added
  to pc64_modload.c. Verified rendering in QEMU (inert "No Intel WiFi card ...";
  e1000 self-test still passes).
- **Firmware pipeline**: tools/fetch-fw.sh (dev) populates gitignored
  pc64/fw-blobs/ from the Debian firmware-iwlwifi .deb (gitlab raw 404s - use the
  Debian pool non-free-firmware); build.sh's `[fw]` step copies them into
  build/esp/FIRMWARE/ (IWLAX200/201/210.UCO + IWLAX210.PNV) when fw-blobs/ exists
  - TEST images only. AX201 uses QuZ-a0-hr-b0 (X1 Gen 8).
- **tools/uno-wifi-fw.py** - cross-platform (Win/Mac/Linux, stdlib only, unpacks
  the .deb in-process) end-user tool: downloads the right blob, finds the UnoDOS
  USB, writes FIRMWARE\ + starter WIFI.CFG. --card/--dest/--source local/
  --list-cards; launchers uno-wifi-fw.cmd/.command. Tested AX201 + AX210.
- **Driver fix**: iwlwifi choose_firmware() picks IWLAX200.UCO for the discrete
  AX200 (device 0x2723), IWLAX201.UCO for CNVi.
- **DEPLOYED** to \\behemoth\unreplicated\unodos\pc64\ (UnoDosFlasher.exe 5.3MB +
  .img.gz + unodos-pc64.iso 43MB), all UNO_STUDIO=0 with the firmware bundle.
- STILL NOT committed: build.sh carries BOTH my edits and the other session's
  STUDIO WIP in the same file, so a non-interactive commit can't exclude their
  work (user forbade committing it). User resolves the build.sh split later.

Round 3 (same day) - full iwlwifi table + Realtek + Marvell WiFi:
- The STUDIO work is now MERGED to mainline (apps/ucc.c present); build.sh builds
  without UNO_STUDIO=0, and my WiFi work is cleanly isolated (modified/untracked)
  so it COULD be committed cleanly - but still NOT committed (user hasn't asked).
- **iwlwifi full device table**: fixed the AX200 bug (0x2723 was wrongly in the
  9000 case), expanded identify_by_pci to the ENTIRE Linux MVM device list
  (7000/8000/9000/22000/AX210 + WiFi-7 Bz/Sc best-effort), reject iwldvm-only
  parts (5000-6000/1000/2000). Firmware sub-splits: 9260(th) vs 9560(pu),
  AX210-Ty vs AX211-So/Ma, Bz/Gl/Sc each own IWL*.UCO+PNV.
- **rtwifi.c/.h** - Realtek PCIe WiFi rtw88 (8822/8821/8723/8814) + rtw89
  (8852/8851/8922). Register-exact: identification, fw-download state machines,
  rings/descriptors (+ mandatory rtw88 TX-desc XOR checksum), H2C (HMEBOX mbox /
  CH12 packet), CCMP key install (direct CAM / SEC-CAM H2C), RX C2H demux.
- **mrvlwifi.c/.h** - Marvell/NXP mwifiex (88W8897/8997/8766). Fully-polled
  fw-download, PFU rings, TxPD/RxPD, HostCmd/event, V2 CCMP KEY_MATERIAL.
- BOTH are MORE metal-pending than iwlwifi: the chip pwr_on_seq / rtw89 DMAC-CMAC
  init tables and the Marvell PCIE_DESC_DETAILS body are NOT reproduced (large
  per-chip Linux tables), so MAC bring-up is scaffolded. Compile clean, inert
  when absent (QEMU regression passes, e1000 still works).
- Wired into build.sh, pc64_http.c probe chain (e1000→ax88179→rtl8152→iwlwifi→
  rtwifi→mrvlwifi), pc64_modload.c exports, Network app WiFi readout (all 3).
- **uno-wifi-fw.py extended**: Realtek/Marvell firmware pulled DIRECT from
  kernel.org linux-firmware /plain (Debian splits them / gitlab raw 404s); Intel
  still via the Debian firmware-iwlwifi .deb. --card now covers rtl88xx/rtl885x/
  rtl8922a/w8897/w8997/w8766; auto-detect knows 0x10ec + 0x11ab/0x1b4b.
- Flasher REDEPLOYED to \\behemoth (BOOTX64.EFI ~1.16MB, all 5 drivers).

Round 4 (2026-07-21) - COMMITTED + the boot-time network test harness
(`d35d1b4` on pc64-debug-stress, pushed; flasher deployed to \\behemoth):
- The network stack turned out to be already committed at 53c5740 (the earlier
  "uncommitted" state resolved when master fast-forwarded); today's work sits
  on the DEBUG branch.
- **usbio.c** - EFI_USB_IO transport: USB NICs now work while firmware-ATTACHED
  (no xHCI takeover, which would strand the USB boot stick = F8). ax88179 +
  rtl8152 are dual-transport; UsbIo attached / native xHCI detached. QEMU
  UsbIo enumeration verified (0 handles with no USB devices - clean).
- **pc64_nettest.c** (UNO_DEBUG): one-shot net test before the stress driver.
  USB eth present -> eth test, WiFi SKIPPED (arin's rule); else WiFi with full
  stage trace; else wired PCI NIC (QEMU e2e via nettest_stage.py: e1000+SLIRP
  DHCP 681ms + 3 pings PASS). Log: CRASH\NETLOG.TXT, line-flushed; net:*
  checkpoints; traces feed the watchdog heartbeat (a >20s join must not reset).
  STRESS.CFG `nonet` skips it.
- **iwlwifi traces name the failing stage per laptop** (card id/fw file/creds/
  BAR0/prepare/RF-kill/APM/fw-load/ALIVE/MVM/join). HONEST GAP stated in-log:
  find_and_join is MLME-scaffolded (broadcast BSSID, no beacon parse, no
  auth/assoc) - a laptop reaching "join:" proved transport+fw+commands; a real
  association CANNOT complete until the MLME tail is written.
- **Creds**: WIFI.TXT accepted alongside WIFI.CFG. Template staged at
  \\behemoth\unreplicated\unodos\pc64\testkit\wifi.txt (ssid=/psk= FILL-ME-IN);
  the flasher dev-options folder-copy (defaults to that testkit) puts it on
  every stick. ARIN FILLS IN THE REAL CREDS on the share.
- fw-blobs fetched (Debian firmware-iwlwifi 20260622-1): AX201 (X1/Yoga/
  Surface), IWL8000 (Latitude 8260/8265), 9000/9260/AX200/210/211 - bundled
  into the deployed image's FIRMWARE\.
- Known stale: nettest.py's TLS scenario still screenshots Control Panel (its
  key-nav predates the launcher overhaul; guest no longer dies mid-run though
  - it now stashes STRESS.CFG). Datapath covered by nettest_stage.py.
Round 5 (2026-07-21, later) - Yoga WiFi result + machine-scoped telemetry
(`3c5f21b`, pushed, flasher redeployed):
- **Yoga run 5 = the first metal WiFi trace, harness worked end to end.**
  AX201 (pci 02f0, hw_rev 351, rf_id 10a100): fw read+parsed, BAR0/card-ready/
  RF-kill/APM all clean -> **F12 (OPEN): no ALIVE within 2 s of gen2 fw
  start**. Suspects in order: (1) Qu-b0 vs QuZ-a0 variant both hiding behind
  the single IWLAX201.UCO name - pick by CSR_HW_REV after BAR map, ship both
  files; (2) polled wait_alive missing an MSI-X-indexed notification - check
  a CSR scratch reg to split "fw booted, notification missed" from "never
  booted". UsbIo enumeration proved itself on metal (8 interfaces listed).
- **F13 (FIXED same day):** the deployed image had shipped QEMU vvfat
  write-back telemetry in CRASH\ (lowercase pf005-013/boots.txt + cluster
  garbage on the Yoga stick) AND a placeholder WIFI.CFG (ssid=YourNetwork)
  that SHADOWED the flasher-staged wifi.txt (driver checks WIFI.CFG first).
  build.sh staging now purges CRASH/BOOTENV and ships no WIFI.CFG; QEMU
  scripts run on a scratch copy (build/esp-nettest).
- **Telemetry is now machine-scoped: CRASH\<MACHINE>\** via SMBIOS Type 1
  (map: X13YOGA/X1CARBON/SURFGO/SURFACE/LATITUDE/MACBOOK/QEMU, sanitized
  fallback, env block records raw smbios strings). One stick covers the whole
  batch; the reflash-between-machines rule is superseded. Both QEMU
  regressions green against CRASH\QEMU\.
- Yoga NET report archived: `~/unodos-metal-reports/x13yoga-2026-07-21-NET/`
  (its PF005-013-lowercase files were QEMU junk; the real Yoga stress files
  are PF014-016).
Round 6 (2026-07-21, the batch) - ALL LAPTOPS TESTED on one stick (build
0548, results archived `~/unodos-metal-reports/batch-2026-07-21-NET/`,
findings committed `cb174c6`):
- Machine folders + SMBIOS naming + WIFI.TXT creds all WORKED (X1CARBON /
  SURFGO / LATITUDE each complete; ssid+psk read on all three). MacBook:
  no folder at all = F9 verbatim (untestable until native FAT reaches USB
  on Apple firmware).
- **F12 is UNIFORM: "no ALIVE in 2 s" on all four wifi machines** across
  hw_rev 0x351 (X1+Yoga QuZ) / 0x332 (Surface, Ice Lake 34f0) / 0x420
  (**Latitude 7280 has an AX210**, pci 2725, GF RF - NOT an 8265!) and
  across gen2 AND gen3 load paths -> prime suspect is now the DRIVER's
  polled wait_alive/RX-notification handling (never executed anywhere
  before - QEMU has no model), not firmware variants. Next probe: dump
  UCODE_LOAD_STATUS/CSR scratch on timeout to split "fw booted, missed
  notification" from "fw never started".
- Bonus: F11 render spikes reproduce FLEET-WIDE (X1 2.09s corpus, Surface
  3.32s close, Latitude 1.20s close); the Surface's Blt present runs ~3x
  WORSE than its own blt bench (163-227ms avg, 415ms max, ~77 hitches/pass)
  - fix-session lead; Latitude is the fleet's fastest present (34ms avg)
  and now boots the stick reliably.
- **NEXT (wifi fix session): instrument wait_alive (scratch regs + raw RX
  pointers), then fix notification polling; then the MLME tail. Ethernet
  round with the AX88179A adapter still pending arin's "go".**

Round 7 (2026-07-21 late) — **F12 ROOT CAUSE FOUND by Linux v6.6 source review**
(`ae0047f` on master, pushed; SPECTEST 65/0/4 clean; flasher redeployed):
- **The CSR_CTXT_INFO_BA write does not start the boot ROM.** Linux's caller
  `iwl_trans_pcie_gen2_start_fw()` continues after the BA kick with
  `iwl_pcie_set_ltr()` then **`iwl_write_prph(UREG_CPU_INIT_RUN=0xa05c44, 1)` —
  the ROM-start doorbell**. The 1ad4002 round removed that write as "spurious
  gen3-only" (it's absent from ctxt_info_init — but lives in the caller's tail).
- **Why the ORIGINAL code (which wrote it) also failed:** our `prph_w` never
  held MAC access; Linux grabs GP_CNTRL MAC_ACCESS_REQ around EVERY PRPH op.
  Un-grabbed PRPH writes silently don't land → the metal readback
  CPU_INIT_RUN=0. Two driver eras broken differently, same fleet signature.
- Fixes shipped in iwlwifi.c: refcounted grab inside prph_w/prph_r; gen2 tail
  = HPM boot-LTR (integrated 22000 = all CNVi AX201s; AX200 uses CSR
  LTR_LONG_VAL_AD=0x88FA88FA) + doorbell; gen3 tail = FW-load int-mask arm +
  LTR(Ty 0x2725)/IML-spin(So/Ma) + doorbell at **+0x300000 UMAC offset** (the
  plain 0xa05c44 is a DIFFERENT register on AX210 — Latitude was never kicked
  either); autopsy reads + PNVM doorbell UMAC-offset; post-ALIVE RX restock
  (RFH_Q0_FRBDCB_WIDX_TRG 0x1C80 is a CSR w32, NOT prph — 9000 path fixed too;
  gen2 restock gated on g_alive per Linux "restock at alive").
- Also: batch-2's "AX210 SW_RESET (0x1) stuck" was a MISREAD — SW_RESET is
  0x80; RESET=0x11 doesn't contain it.
- git.kernel.org blocks fetches (Anubis) — use
  raw.githubusercontent.com/torvalds/linux for reference lookups.

Round 8 (2026-07-22) — **doorbell fix insufficient; round 2 = CNVi power state**
(`abf77d1`, pushed, flasher redeployed):
- Yoga run of build 0350 (round-1 fix): CPU_INIT_RUN read back **0 right after
  writing 1 with MAC access held**; CTXT_INFO_BA readback fine; fh_after_kick=0.
  CSR space works, the MAC ABSORBS PRPH writes → power state, not sequencing.
- Linux `_iwl_trans_pcie_start_hw()` steps we'd skipped, now implemented:
  **(1) `iwl_pcie_gen2_force_power_gating()` — integrated 22000 ONLY (= every
  CNVi AX201, exactly the failing class)**: finish_nic_init → HPM_HIPM_GEN_CFG
  (0xa03458) +FORCE_ACTIVE(b10) → +PG_EN|SLP_EN(b0|b1) → −FORCE_ACTIVE →
  ANOTHER sw reset + ownership retake. **(2) persistence bit**: HPM_DEBUG
  (0xa03440) b12 survives warm boots, cleared no-grab BEFORE the first reset
  (wprot check 0xa04d00 b12). **(3) sw_reset now retakes ownership**
  (prepare_card_hw after every reset) + MBOX OS_ALIVE (b5, 0x088) on hw-ready.
- **Decisive probe staged for the next Yoga boot**: load traces
  "prph window check: HPM_UMAC_LTR wrote 88FA88FA read X" — X=88FA88FA means
  PRPH now works (any remaining failure is past the doorbell); X=0 means the
  MAC still absorbs writes. Autopsy adds HW_IF/HPM_DEBUG/HPM_HIPM/LTR
  readbacks + grab_fail counter.
- After ALIVE the known gap is still the MLME tail (find_and_join scaffolded:
  broadcast BSSID, no real auth/assoc — S-WIFI-20 FAILs until written).
- Reading sticks via Carbon works scripted: ssh carbon + PowerShell
  -EncodedCommand, mount ESP partition at Q:\, read CRASH\<M>\, unmount.

Round 9 (2026-07-22) — **round 2 power fix PROVEN on Yoga, ROM still silent;
round 3 shipped** (`2f61df0`, pushed, flasher deployed):
- Build 0408 Yoga: `HPM_UMAC_LTR wrote 88fa88fa read 88fa88fa` → **PRPH window
  works now** (force-power-gating vindicated; HPM_HIPM=3 post-dance; the run's
  grab_fail=1 is my own early HPM_HIPM trace before clock-ready — harmless).
  But LOAD_STATUS=0, fh_after_kick=0 → ROM still never acts.
- Round 3 (Linux deltas, all pre-load): **nic_config_radio()** — pre-AX210
  iwl_mvm_nic_config parity: MAC step/dash (CSR_HW_REV&0xF) + RADIO
  type/step/dash straps from PHY_SKU TLV (type@0→pos10, dash@4→pos12,
  step@2→pos14) + RADIO_SI(0x200)|MAC_SI(0x100) masked into HW_IF_CONFIG
  (Yoga had HW_IF=00480000, straps unset — HR-RF image may silently refuse);
  **paging sections → dram.virtual_img** (vimg was always zero);
  **doorbell probe**: CPU_INIT_RUN instant readback in the same grab + at
  +10ms ("doorbell CPU_INIT_RUN: instant=X +10ms=Y") — instant=1 means the
  write lands and the ROM consumes it (→ image/ctxt validation next);
  instant=0 means the register still refuses (→ power/ownership). gen2 apm
  no longer sets gen1-only HAP_WAKE/HPET.
- NEXT Yoga NETLOG lines to read: "nic_config: phy_sku=", "fw dram map:
  lmac/umac/paging", "doorbell CPU_INIT_RUN: instant/+10ms", fh_after_kick.

Round 10 (2026-07-22) — **round-3 metal result + round 4** (`b506abb`, pushed,
flasher deployed):
- Build 0421 Yoga: straps + paging all landed (HW_IF=00489301 matches
  phy_sku=00330018 bit-for-bit; lmac=14 umac=15 paging=20). But
  **doorbell instant readback = 0** even inside the same grab → either
  CPU_INIT_RUN is write-only (probe can't tell) or the UREG block refuses
  writes. ROM still silent (LOAD_STATUS=0, fh_after_kick=0).
- Round 4 (cross-validated vs OpenBSD iwx(4), known-working AX201 driver):
  **UREG_CHICK (0xa05c00) = MSI_ENABLE (bit24)** — BOTH Linux
  (conf_msix_hw legacy branch) and OpenBSD program it pre-load on mq-rx
  parts; we never did. It's a READABLE config reg in the same UREG block as
  the doorbell → its readback trace ("UREG_CHICK<=MSI readback=") finally
  answers "do UREG-block writes land". Plus **one-grab kick tail** (iwx
  holds a single nic lock across BA→LTR→doorbell; we re-grabbed per write).
- Reading the stick on devbuntu: mount ro /dev/sdb1 (write-blocker only sets
  ro; hub at USB path 1-3 auto-exports to amanuensis via USB/IP instead).
- If UREG_CHICK readback shows bit24 but ROM still silent: next suspects are
  the MSIX IVAR/cause CSR block (0x2000, OpenBSD always programs it) and the
  ICT/interrupt table; after that, image TLV split validation (dump section
  offsets vs Linux parse of the same .ucode on devbuntu).

Round 14 (2026-07-22) — **NETWORKING FULLY SOLVED; Yoga drove live over the wire;
A/B kernel-push hangs** (commits through `93cad45`, pushed):
- **The Yoga connected over ethernet and I drove it interactively** (uptime/vols
  over URC). Four stacked bugs fixed to get there, all in net.c/pc64_http.c
  (my territory):
  1. `pc64_net_up` grabbed the **cableless onboard Intel I219** (8086:0d4f, e1000e)
     instead of the USB dongle — took the first NIC that BOUND, not one with
     LINK. Rewrote into 3 tiers: wired-with-link wins, then wired fallback, then
     WiFi (expensive probe last). (`07aa3ef`)
  2. USB-eth needs link UP before DHCP (ax88179 programs the RX medium in its
     link() callback); pc64_net_up fired DHCP immediately. Wait-for-link. (`e1bfdab`)
  3. **DHCP had NO retransmission** — one lost OFFER (marginal gigabit link / an
     ax88179 RX drop) stalled the lease forever (`tx=1 rx=1 arp=1 ip=0`). Added
     `dhcp_tick` in net.c net_poll: resend DISCOVER/REQUEST every ~1.5s. THE
     root-cause lease fix (helps eth test AND pc64_net_up). (`be1615b`)
  4. pc64_net_up reuses an existing lease (net_dhcp_done) instead of re-init'ing
     (re-init leaves ax88179 RX dead: tx>0 rx=0). (`bcb487a`)
- **LAN status now VISIBLE**: tray chip (green LAN=lease / amber LAN?=link-no-lease
  / hidden=no link) + System app "Network:" line (IP or NO DHCP lease + tx/rx).
  These on-screen diagnostics were HOW we localized each bug without a console
  (`f9df073`, `c7bd919`). The amber chip + "ASIX bound link up but Network: no
  link" was the smoking gun for bug #1.
- **`iwl` URC verb wired** (`93cad45`): CMD iwl csr/csw/prr/prw/rerun/status →
  iwl_dbg_cmd. Live AX201 register archaeology over the wire, NO reflash per
  experiment. THIS is the F12 debugging tool now.
- **LIVE F12 REGISTER ARCHAEOLOGY (2026-07-22, over the wire, iwl verb):** the
  AX201 was driven live. `iwl rerun` reproduces round-5 (bus-dead ROM). Confirmed
  via live csr/prr/csw/prw: card OWNED (HW_IF=00489301: NIC_READY bit22 set,
  ME_OWN bit25 CLEAR → CSME does NOT hold it), GP_CNTRL=08000005 (clock+INIT_DONE
  +RFkill-SW-on = healthy), CTXT_INFO_BA latched=76cff000 (<4GB), MBOX=00000020
  (OS_ALIVE set). **Experiments that ALL had ZERO effect on the ROM** (FH_INT=0,
  UCODE_LOAD_STATUS=0 throughout): (1) unmask all MSI-X causes (csw 280c 0 / 2804
  0 — they were ffffffff/002bffff = fully masked) + re-kick doorbell; (2) clear
  CSR_RESET (bit4 0x10 is READ-ONLY status, persists); (3) HPM FORCE_ACTIVE
  (prw a03458 0x403 landed, no effect). **FUNC_SCRATCH (csr 02c) = d55555d5,
  write-ignored** (wrote deadbeef, read d55555d5) — a MAC-CPU-domain register
  reading a floating pattern; HPM regs read a5a5a5a0 garbage during early
  force-power-gating. **CONCLUSION: the gen2 CNVi boot ROM never executes the
  context-info load; every host-controllable input is correct; not crackable by
  register-poking from current knowledge.** DECISIVE next step = ground-truth
  Linux iwlwifi_io ftrace of a WORKING AX201 load on THIS Yoga (Ubuntu has
  CONFIG_IWLWIFI_DEVICE_TRACING), diff register-for-register. Needs a spare stick
  (don't touch the UnoDOS test sticks).
- **F12 ROOT CAUSE FOUND 2026-07-22 via ground-truth Linux trace — MSI-X CONFIG
  REQUIRED.** Booted Ubuntu 24.04 on the Yoga (Cruzer stick), captured the
  iwlwifi_io ftrace of a WORKING AX201 load (unbind/bind method: keep module
  loaded so tracepoints persist, `echo dev > .../iwlwifi/unbind`, arm
  events/iwlwifi_io, `echo dev > .../bind`; 950 reg ops). The gen2 AX201 boot
  ROM will NOT start the fw load until **MSI-X is fully configured before the
  context-info kick** — we do NONE of it:
  1. **UREG_CHICK (prph 0xa05c00) = 0x2000000 (bit25 MSIX_ENABLE)** — we wrote
     bit24 (MSI, 0x1000000) = WRONG MODE.
  2. **MSI-X IVAR vector map** (byte writes): io[0x2880]=0, io[0x2881..0x2888]=
     0x01..0x08 (RX IVARs), io[0x2890/91/93/95, 0x28a0-a3/a6-a8, 0x28b9-bb/bd-be]
     = 0x89 (HW-cause IVARs; 0x89 = enable|vector9).
  3. **cause masks unmasked**: CSR_MSIX_FH_INT_MASK_AD (io[0x2804]) = 0xfe00;
     CSR_MSIX_HW_INT_MASK_AD (io[0x280c]) = **0xfffffffe just before the kick**
     (ALIVE cause bit0 unmasked). Settled full-run values FH=0xfe00 HW=0x91fffe30.
  4. THEN io[0x40] 64-bit CTXT_INFO_BA kick + prph 0xa05c44=1 doorbell (we
     already do these right).
  Trace also shows extra prph we skip: 0xa03030=0x80000000 (early, pre-power-gate),
  0xa05c10=0x3000000 (post-doorbell), 0xa09820=0 (RFH). And io regs io[0x100]=
  0xd5d555d5, io[0x240]=0xffff0500, io[0x3c]=0x1f0042, io[0x20]=0x91. Firmware:
  Ubuntu uses QuZ-a0-hr-b0-**77**.ucode (API 77); card crf-id 0x3617 cnv-id
  0x20000302, RF HR B3. Our polling driver doesn't need real MSI delivery — just
  the device in MSI-X-configured state so the ROM proceeds; keep polling the RB
  for ALIVE. Working trace saved scratchpad/iwl_working.txt (950 reg ops).
- **MSI-X FIX IMPLEMENTED + TESTED ON METAL over the wire — ROM STILL WON'T DMA.**
  Three metal iterations via A/B push (fat_alloc fix makes the 1.5MB kernel push
  work; push+reboot+rerun ~5min/cycle): v1 (338b218) conf_msix (UREG_CHICK bit25
  + IVAR table + FH/HW masks); v2 added WFPM 0xa03030=0x80000000 + io[0x3c]=
  0x1f0042 + gen2 HPET + shadow 0x802fffff; v3 (13905d0) added PCI-config MSI-X
  enable. ALL confirmed running on metal ("MSI-X+PCI[v3] configured:
  CHICK=02000000 FHmask=0000fe00 HWmask=fffffffe"). **Every result:
  fh_after_kick=0, UCODE_LOAD_STATUS=0 — the ROM does ZERO DMA.** Metal facts:
  PCI MSI-X was ALREADY enabled (ctl=0x800f before we touched it); WFPM 0xa03030
  reads back 0. We've faithfully replicated the ENTIRE working register sequence
  at both PCI + BAR0 level; ROM still refuses.
- **IOMMU CONFIRMED ACTIVE but NOT the DMA blocker (v5/v6 metal via `iwl dmar`).**
  Added `iwl dmar` (walks RSDP→XSDT→DMAR, reads each DRHD GSTS) + iommu_disable()
  (clears GCMD.TE + PMEN.EPM, wired into bring-up). Yoga result:
  **DRHD@fed90000 TES=off; DRHD@fed91000 GSTS=c0000000 TES=ON** — VT-d DMA-remap
  IS active on the PCH unit covering the AX201. BUT disabling it did NOT help:
  v5 cleared TES (GSTS->40000000), v6 also cleared PMR (PMEN->0) — **both VT-d
  DMA-protection mechanisms OFF, and the ROM STILL does zero DMA (fh_after_kick=0).**
  So the firmware likely set an identity/passthrough IOMMU mapping (boot devices
  DMA through it fine), and the IOMMU was never actually gating the AX201. Bus
  mastering also verified OK (cmd-reg bit2 set + CSR_RESET MASTER_DISABLED clear).
- **F12 STILL OPEN — "device does zero DMA" cause is elusive.** Eliminated: MSI-X
  (needed, done), IOMMU TES+PMR (off, no effect), bus mastering (on). Remaining
  candidates for a fresh session: (1) cache/coherency — the context-info + fw we
  memcpy into the arena not visible to device DMA under UEFI (try uncached/WC
  arena, or a cache flush before the kick); (2) the ROM reads+silently rejects the
  context-info content (a struct field it validates — version/valid); (3) CNVi/
  CSME still owns the radio at a level our ownership handshake doesn't clear;
  (4) capture the Linux load's PCI-CONFIG writes too (the iwlwifi_io ftrace only
  showed MMIO — pci_alloc_irq_vectors / bus-master / other config writes are
  invisible). The MSI-X requirement + IOMMU-active are both confirmed facts; the
  final DMA blocker remains unidentified.
- **LIVE OPS INFRA (2026-07-22):** Ubuntu-on-Yoga reached via a reverse Python
  agent (no sshd installable offline): devbuntu serves ~/agent.py + ~/capture.sh
  via yoga_server.py (:8080, serves *.sh/*.py), Yoga runs
  `wget -qO- http://192.168.2.100:8080/agent.py | sudo python3 -` → dials
  yoga_shell.py (:9002) → I drive via ~/yoga/cmd.txt, read ~/yoga/out.txt. All
  persistent servers MUST run as `ssh devbuntu 'exec python3 ...'` run_in_background
  (nohup/setsid die on session close). ftrace capture needs unbind/bind NOT
  module reload (reload loses the just-enabled events).
- **A/B kernel push FIXED** (agent, 4a9bc40): fat_alloc O(n^2)→O(1) next_free
  hint; 1.5MB push finalize+verify ~10s, gates green. Kernel pushes over the wire
  work now — no more physical reflashes for driver iteration.
- **A/B kernel push HANGS** (was, now FIXED above): pushing BOOTX64.EFI (1.5MB) over
  the wire streamed fine but the finalize (one big uno_fs_write) hung the Yoga
  (TCP alive, app blocked, no watchdog reset). Machine needs power-cycle; boot
  disk BOOTX64 possibly corrupt → physical reflash to recover. **Small pushes
  (configs) are fine; the iwl verb needs NO push.** So: physically full-reflash
  the latest build ONCE (reliable eth connect now + iwl verb), then all WiFi
  work is tiny over-the-wire iwl commands.
- **Bridge/ops facts**: URC listener runs on devbuntu 192.168.2.100:5099 via
  ~/urc_bridge.py (reads ~/urc/cmd.txt, logs ~/urc/session.log; handles push).
  MUST run via a PERSISTENT foreground ssh (nohup/disown/setsid all died on
  session close) — `ssh devbuntu 'exec python3 ~/urc_bridge.py 5099'` as a
  run_in_background task. Both ~/unoauto_remote.py AND ~/urc_bridge.py must be
  the CURRENT tools/ versions (old unoauto_remote.py lacked push_file). The
  Verbatim STORE N GO stick's USB connection on devbuntu is MARGINAL (dropped
  ~5×, mid-write); use a direct port. STRESS.CFG for live iter: nostress +
  remote=192.168.2.100:5099, NO net-force-wifi (eth-first, drive WiFi via iwl).

Round 13 (2026-07-22) — **LIVE-ITERATION WORKFLOW LANDED (unoautomate remote
channel merged)** (`fa980f9` merge + `698d020`/`6ccaaf1`, pushed, deployed):
- origin/unoautomate's URC remote channel merged to master: pc64 DIALS OUT
  over its TCP stack to a dev-PC listener (STRESS.CFG `remote=<ip>:<port>`),
  streams all unoauto LOG channels live, accepts commands (probe/key/launch/
  test/py/poweroff). Debug builds only; one TCP conn shared with Browser/AI.
  Gates green: SPECTEST 65/0/4 + tools/remote_qemu.py e2e.
- **Listener runs on devbuntu (192.168.2.100:5099)** — Windows blocks inbound
  on amanuensis despite a python allow rule (Public-profile stealth). Bridge:
  `~/urc_bridge.py` (nohup) logs frames to `~/urc/session.log` and tails
  `~/urc/cmd.txt` for commands (append lines via ssh; "/msg" for messages).
  ufw inactive on devbuntu. Restart: `nohup python3 ~/urc_bridge.py 5099 &`.
- **iwl_dbg_cmd()** added (iwlwifi.h/.c): csr/csw/prr/prw (live register
  peek/poke), rerun (full bring-up retry), status — request filed in
  UNOAUTOMATE-REQUESTS.md for a URC `iwl` pass-through verb. Until wired,
  stopgap = `test network` (CAUTION: netlive TCP checks fight the remote
  link for the single connection) or key-inject the Network app's W retry.
- **Workflow now**: flash once → Yoga boots with eth dongle + STRESS.CFG
  `net-force-wifi` + `remote=192.168.2.100:5099` (add keys by editing the
  stick on devbuntu) → WiFi test fails → remote link dials out over eth →
  live logs + remote retries; reflash only for driver-code changes.

Round 12 (2026-07-22) — **WiFi round-4 metal: UREG WRITES LAND; round 5 shipped**
(`8ef7c09`, pushed, flasher deployed):
- Yoga (build 0655, net-force-wifi added by editing STRESS.CFG on the stick
  from devbuntu — blockdev --setrw, append, --setro; no reflash needed):
  **UREG_CHICK readback=01000000** → UREG-block writes land, the doorbell was
  reaching the device all along (write-only reg). Firmware verified
  byte-identical to Debian iwlwifi-QuZ-a0-hr-b0 (fw-blobs md5 match). All
  reference-driver pre-kick writes delivered; ROM still never fetches
  (LOAD_STATUS=0, FH=0, no error bits). Lab quirk: modern .ucode carries no
  variant name string (only "release/coreXX::hash").
- Round 5: **device_stop() before bring-up** (Linux stop_device parity: ints
  off, STOP_MASTER+poll MASTER_DISABLED, clear INIT_DONE, reset+retake — we
  always started from the BIOS/CSME-left state; every real Linux load starts
  from a torn-down device) + **MSI-to-RAM probe** (PCI MSI cap enabled with
  the message address aimed at an arena scratch dword, data 0x4D51; autopsy
  prints msi_scratch — any device-initiated interrupt write becomes visible;
  also matches Linux always having MSI/MSI-X config-space-enabled pre-load).
- **If round 5 still shows LOAD_STATUS=0 → STOP source-diffing; get ground
  truth**: Ubuntu live stick on the Yoga, capture the iwlwifi_io ftrace
  register trace of a WORKING load (Ubuntu kernels have
  CONFIG_IWLWIFI_DEVICE_TRACING), diff against our sequence. Needs a spare
  stick (do NOT overwrite the verbatim UnoDOS test stick).

Round 11 (2026-07-22) — **ETHERNET IS REAL on metal; DNS root-caused + fixed**
(`d9488cb`, pushed, flasher deployed):
- Yoga + AX88179A (build 0553): link UP 182ms, DHCP lease 1039ms
  (192.168.2.157), gateway pings 10-15ms — medium-mode fix holds, RX fully
  alive. NEXT-ITERATION Phases 1-2 DONE on metal.
- DNS failure root cause via the ack diagnostics: `len=300 opt3=1 opt6=0` —
  the router GENUINELY omitted option 6 because our DHCP option-55 parameter
  list only requested 1+3 (SLIRP volunteers opt6 regardless, masking it in
  QEMU). Fix: request 1/3/6 + fall back to the gateway as resolver when the
  ACK has no opt6. Metal-verify next eth round → then Phase 3 (real TLS)
  completes via S-AI-01/02.
- WiFi round-4 (UREG_CHICK probe) still awaiting its Yoga run — this stick's
  config had eth ticked so the plan skipped WiFi (only net-force-wifi
  overrides that when the adapter is present).
- Eth round 2 (build 0630): **opt-55 fix CONFIRMED** (ack opt6=1, resolver
  192.168.2.1, lease+pings again) but DNS lookup STILL failed. Retry+backoff
  hardening `e23137c` + dns-diag counters `0b0d374`.
- Eth round 3 (build 0655): diag read **sent=4 rx=4 badid=0 neg=4** — every
  reply arrived and matched. **ROOT CAUSE = THE LAB ROUTER, not our stack:
  the Bell router (mynetwork.home, 192.168.2.1) returns NXDOMAIN for
  `example.com` SPECIFICALLY** — replaying our exact query bytes from
  Windows reproduced it; google/cloudflare/api.anthropic.com all resolve
  with the same minimal query; nslookup agrees. Test target swapped to
  api.anthropic.com (`33d901e`). LESSON: on this LAN, never use example.com
  as a DNS canary. The eth stack (lease/ping/DNS transport + UDP checksums
  against a strict receiver) is now fully proven on metal → next eth run
  should be all-green, unblocking live TLS (S-AI) over the real link.

Round 15 (2026-07-22) — **THE BIG REFRAME: the boot-ROM CPUs ARE RUNNING; F12 is
a parse/handshake stall, NOT "zero DMA/never started."** Resumed after a session
crash; Yoga still live on the URC link (devbuntu :5099), booted on the fix7/v7
build. First-ever live read of the internal-CPU registers via `iwl prr` (they were
NEVER in the autopsy — the whole 14-round "device does zero DMA" conclusion rested
on FH_INT/UCODE_LOAD_STATUS/CPU_INIT_RUN only):
- `iwl rerun` then `iwl prr`: **UMAG_SB_CPU_1_STATUS(a038c0)=00005754,
  SB_CPU_2(a038c4)=00000003, UMAC_CURRENT_PC(a05c18)=8047378e,
  LMAC1_CURRENT_PC(a05c1c)=004bf5da**, LMAC2_PC(a05c20)=0 (single-LMAC, expected),
  legacy SB_CPU_1/2(a01e30/34)=0 (unused on 22000), UMAG_GEN_HW_STATUS(a038c8)=0.
- Sampled UMAC_PC + LMAC1_PC + SB1 **three times** → all IDENTICAL. The CPUs
  started, executed, and are **PARKED at a fixed PC in a spin/error loop**. These
  are real, distinct, stable code addresses — NOT the a5a5a5a0/d55555d5 floating
  garbage a dead PRPH block returns, so high-confidence real.
- **What this overturns:** the doorbell DID release the CPU; MSI-X config WAS
  sufficient; the ROM is NOT inert. `fh_after_kick=0`/`UCODE_LOAD_STATUS=0` mean the
  running ROM parked BEFORE fetching firmware — it read the context-info (or
  attempted the fw DMA) and halted. Transport layer is fine; the fault is one layer
  up. All of rounds 5-14 (MSI-X, IOMMU, bus-master, cache-flush) chased the wrong
  layer — though MSI-X really was a needed prerequisite to even get the CPU running.
- v7 (wbinvd+mfence before kick, uncommitted in iwlwifi.c ~L1335) was the last
  thing pre-crash; metal result = still fh=0 (as expected now — coherency was never
  the issue). msi_scratch=deadc0de (MSI never fired; moot in MSI-X mode).
- **NEXT (prime suspect #2, ctxt-info content):** diff our `load_fw_gen2()` struct
  build (iwlwifi.c ~L1299: version/mac_id/size order, control_flags =
  TFD_FORMAT_LONG|CB_SIZE|RB_SIZE — check for a missing AUTO_FUNC_INIT/VALID bit,
  and the fw DRAM section addrs in place_fw_dram) BYTE-FOR-BYTE against Linux
  `iwl_pcie_ctxt_info_init()` + `struct iwl_context_info` (fetch via
  raw.githubusercontent.com/torvalds/linux — git.kernel.org is Anubis-blocked).
  The context-info lives in host RAM (DMA), so it is INVISIBLE in the saved MMIO
  ftrace (scratchpad/iwl_working.txt) — source diff is the only way. SB1=0x5754 and
  the parked PC are Intel-internal codes with no open decode, so don't chase those.
- Live-debug recipe that worked: append verbs to `~/urc/cmd.txt` on devbuntu
  (`iwl rerun`, `iwl prr <hex>`, `iwl csr <hex>`), read replies in
  `~/urc/session.log`. No reflash needed for register reads.
- **GROUND-TRUTH WORKING FTRACE RECOVERED + now PERSISTENT** at
  `devbuntu:~/iwl_from_yoga.txt` (950 iwlwifi_io reg ops of a WORKING AX201 load on
  THIS Yoga under Ubuntu 6.17; also copied to this session's scratchpad as
  iwl_working.txt). Captured earlier via the unbind/arm-events/bind method
  (~/capture.sh + ~/yoga_server.py + ~/agent.py all still on devbuntu). NEVER
  re-capture unless needed — it's saved. fw = QuZ-a0-hr-b0-77.ucode, crf-id 0x3617,
  cnv-id 0x20000302, RF HR B3, mac 18:26:49:71:91:57.
- **Verified against ground truth this session:** our MMIO kick sequence MATCHES the
  working trace (masks io[0x280c]=fffffffe / io[0x2804]=fe00 → CTXT_INFO_BA kick
  io[0x40] → LTR prph a0348c=0xf, a03480=88fa88fa → doorbell a05c44=1), and our
  context_info struct + control_flags (=0x980: TFD_LONG 0x100 | CB_SIZE 8<<4 |
  RB_SIZE_4K 4<<9) are BYTE-CORRECT vs Linux master pcie/iwl-context-info.h. So the
  MMIO layer and ctxt-info content are NOT the bug.
- **NEW LEADING HYPOTHESIS (supersedes the ctxt-content idea): the DMA'd FIRMWARE
  IMAGE / secure-boot.** SB=Secure Boot; SB_CPU_1_STATUS=0x5754 nonzero + UMAC/LMAC
  parked at DISTINCT code addresses (0x8047378e high, 0x004bf5da low) = the ROM ran
  secure boot over the fw sections we placed in ctxt_info.dram and halted. Suspect
  `place_fw_dram()` (iwlwifi.c ~L1199) + the .ucode section split (LMAC/UMAC/CSS/
  paging via CPU_SEP/PAGE_SEP) — a wrong section boundary, a missing/misplaced CSS
  (secure-boot cert) section, or wrong section DATA would fail auth exactly like
  this. NEXT: dump our parse's section offsets/sizes/counts and diff against a
  Linux-side parse of the same QuZ-77.ucode (write a small .ucode TLV parser on
  devbuntu). fh_after_kick=0 is a RED HERRING (wrong FH channel; live CPU PCs prove
  code executes).

Round 16 (2026-07-23) — **PLACEMENT HYPOTHESIS KILLED; F12 narrowed to fw
early-init wait; fresh sticks flashed.** Resumed for Yoga WiFi.
- **Sticks reflashed** (devbuntu): Cruzer Fit=**disk A** (/dev/sdb), Verbatim STORE
  N GO=**disk B** (/dev/sdd), identical image. Built from an ISOLATED git worktree
  `~/unodos-yoga` at master `55143d03` (UNO_DEBUG=1) so it didn't disturb a
  **concurrent Zima r8169 session** live on devbuntu (its `urc_bridge_ka.py` owns
  :5099, committed at 13:55). fw-blobs repopulated via `pc64/tools/fetch-fw.sh`
  (devbuntu's /lib/firmware had only Qu-b0, NOT the QuZ-a0-hr-b0 the AX201 needs);
  IWLAX201.UCO=1406716 B on both sticks. Image = `~/unodos-yoga/pc64/build/
  unodos-uefi.img` (1024 MiB GPT+ESP via mkuefi.py). **STRESS.CFG on the sticks:
  `remote=192.168.2.100:5098` / `net-eth-only` / `nostress` / `noshutdown`** (port
  **5098** NOT 5099, to avoid the Zima). **Yoga URC bridge = `:5098` → `~/urc-yoga/`**
  (patched urc_bridge.py to take an optional dir as argv[2]); both bridges coexist.
  Boot disk A **with the USB-eth dongle** → dials :5098 → drive `iwl` verbs by
  appending to `~/urc-yoga/cmd.txt`, read `~/urc-yoga/session.log`.
- **The Round-15 placement/secure-boot hypothesis does NOT survive a rigorous diff.**
  `place_fw_dram`/TLV-parse/ctxt-info vs Linux gen2 (`pcie/ctxt-info.c`,
  `iwl-context-info.h`) is byte-correct: gen2 `iwl_context_info_dram` has NO
  `css_addr` (host does NO special CSS placement — CSS rides inside the first lmac
  section; host-CSS is a gen1-8000 / gen3-IML concept only). LMAC/UMAC/paging
  indices, TLV walk, struct offsets (dram@192, total 1792), control_flags=0x980,
  cmd_queue_size=2 all match. Decisive logic: a placement/CSS/split error →
  CPUs never start (LOAD_STATUS=0), NOT "run real code then park." So it's not it.
- **Ground-truth ftrace re-read** (`devbuntu:~/iwl_from_yoga.txt`, 984 ops):
  RBD write-ptr `io[0x1c80]=0x7f8` is posted **47ms AFTER** the doorbell, inside the
  **ALIVE MSI-X irq (hw:0x1)** — so our gen2 skip of pre-doorbell RBD posting MATCHES
  Linux, and ALIVE arrives as a HW MSI-X cause, NOT an RX buffer (the RBD-deadlock
  idea is wrong). The ONLY host op between doorbell (`prph a05c44=1` @2059.011) and
  ALIVE (@2059.058) is **clearing CSR_GP_CNTRL bit3 MAC_ACCESS_REQ** (`io[0x24]`
  0xc04000d→0xc040005). Our `release_nic()` (iwlwifi.c:737) does exactly that
  (`clr_bit MAC_ACCESS_REQ`) and the kick tail calls it right after the doorbell
  (iwlwifi.c:1367) — VERIFIED correct in code. So the post-doorbell path matches;
  the fw parks for a reason invisible after the doorbell.
- Nit worth fixing: `place_fw_dram` silently truncates on arena exhaustion (bare
  `return;`, leaving zero DRAM ptrs) — the one failure that WOULD mimic a park;
  make it loud + trace g_arena_used vs FW_ARENA_MAX (low prob; QuZ ~1MB fits 3MB).
- **METAL RESULTS (2026-07-23 eve, Yoga live on :5098, full A/B loop proven):**
  The live edit→build→`push 1 \EFI\BOOT\BOOTX64.EFI`→reboot→`iwl rerun` loop WORKS
  end to end (~4 min/cycle, no reflash; 1.6MB kernel push VERIFIED, no hang — the
  fat_alloc fix holds). Baseline reproduced EXACTLY: UMAC_PC=8047378e, LMAC1_PC=
  004bf5da (BOTH on-chip 0x8.../0x4... addrs, NOT in the host DRAM arena 0x76cff000
  → the ROM CPUs run in ON-CHIP ROM and park BEFORE jumping to the DMA'd fw),
  SB_CPU1=~0x575x (varies, nonzero), GP_CNTRL=08040005 (MAC_ACCESS_REQ bit3 CLEAR →
  release_nic works on metal), fh_after_kick=0, UCODE_LOAD_STATUS=0.
- **pre-doorbell diff prime suspect (MAC_SI/RADIO_SI in HW_IF) TESTED ON METAL =
  CLEAN NO-OP.** Gated the 0x300 SI bits behind !g_gen2 (iwlwifi.c nic_config_radio):
  HW_IF went 0x00489301→0x00489001 (now matches the working ftrace) but the CPUs
  park at the IDENTICAL PCs. So SI bits were correlation, not cause. Fix KEPT (removes
  a known-divergent variable; matches Linux) but UNCOMMITTED. Placement + SI both
  now disproven — be skeptical of pure static-diff "prime suspects."
- **Cred gate:** `iwl rerun` bails without WIFI.TXT. Push creds via URC: `push 1
  WIFI.TXT <local>` — MUST be **vol 1** (the ESP, label UNODOS); **vol 0 is a RAM
  disk** (volatile, wiped on reboot — my first push went there). Placeholder creds
  (any ssid=/psk=) suffice for F12 (park is far before association). `vols` lists them.
- **LEADING LEAD NOW: `force_power_gating()` two-phase (iwlwifi.c:608).** Working
  ftrace runs it as TWO reset-bounded phases — (1) finish_nic_init → WFPM
  `PRPH[0xa03030]=0x80000000` (under its own grab/release) → **SW RESET (io[0x20]=0x91
  @trace line 84)**; (2) finish_nic_init again → HIPM dance (HPM_HIPM_GEN_CFG 0xa03458
  FORCE_ACTIVE→PG_EN|SLP_EN→−FORCE_ACTIVE) → **second SW RESET**. We do it as ONE phase
  (WFPM → HIPM → single trailing reset), missing the reset BETWEEN and the second
  finish_nic_init. FITS the symptom: a mis-sequenced power-gate can leave the DMA
  fabric gated while config/PRPH writes still land (which is exactly what we see —
  PRPH window works, ROM runs, but fh=0/no fw DMA). Implement the two-phase split next.
- Other lower-conf leads parked: missing `io[0x10]=0xffffffff` FH_INT_STATUS clear in
  device_stop; verify ctxt-info physical addrs are DMA-readable by the ROM (virt==phys
  holds? the ROM reads 0x76cfe000); fine-sample PCs (halt vs spin). Full diffs in the
  session scratchpad (iwl_fwsec_diff.md, iwl_predoorbell_diff.md).
- **force_power_gating two-phase = REGRESSION, WEDGES THE HARDWARE.** Tried splitting
  it into two reset-bounded phases (WFPM+reset ; then finish_nic_init+HIPM+reset) to
  match the ftrace and make the HIPM writes land (our readback is 0xa5a5a5a0 floating
  garbage — they DON'T land in the single-phase version). On metal it HUNG the Yoga
  mid-`iwl rerun` (app blocked on a bus access after the extra mid-sequence sw_reset;
  watchdog does NOT fire during WiFi joins → needs a physical power-cycle). The
  *direction* may still be right (make HIPM writes land) but an extra full sw_reset
  mid-sequence is too violent for our bring-up — needs a gentler approach (maybe just
  finish_nic_init re-assert without the reset, or land the writes another way). REVERTED.
- **AGENTS.md landed on master (93b7bb4e).** Now working on branch **`iwlwifi-f12`**
  (pushed to origin), rebased on origin/master, one-lane commits: `df5aa17d` iwlwifi
  SI-gating + `8eec8ae3` requests(CLAIM iwlwifi + REQUEST urc_bridge dir-arg). I own
  the **iwlwifi** lane; r8169/Zima is a separate lane. urc_bridge dir-arg patch kept
  as untracked `pc64/tools/urc_bridge_yoga.py` (stopgap; tracked file unmodified).
  Merge-gate before landing: rebuild UNO_DEBUG=0 AND =1 + QEMU gate green.
- **RECOVERY PENDING:** Yoga wedged by the two-phase kernel; needs a physical
  power-cycle (normal boot is fine — WiFi isn't auto-triggered under net-eth-only).
  Recovery kernel = branch `iwlwifi-f12` build **818c03cd** (baseline single-phase +
  SI-gating), ready to `push 1 \EFI\BOOT\BOOTX64.EFI` the instant it re-dials :5098.
  Reminder: creds MUST go to vol 1 (ESP), NOT vol 0 (RAM disk).

Related: [[unodos-pc64-storage-drivers]] [[unodos-pc64-x1-trackpad]] (same X1
metal target), [[unodos-ports-hardware-blocked]], [[unodos-pc64-debug-stress]].

Round 23-25 (2026-07-27/28) — **THE LINK, THEN THE DATA PATH, BOTH ON METAL**
(branch `iwlwifi-dhcp`, pushed, not landed):
- Round 23-24 ported association to the link API (MAC_CONFIG/LINK_CONFIG/
  PHY_CONTEXT/STA_CONFIG/SEC_KEY) and fixed five independently-fatal TX bugs;
  the WPA2 4-way completes and keys install.
- Round 25 made data actually move: **DHCP lease, 3/3 gateway pings and a DNS
  answer over the encrypted link**, then `iwl status` reading
  `joined "NimmuNet" ... CCMP keys in tx 9 rx 13`.
- The metal procedure that works: URC `reboot` -> `iwl rerun` -> `iwl mvm
  1/2/e/4` -> `iwl scan` -> **`iwl pick <n>` for 51:8c:8f** -> `iwl mld 1..6`
  -> `iwl auth`+`assoc`+`eapol` (batched) -> `iwl netup` -> `iwl netres`.
- **Do not trust `iwl data`**: an ARP probe from 0.0.0.0 is not reliably
  answered and this WiFi segment carries no broadcast, so it reported "no data
  frames" on a link that then passed real DHCP. `iwl netup` is the real test.
- GUI: the standalone Network app is no longer launchable (2026-07-26), so the
  join UI went into **Control Panel > Network**. Verified on metal: Scan brings
  the card up from cold and lists networks with RSSI; the password field and
  Join both work. The join then picked the refusing BSS - candidate retry is
  the fix and is metal-pending.

Round 25 retry findings (2026-07-28 eve, metal): attempt 1 against the refusing
BSS is clean (fw healthy, auth just times out). **radio_restart() is NOT a
recovery** - the reload says ALIVE then SW_ERRs on the next command
(csr2808=0x02000000); only a reboot fixes it. **Retargeting in place works
better**: `iwl pick <n>` + `iwl mld 5` (STA_CONFIG re-sent with the new peer)
keeps the fw clean, and **a second SESSION_PROTECTION does NOT assert** (round
24's note is wrong) - but the retargeted auth frame is not ACKed (TXRESP tail
0x83 vs 0x01), so something still points at the old AP. Next suspects:
LINK_CONFIG ref_bssid_addr (we send zeros) and a link deactivate/reactivate.
Also: `iwl connect` with no args needs the radio up first (WIFI.CFG is only read
inside iwl_nic) - pass `iwl connect <ssid>|<psk>` meanwhile. The box also wedged
once right after booting a fresh image (link up, then no command ever answered,
no watchdog reset) - a power-cycle fixed it; cause unknown, watch for it.

Round 25 final (2026-07-28 late, metal): **the retry mechanism is PROVEN** - a
full second association (auth+assoc+4-way, "CCMP keys in") completed on
re-pointed contexts. The missing piece was the TX QUEUE: it belongs to the
station, so after STA_CONFIG changes the peer the fw stops transmitting on it
and refuses a second alloc for the same sta/TID. Fix = SCD_QUEUE_CONFIG
operation 1 (REMOVE, {operation, sta_mask, tid} in the same 36-byte envelope)
then a fresh alloc; verb `iwl retarget <n>` drives it. ref_bssid_addr was the
WRONG suspect (Linux sets it only for nontransmitted multi-BSSID links) and was
not touched. STILL OPEN: (a) after a RETARGETED join the data path does not
carry - frames arrive but decrypt to garbage (`tx 8 rx 0 drop 8`), so the old
association's CCMP keys are almost certainly still on the station and want
removing before the new 4-way; a first-attempt join carries data fine. (b) the
mesh APs are individually FLAKY run to run - 47:4e:cf, previously "always
refuses", auth'd fine in the last run and then ignored the assoc request
instead; 51:8c:8f is the most consistent. Batching auth/assoc/eapol matters: an
assoc ~38 s after its auth is past the AP's auth-state timeout.

LANDED 2026-07-28 (ec69431, fast-forward; branch + worktree retired, ~/unodos-yoga
back on detached origin/master). Merge gate: both builds clean, SPECTEST in QEMU
67 PASS / 0 FAIL / 4 SKIP = master's baseline. Landing it surfaced a latent
toolkits bug worth remembering: `unoui_ui_init()` never initialised `full` or the
drag fields, and `handle_inner()` short-circuits on `full` - so SPECTEST's
S-UUI-07 (the one test that builds a STACK-LOCAL unoui_ui) passed or failed on
stack garbage. It failed on my branch purely because ~1.5 KB of new statics moved
the image; master with an equivalent dummy array still passed. Fixed + noted to
the owner in UNOAUTOMATE-REQUESTS.md.

X1 Carbon (2026-07-28) is now the SECOND WiFi rig and a much better loop than
the Yoga: it is a Windows box with an **elevated sshd**, so the stick never has
to move - `ssh carbon`, ship the image gzipped (~10 MB for 1 GiB), expand with a
GzipStream one-liner, and run `pc64/flash/UnoFlashCli.exe 1 <img> <status>
"STORE N GO"`. Reflash = the user boots Carbon into Windows and says so. Note
its SSH shell is **cmd.exe**, not bash. Boot it with NO `remote=` in DEBUG.CFG:
with no ethernet dongle an armed URC calls pc64_net_up() every few seconds,
falls through to WiFi and re-runs the whole firmware bring-up, fighting the
Control Panel join. Also fixed along the way: UnoFlashCli reported
"ABORT: ObjectDisposedException" after every byte-perfect write (the FileStream
owned the disk handle; the ownsHandle "fix" of 2026-07-19 had actually passed
isAsync), and unoui's UI_LIST has NO scrolling at all, so the Control Panel
pages the network list itself.

**A SCAN NEEDED CREDENTIALS - fixed 2026-07-29 (`03f5359` + `662caba`, on
master).** `iwl_nic()` bailed unless some volume held a config file with an
`ssid=` line, AND used that same lookup (`firmware_volume()`) to pick the volume
it read the .ucode from. Two unrelated questions answered by one check, so a
stick with the firmware staged but NO `WIFI.CFG` could not bring the radio up:
Control Panel > Network > Scan returned 0 networks on a working AX201 and the
pane blamed rfkill. Scanning is exactly the credential-less case - you pick a
network and type the password afterwards.
Now: `fw_volume()` locates the .ucode independently (full `FIRMWARE\` path or
flat root), credentials are required only for a JOIN (`g_no_join`, already set
by `radio_up()`, makes them optional), and the firmware read tries the firmware
volume then the config volume so nothing that worked before can regress. The
Control Panel's empty-scan message now prints `iwl_status_str()` instead of
guessing - the real reason was already on screen two lines above it.

**SUPERSEDED 2026-07-29 (user ruling): production images DO bundle the firmware
now** - `build.sh` stages `fw-blobs/` into every build unless `UNO_NOFW=1`
(use that for anything published; the licence is about redistribution). The
paragraph below is kept for why, and the `build/esp` staleness trap still bites.

**(WAS) PRODUCTION IMAGES HAVE NO WiFi FIRMWARE.** `build.sh` copies `fw-blobs/*.UCO`
into `build/esp/FIRMWARE/` only when `UNO_DEBUG != 0` (licence: no
redistribution). So a `./build.sh` + `tools/mkuefi.py` stick cannot do WiFi
unless you stage the blobs by hand first:
`cp fw-blobs/*.UCO fw-blobs/*.PNV build/esp/FIRMWARE/` before mkuefi.
TRAP: `build.sh` does NOT wipe `build/esp`, so a tree that previously built with
UNO_DEBUG=1 leaves the blobs behind and a later production image picks them up
by accident - which is why one stick had firmware and a fresh worktree's did
not. `fw-blobs/` is gitignored, so a NEW worktree never has it; copy the
directory across before building an image meant for metal.

**JOINED BUT NO IP - root cause found 2026-07-29 (`e58dc16`, master):** NOTHING
pumped the net stack from the shell's frame loop. `net_poll()` ran only inside
blocking loops (an HTTP fetch, TLS, the Control Panel's join dialog), so DHCP's
retransmit timer stopped the instant the join dialog's wait ended - a lease that
would have arrived a second later never did, and the link sat associated with no
address and no retry. The frame loop now pumps it (no-op with no NIC), a lease
arriving there refreshes the network pane, the join wait is 20 s with a visible
per-phase clock (it looked frozen), and there is a "Renew IP" button.
If a box still gets no lease, the WiFi status line is the diagnostic: it carries
`tx N rx N` plus `CCMP keys in` vs `4-way pending`. rx stuck at 0 with keys in
points at broadcast/GTK decryption (the DHCP OFFER is broadcast); rx climbing
points at DHCP itself.

**unoui list scrollbars (`e225062`):** a scrollbar painter sizes its thumb as
track/(track+vmax) - vmax in PIXEL units. A list's range is in ROWS, so passing
it raw made the thumb fill the whole bar. `unoui_list_draw` now converts rows to
that pixel domain (thumb = rows/n of the track). Any future widget reusing a
scrollbar painter needs the same conversion.

**2026-08-05 - the "no lease" fault is NOT a regression, and the counter that
said `drop=69` was not attributed to a BSS.** The encrypted data path has been
proven exactly once (round 25: Yoga, QuZ-a0 firmware, NimmuNet `51:8c:8f`,
FIRST-attempt join). Nothing on the key or RX path changed after it: `mld_sec_key`
is untouched since it was introduced, the RX offset logic since `908ba2f8`, and
the flags are Linux-correct (station GTK = MCAST|CCMP|NO_TX = 0x4a). The failure
has an older description: **a RETARGETED join has never carried data** (recorded
2026-07-28), and SURFGO cannot reach SKYNET any other way, since `51:4d:66` never
answers auth so every association it completes is `join: try 2/3` through
`retarget_ap()`. What changed is the fleet, not the code.
LANDED on master (`1d82f542` + `4ea724f6`, branch and worktree retired the same
day). It reads the thing that ends the guessing: **`iwl_rx_mpdu_desc.status` is a `__le32` at desc+12**, ahead of the
v1/v3 union, carrying SRC_STA_FOUND/KEY_VALID/cipher/MIC_OK/DECRYPTED. `sec=0` on
a protected frame = a key never armed; `sec=2 mic=0` = a key that is wrong. Also
filters data frames from BSSes we did not join (a mesh channel is mostly those,
and they were being delivered to net.c). Next suspect if `prot>0 dec=0`:
`mld_sta_cfg()` re-points a LIVE station at a new peer, which Linux never does -
it removes the station (`STA_REMOVE_CMD`, MAC_CONF 0x0b) and adds a fresh one.
