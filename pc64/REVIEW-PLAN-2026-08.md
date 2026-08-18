# pc64 review + opus-worker plan (2026-08-07)

Performance / bug / security review of the pc64 OS, in that priority order. Produced
by a fan-out of subsystem review agents; every finding below was reported with
`file:line` evidence and the load-bearing ones were re-verified against source.

**How to use this doc.** Each **workstream (WS-*)** is a candidate opus-worker branch,
scoped to one ownership lane per [`AGENTS.md`](../AGENTS.md) §1 so two workers never
touch the same subsystem. Branch off `origin/master`, land small, delete the branch the
day it lands (§3). Items inside a workstream are ordered by priority. Effort is S/M/L.

Security items are tagged **[MUST-FIX]** (a remotely- or locally-triggerable
memory-safety / auth defeat) or **[DEFER]** (hardening you may reject until closer to
production, per your standing preference). The four MUST-FIX security items are all
small, and three of them are memory-corruption bugs that also belong under "bugs".

> Coverage note: `hv_phases.c` was being actively edited during the review and was not
> deep-read; `tools/*.py` harness was excluded (another agent owns it). BearSSL and
> QuickJS library internals are vendored and out of scope (only our glue was reviewed).

---

## The two biggest levers (read first)

1. **Idle CPU: the shell busy-spins a full core forever.** After TSC calibration
   *every* `uno_pc64_delay_ms` is a `rdtsc`/`pause` spin (`pc64_native.c:44-49`), and the
   idle frame's `uno_pc64_delay_ms(16)` (`pc64_uui.c:6757`) plus present's 1 ms tail both
   route through it. An idle desktop pins one core at 100% — fan noise, battery drain,
   thermal pressure on every render. Fixing this (WS-P1) is the single highest-value
   change in the review.
2. **Network throughput: the TCP engine caps the whole stack.** `net_send` allows one
   unacked 512-byte segment (`net.c:448-461`) and advertises an 8 KB window
   (`net.c:359,398`), so the browser, TLS handshakes, and URC uploads are all bounded to
   ~512 B out / 8 KB in per RTT regardless of link speed. WS-P2 is the single highest-value
   network change.

These two dominate; everything else is incremental on top.

---

## PERFORMANCE (priority 1)

### WS-P1 — Platform / power  ·  lane: pc64 platform (`uefi_main.c`, `pc64_native.c`)
- **[S→M/L] Kill the idle busy-spin.** `pc64_native.c:44-49`. Arm a periodic timer IRQ
  (RTC periodic via 0x70/0x71, or HPET) and make the frame-loop idle wait `sti; hlt`-based;
  keep the `pause`-spin only for sub-millisecond device delays. This is the one place the
  otherwise fully-polled design needs a single IRQ path. **Biggest item in the review.**
- **[S] Trim boot delays.** ~2.4 s of unconditional sleep on the boot path: `splash_step`
  400×3+700 ms (`uefi_main.c:448-454`, calls at 1141/1150/1162/1293) plus the startup chime
  (~0.5 s). Drop per-step delays to ~100 ms; overlap the chime with `build_desktop`/
  `session_load` by pumping `uno_snd_poll` from the frame loop instead of sleeping.
- **[S] Drop present's 1 ms tail** (`uefi_main.c:2124` and the drag/cursor paths). The main
  loop already paces idle at 16 ms; the tail only adds latency to drag/cursor frames.

### WS-P2 — TCP / IP engine  ·  lane: unonet (`net.c`, `netsock.h`)
- **[L] Pipeline TX.** Replace the single `pend[512]` with a small send ring of unacked
  segments up to the peer's advertised window; raise the per-segment cap toward the peer
  MSS (advertise an MSS option on SYN). `net.c:355,448-461,386-405`.
- **[S→M] Grow the RX window.** `rxq[8192]` → 32–64 KB per socket, or advertise window
  scaling. `net.c:359,398`.
