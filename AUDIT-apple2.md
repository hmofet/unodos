# UnoDOS `apple2` code audit — performance, bugs, security

Scope: the **apple2** world — the 6502 Apple ][+ / //e port (`apple2/`), the
default `./build.sh` target (boot0+RWTS `boot.s`, slim kernel `kernel.s`, and the
disk-loaded apps `theme/dostris/pacman/music/tracker/paint/outlast/files_notepad`,
included via `rwts.i`/`fs.i`/`sys.inc`/`*.i`). Threat model for the security
section: a 1 MHz 6502 with **no memory protection, no MMU, a single 256-byte
hardware stack, and a flat address space** — every out-of-bounds store is a direct
corruption primitive that can hit zero page, the stack, the kernel ($4000), the
hi-res framebuffer ($2000), or the I/O soft-switches. The **only untrusted input
surface is the mounted disk image** (`.dsk`/`.woz`/`.nib` via RWTS): sector GCR
payloads and, above all, the **"USV1" mini-FS catalog** (`fs.i`), whose fields
(`file count`, `next-free`, per-entry `size`/`start`) are read straight off the
media and, per the port's own note, *"not re-validated at boot"*. The port's
stated assumption is *trusted FloppyEmu/AppleWin media* — but a FloppyEmu OS is
distributed as a disk image, so "someone hands you a crafted disk" is the realistic
attack, and that is what section 2 scrutinises. There is **no network, no USB, no
untrusted TTF** here (unlike pc64); the games take no external input at all.

Three areas, most-actionable first. Line numbers are exact against the current tree.

---

## 1. Performance — why every keystroke is a full-screen repaint

### The root cause
Every full-screen app redraws the **entire content area from scratch on every
keystroke**. The pattern (identical in `files_draw`, `notepad_draw`, `theme_draw`,
`tk_draw`, `paint_draw`, and Dostris' `dostris_draw_all`) is: repaint the title
row, repaint the separator, **`fill_rows` the whole rows-1..23 content band**
(`SCRCOLS`=40 columns × `APP_CONTENT_H+8`=184 pixel rows ≈ **7360 byte stores**),
then re-emit every element on top. At 1 MHz that clear alone dominates a frame, and
it runs again for every arrow press and every typed character. There is no
damage/dirty-rect tracking except in the two games that need it (Dostris
piece move, Pac-Man `pm_refresh`).

### Fixes, highest impact first

**P1 — Stop clearing + redrawing the whole content band on every key (biggest win).**
The full-band clear appears at `notepad.i:227-238`, `files.i:203-214`,
`theme.i:164-175`, `tracker.i:161-172`, `paint.i:416-427`, and
`dostris.i:695-706`, each immediately followed by a full re-emit of the list/grid.
Only a few rows change per key. Follow the model the games already use: Dostris'
`dk_left`/`dk_right`/`dk_rotate` (`dostris.i:591-641`) erase+redraw only the moving
piece via `dostris_erase_piece`/`dostris_draw_piece`, and Pac-Man's `pm_refresh`
(`pacman.i:1085-1103`) repaints only the tiles under the old actor positions.
Notepad/Files/Theme/Tracker should redraw only the changed line(s) (and move the
selection highlight by erasing the old row + inverting the new one) instead of
clearing 7360 bytes each time.

**P2 — `notepad_draw` scans the whole buffer twice per keystroke.**
`notepad.i:258-289` (pass 1: count total lines + cursor column) and
`notepad.i:324-369` (pass 2: render) each walk **all `note_len` bytes** (up to
`NOTE_MAXLEN`=2048) every redraw — ~4096 loop iterations per typed character, even
though only the visible tail (22 rows) is drawn. Pass 1 exists only to compute the
line total and the tail-scroll skip; maintain a running line count / cursor
line+col incrementally on insert/backspace (both already touch `note_len` at
`notepad.i:112-115` / `notepad.i:124-136`) and cap the pass-2 scan to the visible
window. Removes the per-key O(buffer) cost.

**P3 — `draw_char` recomputes the glyph pointer and both row-base pointers per
character.** `kernel.s:1049` `draw_char` derives the font-cell pointer
(`kernel.s:1050-1069`) *and* rebuilds `zpRowLoPtr`/`zpRowHiPtr` from a shift-
multiply plus four 16-bit adds (`kernel.s:1071-1089`) for **every glyph**, then its
8-row inner loop re-fetches each scanline base through indirect-indexed loads
(`kernel.s:1096-1112`). `draw_string` (`kernel.s:1121-1133`) calls this per
character, so all text in all apps pays it. Since a string sits on one `zpRow`,
hoist the row-base pointer computation out of the per-char path (compute once per
`draw_string`, step the column), and index the glyph directly. This is the shared
text hot path.

**P4 — `fill_rows` recomputes the destination address per row and stores one byte
at a time.** `kernel.s:916-944`: per row it looks up `rowlo,y`/`rowhi,y`, adds
`zpFX` with carry, then the inner `fr_col` loop writes the pattern byte-by-byte
against a `cpy zpFW` compare. For the big full-band clears (P1) this is
O(rows×cols) with per-row 16-bit address math. Even keeping P1's clears, an
unrolled / word-strided fill would cut the constant; but P1 (not clearing at all)
is the larger win.

**P5 — OutLast repaints the full-width grass for all 28 bands every frame (~4 fps,
measured).** `outlast.i:78` `ol_draw_band` fills the entire `SCRCOLS`-wide grass
row (`outlast.i:97-106`) before overdrawing the black road span and edges, for each
of the 28 bands, every `ol_tick`/steer. The README already identifies the fix
(dirty-band repaint: only the road span + parity that changed). Recorded here as
the measured hot path — `outlast.i:205` `ol_draw` is the frame.

**P6 — Byte-at-a-time full-page clears.** `hgr_clear` (`kernel.s:896-912`) and the
board clears (`dostris.i:441-452` `dng_clr`, `paint.i:463-480` `pt_clearbuf`) store
one byte per iteration. Low frequency (boot / new-game), so minor, but the same
word-strided-fill remark as P4 applies.

*(Already good: TTF-style glyph re-rasterisation is a non-issue — the font is a
fixed 8-byte-per-glyph bitmap table `font7`; Dostris piece moves and Pac-Man steady
state are already dirty-rect; the soft clock only repaints the clock content, not
the desktop, `kernel.s:99-105`.)*

---

## 2. Security — corruption reachable from a crafted disk image

Ordered by reachability × impact. **S1 is the one to fix first.** All three come
from the same omission: the USV1 catalog (`fs_init`, `fs.i:59-67`) is loaded and
trusted verbatim — the `"USV1"` magic is never checked and no field is bounded.

### S1 — HIGH — `fs_read` writes a disk-controlled number of sectors into a fixed buffer
`fs.i:219-230` (`fs_read`) → `fs.i:118-127` (`fs_size_to_sectors`) →
`fs.i:145-167` (`fs_read_sectors`). The sector count comes only from the directory
entry's **on-disk `size` field** (`fs_size`, `fs.i:97-107`, bytes 12-13 of the
entry): `sectors = ceil(size/256)`, up to 255. `fs_read_sectors` then loops that
many times, doing `inc zpFSDat+1` (`fs.i:162`) after each 256-byte `rwts_read`
with **no bound on the destination**. Every caller reads into a small fixed buffer:
- `notepad_load` → `NOTEBUF` = 2048 B / 8 sectors (`notepad.i:29-34`),
- `pt_load` → `PAINTBUF` = 1088 B (`paint.i:696-701`),
- `tk_load` → `TKPAT` = 256 B / 1 sector (`tracker.i:445-450`).

A crafted catalog that declares, say, `size = $8000` for `HELLO.TXT` makes the user
opening it in Notepad trigger **128 sectors (32 KB) written from `$9500` upward** —
straight through `DOSBOARD`/`PMMAZE`/`TKPAT`/`PAINTBUF`, and because `zpFSDat+1`
wraps `$FF→$00` it continues over zero page, the `$0100` stack, the input buffers,
the kernel at `$4000`, and the framebuffer at `$2000`. A **fully disk-triggerable,
whole-address-space OOB write**, reachable by the normal "open a file" flow. The
same primitive exists via `pt_load`/`tk_load` on crafted `PAINT.UNO`/`SONG.UNO`.
Note also `notepad_load` copies `zpFSSize` straight into `note_len`
(`notepad.i:35-38`) with no clamp, so subsequent editing then indexes past the
buffer too.
**Fix:** give `fs_read` a caller-supplied max-sectors (or max-bytes) cap and clamp
`zpFSCnt`/`zpFSSize` to it before the loop; independently, `fs_init` should reject a
catalog whose entries declare `size`/`start`+len beyond `FS_SECTORS`.

### S2 — MEDIUM — Unvalidated catalog `file count` drives draw loops into out-of-table rows
`CATBUF+FSC_COUNT` (byte 4 of the catalog sector, disk-controlled, meant to be
0..15) is used directly as a loop bound in `fs_find` (`fs.i:197`),
`files_next`/`files_prev` (`files.i:92`, `files.i:109`), `files_draw`'s row loop
(`files.i:235`), and `fs_delete`'s fixups (`fs.i:292`, `fs.i:313`). Directory
*accesses* stay in-page (`fs_entry_ptr`, `fs.i:85-95`, keeps the high byte fixed at
`>CATBUF` and wraps the low byte), so the catalog reads themselves don't leave the
`$9400` page. The reachable corruption is in **rendering**: with `count ≥ 24`,
`files_draw` draws list rows at char-row `x+1 ≥ 24` (`files.i:239-285`), so both the
selection-highlight `fill_rows` (`files.i:252-268`, `zpFY = zpRow*8`) and
`draw_char` (`zpRow*8`, `kernel.s:1071-1089`) index **past the 192-entry
`rowlo`/`rowhi` tables** (`kernel.s:1542-1567`) into adjacent code/data, yielding a
bogus `zpDst` and an **OOB store** of the fill pattern / glyph byte. The write
target is table-derived rather than cleanly attacker-chosen, but it is a genuine
disk-triggered OOB write. **Fix:** clamp `CATBUF+FSC_COUNT` to `FS_MAXFILES` (15) in
`fs_init` right after the catalog read (and likewise sanity-check `FSC_NEXTFREE`),
so no consumer ever sees a bogus count.

### S3 — LOW — `fs_delete`/`fs_save` iterate over disk-controlled `next-free`/`start`
`fs_delete`'s heap-shift loop bounds on `CATBUF+FSC_NEXTFREE` (`fs.i:256-280`,
compare at `fs.i:258`) and computes physical sectors from the entry `start` +
counts via `fs_sector_addr` (`fs.i:129-143`), which applies **no clamp** — a rel
sector > 239 maps to `track > 34` (nonexistent). A crafted catalog therefore makes
`d`-delete in Files (`files.i:143-159` → `fs_delete`) spin reading/writing many
sectors and stepping the head to nonexistent tracks. This is **not a RAM overflow**
(every transfer is exactly 256 bytes into the fixed `SECBUF` page, `fs.i:262-266`),
so it is bounded to disk misbehaviour / hang / possible on-disk corruption of the FS
region. **Fix:** validate `FSC_NEXTFREE ≤ FS_SECTORS` and each entry's
`start + ceil(size/256) ≤ FSC_NEXTFREE` at load (subsumed by the S1/S2 catalog
validation).

