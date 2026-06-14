# UnoDOS/AppleII — standalone OS for the 6502 Apple II (milestone 3)

Like the other bare-metal UnoDOS ports, this is **a real operating
system**, not a DOS 3.3 application. The Disk II boot ROM autoloads our
own boot code; from `$4000` on, UnoDOS owns the machine — its own hi-res
renderer, its own keyboard polling, its own desktop and window manager.
There is no DOS 3.3, no ProDOS, nothing else on the disk.

## The "UnoDOS Lite" envelope

A 1 MHz 6502 with a software-only hi-res renderer is the tightest
envelope of any UnoDOS port so far, so M1 is **UnoDOS Lite**: parity
adapted to what the hardware can actually do, with every deviation
documented rather than faked.

- **Hi-res 280x192, effectively 1-bit.** 7 pixels/byte, bit 0 = leftmost,
  bit 7 = palette (kept 0). Rows are the classic Apple II interleaved
  layout (`addr = $2000 + (y&7)*$400 + ((y>>3)&7)*$80 + (y>>6)*$28 + c`).
  Every window/icon dimension is in **byte-columns** (7px steps), not
  pixels — the visible deviation vs. the bitmap-precise 68K ports.
- **Keyboard-driven, II+ floor.** No mouse at M1 (AppleMouse II is
  backlog). Nav is left/right arrow (`$08`/`$15`) + Return + ESC — the
  Apple II+'s full keyboard, no up/down/Tab assumed.
- **No timer hardware.** The Clock app runs on a **calibrated soft
  tick**: the main loop counts passes and converts to seconds with
  `TICKS_PER_SEC` (kernel.s) — there is no real clock to read.
- **No firmware sector services.** The boot ROM autoloads track 0 only;
  everything past that — head stepping, GCR 6-and-2 decode, the DOS 3.3
  skew table — is our own RWTS in [boot.s](boot.s).

## Boot chain

1. The Disk II P5A controller ROM autoloads **all 16 sectors of track 0**
   (`$0800-$17FF`, boot.s byte 0 = `$10`) and jumps to `$0801` with
   `X = slot*16`.
2. [boot.s](boot.s) (`entry:`) builds the 6-and-2 decode table at `$1900`,
   then for tracks `1..KTRACKS` steps the head and denibblizes every
   sector (DOS 3.3 `skew` interleave) into `$4000+`.
3. `jmp $4000` into [kernel.s](kernel.s), which clears hi-res page 1,
   draws the desktop, and never returns to DOS.

## M1 desktop

40 byte-columns x 24 char-rows (8px) of hi-res page 1:

| Rows | Content |
|---|---|
| 0 | Menu bar (`UnoDOS` title + separator) |
| 1-8 | **SysInfo** window — machine ID (via `$FBB3`/`$FBC0`), CPU, RAM |
| 9-13 | **Clock** window — `HH:MM:SS` from the soft tick |
| 14-18 | Empty dithered desktop |
| 19-23 | Icon grid — SysInfo (col 2), Clock (col 14) |

Window manager: open/raise/close with a z-order list and focus tracking;
ESC closes the topmost window. Icon selection (left/right arrow) is shown
by inverting the icon's label band (EOR `$7F`); Return opens or raises
the selected app's window.

## M2: kernel RWTS (read + write), mini-FS, Files + Notepad, beeps

M2 gives the kernel its own RWTS (boot.s's RWTS lived in the now-reclaimed
`$0800-$17FF` boot image), a track/sector mini-FS, and two apps that use
it. The AUTOTEST desktop now shows `RWTS PASS  FS PASS` in the menu bar
(both self-tests run at boot, before the desktop draws) and a third icon,
**Files**.

### Kernel RWTS ([rwts.i](rwts.i))

`rwts_read`/`rwts_write` operate on one logical 256-byte sector at a time
(`zpTrkWant` = track, `A` = logical sector 0-15, `zpBuf`/`zpBuf+1` = buffer
pointer). The read side is boot.s's `seek`/`readsec` ported essentially
unchanged (hunt `D5 AA 96`, 4-and-4 address field, match track/sector, hunt
`D5 AA AD`, 6-and-2 denibble). The write side is new: hunt the target
sector's address field in read mode, switch to write mode (`$C08F`, Q7 on),
emit 5 self-sync `$FF` nibbles + `D5 AA AD` + 343 6-and-2 data nibbles
(`rwts_buildrdbuf2` + `WRTAB`, the mathematical inverse of the read-side
`DECTAB`/`FLIP2` decode) + `DE AA EB`, then back to read mode (`$C08E`, Q7
off — this is also where the harness commits the captured write). `$F1`
(`zpTrkCur`) stays the harness's track-select ABI per HANDOFF.md SS6, unmoved.