- **[S] Wall-clock RTO + fast retransmit.** Make the retransmit timer TSC-based instead of
  tick-count (`retx_at = g_ticks + 30`); send an immediate duplicate-ACK for out-of-window
  data so peers fast-retransmit rather than waiting a full RTO. `net.c:667-681`.
- **[S] Fix serial-number comparisons** (also a bug, WS-B4): use `(int32_t)(a-b) > 0` for
  ACK/seq compares. `net.c:662,685`.

### WS-P3 — HTTP client + browser layout  ·  lane: browser (`pc64_http.c`, `pc64_browser.c`, `webjs.c`) + unoweb
- **[S] Chunked "done" scan cursor.** `frame_update` rescans the whole body every recv
  (O(n²)); keep a persistent cursor and scan only new bytes. `pc64_http.c:851-853`.
- **[S] Drop the receive bounce copy.** Read directly into `r->raw + r->rn` with
  `raw_room()` as the cap instead of via `tmp[1460]` + memcpy. `pc64_http.c:858-880`.
- **[S] Kill the per-paint document re-hash.** `doc_sig()` FNV-hashes the entire document
  every `br_draw` to detect a change that only happens on load; replace with a
  dirty/generation flag bumped on write. `pc64_browser.c:338-343,819,948`.
- **[M] Cache flow layout.** The default flow renderer re-walks the DOM and re-measures
  every word (incl. off-screen) every paint (`pc64_browser.c:944-952,151-190`); `render_md`
  re-parses markdown each frame. Cache the laid-out run list keyed on (generation, width,
  scroll) like the `uw` engine already does (`:833`); hoist the constant space-width out of
  the per-word loop. Independently corroborated by two agents.
- **[S] Cache the inline-image column map.** Two 64-bit divides per pixel per paint
  (`pc64_browser.c:899-907`); precompute a column map once per (src→dst width) like present's
  `gColMap`, or cache scaled pixels in the existing `imgent` decode cache.
- **[M] De-quadratic `querySelectorAll`/`children`.** `sel_nth` does a full document
  traversal per index (`webjs.c:69-91`); collect matches once into a bounded array and index it.

### WS-P4 — Compositor / shell UI  ·  lane: pc64 shell (`pc64_uui.c`, `pc64_font.c`, `uefi_main.c` present) + **shared** unoui (`theme_aurora.c`, `unoui.c` — file a claim, additive only)
- **[S] Music/UnoAmp dirty-on-change.** `pc64_music.c:385` marks the whole scene dirty every
  frame while playing (incl. paused); `unoamp_ui.c:678-682` at 7.5 Hz always. This turns the
  clean "idle frames touch no VRAM" design into a permanent 60 Hz full repaint for the most
  common long-running case. Dirty only when a visible value (meter segment count, seconds,
  marquee-needed) actually changes.
- **[S] Shadow: blend the ring, not the fill.** Aurora blends ~6 full window areas of soft
  shadow per window per dirty frame, then overdraws almost all of it (`theme_aurora.c:39-45`
  → `fb_blend_rect`). Blend only each layer's exposed ~2 px ring, or pre-render the shadow
  once per (w,h,rad) into a cached edge/corner strip. ~60× less alpha work. *(unoui lane —
  claim it.)*
- **[M] Dirty-rect hints into present.** Pass 1 diffs the whole framebuffer even for
  cursor-only frames (`uefi_main.c:2022-2036`); the shell already knows the cursor union rect
  (cursor-only), the dragged-window union (drags), else full. Clamp the `sy` loop and per-row
  scan to the hint.
- **[M] Live-drag partial restore.** `uno_pc64_scene_restore` memcpys the whole ~8.3 MB fb per
  moved frame (`uefi_main.c:186-187`, `pc64_uui.c:6674`) when only the window's old+new union
  changed; restore just that rect and pass it as the present hint.
