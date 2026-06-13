# Apple II port — milestone 2 handoff (for Claude Sonnet)

Prerequisite: **M1 landed** (boot chain + harness + hi-res desktop + WM
+ SysInfo/Clock, per [HANDOFF.md](HANDOFF.md)). Read HANDOFF.md §2–§5
first — the envelope, boot contract, memory map and harness ABI all
still apply. Authoritative direction:
[../docs/PORTS-PLAN.md](../docs/PORTS-PLAN.md) §1, M2 line.

**M2 scope (from the plan):** RWTS **write** path; a track/sector
mini-FS (USV1-style catalog — FAT12 deliberately does NOT fit GCR
sector space sensibly); **Files + Notepad**; paddle/joystick pointer
option; speaker beeps. Bar: code + extended harness + `tests/m2.script`
+ screenshots + commit + README update — one milestone, one commit.

Recommended order: §1 (kernel RWTS + harness write path, proven by a
sector round-trip test) → §2 (mini-FS + mkfs packing) → §3 (Files,
Notepad) → §5 (beeps) → §4 (paddles, optional) → tests/README.

---

## 1. Kernel-side RWTS (read + write)

boot.s's RWTS lives in the `$0800–$17FF` boot image, which the kernel
reclaims — so M2 starts by giving the **kernel its own RWTS** (suggest a
new `rwts.i` included by `kernel.s`):

- **Seek:** port boot.s's `seek` (stepper phase pulses on
  `$C080–$C087`, half-track counter). **Keep the current-track variable
  at zero page `$F1`** — the harness reads `$F1` to know which track to
  serve (HANDOFF.md §6, the zpTrkCur ABI). Do not move it.
- **Read sector:** port boot.s's `readsec` (hunt `D5 AA 96`, 4-and-4
  address, match track/sector, hunt `D5 AA AD`, 6-and-2 denibble).
  Unlike boot0 you want *one named sector to a caller buffer*, not
  whole-track-to-pages: `rwts_read(track, sector, dest)` where
  `sector` is **logical** (apply `skew` internally — keep the skew
  table in one place).