*Checked and clean:* the **GCR RWTS** read/write paths (`boot.s` `readsec`,
`rwts.i` `rwts_read`/`rwts_write`) always transfer **exactly 256 bytes** into the
caller's page — the media supplies sector *content*, never a length that sizes a
copy — and the 6-and-2 decode indexes `DECTAB`/`WRTAB` over their full 256/64-entry
range, so a garbage nibble cannot index out of the table; checksums are
deliberately unverified (documented, trusted-media assumption, not a memory bug).
The `RDBUF2` "twos" buffer is a fixed 86 iterations and `(zpBuf),y` fills exactly
one page. Notepad's own editing is safe: `notepad_insert` caps `note_len` at
`NOTE_MAXLEN`=2048 = the `NOTEBUF` size before storing (`notepad.i:88-99`), and
`notepad_backspace` guards empty (`notepad.i:124-136`). Name compares/copies are
bounded to 12 bytes against ≥12-byte name buffers (`fs_find` `fs.i:203-209`,
`fs_save` `fsv_nm` `fs.i:374-379`, `notepad_load` `nl_name` `notepad.i:22-27`). The
games take **no untrusted input** and bounds-check their own board indices in-loop:
Dostris `dostris_fits` tests `x<10`/`y<20` before `DOSBOARD,x` (`dostris.i:157-179`),
Pac-Man `pm_step_walkable` tests `<13` (and catches the `$FF` underflow) before
`PMMAZE,x` (`pacman.i:102-128`) and `pm_clamp_target` clamps (`pacman.i:571-594`),
Paint clamps the keyboard cursor to `0..PT_CW/PT_CH-1` (`paint.i:519-552`) and the
flood-fill stack is capped at 256 with a full-stack drop (`pf_push`,
`paint.i:315-331`). The app loader reads a **build-generated** (trusted)
`app_secs`/`app_track` table (`kernel.s:238-243`, `build/app_layout.inc`).