- **[S] Guard the half-second housekeeping repaint.** `g_dirty = 1` is unconditional at
  `pc64_uui.c:6564-6576`; set it only when a caret is actually visible or a tray string
  (`fmt_clock`/`fmt_batt`/`fmt_net`) changed (strcmp).
- **[S-M] Glyph compositing.** `paint_cov` plots each covered pixel via `fb_blend_pixel_sub`,
  which recomputes clip per pixel with no LTO (`pc64_font.c:253,264` → `fb.c:233-246`). Add a
  row-span blend (clip once per row) or enable `-flto` for the fb+font TUs.
- **[S] Cache kerning + taskbar metrics.** Kern pairs go through stb_truetype uncached per
  pair per draw *and* measure (`pc64_font.c:215-220`) — add a 95×95 kern table per bank.
  Taskbar helpers re-measure constant strings every draw (`pc64_uui.c:2576-2586`) — cache
  behind a font-generation counter.

### WS-P5 — Storage I/O  ·  lane: unofs (`fat.c`) + usb (`usbmsc.c`, `xhci.c`) + sdhci
- **[M] FAT bulk write arm.** The write path is single-sector RMW through the 8-line cache —
  every written sector costs a read + a write, the exact analog of the already-fixed read
  bulk arm (`fat.c:874-943`, esp. 916-928). Write whole contiguous cluster runs straight
  through `dev->write`, invalidating overlapping cache lines; drop the `tmp[512]` staging.
- **[M-L] SDHCI multi-block.** `drv_read`/`drv_write` loop one 512-byte CMD17/CMD24 per sector
  (`sdhci.c:264-297`), shattering the 64-sector runs the bulk arm hands it — the biggest
  storage gap, on the Surface Go eMMC boot path. Implement CMD18/CMD25 + SDMA/ADMA2.
- **[S] Cache the file locator in the seq cursor.** `uno_fat_read_at` re-runs
  `file_locate` (dir walk) on every chunk (`fat.c:756-764`); cache start-cluster+size keyed on
  (vol,path). Corroborated by two agents.
- **[S] usbmsc poll granularity.** `poll_event_ms` sleeps 1 ms after one ring sweep
  (`xhci.c:273-285`), quantising every BOT phase; spin-sweep ~100 µs (TSC-bounded) before the
  first 1 ms sleep.

### WS-P6 — NIC driver hot paths  ·  lane: per-driver
- **[M] rtl8152: cache link state.** `rtl_recv` does a synchronous USB control round-trip
  (`poll_link`) at the top of every call, inside a `while(recv()>0)` drain — ~ms per frame and
  per idle poll. Cache link, refresh every N ms (ax88179 already does). `rtl8152.c:437,406-412`.
- **[S] e1000/e1000e/igb: use memcpy.** Byte-at-a-time TX/RX copy loops that `-O2
  -ffreestanding` won't re-widen (`e1000.c:180,200` etc.); include string.h, memcpy.
- **[S] Raise Intel NDESC 16 → 64** (`.bss` cost only); r8169's RX-overflow recovery kick is
  evidence the 16-deep ring drops bursts today. Corroborated by two agents.
- **[S] rtwifi/mrvlwifi: batch the RX index MMIO write** to once per drain, not per frame
  (`rtwifi.c:479`, `mrvlwifi.c:414`).

### WS-P7 — Async network bring-up + DNS  ·  lane: unonet + http
- **[M] Make `pc64_net_boot` a per-frame state machine.** It runs synchronously in the frame
  loop (`pc64_uui.c:6489-6499`, `pc64_http.c:163-196`), freezing the desktop up to ~8 s on a
  cable-less box (~12 s on first net use). Pump one `net_poll` slice per frame like
  `pc64_http_poll` already does.
- **[S→M] DNS: shrink the poll quantum + cache TTL.** Synchronous, 8 ms quanta, up to ~5 s per
  miss, no TTL cache (`net.c:986-1003`). Shrink the quantum, raise `DNS_N`; longer term fold
  into the request state machine.

