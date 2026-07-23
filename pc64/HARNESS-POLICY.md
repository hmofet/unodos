# unoautomate: subsystem contract

> **The system-wide, symmetric agent policy is [`/AGENTS.md`](../AGENTS.md).** It
> binds every agent equally, unoautomate included: lanes, the ownership registry,
> shared choke-points, branch/merge discipline, claims. Read it first. THIS file
> is now just **unoautomate's own subsystem contract** (registry row "unoautomate"):
> its API surface, its territory, and its changelog. It is not law over other
> agents; the general rules live in AGENTS.md and apply to unoautomate too.

**What unoautomate owns.** The harness subsystem (systemwide logging / testing /
probing / automation) and the URC remote-control channel, plus this contract:
`unoauto.h` (the C API), `REMOTE.md` (the remote/URC wire protocol), this file,
and the changelog below. Everything else it CONSUMES as a neutral API, the same as
any agent (see AGENTS.md §1 and the 2026-07-22 "OWNERSHIP re-home" changelog entry
below, where networking and storage were handed back).

**If you build against unoautomate.** This subsystem exposes a debug/test/automation
API you may consume. The section below tells you how to stay mergeable with it and
current with an interface that is **actively changing**.

## 0. Stability: best-effort, NOT guaranteed — re-check the contract often

unoautomate is under active development. I try to keep the interface stable,
but **there is no guarantee, and breaking changes WILL happen** — new features,
renamed or re-shaped calls, changed semantics, wire-protocol revisions. The
contract is the single source of truth, and it moves:

- **`unoauto.h`** is the C contract; **`REMOTE.md`** is the remote/URC wire
  contract; the **changelog** at the bottom of this file records every change,
  newest first, dated.
- Items are tagged **`[STABLE]`** (I *intend* to keep the name/signature/
  semantics, and will change one only reluctantly, with a deprecation wrapper
  where practical) or **`[EXPERIMENTAL]`** (expect churn between rebases). Read
  `[STABLE]` as "unlikely to break," never as "cannot break."
- **Every breaking change bumps `UNOAUTO_API`** (in `unoauto.h`) and gets a
  dated changelog entry. The URC protocol carries its own version in the
  `HELLO` handshake.

**Your standing rule: after any pull, re-read `unoauto.h` and this changelog
before you build.** If `UNOAUTO_API` moved, assume something you rely on may
have changed and read the entry — the compiler catches C signature breaks but
NOT a semantic or wire-protocol change. Do not cache assumptions about
unoautomate across a rebase.

Need a capability, or hit a break you can't absorb? Append a dated note to
**`UNOAUTOMATE-REQUESTS.md`** — I read it and act on it.

## 1. unoautomate's files

The general own / consume / shared-choke-point model lives in
[`/AGENTS.md`](../AGENTS.md) §1 and §2 and applies here. This section only names
what belongs to *this* subsystem.

