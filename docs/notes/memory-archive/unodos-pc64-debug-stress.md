# pc64 debug/stress harness and metal test campaign

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-pc64-debug-stress.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---

Built 2026-07-20/21: a metal-testing harness for [[unodos-pc64-decoupling]] pc64 plus a
five-machine test campaign. **RETIRED 2026-07-21: the whole campaign (43 commits) was
fast-forwarded onto `master` and one landing commit added (`bf75ad3`); the
`pc64-debug-stress` branch is DELETED local + origin.** Work is on **master** now — do NOT
look for the branch. Worktree still `C:\Users\arin\Documents\Github\unodos-debug`.

**master build default is now PRODUCTION** (`build.sh` UNO_DEBUG default flipped 1→0; prints
"[build] PRODUCTION build"). The debug/test harness is **opt-in**: `UNO_DEBUG=1 ./build.sh`
(what SPECTEST/stress/dbg scripts + the flasher's Developer options use). Debug-only
`-DUNO_NO_DETACH` lives inside the UNO_DEBUG block, never in a prod build.

**NEXT SESSION reads `pc64/NEXT-ITERATION.md`** = "networking to first lease": localize the
AX88179 DHCP failure with the new frame counters → fix ax88179.c → bring up stack+TLS →
convert the networking SKIPs to real conformance tests → then WiFi F12 by the same method.
Plus quick metal reads (MacBook F9 splash line, Surface F14 power-off). SPECTEST baseline
59 PASS / 0 FAIL / 7 SKIP.

**Two authoritative docs live in the repo — read these first:**
- **`pc64/METAL-FINDINGS.md`** — the findings catalogue (F1-F10), per-machine runs, evidence.
- **`pc64/FIX-PLAN.md`** — prioritized fixes/optimizations (P0-P4) with verification recipes.
- `pc64/DEBUG.md` — how to run it. Raw reports: `~/unodos-metal-reports/<machine>-<date>/`.

**USER RULINGS (standing):** catalogue findings, don't fix OS bugs ad hoc — fixing is a
separate session. DO fix harness/telemetry that blocks testing. **Merge the branch only when
done** (merging as-is would make UNO_DEBUG=1 + detach-off the default on master — wrong).
P0/Blt were implemented only because the user explicitly said yes to them as measured
experiments.

**THE HEADLINE MEASUREMENT:** render ~3 ms vs present ~250 ms (82x) — pc64 was entirely
**framebuffer-bound**, because every machine tested maps the GOP framebuffer UNCACHED (UC MTRR).
Optimising the rasteriser would have bought nothing. Fixed by writing fewer bytes + using the
firmware blitter; **present is now 4.2-6.2x faster on metal** (Yoga: 238576 -> 55889 us,
fps 5 -> 17). Two runs on the same machine agreed to <1%, so the numbers are trustworthy.

**DONE (metal-proven):** dirty *spans* not rows; 64-bit stores; low-res 1:1 present when fb is
slow; **`choose_present_path()` picks GOP Blt() when it measures >=2x faster** (Yoga: 108018 vs
26665 KB/s = 4x — this captured most of P3's value with none of its MTRR risk; `gBltFast` is
kept separate from `gUseBlt` so the detach gate is unaffected, and cleared on detach).

**OUTSTANDING (for the fix session), roughly by value:**
- **F4 (S2)** I2C-HID binds on NO tested machine (`addr=0` where controllers are found;
  `ctrls=0` on the Yoga/Latitude). Fix = enumerate from **ACPI** (`_HID PNP0C50` + `_CRS
  I2cSerialBus` gives the slave address we never discover); we already ship the AML interpreter.
  Knock-ons: kills the Surface OSK and dissolves F6.
- **F8 (S1)** detach strands a USB-booted system (no USB mass-storage driver) — gate on the
  ACTUAL boot device, not "some eligible volume". Debug build currently compiles
  `-DUNO_NO_DETACH` as a workaround.
- **F6 (S2)** neither Surface nor X1 detaches (caused by F4) — so the whole M3 native stack has
  never run on metal.
- **F7 (S2)** `pc64_font.c` `x << 6` is UB for negative x (caught by UBSan) — `x * 64` + clip +
  audit centred-text callers. Silent UB in release builds.
- **F3 (S3)** the fb is still UC; P3 (rebuild MTRRs so UC covers only real BARs) would give more
  but Blt already took most of it — now lower priority.
- **F9** MacBook Pro 2013 won't boot (untriaged, deferred by user); **Latitude** boot-selection
  unresolved (`BOOTS.TXT` now answers "did it boot at all?").
- **P4** USB mass storage over xHCI — the real enabler for F6/F8.
- Small: `crash_vol()` caches only on success (rescans every call when nothing qualifies);
  splash banner overlaps the loading bar.

**DONE 2026-07-21 — Yoga run 4 (worst-frame build `debug-local-20260721-0326`) answered the
render-vs-present question: the hitches are in RENDER (new finding F11).** `present_max` never
exceeded 84 ms in any pass (Blt bounds present on this machine); the single >100 ms hitch was a
**2.34 s `unoui_render_ui()` call during `stress:close`**. Pass 1's render_avg ~75 ms reproduces
across all four Yoga runs — some resident window's draw callback costs ~25x the rest of the
scene. Runner3D's felt hitches are 97 ms render + ~80 ms present stacking to ~180 ms frames that
the per-half 100 ms threshold never counts. Report saved: `x13yoga-2026-07-21-WORST/`; findings
+ fix direction written into `pc64/METAL-FINDINGS.md` (F11).

**2026-07-21 AUTONOMOUS FIX PASS — the whole findings list actioned in one run (uncommitted at
time of writing; commit/push/deploy pending code review).** Every open finding fixed or shipped
as opt-in; QEMU-verified where possible; full status in `pc64/METAL-FINDINGS.md` (new top
section). Headlines:
- **F11 root-caused + fixed**: the multi-second render spike was `pc64_write.c wr_layout`
  calling `align_at()` (O(paragraph) backward walk) per wrapped line = O(doc²), inside draw.
  Now O(n) via incremental `pstart`. Plus a per-window draw profiler (unoui gained
  `unoui_profile_win` hook; PF snapshot `windows:` line names the costliest window), TOTAL
  (render+present) hitch counting, and an in-draw window name in hang reports.
- **F4 fixed**: ACPI PNP0C50 `_CRS`/`_DSM` I2C-HID enumeration (`acpi_host.c` →
  `uno_acpi_i2c_hid_enum`, `i2c_hid.c` → `uno_i2c_hid_acpi_retry` post-`uno_acpi_start`).
- **F8 fixed**: boot-volume-aware detach gate (`boot_device_is_usb()`) + **P4 `usbmsc.c`** (USB
  Bulk-Only-Transport MSC over xHCI, blkdev backend post-detach). Attached-mode USB NIC access
  is `usbio.c` (EFI_USB_IO).
- **F12 diagnostics**: `wait_alive` autopsy (CSR/PRPH dump + brute-RB scan) + gen3 PNVM doorbell
  (was a dead if-block — likely the AX210 cause). Next batch NETLOG names booted-vs-not.
- **F9** watchdog 120 s grace until shell heartbeats; **F7** text_pen `x*64`; **P2.2** crash_vol
  negative caching; **Surface Blt** batched into `blt_present_banded` (one Blt per band);
  **P3** opt-in `mtrr-wc` WC-framebuffer MTRR rebuild (`pc64_mtrr.c`, self-reverting).
- **SPEC.md** (372 contracts) + **UNO_ASSERT** + **SPECTEST** (`pc64_spectest.c`, STRESS.CFG
  `spec` → `CRASH\<M>\SPECTEST.TXT`, 24/24 green on real FAT via `tools/spectest_qemu.py`).
  Fixed 6 spec divergences (S-FAT-28, S-NET-08/19, S-MOD-12, S-UUI-04, S-INST-07), each
  regression-checked. **OPEN: S-LIBC-06** — a TRUNCATING `snprintf(b,8,"%s",long)` HANGS pc64
  (watchdog HG), reproducible, NOT root-caused; SPECTEST avoids executing it.
- New STRESS.CFG keys: `spec`, `mtrr-wc`, `nonet`. QEMU note: **vvfat corrupts multi-cluster
  writes on readback** → use `tools/spectest_qemu.py`'s real-FAT (mformat) path for content
  assertions; nettest_stage's NETLOG check is now vvfat-tolerant (clean power-off = pass).

**COMMITTED + PUSHED + DEPLOYED** as `1f6927a` (branch tip; flasher on
`\\behemoth\unreplicated\unodos\pc64\`, build `20260721-0908`). Three adversarial code-review
passes ran; all findings fixed before commit — notably two MTRR S1s (default-type zeroed on
apply → RAM would go UC; phys_hi u32-truncation → >4GB aliasing) and an S2 stack overflow I'd
introduced in the F11 perf-line loop (snprintf intended-length overrun). All QEMU regressions
green (crash-safe, crash-pipeline, SPECTEST 24/24 deterministic on real FAT, net stage).

**2026-07-21 batch #2 (build 0908) + fixes (`c46bbdd`, deployed):** ran all laptops again.
SURFGO/X13YOGA/X1CARBON gave full telemetry; **MacBook still hangs at the FULL loading bar with
NO telemetry** (F9 — Apple firmware, our FAT never reaches the USB stick; also no debugcon on the
laptops, so the SCREEN is the only channel). **Surface stalls forever on "Shutting down"** — its
firmware ignores EFI_RESET_SHUTDOWN. Two fixes shipped:
- **F14 (Surface shutdown):** ACPI S5 fallback (`uno_acpi_poweroff` → uACPI enter_sleep_state(S5),
  SLP_TYPa|SLP_EN to PM1_CNT) after the firmware ResetSystem returns. QEMU still uses ResetSystem.
- **F15 (splash text):** every loading bar now names its stage in white under the bar (graphics/
  drivers/input/starting up), ALL builds; post-bar-4 core init keeps updating it (chime/timer/
  bootlog/detach/desktop). Cheap via dirty-span. So the MacBook's next boot SHOWS its last stage
  on screen instead of a bare full bar — that's how we'll finally locate the F9 hang.

**2026-07-21 batch #2 results (build 1432 = c46bbdd; `~/unodos-metal-reports/batch2-2026-07-21/`,
findings `feef2f6`):**
- **Surface Blt anomaly METAL-CONFIRMED FIXED**: present_avg 163-227ms → **48-56ms**, fps 3→19-20,
  ~77 hitches/pass gone. The banded Blt was exactly right.
- **F12 autopsy resolved it: firmware NEVER STARTS** (not a missed notification). No ALIVE in any
  RB, rb_status=0 on all. AX201 (X1/Yoga/Surface): `UCODE_LOAD_STATUS=0` — ROM never launched the
  ucode after the CSR_CTXT_INFO_BA write (struct layout matches Linux; suspect the gen2 kick — our
  extra UREG_CPU_INIT_RUN write, or a missing finish-nic-init/fw-load-int arm). AX210 (Latitude):
  `0xbad0f1f2` + `RESET=0x11` — **SW_RESET bit still set**, PRPH inaccessible; gen3 reset/TOP
  handshake incomplete. PNVM now loads (my fix works) but moot until ALIVE. Both metal-iterative,
  catalogued not blind-patched (no card to test against).
- **F4**: ACPI enum now FINDS the PNP0C50 device on Surface (acpi_hits=1, ctrls=3) and Latitude
  (acpi_hits=1, ctrls=0 — the exact PCI-finds-nothing case); binding still fails (present=0
  everywhere). Real progress; the targeted probe still can't talk to the pad. Not blocking.
- **F14 Surface shutdown**: reached "shutting down now" cleanly (uptime 71.5s), 1 boot, no
  hang/residue report → consistent with a clean ACPI-S5 power-off (user confirms physically).
- **MacBook (F9)**: STILL no telemetry folder — unchanged. Its only channel is now the F15 splash
  text; **read the last white line on the Mac's screen to locate the hang stage.**

**2026-07-21 flasher rework (`46d1fac`, deployed):** the flasher now embeds BOTH the production
(UNO_DEBUG=0) and debug (UNO_DEBUG=1) images. **Developer options OFF = flash clean production;
ON = flash debug + pick tests** (conformance/spec, WiFi/Ethernet, mtrr-wc, stress passes, auto
power-off) → the flasher writes \STRESS.CFG. New OS keys `net-force-wifi` / `net-eth-only` give
the WiFi/Eth toggles teeth. Intel firmware ships in the debug tree only (licence). Standing rule
in repo CLAUDE.md updated (supersedes "ships ONE flasher = debug build"). So conformance results:
tick **Conformance** in Developer options and each machine writes CRASH\<M>\SPECTEST.TXT (they
were absent last batch because the sticks had no `spec` key).

**2026-07-21 SPECTEST expanded to the frameworks + apps (`0fefd32`, deployed):** 24 → **58 [auto]
checks + 7 SKIPs**, real-FAT green, deterministic. Covers unoui (widget lifecycle + events),
uno3d (NEW S-3D: soft backend raster/cull/clip via u3d_last_tris — transform state is private),
unosound (S-SND: the live sequencer; the offline unosound_render baker is NOT kernel-linked),
unomedia (NEW S-MEDIA: WAV sample-exact, MIDI synth, real tiny MP3+AAC clips embedded via
`spec_media.h`/`tools/gen-spec-media.sh`; image decoders are PHOTOS.UNO-only → skip), Editor
(S-WRITE via new `#ifdef UNO_DEBUG` hooks in pc64_write.c), Music (pc64_media_open path), Studio
(file save/load + **Python-on-metal**: load PYRT.UNO, run a uno.App snippet, verify uno.write side
effect). UnoC compile/build are UI-only in STUDIO.UNO → SKIP (host tools/ucc_test.c). New SKIP
status for deferred contracts; WiFi/LAN/AI-assistant are SKIP-pending-live-networking (write the
real ones once networking works). Gotchas hit: `um_audio_*`/pc64_media read via `uno_fs_*` (write
test files with uno_fs_write, NOT uno_fat_write — different vol indexing); uno3d transform state
is static so test via last_tris not pixel reads.

**2026-07-21 Yoga + AX88179A ethernet round (analyzed):** the USB-ethernet stack WORKS — enumerated
ASIX `0b95:1790` via UsbIo, reset, read a valid MAC (8c:ae:4c... ASIX OUI), `net-eth-only` plan
correctly skipped WiFi, link UP in 1ms. **Failure is DHCP: no lease in 12s.** Old NETLOG couldn't
say which side, so I added link-level frame counters to net.c (tx/rx/arp/ip, reset per net_init, via
a `nic_tx()` wrapper + count in net_poll's RX drain) surfaced on the eth DHCP-fail line. Next round
the log says it outright: tx==0 → our send/bulk-out; tx>0 rx==0 → RX parse/cable/dead server; rx>0
ip==0 → RX filter/descriptor offset; else → DHCP option parsing. Still needs the adapter present to
fix (metal-iterative). ax88179.c RX descriptor parse + TX 8-byte header are the prime suspects.

**2026-07-21 test-suite REORG (`300dea4`, pushed + deployed):** organised the harness into four
SUITES the flasher exposes individually + a cross-cutting interactive switch.
- **SPECTEST areas**: `storage system frameworks apps network` (+ opt-in `interactive`). Gated via
  new `spec=<areas>` value (bare `spec`=all). `pc64_stress_cfg_value()` reads it; section banners in
  the output. VERIFIED: full run still 58/0/7 clean; `spec=storage,frameworks` → 28/0/1 (gating works).
- **INTERACTIVE area (S-INT)**: real-keyboard check (press K) + display colour/text check (RGB bars,
  Y/N) — the paths injection can't prove. Gated by the `interactive` key, bounded ~25s, SKIP on
  timeout so an unattended stick never hangs. New debug hook `uno_pc64_dbg_key_wait` in uefi_main.c
  latches real keys in map_key under a capture flag. S-INT-03 (audio) is a documented SKIP (needs the
  main-loop audio pump during the blocking suite; audio metal-pending).
- **Flasher Developer options rebuilt** (UnoSettings v2 + DevForm): 1.Conformance (per-area) /
  2.Standard (stress) / 3.Network (WiFi+eth) / 4.Diagnostics (mtrr-wc + crash self-test), each a
  master toggle over its own tests, + "include interactive tests" box. Back-compat loads v1 keys.
- **Fixed spectest_qemu.py verdict**: it matched substring "FAIL" and tripped on the legend
  "(PASS/FAIL/SKIP)"; now reads the summary's FAIL count. (Pre-existing bug, not from this change.)
- Nothing was genuinely redundant to delete — stress (fuzz) vs SPECTEST (assert) are complementary;
  the redundancy removed was the flat, unstructured flasher UI + ungrouped SPECTEST ordering.

**2026-07-21 flasher: reconfigure + disabled-suite fix + rename (`d4943d3`, pushed + deployed):**
- **BUG FIXED — disabling the Stress Test suite ran it endlessly.** The flasher wrote `passes=0`
  for "off", but the stress driver reads `passes=0`/absent as ENDLESS (0=unlimited). Added `nostress`
  as the real off switch: `arm()` disarms the fuzz driver when it's set (one-shot net/spec still run);
  the flasher emits `nostress` when the Stress Test master is unticked, `passes=N` only when on.
- **Reconfigure an already-flashed disk (no erase).** The UnoDOS volume is an ESP (type GUID
  C12A7328...), which Windows HIDES from Explorer → can't edit STRESS.CFG by hand without admin/
  diskpart. New **`flash/UnoReconfig.cs`** parses GPT→ESP partition→FAT32 BPB→root dir, finds
  \STRESS.CFG and overwrites its existing cluster + size IN PLACE over the raw \\.\PHYSICALDRIVE
  handle the flasher already opens elevated. Deliberately narrow (no cluster alloc, no dir growth) so
  it can't corrupt the volume; refuses safely if absent (production disk) or >1 cluster. New
  "Reconfigure tests (no erase)" button writes the current Dev-options StressCfg. VERIFIED e2e: built
  a real GPT+FAT32 image, reconfigured it, mtools read back the exact new STRESS.CFG (52B, size field
  updated). Added UnoReconfig.cs to build-flasher.ps1's csc list.
- **Renamed the "Standard" suite → "Stress Test"** (DevForm, summary chip, DEBUG.md); noted 0 passes
  = endless, unticking the suite = truly off.

**2026-07-21 close-out — S-LIBC-06 FIXED + review findings (`3e02177`, pushed + deployed):**
- **S-LIBC-06 root-caused + fixed** (the long-open truncating-snprintf hang). NOT UBSan as guessed:
  `pc64_libc.c`'s `PUT(ch)` macro gated `buf[o]=ch; o++` on `o+1<cap`, and `%s` called `PUT(*s++)`,
  so once the buffer filled the `s++` side effect stopped firing and `while(*s)` spun forever (same
  latent bug in the `%f` "<flt>" path + `%c` va_arg desync under truncation). Fix: `PUT` evaluates
  its arg exactly once, before the space check. SPECTEST **S-LIBC-06 now EXECUTES** the truncating
  case (was skipped because it hung) → suite is now **59 PASS / 0 FAIL / 7 SKIP**, clean power-off in
  QEMU. Reproduced + fixed on host gcc first, so the root cause is certain.
- **Review findings fixed** (adversarial review of the session diff cleared UnoReconfig's core FAT
  math as correct — no corruption path from flasher inputs). Hardened anyway: range-check the matched
  dir-entry's first cluster vs the volume cluster count before the raw disk write; refuse a
  multi-cluster STRESS.CFG (orphan-cluster guard); cap new config at 511B to match the OS read buffer.
  Flasher UI: gate the crash self-test checkbox on the Stress Test suite (it only fires on stress pass
  1) + summary chips now match what StressCfg actually emits. net.c `nic_tx` counts sends that left
  the driver, not attempts.

**METAL-BLOCKED (cannot fix without the hardware in hand — genuinely blocked, not deferred):**
F4 I2C-HID binding, F6/F8 detach on Surface/X1, F9 MacBook boot-hang, F12 WiFi firmware-launch
(AX201 gen2 kick / AX210 stuck SW_RESET), AX88179 DHCP no-lease. This session made them MORE
diagnostic (eth tx/rx/arp/ip counters, interactive keyboard+display checks) for the next metal round.

**NEXT (metal):** (0) run the reorganised conformance suite + the next AX88179 eth round (the new
counters will localise the DHCP failure); (1) MacBook — report its last splash line; (2) confirm
Surface physically powers
off now; (3) wifi fw-launch fix session with a card present (AX201 gen2 kick + AX210 stuck
SW_RESET); (4) AX88179A ethernet round. Open SW bug: **S-LIBC-06** truncating-snprintf hang.

**2026-07-21: network test wired into the harness** (`d35d1b4`, pushed, flasher deployed):
boot-time net test before the stress driver -> `CRASH\NETLOG.TXT`. USB eth (AX88179A batch
adapter) only if present and then wifi SKIPPED; else wifi with stage-by-stage trace; else
wired PCI (QEMU e2e green via `nettest_stage.py`). `nonet` key skips. Details + the waiting
batch round: [[unodos-wifi-usbeth-drivers]]. Note branch tip moved 686619e -> d35d1b4.

**2026-07-21 evening — Phase 4 tests + reconfigure bug (`fa0e55f`, master, DEPLOYED to the share,
UNPUSHED):**
- **arin's field report** ("Reconfigure: stress test can't be turned off, passes has no effect,
  passes=1 = infinite loop") **root-caused: not an OS/flasher logic bug — a STALE ON-STICK OS.**
  `nostress` only landed 07-21 15:36 (d4943d3); Reconfigure rewrites STRESS.CFG but never the OS,
  and the older embedded OS ignores unknown keys → armed endless run. Current-OS key semantics
  PROVEN in QEMU (`tools/stresscfg_qemu.py`: nostress = disabled, passes=1 = one pass + self
  power-off). Fix = **version gate**: build.sh stamps `cfgver: 2` in BUILD.TXT;
  `UnoReconfig.CFG_GENERATION` must match or Reconfigure refuses with "reflash instead" (bump BOTH
  when adding/renaming keys). **arin's stick needs one reflash with the new flasher.**
- **STRESS.CFG staging leak closed**: build.sh now ALWAYS rewrites the shipped default (the
  if-absent guard could ship a QEMU harness's allow-force endless config); spectest_qemu +
  dbg_crash_test overlay their cfg into the disk image only, never build/esp.
- **Phase 4 built — test_netlive() replaces the netstub SKIPs**: S-NET-30 (lease + TCP round-trip;
  link-up-no-lease = FAIL with the frame counters), S-WIFI-20 (live join when Intel card+creds
  present; FAILs with iwl_status_str until MLME lands), S-AI-01/02 (DNS → CA-validated TLS to
  api.anthropic.com + full HTTPS request/response on ONE connection; the keyless 401+JSON is the
  proof). spectest_qemu boots a SLIRP e1000 → live checks run in regression (needs host online).
  The stub id "S-NET-20" collided with SPEC's RX-drain contract → renumbered S-NET-30.
- **Building them caught 3 real transport bugs** (all would bite Browser/AI on metal):
  net.c S-NET-15/23 (fixed 4096 window over 2 KB rxq + ACKing dropped bytes → every >2 KB TLS cert
  flight corrupt; now free-space window, 8 KB rxq, ACK-only-stored, overlap trim); tls.c low_read
  (data+FIN in one poll batch = bytes thrown away, EOF reported); tls.c tls_close (BearSSL
  br_sslio_close loops FOREVER on a dead transport once shutdown_recv is set — 20 s watchdog reset;
  replaced with a bounded polite close). **SPECTEST baseline now 62 PASS / 0 FAIL / 4 SKIP.**
- Debugging pattern that worked: TLSTRACE failure-path logs in tls.c (kept, debug-only) + a
  throwaway tcp_input segment trace + `stresscfg_qemu.py`-style debugcon boots.

**2026-07-21 late — Yoga "input dead" report RESOLVED + progress banner (`2f2c96e`, deployed):**
The freshly-flashed debug stick (all suites + interactive + mtrr-wc) was never hung: telemetry
read on devbuntu showed the full ~90 s boot test phase completed (60/2/7), the operator's mashed
keys landed in the silently-waiting interactive prompts (S-INT-01 caught a 'b', S-INT-02 PASSed
on a real Y — keyboard provably fine), mtrr-wc REFUSED on the Yoga (>10 MTRRs to tile), and
`nostress` disarmed correctly. **Bonus: the eth round localized the AX88179 DHCP failure —
link UP, tx=1261 rx=0 → the RX path never sees a single frame (ax_recv / bulk-in polling), TX
and LAN are fine. That is NEXT-ITERATION Phase 1 answered; Phase 2 = fix ax_recv.**
Fixes shipped: a full-width amber "BOOT TESTS" banner painted from inside the blocking phase
(uno_dbg_progress in pc64_nettest.c, fed by net traces + SPECTEST emits, cleared at phase end)
so a blocked machine names what it is running; splash DEBUG banner moved H/2+68 → +92 (it sat
dead on top of the F15 stage line at +69). Both verified by QEMU monitor-socket screendumps.

**2026-07-21 — AX88179 eth RX FIXED (`e0b093c`, deployed, metal-pending verify):** the Yoga
localization (tx=1261 rx=0) was NOT the descriptor parse (that's rx>0 ip==0) — it was the MAC
**medium mode**: ax_reset hardcoded gigabit+full-duplex and the driver never programmed the
medium from the PHY's negotiated speed. Wrong speed → MAC RX clock domain wrong → TX queues but
RX dead; also missing AX_MEDIUM_EN_125MHZ for gigabit. Fix = ax_apply_medium() reads GMII_PHY_PHYSR
(reg 0x11), builds medium from real speed/duplex (giga→GIGAMODE|EN_125MHZ, 100→PS, 10→neither,
+FULL when negotiated), sets matching bulk-in qctrl, called from ax_link() before DHCP; traces
negotiated speed to NETLOG. Port of Linux ax88179_link_reset. **NEEDS the AX88179 adapter on any
laptop to verify** (QEMU has none). If rx still 0: next suspects ax_recv descriptor/pkt_len/pad,
then TX header pad-bit — see NEXT-ITERATION Phase 2.

**2026-07-21 — WiFi (F12) + trackpad (F4) fix attempts on Yoga (`1ad4002`, deployed, UNPUSHED,
BOTH metal-pending — can't verify in QEMU):**
- **F12 AX201 (UCODE_LOAD_STATUS=0, ROM never starts):** `load_fw_gen2` now matches Linux
  `iwl_pcie_ctxt_info_init` — ARM the FW-load interrupt mask (`CSR_INT_MASK=ALIVE|FH_RX`, the
  `iwl_enable_fw_load_int_ctx_info` step we skipped) BEFORE the `CSR_CTXT_INFO_BA` kick, and drop
  the spurious `UREG_CPU_INIT_RUN=1` (gen3/IML-only). Autopsy now reads back CTXT_INFO_BA + FH_INT
  + placed-fw-dram so the next Yoga boot separates "CSR write dead / fw not placed / missed ALIVE."
- **F4 trackpad — TWO root causes:** (1) the ACPI retry only reached a controller via a PCI BAR, so
  the Yoga (LPSS I2C in ACPI mode, hidden from PCI → ctrls=0) had nothing to probe. `uno_acpi_i2c_hid_enum`
  now returns the controller's own `_CRS` MMIO base (`ctrl_mmio`) + evals its `_PS0`; retry probes it
  directly, logging COMP_TYPE readback. (2) `uacpi_find_devices` SKIPS `_STA`-not-present devices, and
  a power-gated LPSS touchpad reads `_STA=0` in our read-only ACPI ctx (Yoga's acpi_hits=0) → added an
  `_STA`-blind namespace walk fallback matching PNP0C50/ACPI0C50 by `_HID`/`_CID`. HID-descriptor
  signature still gates binding. **Next Yoga boot's i2c-hid log says: no PNP0C50 at all (→ PS/2 pad,
  wrong subsystem) / COMP_TYPE wrong (→ still power-gated) / slave NAKs (→ timing).** NOTE: unconfirmed
  whether the Yoga pad is even I2C-HID vs PS/2 — the diagnostics settle that.

**2026-07-21 late — stress driver REMOVED + build made 5.7x faster (commits 70af29e, 5b56779 on
master, UNPUSHED; deploy in progress):**
- **Stress fuzz driver removed** at arin's request — it ran even when the flasher wrote `nostress`
  and looped forever. Reproduced on the Yoga with a CURRENT OS (cfgver 2, disk STRESS.CFG=`nostress`)
  so this was NOT the stale-stick bug; the OS config gate has a real defect NOT yet root-caused.
  Disconnected two ways so no config can revive it: `pc64_stress_tick()` hard early-returns +
  its call site in pc64_uui.c is commented out. Flasher "Stress Test suite" disabled/force-unticked,
  `StressCfg()` always emits `nostress`. Conformance (SPECTEST) + net suites are a SEPARATE path
  (pc64_nettest_tick), untouched.
- **GOTCHA that removal exposed:** the SPECTEST harness relied on the stress driver's `passes=N`
  auto-poweroff to end the guest → after removal "guest did not power off (hang)". Fixed by giving
  the one-shot path its OWN headless poweroff: `nettest_finish()` (pc64_nettest.c) powers off when the
  suites finish IF `poweroff` (new explicit key) or legacy `passes`/`once` present, vetoed by
  `noshutdown`; no STRESS.CFG = never powers off. Reachable from every exit incl. the `nonet` early
  return. spectest_qemu.py now writes `poweroff`. TODO: prune stresscfg_qemu.py + dbg_crash_test (they
  test removed functionality); root-cause why the OS ignored `nostress`.
- **Build 5.7x faster (246s→43s):** `build.sh` recompiled ALL ~530 TUs serially every run (294 BearSSL
  + 127 MicroPython + 19 uACPI never change) on a 12-core box, no cache. Fix = (1) ccache via
  `PATH=/usr/lib/ccache:$PATH` (ships a mingw symlink → transparent; `apt install ccache` done, 5G,
  NOCCACHE=1 opts out; preprocessor-mode fallback means minute-stamped UNO_BUILD_ID doesn't bust it);
  (2) a `pc`/`pcwait` bounded-parallel helper (JOBS=nproc) wired into every compile loop with a pcwait
  before each link. `pc` MUST `return 0` or a not-full pool trips `set -e`. Byte-identical image
  (debug SPECTEST 62/0/4, prod clean). Steady-state iteration ~43s, now link/packaging/PYRT-codegen
  bound → `UNO_PYRT=0 UNO_STUDIO=0 UNO_PHOTOS=0` cuts it further for net/driver work.
- **Yoga metal telemetry (build 2329):** WiFi AX201 — my int-mask fix armed (MASK=80000001) + CTXT_INFO_BA
  writes/reads-back, but FH_INT=0 → ROM's DMA never runs, fw self-load never starts (deeper than the
  int-mask). Eth AX88179 — medium-mode fix ran (negotiated 1000full→medium=01b3) but RX still 0
  (tx=1261 rx=0) → medium wasn't it; next suspect ax_recv descriptor parse. Both still metal-iterative.

**2026-07-22 — eth DHCP works on metal + WiFi >4GB DMA fix attempt (commits after 5b56779,
PUSHED to 6ee44c3, flasher deploying):**
- **AX88179 ethernet WORKS on metal now**: real DHCP lease 192.168.2.157 on the physical LAN
  (was tx=1261 rx=0 = RX-dead before; likely earlier failure was late USB insertion / marginal
  link). RX functional. REMAINING: DNS stuck at SLIRP default 10.0.2.3 (net.c:~11) because DHCP
  option 6 wasn't applied, + ping gw no reply. Root-cause hypothesis: AX88179 RX still truncates
  larger frames (ax_recv suspect) → ACK tail (DNS opt, after opt3) lost, replies dropped. Added
  net_dhcp_ack_len/_had_dns/_had_rtr diagnostics: next run says "our RX cut it" (short ack_len,
  had_rtr=1 had_dns=0) vs "router omitted opt6" (full ack_len).
- **WiFi AX201 root-cause candidate = DMA above 4GB.** Forced-WiFi run: same FH_INT=0 (ROM DMA
  never runs), no ALIVE, despite int-mask armed + CTXT_INFO_BA read-back OK. KEY CLUE: autopsy
  addresses CTXT_INFO_BA=0x1_42bf0000, fw_dram0=0x1_42bf1000 — BOTH >4GB. WiFi DMA mem is static
  .bss → lands >4GB on this >4GB-RAM Yoga; gen2 boot ROM's early DMA can't reach it. FIX: back the
  fw/ctxt-info arena with AllocatePages(AllocateMaxAddress, <4GB) in iwlwifi.c (falls back to static
  on ≤4GB/QEMU). Added trace of arena base + FH_INT latch (fh_after_kick) post-kick. STAGED: if
  FH_INT goes non-zero next metal run → confirmed, then relocate the RX/cmd rings (g_rbstts/g_rbd_*/
  g_rb/g_cmd_ring — still static >4GB) the same way for full ALIVE. Metal-pending (no AX201 in QEMU).
- **GOTCHA (harness, not code):** spectest_qemu.py hung twice ("guest did not power off") but the
  SAME source with UNO_DBGCON=1 passed 62/0/4, and 3 subsequent DBGCON=0 runs ALSO passed → it was
  a LIVE-NETWORK flake in S-AI-02 (real TLS to api.anthropic.com), not a regression. When the gate
  hangs, rule out the live S-AI test before suspecting code: rebuild UNO_DBGCON=1 and boot with
  -debugcon file:... -global isa-debugcon.iobase=0x402 to see the last kernel-log line.

**HARNESS COEXISTENCE POLICY (read `pc64/HARNESS-POLICY.md` before touching harness files) —
effective 2026-07-21:** a second agent is refactoring the debug/test harness into **unoautomate**
(branch `unoautomate`, worktree `Documents\Github\unodos-unoautomate` — HANDS OFF both). To stay
mergeable: **driver files are yours to edit freely** (iwlwifi/ax88179/e1000/igb/mrvlwifi/net.c+.h/
tls.c/pc64_http + NETWORK.md/METAL-FINDINGS.md/NEXT-ITERATION.md). **Frozen core = additive-only**
(pc64_nettest.c, pc64_spectest.c, pc64_stress.c, uno_debug.h/.c, uefi_main.c harness parts,
pc64_uui.c debug-shell parts, build.sh harness/test sections, DEBUG.md, tools/spectest_qemu.py +
nettest*.py): append at the end, go through existing entry points (uno_dbg_log/net_trace/check/
note, UNO_ASSERT, uno_dbg_progress), NEVER rename/reorder/move functions or change uno_debug.h
signatures. **Never mix driver + frozen-core in one commit** (driver commit first, test second);
**prefix frozen-core commits `harness:`**. Genuinely new harness capability → append a dated entry
to `pc64/UNOAUTOMATE-REQUESTS.md` (don't build it into the old core). A one-time checkpoint (told
when the seam lands) switches diagnostics to a new `unoauto_*` API. NOTE: this session's already-
pushed commits (70af29e, 5b56779, 6ee44c3) predate the policy — they mix territories / restructure
frozen core; do NOT rewrite pushed history (never force-push; the unoautomate branch forks here).

**2026-07-22 — WiFi >4GB DMA theory DISPROVEN on metal (Yoga, build 0219).** The <4GB arena fix
worked mechanically (arena base=0x76cfe000, CTXT_INFO_BA + fw_dram all ~2GB, logged "below 4GB"),
but **FH_INT=0 AND fh_after_kick=0 — the ROM's DMA engine still never runs**, no ALIVE, UCODE_LOAD_
STATUS=0. So memory addressing was NOT the AX201 root cause; do NOT bother relocating the RX/cmd
rings. Device is up (GP_CNTRL=08040005 clock+INIT_DONE, MASK=80000001 int armed, CTXT_INFO_BA reads
back what we wrote) yet the gen2 boot ROM never acts on the context-info kick. The <4GB change +
instrumentation (arena-base trace, fh_after_kick latch) are KEPT (defensive + useful), marked
confirmed-not-root-cause. NEXT candidate hypotheses (grounded, unverified, low-confidence): (1)
missing iwl_finish_nic_init ready-poll immediately before the kick (ROM may latch a stale/zero BA if
kicked before truly ready); (2) a context_info struct field the ROM validates (the `valid`/version
u16 we leave 0 — Linux struct is mac_id/version/size/valid); (3) an RFH/RX-ring hardware program
step the ROM expects pre-load. WiFi is now a hard silicon problem, diminishing returns per metal
cycle — the AX88179 eth path (DHCP works; RX-truncation/DNS-option) is far more tractable.

**Reading a stick on Carbon (Windows, ssh works but no lsblk):** the flashed UnoDOS volume is one
big ESP-typed FAT32 partition (hidden from Explorer). Mount it read it unmount via base64-EncodedCommand
PowerShell over ssh: `Get-Partition -DiskNumber <n> | sort Size -desc | select -first 1`, then
`Add-PartitionAccessPath ... -AccessPath 'Q:\'`, Get-Content the CRASH files, `Remove-PartitionAccessPath`.
The USB stick is usually PhysicalDrive 1 (`Get-Disk | ? BusType -eq USB`). ssh carbon runs elevated
(storage cmdlets + raw \\.\PhysicalDrive both work). Strip `#< CLIXML`/`<Objs` progress noise from output.
Raw-sector dump is a poor fallback here: a 29GB FAT32 has ~15-29MB of FAT tables before the data region.

**Harness facts worth keeping:** bounded runs `passes=N` (default 3, ~30 s/pass) and the run
**powers the machine off itself** when done; **F12** stops the driver; `\CRASH\BOOTS.TXT`
(earliest proof-of-boot), `BOOTLOG.TXT` (every boot, pre-detach), `BOOTENV.TXT`, `PF###` perf
snapshots. Reading sticks: `ssh carbon` works (raw-dump via base64 PowerShell, then parse with
mtools); devbuntu has a **USB write-blocker** (`blockdev --setrw` first, restore `--setro`).
**Always cross-check `cpu:`/`fb_base` before attributing a report to a machine** — a reused
stick's stale telemetry is indistinguishable from a failed run, which nearly caused a
misreport. Git Bash heredocs eat `\\` — build device paths/escapes from char codes.