---

## BUGS (priority 2)

### WS-B1 — FAT / filesystem correctness  ·  lane: unofs (`fat.c`, `unodos.c`)
- **[M] 64→32-bit LBA truncation corrupts >2 TB disks.** `fat_start`/`data_start`/`root_start`
  are `uint32_t` and `mount_at` casts a 64-bit GPT `start` to `uint32_t` (`fat.c:37-40,
  156-160`, verified). A FAT partition beginning past sector 2³² mounts fine (validation uses
  the full 64-bit `start`) then reads/writes truncated LBAs — silently scribbling a partition
  ~2 TiB lower (crash telemetry / prefs writes hit any mounted volume). **Double-confirmed.**
  Fix: make the LBA fields and `clus_lba()` 64-bit, or refuse the mount when the geometry
  exceeds 2³².
- **[S/M] FAT copy #1 is never written.** `fat_set` updates FAT #0 only (`fat.c:438-453`) though
  mkfs writes two mirrored FATs; every create/delete/grow leaves the mirror stale, so another
  OS's chkdsk "repair to the second copy" frees every UnoDOS-allocated cluster. Also write the
  sector at `+fat_sectors` per extra copy.
- **[S] Write-failure rollback.** `uno_fat_write` leaks clusters and leaves a phantom 0-byte
  entry on the ENOSPC / cache-fail paths (`fat.c:896-927`); mark the new slot `0xE5` and
  `fat_free_chain(first)` on every post-slot-creation failure. Same undo gap in `uno_fat_mkdir`
  (`fat.c:1018-1030`).
- **[S] FAT12 core frees the old chain before committing the new one** (`unodos.c:741-756`) —
  the S-FAT-28 bug already fixed in `fat.c`, still present in the portable core (reaches real
  media on the Mac/floppy targets).
- **[S] Guard cluster < 2.** `clus_lba` has no `clus >= 2` check (`fat.c:457-458`); a `..` entry
  storing cluster 0 lands the cursor inside the FAT/root region and a write plants dir entries
  into the FAT. Reject `.`/`..` in `resolve_parent` and treat `clus < 2` as invalid.
- **[S] Clamp the GPT entry count** (also security S-def): `scan_disk` loops `num = rd32(gh+80)`
  straight off the medium (`fat.c:204-219`); a crafted `num=0xFFFFFFFF` = billions of sector
  reads = boot hang. Mirror `installer.c:245-246` (cap `num`, validate `esz`). Double-confirmed.

### WS-B2 — Boot / install / EFI variables  ·  lane: installer (`installer.c`) + pc64 platform (`uefi_main.c`)
- **[S] `EFI_BUFFER_TOO_SMALL` treated as a free Boot#### slot.** A foreign boot entry >512 B
  returns BUFFER_TOO_SMALL, not NOT_FOUND, and gets overwritten on install
  (`installer.c:727-743`, `uefi_main.c:2391-2401`); the BootOrder splice likewise treats any
  read failure as "empty" and orphans other entries. Only NOT_FOUND is free; abort the
  BootOrder rewrite if it can't be read.
- **[M] `uno_efifs_write` deletes the old file before the replacement exists**
  (`uefi_main.c:2607-2630`) — a failed create or power loss mid-write bricks the boot stick
  (the A/B OS-update path rides this). Write to a temp name and rename over the original.
- **[S] Installer assumes GPT entry 0 is the boot partition** (`installer.c:936-945`); locate
  the first used / ESP-typed entry instead.