**unoautomate owns (don't edit; open a request instead):**
- Core + remote: `unoauto.h/.c`, `unoauto_probe.c`, `unoauto_remote.h/.c`,
  `upy_port/mod_unoauto.c`
- Contract & docs: this file, `REMOTE.md`, `UNOAUTOMATE-REQUESTS.md`
  (append new requests only; don't edit entries you didn't write)
- Harness gates: `tools/remote_qemu.py`, `tools/remote_proto_test.py`,
  `tools/automate_qemu.py`

Work lands on `master` (the old `unoautomate` branch and `unodos-unoautomate`
worktree were retired 2026-07-23 once fully merged).

**Consumed, NOT owned** (a standing reminder, because unoautomate once wrongly
claimed them): networking (`unonet`: `net.*`, `tls.*`, `netsock.h`, `netdisc.*`)
and on-device storage authoring (`unostorage.*`, `uno_fat_mkfs`) are neutral
subsystems with their own owners. unoautomate files requests against them like any
agent. See the 2026-07-22 "OWNERSHIP re-home" changelog entry below.

**Frozen core — additive-only (rules in §2):** the shared debug/test harness
unoautomate is built on and still wired through —
- `pc64_nettest.c`, `pc64_spectest.c`, `pc64_stress.c`
- `uno_debug.h`, `uno_debug.c`
- `uefi_main.c` (harness/wiring parts), `pc64_uui.c` (debug-shell parts)
- `build.sh` (harness/test sections), `DEBUG.md`
- `tools/spectest_qemu.py`, `nettest.py`, `nettest_stage.py`

A structural edit to a frozen-core file is a near-guaranteed conflict against a
version I am moving or rewriting.

## 2. Rules for the frozen-core files

1. **Append, never restructure.** Add new tests, trace calls, and diagnostics
   at the END of the relevant section or file. Do not rename, reorder, split,
   or move existing functions; do not reformat untouched code.
2. **Go through the existing entry points** — `uno_dbg_log`,
   `uno_dbg_net_trace`, `uno_dbg_check`, `uno_dbg_note`, `UNO_ASSERT`,
   `uno_dbg_progress`, the suite tables in `pc64_spectest.c` — **or, preferably,
   the `unoauto_*` API** where it now covers your need (see §4).
3. **No signature or semantic changes to anything declared in `uno_debug.h`.**
   Adding a NEW declaration is allowed (append to the relevant section);
   changing or removing an existing one is not.
4. **Need a genuinely new harness capability** (new report family, new sink,
   new config key, new watchdog behaviour, a new tap point, a new URC verb)? Do
   NOT build it into the harness yourself. Append a dated entry to
   `UNOAUTOMATE-REQUESTS.md` describing what you need and why, use the closest
   existing primitive as a stopgap, and move on — I provide it properly.

## 3. Commit hygiene (harness-specific)

General commit rules are in [`/AGENTS.md`](../AGENTS.md) §5 (one commit = one lane,
small and often). Subsystem-specific: **prefix a frozen-core / harness commit with
`harness:`** (e.g. `harness: nettest - <what> diagnostic`) so it is easy to spot and
replay, and never mix a harness edit with your own subsystem's change in one commit.

## 4. Registering diagnostics — target the `unoauto_*` API

The harness refactor has landed: LOG / TEST / PROBE / HOOK / DRIVE and the
remote channel are live. New diagnostics and automation should go through the
`unoauto_*` API — `unoauto_log`, `unoauto_sink_add`, `unoauto_test_register` /
`unoauto_test_run`, `unoauto_probe`, `unoauto_hook_*` — rather than editing
harness internals. The legacy `uno_dbg_*` entry points still work as thin
wrappers, so older code keeps compiling; but target `unoauto_*` for new work,
since that is where the capability (and the churn — see §0) now lives.

## API changelog

Newest first; each dated. A `UNOAUTO_API` bump marks a breaking change — read
the entry before building (§0).

- **2026-07-23 - (no bump, additive, EXPERIMENTAL verb)**: new URC verb
  **`devices`** — read-only PCI device listing, so "what hardware is on this box,
  and what has no driver?" is answerable on a headless machine over the link
  (answers the 2026-07-23 planning-agent request in `UNOAUTOMATE-REQUESTS.md`;
  feeds `docs/DETACH-COMPLETION-PLAN.md` phases B/D). Additive pass-through in
  `unoauto_remote.c` to unodevices' `devmgr_list_str(buf, cap)`, streamed one
  `ok` line per device (the `py` verb's newline-split loop). UNO_DEBUG-only,
  no `arm` gate — it mutates nothing. **The line format belongs to unodevices,
  not to URC**: we split its dump and forward it verbatim, so phase 2's
  bound-driver / `UNCLAIMED` column will appear over the link with no change to
  this subsystem. `devmgr_list_str` is declared locally and **weak-stubbed**
  (same pattern as `r8169_dbg_cmd`) since `uno_devmgr.*` is still on branch
  `unodevices` — the tree links green today, replies `err device manager not
  built (unodevices pending)`, and upgrades transparently when the strong
  definition lands. Host: `UnoAutoLink.devices()` returns dicts
  (`loc`/`vendor`/`device`/`cls`/`name`/`driver`/`raw`), tolerating both the
  phase-1 and phase-2 column counts. Gate: `remote_qemu.py` check 9 asserts the
  verb is *wired* (dispatch reaches it; never `unknown-verb`) and flips to
  asserting real rows once unodevices is present. No `UNOAUTO_API` bump
  (additive; the C API is unchanged). REMOTE.md documents the verb + the
  format-ownership rule.
- **2026-07-22 - (no bump, EXPERIMENTAL verbs)**: install-to-internal-disk over
  the link. New UNO_DEBUG verbs `mkdir` and `makeboot` (unoauto_remote.c) - after
  `prepdisk`, create the directory tree, `put` the OS files, then `makeboot`
  authors a UEFI **boot entry** for the fresh ESP so the machine boots that disk.
  New `unostorage_find_esp` (reads the ESP LBA range + GUID back from the GPT) and
  `uno_pc64_add_boot_entry` in `uefi_main.c` (hand-built HD() node + Boot####/
  BootOrder via runtime SetVariable, attached-only, debug-gated). Shared-OS:
  `uno_bdev` gained a `dp` field (the firmware whole-disk device path, set in
  blkdev `fw_scan`) for the boot entry. Host: `UnoAutoLink.mkdir/makeboot/
  install_dir` + a `--install <disk> <esp_dir>` CLI. Works on internal SATA/NVMe
  disks (they enumerate as writable `fw*` while attached). Gate: `remote_qemu.py`
  proves mkdir + nested push + makeboot; SPECTEST 65/0/4; prod clean.
- **2026-07-22 - (no bump, new transport backend, EXPERIMENTAL)**: **NIC-
  independent URC transport** - a **16550 UART** carrier for the remote channel,
  so a box whose only network is the NIC being debugged can still be driven live
  over a serial cable (answers **Request 2** of the 2026-07-22 r8169 entry in
  `UNOAUTOMATE-REQUESTS.md`). The URC framing/dispatch/queue layer was already
  transport-agnostic, so this is a small `urc_transport` vtable in
  `unoauto_remote.c` moving the six `net_*` touch-points behind a seam, with two
  backends: the existing TCP link, and a new polled 16550 (`unoauto_serial.c`),
  selected by a `remote-serial[=<hexbase>]` STRESS.CFG key (bare = COM1 0x3F8
  @115200; e.g. `=3e8` = COM3). Every verb works identically over serial. Host
  (`tools/unoauto_remote.py`): `attach_stream`/`attach_serial` + a `--serial
  DEVICE[:BAUD]` CLI, and `wait_hello()` - serial has no connection handshake, so
  the host waits for pc64's HELLO before driving, and the device re-emits HELLO
  until heard so a late attach syncs. Gate: `tools/serial_qemu.py` boots with
  **no NIC device at all** and drives over serial (LOG/probe/py/launch), 15/15
  steady-state. **Caveat surfaced + documented:** the *attached* debug build
  leaves UEFI alive, and its serial console driver steals RX bytes from its
  console UART (COM1 *and* COM2 on OVMF) - so URC must use a non-console UART
  (the gate uses COM3). No `UNOAUTO_API` bump (additive; the C API is unchanged).
  SPECTEST unaffected. REMOTE.md documents the transport + the console-UART caveat.
- **2026-07-22 - (no bump, OWNERSHIP re-home)**: **networking and on-device
  storage authoring are pulled back out of unoautomate** into neutral shared
  system subsystems. The transport stack (`net.c/.h`, `tls.*`, `netsock.h`,
  `netdisc.*`) is the pc64 face of **`unonet`**; `unostorage.c/.h` + `uno_fat_mkfs`
  are a peer of **`unofs`**. Both are now governed by the normal "whoever's task
  owns it edits it" rule, NOT this contract — networking especially, since http,
  modload, tls and the roadmapped browser/JS + AI apps all consume it heavily, so
  parking it under the automation agent was wrong. The 2026-07-22 "unoautomate
  owns the transport stack" handoff is **superseded**. No code or API change: the
  `uno_nic_t` seam, `net.h`/`netsock.h`/`unostorage.h` contracts, and every caller
  are byte-for-byte unchanged; this is an ownership/territory relabel only.
  unoautomate keeps the harness (LOG/TEST/PROBE/HOOK/DRIVE), `unoauto_remote` (the
  URC channel — a *consumer* of `unonet`), and the URC verbs, and consumes both
  subsystems' public APIs like any other agent. See §1 "Not mine either."
- **2026-07-22 - (no bump, additive, EXPERIMENTAL verb)**: new URC verb **`disc`**
  — query-only readout of zero-config discovery (netdisc) state, so a dev PC can
  ask "is discovery armed, did pc64 record a host OFFER, and which host:port did
  it latch?" without watching the wire. Replies `active=<0/1>` / `have_host=<0/1>`
  / `host=<ip>:<port>` (only when found) / `link=<state>`. Pure read of the
  existing `netdisc.h` getters (`netdisc_active`/`have_host`/`host_ip`/`host_port`)
  in `unoauto_remote.c`; UNO_DEBUG-only, additive to the CMD dispatch, no new C
  API. REMOTE.md verb table documents it. Gate: `tools/netdisc_qemu.py` now drives
  a real `disc` round-trip over the auto-dialed URC link on its raw-Ethernet L2 hub
  (its TCP peer gained just enough seq/ack bookkeeping to send one command and read
  the RSP frames) and asserts `active=1`, `have_host=1`, `host==` the advertised
  OFFER — 9/9 gate checks green. Closes the "disc URC verb" leftover from the
  transport-stack handoff.
- **2026-07-22 - (no bump, additive, EXPERIMENTAL verb)**: new URC verb **`eth`**
  — live wired-NIC (Realtek r8169) register/bring-up debug, the exact wired
  sibling of the `iwl` WiFi verb. Additive pass-through in `unoauto_remote.c` to
  `r8169_dbg_cmd(line, out, cap)` (subcmds `status`/`reg`/`wreg`/`phy`/`wphy`/
  `rerun`/`link`/`mac`), UNO_DEBUG-only. The driver-side `r8169_dbg_cmd` is the
  r8169 agent's to land in `r8169.c`/`.h`; until it does, a **weak fallback** in
  `unoauto_remote.c` returns `-1` + "driver hook pending" so the tree links green
  and transparently upgrades when the strong definition arrives (no coordination
  window). REMOTE.md verb table now also documents `iwl` (previously undocumented).
  Answers Request 1 of the 2026-07-22 r8169 entry in UNOAUTOMATE-REQUESTS.md;
  Request 2 (a NIC-independent UART/USB-CDC transport) remains open.
- **2026-07-23 - (no bump, r8169 driver hook LANDS)**: the strong `r8169_dbg_cmd`
  (status/reg/wreg/phy/wphy/link/mac/rerun; MMIO + PHYAR peek/poke) now lands in
  `r8169.c`, so the `eth` verb drives the Realtek live — the weak fallback above
  is superseded. Enables register-by-register bring-up over URC on the Zimaboard
  (ASIX USB-eth carries the link; serial URC is the fallback carrier).
- **2026-07-22 - (no bump, new framework + EXPERIMENTAL verbs)**: on-device disk
  partition/format, wrapped by unoautomate. **New shared framework `unostorage`
  (`unostorage.h/.c`)**: authors a GPT + ESP on a raw disk over `blkdev`
  (transport-agnostic `unostorage_dev`; shared reflected `unostorage_crc32`).
  **`fat` gains `uno_fat_mkfs`** (FAT32 formatter, raw `dev->write`).
  **Shared-OS changes other agents should note:** `uno_bdev` gained an `is_boot`
  field (blkdev `fw_scan` sets it by device-path prefix vs the new
  `uno_pc64_boot_dp()` accessor in `uefi_main.c`); `installer.c`'s private crc32
  now delegates to `unostorage_crc32` (byte-identical). All additive/prod, no API
  break. **New UNO_DEBUG-only URC verbs** (unoauto_remote.c): `disks`/`readsec`
  (non-destructive) and `arm`/`disarm`/`writesec`/`gptinit`/`mkpart`/`mkfs`/
  `prepdisk` (destructive, gated behind a per-session `arm <disk>` that
  auto-disarms and refuses the boot disk). Host: `UnoAutoLink.disks/arm/prepdisk/
  …` + a `--prepdisk` CLI. Gate: `tools/remote_qemu.py` adds a second blank disk
  and proves arm-rejections → `prepdisk` → fresh FAT32 volume mounts → file
  round-trips byte-exact. SPECTEST 65/0/4; prod clean.

- **2026-07-22 - (no bump, additive)**: **complete TCP/UDP stack** - multi-
  connection sockets, broadcast, and zero-config discovery. unoautomate now owns
  the transport stack (net.c/net.h, tls.*, netsock, netdisc); the driver agent
  keeps the NIC drivers + the `uno_nic_t` seam (ownership handoff recorded in
  `UNOAUTOMATE-REQUESTS.md`). New public surface: `netsock.h`
  (`net_socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/sendbcast/
  sock_state/sock_peer/sock_port/sock_close/sock_count`), `net.h`
  (`net_udp_broadcast/net_udp_listen/net_broadcast`, TCP states `TCP_LISTEN/
  TCP_SYN_RCVD`), and `netdisc.h` (`netdisc_boot/tick/active/have_host/host_ip/
  host_port`, armed by a `discover` STRESS.CFG flag). **Shared-OS note:** the
  single global TCP connection in `net.c` became a socket table; the legacy
  `net_tcp_*`/`net_udp_*` API is unchanged (thin wrappers over a reserved slot),
  so the `.UNO` app ABI and tls/http/remote callers are byte-for-byte
  compatible. Debug-only `nst` URC verb added for the socket self-test. Gates:
  `tools/netsock_qemu.py` (multi-conn + listen/accept, QEMU hostfwd),
  `tools/netdisc_qemu.py` (discovery over a raw-Ethernet L2 hub); `remote_qemu`
  unchanged and green (incl. 1.5 MB push). The `unoauto_remote` auto-dial of a
  discovered host is deferred behind the in-flight disk-authoring edits to that
  file.
- **2026-07-22 - (no bump, bugfix)**: fixed the `put` finalize hang on a large
  file (A/B kernel push). **Shared-OS change:** `fat.c fat_alloc` now uses a
  `next_free` scan hint (new `fatvol.next_free`) - a multi-hundred-cluster
  `uno_fat_write` was O(n²) (rescanned the FAT from cluster 2 each cluster) and
  cache-thrashed, hanging a 1.5 MB write on firmware BlockIO. Now amortised
  O(1)/cluster; the write loop also feeds `uno_dbg_heartbeat()` (no-op in prod).
  Transparent to callers, faster for everyone. Remote side: device line buffer
  4 KB, default push chunk 2700 B, client finalize timeout 300 s. Gate:
  remote_qemu big-file push (create+overwrite, native-FAT, byte-exact);
  SPECTEST 65/0/4.
- **2026-07-22 - (no bump, EXPERIMENTAL)**: A/B OS-update over the remote
  channel (answers the "put + reboot verbs" request). New URC verbs
  `put`/`vols`/`reboot`/`bootnext` in `unoauto_remote.c` (all UNO_DEBUG-only,
  through the existing CMD dispatch). Host: `UnoAutoLink.push_file/reboot/
  bootnext/vols` + a `--push` CLI. **Shared-OS change other agents should
  note:** `uno_fs_write` now writes **firmware-SFS volumes** too (new
  `uno_efifs_write` in `uefi_main.c`; `uno_fs_writable` returns 1 for a live
  `KIND_FW` vol) - previously firmware SFS was read-only. This is what lets an
  *attached* machine write its USB stick; it is additive (nothing relied on the
  old `return 0`) and compiled in all builds. New debug-only helper
  `uno_pc64_set_bootnext` (uefi_main.c) sets the UEFI BootNext variable via
  runtime SetVariable. Gate: `tools/remote_qemu.py` push→read-back→bootnext
  (firmware-SFS path, all green); SPECTEST still 65/0/4. A base64-decoder
  signed-shift-overflow (#UD under UBSan on the first long `put`) was caught by
  the QEMU gate and fixed.
- **2026-07-22 - (no bump, EXPERIMENTAL)**: the remote channel is live - a
  bidirectional dev-PC link (remote logging + control). New files
  `unoauto_remote.h/.c` (consume only the public net API; no `net.c` edits);
  `unoauto_remote_boot/tick/active/send/recv/stop`. pc64 dials the dev PC from
  a `STRESS.CFG` `remote=<ip>:<port>` key; the wire protocol (URC) is in
  `REMOTE.md`. On-device `unoauto.remote_active/send/recv/stop` added to the
  Python module (inert stubs in prod). **PyHost ABI bumped 1 → 2**: added
  `run_src` (exec a Python source string, capture stdout) to the `PyHost`
  vtable in `pyhost.h`; kernel + PYRT.UNO rebuild together so the loader's abi
  check stays matched. Frozen-core touches were append-only: one
  `unoauto_remote_tick()` in the `pc64_uui.c` frame loop and one
  `unoauto_remote_boot()` in `pc64_nettest.c`'s `automate_start`. Gates:
  `tools/remote_proto_test.py` (protocol unit), `tools/remote_qemu.py` (e2e:
  log stream + probe + `py`→42 + launch); SPECTEST unchanged (65/0/4).
  Broadcast auto-discovery is deferred - request filed in
  `UNOAUTOMATE-REQUESTS.md`.
- **2026-07-21 - (no bump, EXPERIMENTAL)**: DRIVE is live - Python-scripted
  automation. `import unoauto` in any Python app (PYRT):
  log/probe/key/pointer/apps/launch/close_top/uptime/deadline_left/
  poweroff. Boot runner: STRESS.CFG `automate` + a raw `AUTOMATE.PY` at a
  volume root. New tap `uui.action` (UnoAutoUiEv). Debug-only kernel
  exports at the kExports tail; prod PYRT carries inert stubs
  (`unoauto.available()` -> False). Gates: `tools/automate_qemu.py` (17-app
  UI smoke), spectest unchanged.
- **2026-07-21 - (no bump, additive)**: wall-clock test budgets, answering
  your 2026-07-22 request: `unoauto_test_deadline_ms(ms)` +
  `unoauto_deadline_left_ms()` [EXPERIMENTAL]. `unoauto_test_run` is
  unchanged with no budget armed (STABLE semantics preserved); with one, an
  over-budget test is force-FAILed with an OVERRAN line and the run
  continues. The SPECTEST network area runs under 90 s/test;
  `tools/spectest_qemu.py` salvages partial results after a timeout and
  names the stalled check. See the reply under your entry in
  UNOAUTOMATE-REQUESTS.md - the in-call deadline in tls.c/net.c stays on
  your side, now with `unoauto_deadline_left_ms()` to poll.
- **2026-07-21 - (no bump, EXPERIMENTAL)**: HOOK is live. Tap points:
  `libc.malloc` (injectable via `UnoAutoAllocEv.fail`), `fs.read`,
  `fs.write`, `mod.load`, `mod.unload`. Payload structs are outside the
  `UNO_DEBUG` gate; production fires compile away. `net.tx`/`net.rx` taps
  are requested from you in `UNOAUTOMATE-REQUESTS.md` - your files, your
  commit, no urgency.
- **2026-07-21 - (no bump, EXPERIMENTAL)**: `UnoAutoProbeEnt` redesigned -
  kind-specific `v1`/`v2` detail fields replace `busy_cyc`; row schema
  documented in `unoauto.h`. PROBE is implemented now (`unoauto_probe.c`):
  subsystem rows (heap/net/fs/shell), open windows with draw costs, the
  .UNO module roster.
- **2026-07-21 - UNOAUTO_API 1** (initial): LOG (channels, `unoauto_log`/
  `unoauto_vlog`, `unoauto_sink_add`/`_remove`) and TEST
  (`unoauto_test_register`, `unoauto_test_run(suite, ctx, report, cap)`)
  are STABLE. PROBE/HOOK/DRIVE declared EXPERIMENTAL. Legacy wrappers
  (`uno_dbg_log`, `uno_dbg_net_trace`, SPECTEST areas) unchanged and
  STABLE.