---

## 3. Correctness / robustness bugs

### B1 — LOW — `fs_size_to_sectors` overflows for ≥ 65281-byte sizes (and is unclamped)
`fs.i:118-127`. `A = size_hi (+1 if size_lo≠0)`. For `size = $FFFF` this is
`255+1 = 256`, which wraps the 8-bit `A` to **0 sectors** — a very large declared
file reads *nothing* rather than its data (silent truncation). It is also the root
of S1: no clamp against the destination buffer. Not reachable with a well-formed
image (files never exceed the FS), but it is the exact spot the S1 cap belongs.
**Fix:** clamp to a caller max and treat overflow as "too big / reject".

### B2 — LOW — `fs_selftest` reads a 2-sector file into a 1-page buffer (AUTOTEST only)
`kernel.s:1345-1351`. Step 1 `fs_read`s the seeded `README.TXT` (361 bytes → 2
sectors) into `SECBUF` (`$9200`, one page); `fs_read_sectors`' `inc zpFSDat+1` puts
the 2nd sector into `SECBUF2` (`$9300`). Benign — both are scratch pages,
`rwts_selftest` (which uses `SECBUF2`) has already run by then, and nothing reads
the spilled bytes — but it is a real over-write relative to `SECBUF`'s documented
256 bytes, and a latent example of the S1 "caller must pre-know the size" footgun.
Only in the `-DAUTOTEST` build. **Fix:** read into a 2-page buffer, or read only
sector 0 for the magic-byte check it actually performs.

