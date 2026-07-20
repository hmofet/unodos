# UnoDOS deep code review - consolidated report (2026-07-20)

Scope: whole codebase, with emphasis on the pc64 flagship (graphics, uno3d, Duum),
the untrusted-input surfaces (media parsers, network/TLS, storage, module loader),
and an adversarial pass on architecture. Seven subsystem reviews ran in parallel;
every CRITICAL and the featured HIGH findings were then verified first-hand by
re-reading the cited code. Per-area detail lives in the `FINDINGS-*.md` files
alongside this report. No code was changed.

Prior perf work (`HANDOFF-perf.md`, the `AUDIT-*.md` retro-port damage-rect program)
was treated as out of scope and not duplicated; this review targets fresh ground.

---

## 1. Your three reported slowness symptoms - root causes

All three share one structural theme: the drawing architecture is actually sound
(RAM backbuffer `fb[]`, no MMIO readback, cached desktop, precomputed scale tables,
rubber-band window drag), and the cost has moved into a few specific hot paths.

### Window drag / redraw is slow
- **The headline cause: every pointer move repaints the whole scene.** Any input
  event, including bare mouse motion whose cursor is composited separately at
  present time, sets a single global dirty flag and re-runs the full alpha-blended
  Aurora painter, then present diffs the entire frame to find what changed
  (`pc64_uui.c:1928`, `unoui.c:1015-1097`). unoui has no damage rectangles at all.
  Fix: on pure cursor motion with no hover-state change, call present directly and
  skip the repaint.
- **Damage is tracked per row with no column extent.** The drag outline's vertical
  edges dirty every row of the window's height, so each drag frame rewrites roughly
  window-height by full-panel-width to VRAM (about 8 MB) to move two hairlines
  (`uefi_main.c:106,1066-1088`). Fix: per-row min/max column extents, or present
  only the known outline rects during a drag.
- **Desktop-icon drag missed the rubber-band optimization** that window drag got, so
  it takes the full-repaint path (`pc64_uui.c:1665-1671`).
- **Unverified but potentially dominant on metal:** nothing in pc64 programs MTRR/PAT,
  so the GOP framebuffer keeps whatever cache attribute the firmware left. If it is
  uncached (UC) rather than write-combining (WC), the per-pixel `volatile` store loop
  (`uefi_main.c:1085`) costs hundreds of milliseconds per full frame by itself. This
  single unknown spans "fine" to "unusable" and should be measured first (see
  Open Questions). On the `gUseBlt` path it is worse: one firmware Blt per row.

### uno3d is slow
- **The rasterizer recomputes all three edge functions from scratch per pixel**, with
  no incremental stepping, plus a redundant per-pixel reciprocal multiply, per-pixel
  z, and 9-multiply color interpolation (`uno3d_soft.c:64-88`). All of it is affine
  and should be stepped by addition. This is the dominant cost and the reason the
  renderer had to drop to quarter resolution. Expected 5 to 10x from incremental
  half-space rasterization.
- **No near-plane clipping, only a whole-triangle reject** (`uno3d.c:153`). Two
  effects: the corridor floor/ceiling slabs likely render nothing at all, and
  triangles that barely survive project to near-infinite coordinates whose clamped
  bounding box is the whole screen, spiking frame time exactly when a wall reaches
  the player.