### WS-B3 — libc formatting  ·  lane: pc64 core (`pc64_libc.c`)
- **[S] `vsnprintf` ignores `%s` precision and doesn't consume `%.*`** (`pc64_libc.c:364,
  377-381`). QuickJS links this libc: `Date.toString` emits the whole packed month/day table
  and `js_parse_error("%.*s")` prints garbage; a future `%.Ns` on a non-terminated buffer reads
  OOB. Implement `%s` precision and consume `*` width/precision args.

### WS-B4 — HTTP / TCP framing bugs  ·  lane: browser (`pc64_http.c`) + unonet (`net.c`)
- **[M] Chunked "done" false-positive poisons keep-alive.** The `\n0\r`/`\n0\n` substring scan
  matches ordinary body data (`pc64_http.c:851-854`); a mid-stream connection can be marked done
  and returned to the pool, so the next request reads leftover bytes. Track real chunk framing.
  Corroborated by two agents.
- **[S] Relative `Location` drops the current directory** (`pc64_http.c:917-936`); resolve
  against the directory of `r->path`.
- **[S] TCP seq/ACK wraparound** — folded into WS-P2's serial-number fix.

### WS-B5 — NIC / driver correctness  ·  lane: per-driver
- **[S] r8169 signed ring cursors overflow to a negative index** after 2³¹ frames
  (`r8169.c:101,214,229,236,270`) — make them unsigned / mask.
- **[S] Missing compiler barrier between descriptor fill and doorbell** in rtwifi/mrvlwifi and
  (between the volatile DD check and the non-volatile copy) e1000/e1000e — add
  `asm volatile("":::"memory")`.
- **[M, metal-pending] rtwifi station MAC never read from the chip** (`rtwifi.c:55,491,552`) so
  the 4-way handshake runs with an all-zero SA; **no TX completion/backpressure** in
  rtwifi/mrvlwifi (write ptr can overrun in-flight DMA). **[S] e1000e/igb read MAC after the
  reset that clears RAR0.** **[S] iwlwifi non-AX210 DMA status reads aren't volatile.** These
  matter as those radios move toward hardware validation.

### WS-B6 — Editor / apps / present buffers  ·  lane: apps + pc64 platform
- **[S] UWD loader trusts an unvalidated text-length header** (`pc64_write.c:637-644`) — a short
  file with a large `tl` shows ~32 KB of stale buffer as the document and Save persists it.
  Clamp `tl` to `n - 12`.
- **[S] Editor recomputes Ln/Col by full scan every draw** (`pc64_write.c:361-378`) — cache it,
  recompute only on caret/len change.
- **[S] Present scratch buffers are sized for 4K with no clamp** — `gRow`/`gColMap`
  (`GROW_W=3840`) and the Blt band are indexed to `gOutW` with nothing clamping it
  (`uefi_main.c:120,122,1970`); a >3840-px panel overflows them in the present hot loop. Clamp
  `gOutW`/`gOutH` (or letterbox). *(Confirm the clamp is truly absent first.)*
- **[S] logview `u2s` writes into `tmp[12]`** for a 64-bit `unsigned long` (up to 20 digits) —
  size to `tmp[24]`. Low reachability.

### WS-B7 — Debug harness  ·  lane: debug (`uno_debug.c`, `pc64_spectest.c`)
- **[S-M] `next_seq` declares a ~23 KB stack frame (`uno_fat_entry e[999]`) and does a full dir
  enumeration on every crash report / snapshot** — including inside the trap handler right after
  a possible stack smash (`uno_debug.c:733-740`). Cache a monotonic counter.
- **[S] `emit_skip` bypasses the flush budget** and reintroduces the O(n²) buffer-rewrite storm
  on SKIP-heavy netless runs (`pc64_spectest.c:103`); route it through `since_flush`.

### WS-B8 — Resource leaks  ·  lanes: usb / modload / unovirt
- **[S] xHCI slot + DCBAA leak on enumeration failure** (`xhci.c:483-564`) — DISABLE_SLOT and
  clear the entry on the failure paths.
- **[S] Module loader leaks executable pages when a loaded module is refused post-instantiation**
  (`pc64_modload.c:538-545` etc.) — a wrong-tier `.UNO` retried per launch exhausts the 4 MB
  post-detach arena. Capture base/np and `mod_free`.
- **[S] `!UNO_XHCI` stub doesn't compile** (duplicate fn + undefined symbol, `xhci.c:22-29`).
- **[S] unovirt RAM probe returns 0 MB when the UEFI memory map exceeds 48 KB**
  (`unovirt.c:117-138`) — retry on BUFFER_TOO_SMALL instead of reporting "no RAM for a carve".

---

## SECURITY (priority 3 — MUST-FIX vs DEFER tagged)

The four MUST-FIX items are all small; three are memory-corruption bugs that also belong under
"bugs". The reassuring headline: the RBAC gate, hypervisor GPA/EPT containment, virtio device
model, `.UNO` module loader, TLS entropy (fail-closed), and Ed25519 verification were all audited
and found **well-engineered** — see "Verified clean" below.

### MUST-FIX
- **[S] S-1 · HTTP chunked-encoding heap overflow.** `dechunk` accumulates the hex chunk size
  into `long sz` with no digit cap (`pc64_http.c:339-364`); a size with the top bit set goes
  negative, bypasses `in + sz > len`, and runs `memmove` with `(size_t)sz ≈ SIZE_MAX` on
  attacker-controlled response data — reachable from any address-bar navigation or a MITM on
  plain HTTP. **Triple-confirmed.** Cap digits ≤ 8 and bound `sz` unsigned before the memmove.
  *(lane: browser)*
- **[S-M] S-2 · URC PIN brute-force lockout is defeatable.** Two compounding defects
  (`unoauto_gate.c:389-427`, `unoauto_remote.c:1418-1431`, `pc64_accounts.c:161`): `auth` never
  short-circuits after 3 failures and `drain_rx` evaluates every pipelined `auth` line in one
  socket read (~450 guesses/arming), and the modal Remote-Control panel pumps
  `unoauto_remote_tick` but **not** `unoauto_gate_tick`, so while the arming panel is open the
  lockout never fires and the full 10⁶-PIN space falls in ~30–40 s. The listener is advertised
  on the LAN via netdisc. Fix: short-circuit + disarm inline in `unoauto_gate_auth`; pump
  `unoauto_gate_tick` from `modal_frame`; cap auth attempts per drain. *(lane: unoautomate)*
- **[S] S-3 · rtwifi RX stack overflow from an untrusted descriptor length.** `plen = w0 &
  0x3FFF` (≤16383) from the DMA'd RX descriptor is memcpy'd into a 1600-byte **stack** buffer;
  the `n>1600` check happens *after* the copy (`rtwifi.c:399-468`). Reachable whenever the
  rtwifi radio is active. Clamp `plen` to `RXBUF` and pass an output capacity into `wifi_to_eth`.
  *(lane: rtwifi driver)*
- **[S] S-4 · SSH packet-length signed-integer overflow → remote DoS.** `plen` (int) from 4
  server bytes: `plen=0x7FFFFFFC` makes `total = 4 + plen` wrap to `INT_MIN`, passing the
  `total > RXCAP-32` guard, and the negative length reaches `br_hmac_update` as a huge `size_t`
  → OOB read / reboot (and a UBSan trap on the debug build) (`unossh.c:176-179`). Validate `plen`
  directly before deriving `total`. *(lane: unossh)*

### DEFER (hardening — reject any of these until closer to production)
- **[S] unojs parser has no recursion-depth guard.** Runtime call depth is capped
  (`ujs_vm.c:194`) but the recursive-descent compiler isn't (`ujs_comp.c:452,494,669`); deeply
  nested valid input (`((((…))))`) overruns the native C stack in a UEFI target with no guard
  page. Add a parse-depth counter tripped ~200. *(This is the one open HIGH from the prior
  AUDIT-pc64.md S5; consider promoting to MUST-FIX if scripts run from fetched pages.)*
- **[S each] URC hardening:** don't advertise the listener via netdisc once armed / after a
  client connects (F2); block `screen` OBSERVE grabs while a credential sheet is modal (F3);
  the debug-only `nst`/`disc`/`wedge` verbs should be `#ifdef UNO_DEBUG`-gated out of production
  (F10). *(unoautomate)*