### B3 — LOW — `notepad_draw` help-fit test does `adc #19` without `clc`
`notepad.i:433-438` (`nd_help`): `lda zpCol` / `adc #19` / `cmp #41` decides whether
the help string fits, but there is no `clc` before the `adc`, and the carry left by
the preceding `draw_dec16` (`notepad.i:412-416`) is indeterminate — so the fit test
is off by one depending on the byte count just printed. Cosmetic only (it merely
gates whether the help line is drawn). **Fix:** `clc` before `adc #19`.

### B4 — LOW (latent) — `load_app` has no cap on `app_secs` vs the `$6000..$9000` app window
`kernel.s:197-226`. `la_loop` reads `app_secs` sectors into `APP_BASE`=`$6000`,
`inc zpBuf+1` per sector, with no check against `KBSS`=`$9000` (48 pages). `app_secs`
is build-generated so this is trusted today, but an app binary grown past 48 sectors
would silently overwrite the `$9000+` kernel BSS (`DECTAB`/`CATBUF`/…). **Fix:** a
build-time assert (or a runtime cap) that `app_secs ≤ (KBSS-APP_BASE)/256`.

*Checked and clean:* the LCG RNGs `dos_rand7`/`pm_rand` use `cmp`-then-`sbc` where
the `cmp` correctly sets carry for the subtract (`dostris.i:396-411`,
`pacman.i:294-303`); the Tracker/Dostris/Paint cursor-wrap paths set `sec` before
`sbc` (`tracker.i:532-540`, `dostris.i` movement, `paint.i`); `fmt2`/`clock_format`
and `draw_dec16` repeated-subtraction division are bounded and digit-correct
(`kernel.s:611-702`, `kernel.s:1176-1229`); `tk_cell_x`/`dos_board_idx`/
`pm_calc_idx` index math stays within their fixed board arrays for in-range inputs;
`fs_delete`'s directory-shift and `zpFSEnt` aliasing are correct for a well-formed
count (the aliasing hazard is documented at `fs.i:39-44`); the hi-res `rowlo`/`rowhi`
tables and the fixed-layout `fill_rows`/`frame_rect`/`dither_rect` rectangles never
exceed pixel row 191 for the built-in windows/icons/apps (only the S2 disk-count
path pushes a row index out of range).

---

## Suggested order of work
1. **S1** (`fs_read` destination cap) — closes the whole-address-space OOB write
   that the normal open-a-file flow triggers on a crafted disk; **B1** is the same
   fix site.
2. **S2** (clamp `FSC_COUNT`/`FSC_NEXTFREE` in `fs_init`) — one clamp closes the
   render-path OOB store and de-fangs S3.
3. **P1 + P2** — stop the per-keystroke full-content clear and Notepad's double
   full-buffer scan (the interactive-lag win).
4. **P3 + P4** — the shared `draw_char`/`fill_rows` hot paths.
5. **S3, P5, B2–B4** — disk-loop hardening, OutLast dirty-band repaint, and the
   latent/cosmetic fixes.

Building/testing note: the apple2 target builds with `dasm` (6502 cross-assembler,
`C:\Users\arin\apple2-tools\dasm.exe`) and runs under the repo's `py65`-based
`harness.py` (ROM-free), with AppleWin as the timing oracle before metal. `dasm`
isn't invoked as part of this audit, so **none of the above was assembled or run
here — the findings are from source reading and the fixes are proposed, not
applied.**