**Write timing is unproven until AppleWin.** The harness validates the
*logical* write protocol only (`DiskII._commit_write` denibblizes the
captured nibble stream and drops it straight into the `.dsk` image — it
does not model nibble cell timing). On real hardware every nibble must
land ~32 cycles apart and the 5 self-sync nibbles use an extended ~40-cycle
bit cell; `rwts_write` is not yet cycle-counted for that. Before metal:
boot `unodos_apple2.dsk` in AppleWin (cycle-honest Disk II), edit+save a
file in Notepad, quit and re-open — if the edit didn't stick, the write
loop's timing is the first thing to fix.

A boot-time AUTOTEST self-check (`rwts_selftest`, kernel.s) reads track 2
sector 3, XORs a fixed pattern into the buffer, writes it back, re-reads
and compares, then **XOR-restores the original bytes and writes them back
again** before reporting PASS/FAIL — so the round-trip test is non-
destructive to the disk image.

### Mini-FS — "USV1" sector-heap catalog ([fs.i](fs.i), [mkfs.py](mkfs.py))

Same design as [genesis/sram.i](../genesis/sram.i)'s byte-heap, ported to a
sector heap (FAT12 doesn't fit GCR sector space sensibly):

- **FS region:** tracks 20-34 (`FS_TRACK=20`, `FS_TRACKS=15`,
  `FS_SECTORS=240` 256-byte sectors). `mkdsk.py` asserts
  `1 + KTRACKS <= FS_TRACK` — the kernel has tracks 1-19 (~76 KB) to grow
  into through M3 before it would collide with the FS.
- **Catalog = rel sector 0**, cached in RAM at `CATBUF` ($6400) and flushed
  on every mutation:
  - `0..3` magic `"USV1"` (written by `mkfs.py`, not re-validated at boot)
  - `4` file count (0..`FS_MAXFILES`=15)
  - `5` next-free-sector (rel index; heap grows upward from 1)
  - `6..15` reserved (0)
  - `16..255` 15 directory entries x 16 bytes: `name[12]` (NUL-padded),
    `size` (word, **little-endian** — interchange media, unlike Genesis's
    BE SRAM), `start` (word LE, rel sector; high byte always 0 since
    `FS_SECTORS=240 < 256`).
- A rel sector `r` maps to `(track, logical) = (FS_TRACK + (r>>4), r&$0F)`
  — `FS_SECTORS=240` is a multiple of 16, so this is exact (no remainder
  sector spanning two tracks).
- Files are contiguous runs of rel sectors in the heap (1..239); the last
  sector of a file may have trailing garbage past its byte size.
  `fs_delete` compacts the heap (shifts later files' sectors down by the
  freed count, fixes every entry's `start`, drops the directory slot,
  flushes). `fs_save` overwrites by **delete-then-append** — same
  semantics as Genesis, so an edited file moves to the end of the
  directory listing (visible in `m2_persist_files`: HELLO.TXT, originally
  first, is last after being edited+saved).
- API surface matches Genesis: `fs_find`/`fs_read`/`fs_save`/`fs_delete`
  + `fs_entry_ptr`/`fs_size`/`fs_start` for listing — Files/Notepad call
  these directly.

`mkfs.py` formats the catalog and seeds `disk/*.TXT` (currently
`README.TXT` and `HELLO.TXT`) at image-build time; the kernel only ever
reads/writes the catalog it finds (`fs_init` loads it, never reformats).

### Files + Notepad ([files.i](files.i), [notepad.i](notepad.i))

- **Files**: lists the catalog (name + size, right-justified). Left/right
  moves the selection, Return opens the selected file in Notepad, ESC
  returns to the desktop. (`d`-delete and `r`-rescan from HANDOFF-M2 SS3
  are not yet wired up — only open is needed for M2's read/write/persist
  loop.)
- **Notepad**: 2 KB text buffer (`NOTEBUF`, $6500-$6CFF). Typing inserts at
  the cursor, `$88` (left-arrow/backspace) deletes the previous char,
  Return inserts a newline, ESC closes. **Ctrl-S saves** (`$93` —
  Ctrl-letters arrive as codes < `$20 | $80` on every Apple II keyboard).
  Status line shows `Ln:n Col:n Bytes:n`, redrawn on every edit; a `SAVED`
  flash confirms `fs_save` completed. Hard line breaks only (no word wrap).

### Speaker beeps ([kernel.s](kernel.s) `beep`/`beep_click`)

`$C030` is a read-toggled cone; `beep` is a cycle-timed toggle loop
(half-period + count parameters — blocking, the honest Apple II reality).
`beep_click` (~1 kHz, ~100 ms: `BEEP_CLICK_HALF=99`, `BEEP_CLICK_COUNT=200`)
fires on app launch (Files, Notepad) and Notepad save-complete/buffer-full.
The harness counts `$C030` accesses; `tests/m2.script` asserts the count is
nonzero after the edit+save sequence.

### Paddle/joystick pointer

**Not implemented in M2** (HANDOFF-M2 SS4 flags this as optional, "do not
block M2 on it"). Files/Notepad are fully keyboard-driven (left/right +
Return + ESC + Ctrl-S), so M2 ships without `$C070`/`$C064`/`$C065` paddle
polling or a pointer mode.

## M3: the scaled app roster (UnoDOS Lite, the games-and-media test)

M3 fills the desktop out to **ten icons** (a 3-row x 4-col grid) — the
shared UnoDOS app roster re-expressed for the 1 MHz envelope, each
deviation measured and documented rather than faked. Two items were
**feasibility-gated** (OutLast, the scheduler); for those, "we measured,
here's the number" is the deliverable.

The kernel now spans 4 tracks; M3 BSS tables live above the M2 buffers
(`DOSBOARD $9D00`, `PMMAZE $9E00`, `TKPAT $9F00`, `PAINTBUF $A000`). New
zero-page lives at `$50-$80`. Full-screen apps are dispatched by `app_mode`
(3=Theme … 9=OutLast); the games and OutLast advance off the M1 soft tick
(`ml_dos`/`ml_pac`/`ml_ol`), so harness `wait N` drives deterministic
frames.

### The apps and their honest deviations

| App | What it is | 1 MHz / 1-bit deviation |
|---|---|---|
| **Theme** ([theme.i](theme.i)) | Desktop dither-scheme picker, 6 presets incl. a full invert | Only the chrome fills are themeable (a mutable 4-byte `pat_tab`); window/app **content** stays fixed black/white for legibility (vs. macplus's full 4-logical-colour table) |
| **Dostris** ([dostris.i](dostris.i)) | 10x20 falling-block puzzle, 7 tetromino x 4 rotations, soft-tick gravity, line-clear scoring | Cells are 2-bytecol x 8px and absolute black/white (content, not themed) |
| **Pac-Man** ([pacman.i](pacman.i)) | 13x13 pillar maze, two ghosts (Blinky direct-chase, Pinky 4-ahead), scatter/chase + frightened/eat-chain | **13x13 / 2-ghost / tile-stepped 7px actors** (not 28x25 / 3-ghost / pixel-substep) — the maze and AI fit the frame budget; Manhattan-distance steer (no divide) |
| **Music** ([music.i](music.i)) | Canon in D on a 5-line staff, cursor advances note-by-note | **Blocking** square-wave playback monopolises the CPU (freezes the desktop while sounding — real Apple II reality); transposed up an octave so the single-byte speaker delay stays in range, staff plots the true contour |
| **Tracker** ([tracker.i](tracker.i)) | Shared 32-row x 4-channel pattern editor, demo song, SONG.UNO save/load | **Single-voice** playback (leftmost channel per row — the x86 PC-speaker precedent); instruments stored/editable but all sound as the one square voice |
| **Paint** ([paint.i](paint.i)) | MacPaint-style editor: pencil / rect / filled-rect / flood-fill, four dither inks, PAINT.UNO save/load | **32x34 byte-aligned fat-pixel cells** (not a pixel grid); **keyboard cursor** (arrows + Space), not mouse drag; line/oval/brush/spray omitted on the 6502 |
| **OutLast** ([outlast.i](outlast.i)) | Pseudo-3D road racer — *feasibility prototype* | **Ships, marginal** — see verdict below |

The note tables for Music and Tracker are generated by
[mknotes.py](mknotes.py) into `build/notes7.s`: a square-wave tone is a
(half-period delay, toggle count) pair on `$C030` (`note_play`).

### Feasibility verdict — OutLast: SHIPS (marginal, ~4 fps)

Built the cheapest honest variant per the handoff: a per-band raster (28
bands x 4px = the half-vertical-resolution road), each band grass-then-road
with a perspective half-width + a scrolling curve, speed stripes by band
parity, a steerable car, distance score and off-road detection.

**Measured:** one full road frame is ~75k instructions ≈ 0.24 s ≈ **~4 fps**
at 1 MHz — just *under* the ~5 fps bar, but each steer redraws immediately
on the keypress, so input stays responsive. The cost is dominated by the
full-width grass repaint every frame; a **dirty-band repaint** (only the
road span + stripe parity change per frame) is the identified path past
5 fps. Recorded honestly and shipped as a playable prototype rather than
faked or dropped. (`tests/m3_outlast.script`.)

### Feasibility verdict — scheduler: option 1 PROVEN, ships option 3

The 6502 has one 256-byte hardware stack at `$0100`.
[scheduler.i](scheduler.i) (a `-DSCHED_PROTO=1` boot path,
`./build.sh sched`) prototypes **option 1, stack partitioning**: two tasks
each own a slice of page 1, `task_yield` saves the live `S` and loads the
next task's, and a canary byte at each slice floor catches overflow. task0
drives the kernel's deepest typical chain (`draw_string`→`draw_char`).

**Measured:** 40 cooperative context switches complete with **both canaries
intact**; the render chain fits the 128-byte slice with room to spare. So
option 1 *works*. **Verdict:** the shipping kernel keeps **option 3** (the
M1/M2 poll-and-dispatch loop) — the apple2 app model is one full-screen app
at a time (the desktop windows are refreshed by the mainloop), so a live
task scheduler adds risk without benefit. The prototype is the evidence
option 1 would work if the model ever needed concurrent app tasks.
(`tests/m3_sched.script`.)

### M3 testing

`tests/m3.script` is the all-in-one smoke test (opens every app, drives a
key, screenshots each). Detailed per-app regressions:
`tests/m3_{theme,dostris,pacman,music,tracker,paint,outlast}.script` (the
two sound apps assert `beep>0`; Tracker and Paint assert a SONG.UNO /
PAINT.UNO save→clear→load round trip restores the work), plus
`tests/m3_sched.script` for the scheduler proto.

## Building

```sh
./build.sh        # build/unodos_apple2.dsk       (plain boot)
./build.sh test   # build/unodos_apple2_test.dsk  (AUTOTEST: opens SysInfo + Clock,
                  #                                 runs RWTS + FS self-tests)
./build.sh sched  # build/unodos_sched.dsk        (scheduler feasibility proto)
```

Needs `dasm` (6502 cross-assembler, `DASM` env var or
`C:\Users\arin\apple2-tools\dasm.exe`) and Python 3. `mkfont.py` converts
the shared font (`amiga/mkdata.py`'s output) to the hi-res 7px convention;
`mkdsk.py` packs `boot.bin` + `kernel.bin` into a 35-track DOS-order
140 KB `.dsk`, patching boot.s's `KTRACKS` placeholder to match the
kernel's size (currently 2 tracks); `mkfs.py` then formats the mini-FS
catalog (tracks 20-34) and seeds `disk/*.TXT`.

## Testing without real hardware

Real Apple II emulation needs Disk II nibble-level timing that's
overkill for regression screenshots, so this port ships its own
ROM-free harness: [harness.py](harness.py) is a `py65`-based 6502
emulator that plays the boot ROM's part (autoloads track 0 from the
`.dsk` image), serves Disk II nibbles for `boot.s`'s RWTS, emulates the
`$C000`/`$C010`/`$C030` keyboard/speaker soft-switches, and de-interleaves
hi-res page 1 to PNG screenshots (`pip install py65`):

```sh
./build.sh test
python3 harness.py build/unodos_apple2_test.dsk shots < tests/m1.script
```

[tests/m1.script](tests/m1.script) drives the M1 surface: boot to the
AUTOTEST desktop (SysInfo + Clock open), toggle icon selection, launch
the selected app, close it, and let the soft clock advance — 4
screenshots (`m1_boot`, `m1_select`, `m1_launch`, `m1_clock`).

[tests/m2.script](tests/m2.script) drives the M2 surface: boot (now
showing `RWTS PASS  FS PASS` and the Files icon), open Files (lists
`HELLO.TXT` 87 / `README.TXT` 361), open `HELLO.TXT` in Notepad, type
`hello` (status goes `Bytes:87` -> `Bytes:92`), Ctrl-S to save (asserts
the speaker beep counter > 0) — 5 screenshots (`m2_desktop`, `m2_files`,
`m2_notepad`, `m2_edited`, `m2_saved`). Run with `--writeback` to persist
the edit to a new `.dsk`:

```sh
python3 harness.py build/unodos_apple2_test.dsk shots \
  --writeback build/unodos_apple2_test_written.dsk < tests/m2.script
```

[tests/m2_persist.script](tests/m2_persist.script) then re-boots from
`build/unodos_apple2_test_written.dsk` and verifies the mutation survived
the power cycle: Files now lists `HELLO.TXT` at 92 bytes (delete-then-
append moved it to the end of the directory, after `README.TXT`), and
opening it in Notepad shows the edited content ending `...HELLO` with
`Ln:5 Col:6 Bytes:92` — 2 screenshots (`m2_persist_files`,
`m2_persist_notepad`):

```sh
python3 harness.py build/unodos_apple2_test_written.dsk shots \
  < tests/m2_persist.script
```

Two harness calibration constants matter for reproducibility:
- `TICK_INSTRS` (5000): 6502 instruction-steps per `wait` tick, tuned so
  `wait 60` clears AUTOTEST setup with the soft clock at ~2s.
- `KEY_INSTRS` (30000): instruction-steps a `key` press gets to run
  `handle_key` to completion — sized for the heaviest M1 redraw (opening
  a window, ~26000 steps) so a `shot` right after a `key` never catches a
  redraw mid-frame.

Verified in the harness (milestone 1): boot chain end-to-end (ROM
autoload -> RWTS -> `$4000`), hi-res desktop + menu bar, SysInfo (machine
ID via `$FBB3`/`$FBC0`, CPU, RAM) and Clock (soft-tick `HH:MM:SS`) windows,
arrow-key icon-selection highlighting, Return open/raise, ESC close, and
the soft clock advancing across `wait`s.

Verified in the harness (milestone 2): boot-time RWTS + FS self-tests
(`RWTS PASS  FS PASS`), Files listing the mini-FS catalog, opening a file
in Notepad, editing it, Ctrl-S writing the new content + updated catalog
back to the `.dsk` via the GCR write path (speaker beep on launch/save),
and — across a simulated power cycle (`--writeback` + re-boot) — the
edited content, new size, and reordered catalog entry all persisting
correctly.

## Real-hardware path (AppleWin -> FloppyEmu)

1. **AppleWin first.** Boot `build/unodos_apple2.dsk` in AppleWin and
   confirm the desktop renders and keyboard nav works before touching
   metal — AppleWin's cycle-honest Disk II emulation is what proves the
   RWTS.
2. **Autoload-count risk (first thing to check).** `boot.s` byte 0 = `$10`
   assumes a 16-sector P5A-ROM autoload of track 0. If AppleWin (or a
   clone ROM on real hardware) only honors a 1-sector autoload, `boot0`
   needs to replicate the ROM's read loop itself — see HANDOFF.md §3.1.
3. **FloppyEmu on real Apple II+ (or compatible) hardware**, once AppleWin
   passes.

## Milestones

- **M1**: boot chain (ROM autoload + GCR RWTS-read), hi-res
  desktop + menu bar, keyboard-driven window manager, SysInfo + Clock
  (soft tick), shared 7px font, ROM-free harness + `tests/m1.script`.
- **M2 (this)**: RWTS write path, a track/sector mini-FS ("USV1" catalog),
  Files + Notepad, speaker beeps, `tests/m2.script` +
  `tests/m2_persist.script`. Paddle/joystick pointer (optional per the
  handoff) not implemented. See [HANDOFF-M2.md](HANDOFF-M2.md).
- **M3 (this)**: the scaled app roster — Theme, Dostris, Pac-Man, Music
  and Tracker (blocking single-voice speaker), Paint (save/load
  PAINT.UNO), plus the two feasibility verdicts: OutLast ships marginal
  (~4 fps, measured), and the cooperative scheduler is proven feasible
  (stack-partitioning prototype) but the shipping kernel keeps
  poll-and-dispatch. 10-icon 3-row desktop. `tests/m3.script` +
  per-app + `tests/m3_sched.script`. See [HANDOFF-M3.md](HANDOFF-M3.md).