- **[M] Manifest "signatures" are symmetric HMAC with the key stored on-device**
  (`unosecure.c:1160-1226`) — anyone who can read `UNOSEC.DB` (URC SYSTEM, or offline disk)
  forges any-capability manifests. Move to asymmetric (Ed25519 is already in the tree) against a
  public key. Must-fix *only if* manifests are a trust boundary against local code. (F4)
- **[M] URC `py` runs under the console INTERACTIVE session, not the REMOTE-trust link session**
  (`unoauto_remote.c:1249-1260`) — decide and document, or bind to `g_link_sess`. (F5)
- **[S] Password/credential hygiene:** zero `g_pw` after a *successful* login/create/passwd
  (`pc64_accounts.c:60`, F8); route salt generation through the fail-closed `tls_entropy_get`
  instead of a whitened-TSC fallback (F9); wire the dead `account_t.locked` field to a
  failed-attempt counter or remove it (F7).
- **[S] Wire-parse bounds:** DHCP option parser can read a few bytes past the option area
  (`net.c:834-846`, S2-net); Content-Length parsed unbounded → negative `clen`
  (`pc64_http.c:836-838`, S3-net); `fire_cb` splices its arg into a single-quoted JS literal
  without escaping (`webjs.c:525-544`, not attacker-reachable today, S4-net).
