# unoautomate = generalizing the pc64 debug harness into systemwide logging/automation (Python-scriptable); seam-first strategy to coexist with the parallel WiFi agent

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-unoautomate.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---


**✅ FULLY MERGED INTO MASTER + PUSHED + BRANCH RETIRED (2026-07-23, merge
`1a1bb0a`, master now `33e64ef` = origin/master).** The remaining unoautomate
line (unosecure security subsystem + unoscript ui/app/power surfaces + the
`install <disk>`/`mkdir` URC verbs — the 9 commits past the earlier partial fold
`454a027`) was merged into master via a union merge. The one real collision was
the duplicated "install over the link": resolved on the **unostorage foundation**
(arin's call) — unoautomate's `install <disk>` verb already composed
`unostorage_prepare_esp`, so it was KEPT beside master's low-level disk-authoring
verbs + `installer.c` raw-clone path (two tools, one framework); `mkdir`
de-duplicated to unoautomate's idempotent `uno_fat_sync`-durable `do_mkdir`.
Conflicts were only in `unoauto_remote.c` (dispatch), `tools/unoauto_remote.py`,
`UNOAUTOMATE-REQUESTS.md`. Prod build green. **Branch `unoautomate` +
`r8169-zima-bringup` deleted (local + origin), their worktrees removed**; content
preserved via the merge 2nd parent + `backup/unoautomate-pre-merge` +
`pre-merge/origin-master` tag. `unodevices` was rebased onto the new master (3
feature commits, builds; local-only, unpushed). **KNOWN PRE-EXISTING (not a merge
regression):** `tools/automate_qemu.py` headless smoke fails identically on BOTH
pre-merge `unoautomate` and merged master — guest boots but `AUTOMATE.PY` never
reaches `unoauto.poweroff()` in 180s (likely slow TCG or a real automate-path gap;
NOT the security subsystem — login gate no-ops on a fresh disk). See
[[unodos-flasher-deploy-retired]]. The rest below is historical.

**OWNERSHIP + STABILITY POSTURE (arin, 2026-07-22):** I (the unoautomate agent)
am responsible for **all** development on unoautomate AND for writing/owning the
contract — `unoauto.h`, `REMOTE.md`, `pc64/HARNESS-POLICY.md`, the changelog.
The interface is **best-effort stable, NOT guaranteed — breaking changes WILL
happen** (mine to make). Other agents must re-read the contract + changelog
after every pull; `UNOAUTO_API` bumps mark breaks. HARNESS-POLICY.md was
rewritten (5e1c467) from a WiFi-agent-specific doc into a general contract for
ANY agent: §0 stability disclaimer, generalized territory ("whatever your task
owns"), my files marked don't-edit. unoauto.h `[STABLE]` softened from "FROZEN"
to "intended, unlikely to break." Requests still land in UNOAUTOMATE-REQUESTS.md.

**unoautomate** (started 2026-07-21): replace the pc64 UNO_DEBUG harness
(`pc64_nettest.c`/`pc64_spectest.c`/`uno_debug.c`) with a systemwide
logging + automation subsystem — probe every process/thread, hook call
surfaces, drive UI/apps/APIs from Python (binds to the PYRT PyHost, which
IS on master), robust channelled logging.

- Branch `unoautomate`, worktree `Documents\Github\unodos-unoautomate`.
- **Stage 1 LANDED ON MASTER + pushed** (`05b1b04`, 2026-07-21): the WiFi
  agent parked at `6ee44c3` = the branch point, so it fast-forwarded with
  zero conflicts. `unoauto.h/.c` = LOG channels+ordered sinks + ctx-aware
  TEST registry; `uno_dbg_net_trace` is a wrapper over
  `unoauto_vlog(UA_CH_NET)` (NETLOG machinery = a registered sink, order
  kring→netlog preserved); SPECTEST's 15 suites are registry entries
  driven by `unoauto_test_run`. QEMU spectest 62/0/4 = baseline exact.
  DEBUG.md documents the "new diagnostics go through unoauto_*" rule.
  PROBE/HOOK/DRIVE stubbed for Stage 2 (`unoauto_probe.c`,
  `unoauto_py.c`).
- WiFi agent resumes on the new master; its worktree hasn't rebuilt yet,
  so its next build+deploy refreshes the NAS flasher (deliberately not
  done from the seam session).
- **Strategy (locked with arin): seam-first, not wait, not raw parallel.**
  A second agent is doing WiFi bugfixing for days on master in
  `unodos-debug`, editing the exact files being dissolved. Plan: land the
  seam on master at that agent's next commit checkpoint (brief pause, one
  cheap rebase), then it registers diagnostics via `unoauto_*` while
  Stage 2 builds out in NEW files (`unoauto_probe.c`, `unoauto_py.c`).
  Legacy `uno_dbg_*` entry points stay as thin wrappers until last caller
  migrates.
- **Coexistence policy for the WiFi agent** written at
  `unodos-debug/pc64/HARNESS-POLICY.md` (untracked): frozen-core files are
  additive-only, driver vs `harness:`-prefixed commits kept separate, new
  harness needs go in `pc64/UNOAUTOMATE-REQUESTS.md` instead of the old
  core. Arin delivers it to that agent.
- Do NOT deploy the flasher to \\behemoth from this branch — the share
  must keep tracking the WiFi agent's master builds.
- Repo housekeeping same day: `logo-rebrand` deleted (was empty),
  `unodos-3-legacy` + `parity-wip` pushed as frozen refs.

- **Phase A landed** (`c119459`): unoauto.h is the formal contract —
  `UNOAUTO_API 1`, [STABLE] LOG/TEST/wrappers vs [EXPERIMENTAL]
  PROBE/HOOK/DRIVE, break-with-compat-wrapper rule, dated changelog in the
  now-TRACKED `pc64/HARNESS-POLICY.md`. Arin's sync question is answered
  by mechanism: header + changelog + compiler, no manual relaying.
- **Phase B landed** (`4f4ff0c`, on master, pushed): `unoauto_probe.c` —
  subsystem rows (heap/net/fs/shell), open windows + F11 draw costs, .UNO
  roster; additive accessors `uno_dbg_win_stat`,
  `pc64_shell_win_count/title/focused`. New check S-DBG-20; QEMU spectest
  baseline is now **63**/0/4.

- **Phase C landed** (`95d3fd7` on master, pushed): HOOK live — taps at
  `libc.malloc` (injectable, re-entrancy-guarded), `fs.read/write`,
  `mod.load/unload`; payload structs outside the UNO_DEBUG gate; prod
  `unoauto_hook_fire` no-op consumes args (warning-free, verified).
  Checks S-DBG-21/22. **The two-agent loop WORKS**: the WiFi agent
  resumed, followed the policy (harness: prefix), and filed a request in
  UNOAUTOMATE-REQUESTS.md (live net checks stall the QEMU gate);
  fulfilled same-day: `unoauto_test_deadline_ms` + OVERRAN-FAIL runner
  budgets, `unoauto_deadline_left_ms()` for cooperative driver loops
  (tls.c half stays theirs), network area 90s/test, spectest_qemu.py
  salvages partial SPECTEST.TXT ("stalled after <check>"). My net.tx/rx
  tap request to them is OPEN. One rebase so far (add/add on the requests
  file, trivial). QEMU baseline now **65**/0/4.

- **Phase D landed** (`b3881bd`/`67f254f` on master, pushed): DRIVE via
  Python. `upy_port/mod_unoauto.c` = the `unoauto` MicroPython module in
  PYRT.UNO (log/probe/key/pointer/apps/launch/close_top/uptime/
  deadline_left/poweroff); automation scripts are ordinary uno.App apps
  whose tick() advances a generator → real frame interleaving. Boot
  runner: STRESS.CFG `automate` + RAW AUTOMATE.PY at a volume root
  (debug-only container bypass in uno_mod_load_pyapp). Debug-gated
  kExports tail; prod PYRT = inert stubs, verified clean. `uui.action`
  tap live. **tools/AUTOMATE.PY smoke: 17/17 apps open+close in QEMU**
  (tools/automate_qemu.py is the gate). Traps burned: .UNO import names
  cap at 23 chars (unoauto_deadline_left alias); vol 0 can be the RAM
  disk so results must write to all volumes; the script must never
  close_top its own window. Two rebases over WiFi-agent commits so far,
  both trivial.

- **Remote channel landed + PUSHED** (`c5e4ba9` = `origin/unoautomate`,
  fast-forward from Phase D `67f254f`; local base was master `d9488cb`.
  origin/master had since moved +3 ahead — did NOT rebase onto it, pushed the
  branch as-is per arin. So other agents now see the updated contract:
  HARNESS-POLICY changelog, the unoauto.h REMOTE pointer, pyhost.h ABI 1→2,
  REMOTE.md):
  bidirectional dev-PC link = the "remote logging + control" ask. NEW files
  `unoauto_remote.c/.h` — a cooperative non-blocking TCP client; pc64 DIALS
  OUT (stack is client-only) using a `STRESS.CFG` `remote=<ip>:<port>` key,
  registers an `unoauto_sink_add` sink so every LOG channel streams out, and
  runs the URC line protocol (`HELLO/LOG/MSG/CMD/RSP/BYE`, newline-delimited,
  symmetric). Verbs: probe/log/key/pointer/apps/launch/close/uptime/test/py/
  poweroff. **Consumes only the public net API — zero `net.c` edits.** Remote
  Python eval: **PyHost ABI 1→2** adds `run_src` (exec source string, capture
  stdout) in `pyhost.h`+`apps/pyrt.c`; `pc64_shell_py_exec` (pc64_uui.c) wraps
  it. On-device `unoauto.remote_active/send/recv/stop` (mod_unoauto.c, inert in
  prod). Frozen-core touches append-only: `unoauto_remote_tick()` in the
  pc64_uui.c frame loop, `unoauto_remote_boot()` in pc64_nettest.c
  `automate_start`. Host tool `tools/unoauto_remote.py` = `UnoAutoLink` client
  lib + CLI (listener; pc64 dials in). Doc `pc64/REMOTE.md`.
  **Gates green:** `tools/remote_proto_test.py` (protocol unit, pure Python),
  `tools/remote_qemu.py` (QEMU e2e: log stream + probe(19 rows) + `py`→42 +
  launch, via SLIRP `remote=10.0.2.2:PORT`). SPECTEST unchanged **65/0/4**;
  prod build carries no remote symbols. Design notes: user chose plaintext
  LAN + static IP (broadcast auto-discovery DEFERRED — pc64 can't send an L2
  broadcast yet: ARP routes 255.255.255.255 to the gw MAC; `ip_recv` DOES
  accept inbound broadcast; request filed in UNOAUTOMATE-REQUESTS.md for
  `net_udp_broadcast`/`net_udp_listen`). Single TCP connection = mutually
  exclusive with Browser/AI. `py` is one line only (newline breaks the frame).

- **A/B OS-update landed ON MASTER** (`7c5500d`, master + origin/unoautomate,
  2026-07-22; answers the "put + reboot verbs" request the driver agent filed at
  master `2772e04`). Push a rebuilt BOOTX64.EFI to a spare stick over the URC
  link + reboot into it → no physical reflashing per driver round. NEW URC verbs
  (unoauto_remote.c, UNO_DEBUG, existing dispatch): `put <vol> <path> <off-hex>
  <b64>` (RAM-stages a base64 chunk), `put <vol> <path> done <total-hex>` (writes
  in ONE uno_fs_write + verifies on-disk size — RAM-staged so a partial transfer
  never touches stick B; A stays fallback), `reboot` (uno_native_reset after TX
  drain), `bootnext <n>` (UEFI BootNext via runtime SetVariable — attached only,
  new debug-only `uno_pc64_set_bootnext` in uefi_main.c), `vols` (vol/kind/
  writable/name so host finds stick B). **Shared-OS change: `uno_fs_write` now
  writes firmware-SFS volumes** (new `uno_efifs_write` in uefi_main.c;
  `uno_fs_writable` allows live KIND_FW) — REQUIRED because an ATTACHED machine
  (Yoga builds -DUNO_NO_DETACH) sees its USB stick as firmware SFS (KIND_FW), was
  read-only. Additive, all builds. Host: `UnoAutoLink.push_file/reboot/bootnext/
  vols` + `--push <vol> <path> <local> [--reboot --bootnext N]` CLI. Gates green:
  remote_proto_test (vols + push byte-exact), remote_qemu (push→firmware-SFS
  vol→read-back via uno.read→bytes match + bootnext SetVariable ok), SPECTEST
  65/0/4. **Trap burned: base64 decoder had a signed left-shift overflow (acc
  int) → #UD under UBSan on the first long `put`; the QEMU gate caught it (the
  proto test used python b64 on a fake device, so it couldn't). Fixed: unsigned
  acc.** CAVEAT: full remote+SPECTEST gates were green PRE-rebase; the merged
  master tree BUILDS clean (debug+prod) but I pushed before re-running the QEMU
  e2e on the exact merged commit (user: "other agent is about to flash"). Merge
  was conflict-free (driver's +10 commits touch no files I changed).

- **`put` finalize-hang FIXED on master** (`4a9bc40`, 2026-07-22): pushing a
  ~1.5MB BOOTX64.EFI hung the Yoga at finalize. Root cause = **`fat.c fat_alloc`
  rescanned the FAT from cluster 2 on EVERY cluster → O(n²)** for a
  multi-hundred-cluster file, and each rescan re-read FAT sectors the data writes
  had just evicted from the 8-line sector cache → on firmware BlockIO a 1.5MB
  write = minutes = hard hang. Fix: `fatvol.next_free` scan hint (scan from hint,
  wrap 2..hint) → amortised O(1)/cluster; write loop feeds `uno_dbg_heartbeat()`
  (prod no-op macro; added `#include "uno_debug.h"` to fat.c); client finalize
  timeout 30→300s. **Throughput bonus:** device line buffer `g_rx` 2KB→4KB +
  default push chunk 750→2700B → a 1.5MB push ~33s→~9-12s (the streaming, 2000
  synchronous per-chunk round-trips at the 60Hz device tick, was the remaining
  cost — the finalize WRITE is 0.23s in QEMU). Verified: remote_qemu.py pushes
  1,518,995B to **native-FAT vol 1** (QEMU's native-AHCI-bound boot disk),
  CREATE + OVERWRITE (the exact repro), finalize returns + byte-exact read-back;
  SPECTEST 65/0/4; prod clean. Note: QEMU can't reproduce the metal *timing*
  (fast BlockIO) — the perf fix is algorithmic; the gate proves correctness (no
  corruption from the allocator change). Request marked FIXED in
  UNOAUTOMATE-REQUESTS.md.

- **unostorage: on-device disk partition/format LANDED on master** (merge
  `636ed94`, my commit `8ef2a52`, 2026-07-22). unoautomate can now partition +
  format a raw disk (disk B) over the link, toward installing UnoDOS off the UEFI
  stick. **Design rule honored: unoautomate wraps frameworks, implements no
  storage logic.** Storage stack surveyed: `blkdev` (raw sectors, `uno_bdev`
  read/write) + `fat`/`pc64_fs` (files) existed; NO partition/format framework
  did (installer only *clones*; formatting was host-only in `flash/UnoDisk.cs`).
  Built: **NEW `unostorage.h/.c`** (GPT+ESP authoring over blkdev, transport-
  agnostic `unostorage_dev`, shared `unostorage_crc32`, `gpt_init/gpt_add/
  prepare_esp`), **`uno_fat_mkfs`** in fat.c (FAT32 formatter, raw dev->write,
  ported byte-exact from UnoDisk.cs so fat.c's `mount_at` mounts it). Shared-OS:
  `uno_bdev.is_boot` (blkdev fw_scan sets it via device-path prefix vs new
  `uno_pc64_boot_dp()` in uefi_main.c); installer crc32 now delegates to
  unostorage. URC verbs (UNO_DEBUG): disks/readsec + arm/disarm/writesec/gptinit/
  mkpart/mkfs/prepdisk — **destructive verbs gate behind per-session `arm <disk>`
  that auto-disarms + refuses the boot disk** (arin's chosen safety). Host:
  UnoAutoLink.disks/arm/prepdisk/... + `--prepdisk` CLI. **MUST run ATTACHED**
  (blkdev only exposes firmware `fw*` disks while attached; native drivers would
  fight firmware — same constraint as the installer). Gate: remote_qemu.py second
  blank disk, 19/19 green incl. is_boot flagged, arm-rejections, prepdisk→fresh
  FAT32 mounts→file round-trips byte-exact. SPECTEST 65/0/4, prod clean. NEXT
  (deferred): author a boot entry for the fresh partition (installer HD() path)
  for a full bootable install-to-disk-B.
- **SHARED-WORKTREE HAZARD (2026-07-22):** the WiFi/net agent is ACTIVELY editing
  `unoauto_remote.c` in the SAME worktree (`unodos-unoautomate`) — adding netdisc
  auto-dial + a private g_sock (migrating off the shared net_tcp_* slot). They
  committed netsock/netdisc/docs/nst locally and have MORE uncommitted. I landed
  unostorage via an **isolated detached `git worktree`** (merge origin/master +
  push HEAD:master) so the shared worktree's uncommitted work was never touched.
  If landing again while they're active: DON'T rebase/checkout the shared tree;
  use an isolated worktree. Networking ownership was handed to unoautomate (see
  UNOAUTOMATE-REQUESTS.md handoff); their auto-dial waits on my disk work landing
  (now done).
- **install-to-internal-disk LANDED on master** (`3baa147`, 2026-07-22): completes
  the disk story — after `prepdisk`, the channel lays down the OS tree + authors a
  UEFI boot entry so the machine boots the fresh disk. **Works on internal
  SATA/NVMe disks** (they enumerate as writable `fw*` while attached; the QEMU gate
  disk IS an internal AHCI disk). New: `uno_pc64_add_boot_entry` (uefi_main.c,
  hand-built HD() device-path node + Boot####/BootOrder via runtime SetVariable,
  attached-only, debug-gated), `unostorage_find_esp`, `uno_bdev.dp` (fw whole-disk
  device path set in fw_scan), verbs `mkdir` + `makeboot`, host `install_dir` +
  `--install <disk> <esp_dir>` CLI. Gate proves mkdir+nested-push+makeboot; 65/0/4;
  prod clean. Landed via a FRESH isolated worktree on origin/master (shared
  worktree still has the other agent's uncommitted netdisc work — NEVER touch it).
  NEXT (deferred): boot entry is authored+default, but "actually boots disk B"
  needs a metal reboot to confirm firmware honors the hand-built HD() path.

- **`eth` URC verb LANDED + PUSHED (resume after crash, `9b8d32b` = origin/unoautomate,
  2026-07-22).** Answers Request 1 of the r8169 agent's
  2026-07-22 entry in UNOAUTOMATE-REQUESTS.md: a live wired-NIC (Realtek r8169)
  register/bring-up debug verb, the wired sibling of `iwl`. Additive pass-through in
  unoauto_remote.c to `r8169_dbg_cmd(line,out,cap)` (subcmds status/reg/wreg/phy/
  wphy/rerun/link/mac; UNO_DEBUG-only). **Driver hook not landed yet** — the r8169
  agent owns `r8169_dbg_cmd` in r8169.c/.h; a `__attribute__((weak))` fallback in
  unoauto_remote.c (returns -1 + "driver hook pending") keeps the tree linking green
  and auto-upgrades when their strong def arrives (weak-override verified in this
  mingw toolchain). Docs: REMOTE.md verb table gained eth + iwl rows (iwl was
  undocumented); HARNESS-POLICY changelog entry (additive, no API bump). Gates:
  debug+prod build clean (eth/fallback strings in debug binary, absent in prod);
  remote_proto_test all-pass; **remote_qemu.py e2e green** (>> remote channel OK) —
  eth itself is metal-only (no RTL8168 in QEMU). **Request 2 (NIC-independent UART/
  USB-CDC URC transport) left OPEN** — a real second-transport design task, not
  started; the requester deprioritized it vs Request 1. NEXT: push branch so the
  r8169 agent sees the contract + lands r8169_dbg_cmd; then optionally scope Request 2.

- **`disc` URC verb LANDED + PUSHED** (`ffa2dee` = origin/unoautomate, 2026-07-22,
  resume after the transport-stack handoff). Query-only verb so the dev PC can ask
  pc64 for its netdisc state without watching the wire: replies `active=<0/1>`
  (discovery armed), `have_host=<0/1>` (a host OFFER recorded), `host=<ip>:<port>`
  (the latched host, only when found), `link=<state>`. Pure read of the existing
  netdisc.h getters (netdisc_active/have_host/host_ip/host_port); UNO_DEBUG-gated,
  additive to the verb table, no API bump. Verified END TO END: extended
  tools/netdisc_qemu.py's raw-Ethernet L2-hub TCP peer with just enough seq/ack
  bookkeeping to send one `CMD d1 disc` over the auto-dialed URC link and parse the
  RSP frames — asserts active=1, have_host=1, host==our advertised OFFER
  (10.0.2.1:5099). All **9/9** gate checks green. REMOTE.md verb table documents it.
  Of the three handoff leftovers, this was item 2 (disc query); still open: full
  inbound multi-dev-PC server (accept primitive exists, fan-out unwired) and
  live-Yoga metal verification (board was mid-WiFi-iteration by the driver agent).

- **unoautomate branch FOLDED INTO master + PUSHED** (merge `454a027`, 2026-07-22):
  the branch's last 3 commits (own-socket refactor + `discover`/auto-dial +
  `eth` r8169-debug verb) landed on master via a union merge. The whole TCP/UDP
  stack (net.c/netsock/netdisc + DHCP-retrans/DNS fixes) was ALREADY on master;
  only `unoauto_remote.c` (the channel consuming the socket API) plus REMOTE.md /
  HARNESS-POLICY.md differed — union-resolved vs master's install-to-disk work
  (kept `do_makeboot`/`mkdir`/`makeboot` AND the socket refactor + verbs). Verified:
  `UNO_DEBUG=1` build clean (BOOTX64.EFI 1.5MB), `remote_qemu.py` e2e 21/21 PASS
  (makeboot + the merged channel both work). `eth` links via a weak `r8169_dbg_cmd`
  stub until the r8169 agent lands the real one. Branch `unoautomate` is now fully
  contained in master; safe to delete (commits preserved via the merge's 2nd
  parent; re-pin with `git branch unoautomate 9b8d32b`). origin/master @ 454a027.
NEXT: live LAN smoke (host
CLI on amanuensis + a flashed debug stick) — the
remote link arms on any debug boot that reaches
`automate_start` (no `automate` key needed). Then broadcast discovery once the
net owner adds the UDP helper. Older Phase D+ ideas still open: fb_dump→BMP
screenshots from scripts; richer drive (menu/widget targeting via uui ids);
unoauto.test.run binding. Phase E (dissolve legacy) waits for WiFi quiescence.
Builds cause LF/CRLF churn in `amiga/gen_data.i` + `pc64/upy/MICROPY_VERSION`
— checkout before rebasing.
Related: [[unodos-pc64-debug-stress]], [[unodos-wifi-usbeth-drivers]],
[[unodos-pc64-python]].