- **Write sector:** the new code. Kernel side needs the 6-and-2
  *encoder* (the inverse of the read path — `wrtab` from boot.s is the
  64-nibble table; the 86-byte "twos" buffer logic is mirrored in
  `harness.py`'s `nibblize_data`, use it as the executable spec).
  Sequence per "Beneath Apple DOS": find the target sector's *address*
  field in read mode, then switch to write mode (`Q7` on: access
  `$C08D` with `$C08E/$C08F` per the protocol), write 5 self-sync
  nibbles, `D5 AA AD`, the 343 data nibbles, `DE AA EB`, then back to
  read mode. On real hardware every nibble must land **exactly 32
  cycles** apart and sync bytes are 40-cycle 10-bit nibbles — write
  the loop cycle-counted from the start (count it in comments, the way
  boot.s annotates timing), but know that **the harness will not check
  timing**; AppleWin is the timing oracle (§6 risk).
- **Motor:** boot leaves the motor on; the kernel RWTS should turn it
  on (`$C089,X`) before I/O and may leave it on (the harness doesn't
  model spin-up; on real hardware add a spin-up delay before the first
  read after motor-on — calibrate in AppleWin).

**Harness extension (write path).** Teach `harness.py`'s `DiskII` to
accept writes:

- Hook writes to `$C08D+slot*16` / `$C08F+slot*16` (and the `Q7`/`Q6`
  mode switches). While in write mode, append each latched nibble to a
  capture list instead of serving reads.
- When the kernel switches back to read mode, parse the captured
  nibbles: find `D5 AA AD`, denibblize 342 nibbles
  (`denibblize_data` already exists), and store the 256-byte payload
  into the in-memory `.dsk` image at the **last address field the
  kernel read** (track from `$F1`; remember the physical sector of the
  most recent address-field match — easiest is to have the harness
  remember the physical sector whose address field bytes it most
  recently served). Invalidate that track's nibble-stream cache so the
  next read sees the new data.
- Add a harness flag to **persist** the mutated image (e.g.
  `--writeback out.dsk`) so a test can save, quit, re-boot the written
  image, and verify persistence — the macplus m2 script does exactly
  this shape (save → wipe → reopen).
- **Self-check first:** before the kernel encoder exists, unit-test the
  harness write path by replaying a synthetic capture (take
  `nibblize_data` output, feed it through the parser, assert the
  payload lands in the image). Extend `--selftest`.

**Proof gate for §1:** an AUTOTEST kernel path (or a tiny test command)
that reads logical sector (T,S), XORs a known pattern into it, writes
it back, re-reads, and renders PASS/FAIL on screen — screenshot it.
Only then build the FS on top.

## 2. The mini-FS — "USV1-style catalog" on tracks

Port the Genesis USV1 design ([../genesis/sram.i](../genesis/sram.i)
lines 1–30 document the format) from a byte heap to a sector heap.
Suggested concrete layout (you may adjust; document what you choose):

- **FS region:** tracks `FS_TRACK..34`, where `FS_TRACK` is fixed and
  generous (e.g. 20 — leaves the kernel ~19 tracks ≈ 76 KB to grow
  through M3; the FS gets 15 tracks = 60 KB). `mkdsk.py` asserts
  `1 + KTRACKS <= FS_TRACK`.
- **Catalog = track FS_TRACK, sector 0** (256 bytes):
  - `0..3` magic `"USV1"`, `4` file count, `5` next-free-sector index
    (relative to the FS region, heap grows upward), `6..15` reserved,
  - `16..255`: **15 directory entries × 16 bytes**: `name[12]`
    (NUL-padded, same convention as Genesis), `size` (word, **little-
    endian** — pick LE to match the FAT12 family and `.BIN` headers,
    unlike Genesis's BE SRAM; it's interchange media), `start`
    (word, sector index relative to the FS region).
- Files are **contiguous runs of 256-byte sectors** in the heap;
  delete compacts (move later files down, rewrite catalog) — same
  semantics as Genesis `sram.i`, so its `fs_find/fs_read/fs_save/
  fs_delete/fs_list` entry points port one-to-one. Save-by-name
  overwrites (delete + append).
- Keep the API surface identical to Genesis's so Files/Notepad port
  cleanly: find → (start, size), read(start, size, dest),
  save(name, src, size), delete(name), list(callback/iterate).

**`mkfs` side:** extend `mkdsk.py` (or add `mkfs.py` mirroring
[../macplus/mkfs.py](../macplus/mkfs.py)) to format the catalog and
pre-load seed files — at minimum a `README .TXT`-style text file so
Files has something to list on first boot (macplus packs `disk/*.TXT`;
copy that convention with a `disk/` directory).

**Caching:** one 256-byte sector buffer + the catalog cached in RAM is
plenty. Place buffers in the `KBSS` zone; remember to bump `KBSS`
asserts if the kernel grew (HANDOFF.md §7).

## 3. Files + Notepad (keyboard-driven)

Model the app *behavior* on macplus (`files_draw`/`files_key`,
`notepad_*` in [../macplus/kernel.asm](../macplus/kernel.asm)) and the
*input style* on M1's keyboard nav (II+ floor: left/right + Return +
ESC; remember left = `$88` doubles as backspace — inside Notepad,
`$88` must mean backspace/caret-left, which is the natural Apple II
convention anyway):

- **Files:** lists the catalog (name + size), left/right (or up/down
  on IIe) move the selection, Return opens the file in Notepad, `d`
  deletes (with a y/n confirm line), `r` re-reads the catalog.