- **[S] rtwifi SSID overflow from a malformed local `WIFI.CFG`** (`rtwifi.c:580`) — bounded copy,
  reject > 32.
- **[S] SSH host-key contract is caller-enforced, not transport-enforced** — both shipped callers
  are correct, but a future caller that forgets leaks the auth signature; consider refusing
  channel/auth traffic until a verify result is recorded. Also fix the stale "not implemented"
  comment (`unossh.c:23-27`). (SSH-2/3)
- **[S] `safe` clears the dead-man guard with no token when none is supplied**
  (`unoauto_remote.c:1183-1190`) — DRIVE-tier reliability control, low severity. (F6)

### Verified clean (do not re-litigate)
Prior AUDIT-pc64.md S1–S4, S6 all confirmed fixed. RBAC/URC gate is fail-closed with a
constant-time PIN compare, rejection-sampled token from fail-closed entropy, 3-strike design,
and a complete GATE[] verb table (every reachable verb gated; unknown verbs default to SYSTEM and
are refused). PBKDF2-HMAC-SHA256 password hashing with constant-time verify. TLS entropy refuses a
weak handshake. Hypervisor `uno_vmm_gpa`/EPT carve containment, the virtio model, and the `.UNO`
module loader are overflow-safe. Ed25519 verification is complete and correct (canonical-S
malleability rejection, point-on-curve, deterministic nonce); SSH transport MACs the plaintext and
checks it constant-time before dispatch; **SSH is client-only** (no inbound server surface). QOI
and cookie parsing are bounded.

---

## Suggested sequencing

- **Wave 1 (parallel, highest value):** WS-P1 (idle spin), WS-P2 (TCP), plus the four MUST-FIX
  security items (S-1..S-4) — all small and independent, in four different lanes.
- **Wave 2:** WS-B1 (FAT correctness, esp. the 64-bit LBA + mirror-copy data-loss bugs) and
  WS-B2/B3 (boot-entry + vsnprintf), which are correctness landmines independent of perf.
- **Wave 3:** the remaining perf workstreams (P3–P7) and driver bug cleanups (B5–B8), lane by lane.
- **Deferred:** the DEFER security list, at your discretion.

Shared choke-points to claim before touching (per AGENTS.md §2/§4): the aurora shadow fix and any
`unoui.c` damage-region work (unoui lane), the `build.sh` file list (if adding a TU for `-flto` or
a timer-IRQ module), and `pc64_uui.c` boot/tick wiring (append only).