- Same full-panel present cost as the drag path applies (uno3d draws a full-screen
  frame every frame, so present pass 1's full-frame dirty scan is pure overhead).
- No delta-time (`uno3d_game.c:147-158`): frame-rate-coupled motion means slowness
  also plays in slow motion, amplifying the felt problem.

### Duum is slow
- **The renderer runs as plain interpreted bytecode.** The MicroPython x64 native
  emitter is compiled in, and the config comment even claims `@micropython.viper`
  compiles Duum's renderer, but `DUUM.PY` has zero native decorators
  (`mpconfigport.h:35` vs `DUUM.PY:338-378`). Under `MICROPY_OBJ_REPR_A` every float
  intermediate is a heap allocation, so the column loop makes on the order of 10^5
  allocations and 10^6 bytecode dispatches per frame. A `@micropython.native`
  `draw_seg` is a 2 to 4x win for near-zero effort; viper or a C span-helper is more.
- **It forces a full repaint every frame even when standing still.** `tr_frame`
  unconditionally dirties the shell (`pyrt.c:153`), so `draw()` replays the whole
  display list continuously and the dirty flag defeats the 16 ms idle sleep (100%
  CPU). Fix: dirty only when `tick()` reports a change.
- Secondary: `cv.wall_col` re-clips per pixel inside `fb_pixel` and wide canvases
  sample each column twice; per-frame closure redefinition and tuple garbage thrash
  the GC.

**One shared win underlies all three:** an explicit damage/dirty-region and a
full-invalidate present hint would remove present pass 1's mandatory full-frame scan,
which every heavy-redraw path currently pays.

---

## 2. Security findings (untrusted input) - verified

### CRITICAL (all confirmed first-hand)

| ID | Where | Bug | Trigger |
|----|-------|-----|---------|
| STG-C1 | `pc64/pc64_modload.c:335-349` | `mem_size` (u32) never bounded; `(mem_size+4095)>>12` overflows to a tiny/zero page count, then `memset(base+file_size, 0, mem_size-file_size)` writes ~4 GB OOB | loading any crafted `.UNO` module (CRC is integrity-only, attacker-computable) |
| STG-C2 | `pc64/pc64_modload.c:353-357` | relocation bound `rel[i]+8u > mem_size` overflows in u32, so `rel[i]=0xFFFFFFF8` passes and writes 8 bytes at an attacker offset | crafted `.UNO` with a valid `mem_size` |
| STG-C3 | `unofs/unofs_core.c:17-29` | `fat`/buffer is 4608 bytes but `fat_get`/`fat_set` index `cl+cl/2` (~6130 for a legal `cl<0xFF8`); chain-follow accepts any on-disk cluster. OOB read on open/read, OOB heap write on delete/append | crafted FAT12 image with a `start_cluster` or chain link in ~2739..4087 |
| NET-C1 | `pc64/wifi_wpa.c:299-323` | `aes_unwrap` writes `n*8` bytes (up to ~400) into `kd[256]` with no capacity check; `kdlen` is a raw frame field bounded only by the 512-byte MIC gate | a malicious/evil-twin AP that knows the WPA2 PSK sends msg 3/4 or a group rekey with Key-Data-Length ~408 and a valid MIC. 144-byte stack smash -> code execution. MIC-gated (AP-authenticated) |

### HIGH (confirmed / high-confidence)

- **MEDIA-H1 (confirmed)** `unomedia/um_midi.c:271,490,496` - MIDI Program Change byte
  stored unmasked (0-255), used as `kFamily[program>>3]` into a 16-entry array ->
  OOB read up to `kFamily[31]`. Read-only, but UB and fuzz-reachable. Fix: `& 0x7F`.
- **ARCH/STUDIO (confirmed)** `pc64/apps/studio_ai.c:283-289,316-321` - an on-disk
  `AI.CFG` with `insecure=1` drops to unvalidated `tls_connect`, and `host=` overrides
  the destination. Any app that can write the volume can point Studio at an
  attacker's IP with cert checking off and exfiltrate the API key on the next
  request. Remove `insecure`/`host` from on-disk config.
- **NET-H1/H2 (suspected, metal-pending WiFi)** `mrvlwifi.c:383-397`,
  `iwlwifi.c:707-748` - RX paths trust device-supplied `pkt_off`/`pkt_len` /
  descriptor `mpdu_len` without bounding to the DMA'd length -> OOB read / info leak.
- **STG-H1/H2 (confirmed)** installer GPT `num*esz` multiply overflows its buffer
  guard (`installer.c:229-240`); FAT `TotalSectors` underflow yields ~4 billion
  clusters and a `fat_alloc` scan that effectively hangs (`fat.c:117-118`).

### Verified CORRECT (worth stating plainly)

- **TLS validation is properly enforced.** Real bundled trust anchors (ISRG Root X1
  confirmed), SNI/hostname checked, validity-time checked, pinned path uses a public
  key. No skip-verify path in the normal flow (the `insecure` toggle above is the one
  hole) and no hardcoded private key.
- **unomedia is unusually disciplined.** Across 16 decoders plus inflate, no OOB
  write, no integer-overflow-into-undersized-alloc, no LZ/Huffman overread: every
  decoder guards dimensions before allocating. The MIDI read above is the sole
  memory-safety defect found.
- Wired/USB NIC RX paths, the HTTP client, and the browser parsers all clamp
  correctly. WPA MIC verification is present; the only WPA hole is NET-C1.

### One systemic note

The module CRC32 authenticates nothing (an attacker recomputes it trivially), so
every `.UNO` header field is attacker-controlled. STG-C1/C2 are the sharp edge of
that. The whole `.UNO` header should be validated defensively: bound `mem_size`,
`file_size`, `nreloc`, `imp_count`, and every RVA with subtraction-form checks in
64-bit, before any allocation or write.

---

## 3. Adversarial architecture findings (the sharp ones)

1. **The Contract governs the frozen ports and skips the flagship.** `unodef` is the
   billed single source of truth and is genuinely `%include`d by the asm kernels, but
   the ~45k-line pc64 port (the one under daily development, where drift actually
   happens) consumes it nowhere. The anti-drift tool covers the code that does not
   move and gives no cover to the code that does.
2. **The module ABI, which the theory says to generate, is hand-copied and already
   divergent** across four C ports (`uno_mod.h` 129/94/75/54 lines), and
   `KernelApi.abi_version` is written by every loader but read by no app - a dead
   handshake.
3. **One damage-rect bug was fixed and re-verified independently in about ten ports**
   (`HANDOFF-perf.md`); `dostris.c` exists as three byte-identical copies plus a
   diverged fourth plus seven asm rewrites. Logic is N-plicated by hand and the
   Contract cannot help, because it shares data shape, not behavior. The single
   highest-leverage move is a shared `unowm`/`unoapp` C policy the four C ports link,
   turning five-commit cross-port campaigns into one.
4. **`CONTRACT-ARCH.md` is 660 lines of mostly-unbuilt forward design**, and the
   flagship networked app contradicts its centerpiece: `studio_ai.c:329` blocks the
   entire cooperative desktop on every HTTP round-trip. Split "what ships" from a
   clearly-labeled speculative section.
5. **"Executable conformance" tests a Python reference model, not any of the 30 real
   implementations.** It proves the spec is self-consistent and nothing about what
   ships.
6. **Build system:** 30 hand-written shell scripts, no dependency tracking,
   `pc64/build.sh` duplicates BearSSL/QEMU/app blocks across two drifting modes, and
   **129 build artifacts are committed to git**.

Done right and worth extending: the asm "five places" collapse for the asm family,
the shared C libraries (`unofs`/`unoui`/`uno2d`/`uno3d`/`unomedia`/`unoacpi`) that
pc64 links, and the `.UNO` decoupling asserts. The shared-library pattern is the real
duplication cure; extend it to the window manager and apps.

---

## 4. Recommended priority order

1. **Fix the four CRITICALs** (STG-C1/C2/C3, NET-C1) and MEDIA-H1. Small, contained,
   high-severity. Add the defensive `.UNO` header validation as one change.
2. **Remove `insecure`/`host` from Studio's on-disk config** (ARCH/STUDIO).
3. **Measure the framebuffer cache attribute on real hardware** (the single biggest
   perf unknown), then force WC after detach if needed. Cheapest possible test with
   the largest possible payoff.
4. **Graphics: stop repainting the world on bare mouse motion**, give icon drag the
   rubber-band path, and add per-row column extents to the dirty tracker.
5. **uno3d: incremental edge stepping** (5-10x) and near-plane clipping.
6. **Duum: gate the repaint on `tick()`** and add `@micropython.native` to the hot
   loop (after a one-off on-device smoke test of viper/native under the mingw ABI).
7. **Architecture: a shared `unowm` for the four C ports**, and either onboard pc64 to
   the Contract or stop calling it OS-wide truth. Add a fuzz harness over
   `um_image_open`/`um_audio_open` and untrack the committed build artifacts.

---

## 5. Open questions (need hardware, do not treat as settled)

- **GOP framebuffer cache attribute (UC vs WC) on the real laptops.** Decides whether
  present is ~5 ms or ~500 ms per full frame. No MTRR/PAT code exists to force it.
- **Where was "very slow" observed - metal or the QEMU harness?** `pc64/harness.py`
  runs QEMU with no acceleration (pure TCG), which guarantees single-digit fps for
  the float-heavy uno3d/Duum paths regardless of any fix. Metal numbers may be much
  better (though still afflicted by the issues above).
- **Does any supported machine take the `gUseBlt` per-row present path?** If so, that
  is a standalone P1 for that machine.
- **NET-H1/H2 and the WiFi RX paths are metal-pending** (no QEMU model); confirm on
  real silicon. Note also that NET-C1 is only reachable via the Marvell driver today
  because iwlwifi/rtwifi pass the wrong offset to the EAPOL parser, which is itself a
  functional bug (their 4-way handshake cannot complete).