- **Notepad:** view/edit a text buffer (suggest 2 KB cap, the 68K
  ports cap at 2–4 KB), typing inserts, `$88` backspaces, Return
  inserts newline, ESC closes (kernel-side, as at M1). **Save key:**
  `Ctrl-S` would be natural but `$C000` gives you it only as `$93`
  (Ctrl letters arrive as codes < $20 | $80) — use it; it exists on
  every Apple II keyboard. Status line with Ln/Col/bytes (the audit's
  stale-status rule: redraw it on every edit).
- 40-column text in 7-px columns; window content fits the M1
  byte-aligned renderer. Word wrap is NOT required — hard lines only.

## 4. Paddle/joystick pointer (optional — do not block M2 on it)

`$C070` (trigger) starts the analog timers; `$C064/$C065` bit 7 stays
set for a time proportional to the paddle position (~11 µs per count,
0–255). Reading = trigger, then count loop iterations until bit 7
drops. It is a **blocking, timing-sensitive poll** (~3 ms worst case
per axis) — acceptable once per main-loop pass.

- Kernel: an optional pointer mode (toggled by a key, e.g. `Ctrl-P`)
  that maps paddle 0/1 to an XOR-drawn crosshair cursor, button 0
  (`$C061` bit 7) = select. Byte-aligned cursor is fine.
- Harness: model `$C064/$C065` as instant values settable by new script
  commands (`paddle X Y`, `button 0/1`) — no decay-timing fidelity
  (that is AppleWin's job, same split as the RWTS).

If time pressure hits, ship M2 without it and leave it flagged in the
README — the plan calls it an "option".

## 5. Speaker beeps

`$C030` read toggles the speaker cone. A beep = a cycle-timed toggle
loop (frequency = 1 MHz / (2 × half-period cycles)), **blocking** while
it plays — that is the honest Apple II reality (the plan's M3 sound
note). For M2 just add `beep` (~1 kHz, ~100 ms) on: app launch, save
complete, error/invalid key. Keep it one routine with period + duration
parameters — M3's Music/Tracker grow out of it.

Harness: count `$C030` accesses (replace the no-op lambda with a
counter) and, if you want a real assertion, record the step-index of
each toggle so a test can sanity-check the period. Minimum bar: the
m2 script asserts the toggle count is nonzero after a save.

## 6. Tests, README, risks

`tests/m2.script` (shape, adjust to taste):

```
wait 60
shot m2_desktop          # Files + Notepad icons present
key right
key return               # launch Files
wait 10
shot m2_files            # seed file listed
key return               # open in Notepad
wait 10
shot m2_notepad
keys hello
shot m2_edited
key ctrl-s               # save (add ctrl-key support to KEYS)
wait 20
shot m2_saved            # status line confirms; beep counter > 0
quit
```

…then a **persistence pass**: run again with `--writeback`, re-boot
the written image, reopen the file, screenshot the edit still there.
Two harness invocations in `build.sh test`'s m2 recipe (or a small
wrapper) is fine.

README: new sections for the FS layout (the catalog format verbatim),
the write-RWTS timing caveat, paddle support status, beep behavior.

**Risks to carry:**
- **Write timing is unproven until AppleWin.** The harness validates
  the logical write protocol only. Before metal: AppleWin write test
  (save a file, re-open) is mandatory — AppleWin's cycle-honest Disk
  II will catch a mis-timed write loop the harness cannot.
- **`$F1` ABI**: the kernel RWTS must keep its track variable at `$F1`
  (HANDOFF.md §6) or the harness gains real stepper modeling — keeping
  `$F1` is much cheaper; do that.
- **Track budget:** `FS_TRACK` freezes the kernel's maximum size on
  disk. Pick it once, assert it in mkdsk.py, document it.
- **Catalog endianness:** this file format IS interchange media
  (FloppyEmu images, AppleWin `.dsk`s) — little-endian on disk,
  documented, and `mkfs`/kernel agree.

When M2 lands: commit, update `../docs/PORTS-PLAN.md` §1 (M2 → DONE),
then continue with [HANDOFF-M3.md](HANDOFF-M3.md).
