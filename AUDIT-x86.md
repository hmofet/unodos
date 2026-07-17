# UnoDOS `x86` code audit — performance, bugs, security

Scope: the **x86 reference OS** — the self-booting 1.44 MB floppy / FAT16-HD build.
That is `boot/` (boot.asm, mbr.asm, vbr.asm, stage2.asm, stage2_hd.asm) plus the
`kernel/` real-mode kernel (kernel.asm, ~23,069 lines, + the three bitmap-font
`.asm` blobs). It boots on a real PC/XT-class machine, an emulator, and from
`build/unodos-144.img`.

**Threat model (honest).** This is 16-bit real/protected-mode 8086 code: **no NX,
no guard pages, no paging** — any out-of-bounds *write* corrupts memory directly and
the bootloader does raw segment/pointer math. But unlike the `pc64` port this world
has **no network stack, no USB, no JS interpreter, no TLS** — so the remote /
device-reachable attack surface those sections covered simply does not exist here.
The only untrusted inputs are:
1. **The keyboard** — bounded: a 16-byte ring, scancodes range-checked to `< 0x60`
   before table lookup.
2. **The filesystem** off the floppy/HD image — and the disk is *project-authored
   (trusted)*. Per the brief I still scrutinised every bound that is *taken from an
   on-disk structure* (cluster numbers, file sizes, dir-entry bytes), on the "what
   if the image were swapped/corrupt" basis. Those are the S/B items below, and they
   are correctly **LOW / latent** because the disk is trusted and, in every case I
   found, a bad on-disk value produces a handled disk error rather than a memory
   write.

The consequence: this port is **markedly more hardened and more optimised than
pc64**. It already has damage-rect redraw, a cached CGA row table, byte-aligned
`rep stos` fill/clear with hybrid masked-edge paths, a glyph fast path that is gated
on the cell being fully on-screen, an XOR rubber-band drag (no full-scene repaint),
and a FAT sector-pair cache. There is no "the whole screen repaints every mouse
move" headline the way pc64 had. So the Performance section below is genuinely
smaller, and Security/Correctness are dominated by latent/LOW items — that is the
accurate state of this code, not a gap in the review.

**Coverage note (kernel.asm is large — what I read vs deferred).**
Fully read: **all of `boot/`**; and in `kernel.asm`: the INT 0x80 dispatcher &
coordinate-translation front end (223-576), the keyboard driver + ring (577-960),
mouse/drag/cursor (960-1930, 4566-5510), the event queue (11445-11830), the heap
allocator (11445-11610), the **FAT12** driver — open/read/readdir/get_next_cluster
(12103-13820), **FAT16** read/delete chain (14720-14860, 15790-15886), the app
loader (16375-16600), settings load (2457-2566), the file dialog scan/draw
(5973-6630), the render hot paths — `gfx_fill_color`, `gfx_clear_area_stub`,
`draw_desktop_region`, `redraw_affected_windows` (11075-11200, 17843-18600),
`gfx_blit_rect` + the CGA `draw_char` fast path (8372-8877), `gfx_draw_string_stub`
(8925-9046), `win_create_stub` (17188-17400), the number/format utilities
(7551-7620), and the scancode + font layout. **Deferred** (skimmed or not
line-audited, lower input exposure): FAT12 write/create/delete/rename internals, the
mode-12h / VESA plotting helpers in detail, and the large body of in-kernel apps,
widgets and demos (paint flood-fill, tracker, the games, notepad edit buffer, combo/
menu widgets). Nothing was assembled/run — this is a static review.

Line numbers are exact against the current tree. Three areas, most-actionable first.

---

## 1. Performance

The expensive primitives (fill, clear, glyph raster, drag) are already optimised.
What remains are a few real but second-order costs.

### P1 — `gfx_blit_rect` has no *hybrid* CGA path, so any unaligned blit drops to per-pixel
`kernel/kernel.asm:8420-8473` (`gfx_blit_rect` `.blit_cga_check_done` → `.blit_slow_*`).
CGA (mode 0x04) is the **default boot video mode** (`video_mode: db 0x04`,
`kernel.asm:22525`). The blit has exactly two CGA speeds: a whole-byte fast path that
requires `src_x`, `dst_x` **and** width to all be 4-pixel aligned
(`.blit_cga_fast`, 8586), and otherwise a pure per-pixel loop that calls
`read_pixel_internal` **and** `plot_pixel_color` for every pixel (8461/8468) — two
full mode-dispatches + two `cga_pixel_calc` MULs per pixel. Window content sits at
`win_x + 1` (odd), so **content-area blits/scrolls are almost always unaligned** and
take the slow path. `gfx_fill_color` and `gfx_clear_area_stub` already solved this
with a masked lead/trail + `rep stos` middle "hybrid" (`.gfc_hybrid` 18136,
`.slow_path` 11169). **Fix:** give `gfx_blit_rect` the same hybrid treatment for the
CGA case (byte-copy the aligned middle, mask the 1-3 lead/trail pixels), so an
unaligned blit is ~byte-width work instead of ~4×-pixel work. Biggest real per-op win.

### P2 — Single-sector FAT12 fallback re-walks the chain one cluster at a time
`kernel/kernel.asm:13678-13755` (`fat12_read` `.single_sector`). The multi-sector
fast path (`.run_walk`/`.run_read`, 13594-13675) is good and handles the common
contiguous case with one INT 13h per track run. But whenever it falls back
(partial <512-byte tail, or a buffer within 512 B of a 64 KB DMA boundary) each
cluster is a separate `floppy_read_sector` + a `bpb_buffer` bounce copy + a
`get_next_cluster`. Minor because allocation is first-fit-ascending so files are
near-contiguous; still, a fragmented file pays one full round trip per 512 bytes.
**Fix:** allow the fast-path run accumulator to also cover the sub-512 tail via the
bounce buffer rather than dropping fully to per-cluster.

### P3 — FAT sector cache is a single pair; fragmented walks re-read from disk
`kernel/kernel.asm:13143` (`load_fat_pair`) + `12882` (`fat_cache_sector` compare in
`get_next_cluster`). Only one FAT sector-pair is cached, so a cluster chain that
crosses back and forth between two FAT sectors re-reads on every hop. Sequential
files hit the cache; fragmented ones thrash. **Fix:** a 2-3 entry FAT-sector cache
would remove the re-reads for the fragmented case. Low impact given trusted, mostly
contiguous images.

### P4 — `draw_desktop_region` re-rasterises every overlapping icon *label* per damage rect
`kernel/kernel.asm:17938-17988`. For each damage rectangle the routine re-runs the
full icon overlap scan and, for every overlapping icon, re-copies + re-draws the
label string through `gfx_draw_string_stub` (a per-glyph raster). During a drag the
XOR-outline path avoids this, but window close / z-order shuffles repaint the
affected band and redraw labels from scratch each time. **Fix:** cache the desktop
(background + icons + labels) into an off-screen CGA buffer once and blit the damaged
band from it, the same idea as pc64's P1. Lower priority here because damage is
already rect-limited, not full-screen.

*(Already good, do not "fix": byte-aligned `rep stosw` fill/clear with a single
hoisted MUL and interlace-bank toggling (`gfx_fill_color` 18096, `gfx_clear_area_stub`
11132); the masked-edge hybrid paths; the `cga_row_table` lookup (22715); the
`draw_char` CGA fast path gated by `dcf_check` so it only runs when the whole cell is
on-screen (8718); the XOR rubber-band drag that touches only the outline
(`mouse_process_drag` 5279+); event-driven `hlt` main loops with no polling spin;
and `cursor_dirty`-gated cursor resync (`mouse_cursor_sync` 4566).)*

---

## 2. Security — memory-corruption / reachability

No HIGH or MEDIUM item exists in this port's real threat model (no net/USB/JS; disk
trusted; keyboard bounded). Everything below is **LOW / latent**, kept because the
brief asked me to scrutinise FS-structure bounds and string handling as if the image
were hostile. None is a reachable write primitive under the trusted-disk model.

### S1 — LOW / latent — FAT12 starting cluster from the directory entry is never range-checked
`kernel/kernel.asm:12727` (`fat12_open` reads `[si+0x1A]` straight into the file
handle), consumed at `13546`/`13680` (`fat12_read`: `(cluster-2)*spc + data_area_start`)
and `12866-12870` (`get_next_cluster`: `cluster*3` for the FAT offset). The value is a
raw 16-bit word off disk with **no check that `2 ≤ cluster ≤ max_cluster`**. Two
distinct latent effects on a crafted/corrupt image:
- `cluster == 0 or 1` → `(cluster-2)` underflows to 0xFFFE/0xFFFF, `*spc + data_start`
  yields a wild LBA. It is **not** a memory write — it becomes a CHS conversion that
  produces a cylinder past the media, so INT 13h returns error and the read fails
  cleanly (`floppy_read_sector` ret/`stc`).
- `cluster > 0x5555` → `cluster*3` **overflows the 16-bit `add ax,bx`** at 12869, so
  the FAT byte offset wraps. The subsequent read still targets an in-bounds
  `fat_cache` slot (offset is masked by the `div 512`), so again no OOB access — just
  a wrong "next cluster", and the walk still terminates because `fat12_read` clamps
  the total to the (also on-disk) file size. **Fix:** after reading the start cluster
  and in `get_next_cluster`, reject `cluster < 2` or `cluster > (data_sectors/spc)+1`
  and treat as end-of-chain / error. Also compute the FAT offset in a way that can't
  wrap (the value is ≤ 0xFF6 for a *valid* FAT12, so this only bites a corrupt image).

### S2 — LOW / latent — `gfx_draw_string_stub` / `draw_char` read past the glyph table for bytes ≥ 127
`kernel/kernel.asm:8964-8971` (`sub al, 32` then `mul draw_font_bpc`, `add si,
draw_font_base`, `call draw_char`). The bitmap fonts only define **ASCII 32-126**
(font8x12.asm header: "Complete ASCII character set (32-126)"; `font_8x8` base at
22505). There is no clamp of the character code to `[32,126]`: a byte of 127 (DEL)
indexes the first glyph *past* the table, and bytes 128-255 index far past it
(e.g. 0xFF with the 8×12 font → `(255-32)*12 = 2676` bytes beyond a ~1140-byte glyph
table). `draw_char` then reads `bpc` bytes from there. This is a **read-only** OOB — it
draws garbage rows from adjacent font/kernel data, no write, no corruption — but it is
reachable when rendering *disk-supplied* text: a file whose 8.3 name contains high
bytes is converted and drawn by the file dialog (`fdlg_scan_files` 6389+ →
`widget_draw_listitem`), and any text-viewer content flows through the same string
API. **Fix:** in the draw loop, map any code `< 32` or `> 126` to a substitute glyph
(e.g. '?') before the `sub al,32`.

### S3 — LOW / latent — `gfx_blit_rect` fast paths do no destination bounds clamp
`kernel/kernel.asm:8505-8573` (VGA `.blit_vga`) and `8586-8680` (`.blit_cga_fast`).
Unlike `gfx_fill_color`/`gfx_clear_area_stub`, which clamp W/H to the screen edge
before writing, the blit fast paths compute `dst_y*320 + dst_x` (VGA) or the CGA row
base and `rep movsb` `width` bytes for `height` rows with **no clamp** against
`screen_width`/`screen_height`. width/height are byte-packed (≤255), and every write
lands inside the **video segment** (0xA000 for 0x13, the CGA segment for 0x04), so
this cannot corrupt kernel RAM — it can only scribble off-screen *within VRAM* if a
window is positioned so its content extends past the panel. Visual glitch, not a
memory-safety bug. **Fix:** add the same edge clamp the fill/clear paths use, for
correctness and to match their contract. (The per-pixel slow path is already safe —
`plot_pixel_color` bounds-checks every pixel.)

*Checked and clean:* the keyboard ISR (`int_09_handler` 626) — scancodes are gated
`cmp al,0x60 / jae .done` (672) before indexing the two **exactly 0x60-byte** tables
(`scancode_normal`/`scancode_shifted` 2746-2760), the 16-byte ring wraps with
`and bx,0x0F` and has a full-buffer guard (816-822); the event queue is 96 B = 32×3,
indices `≤ 31*3+1` in bounds, tail RMW is `cli`-guarded (`post_event` 11624); the
heap allocator validates the free pointer range and the `0xFFFF` used-marker, rejects
zero-size and catches the `add bx,7` size overflow, splits and full-sweep-coalesces
(`mem_alloc_stub` 11445, `mem_free_stub` 11543); the **app loader validates the
on-disk file size** (high word 0, `1 ≤ size ≤ 0xFFE0`) *and* verifies the actual bytes
read before executing (`app_load_stub` 16496-16517) — the one place a disk value
drives a bulk write into a segment, and it is correctly bounded; `load_settings` reads
exactly 6 bytes into a 6-byte buffer and validates the magic/font/mode (2485-2540);
`fat12_read`/`fat16_read` clamp the copy to `min(requested, file_size − pos)` so the
caller's own count bounds the write (13560, 14755); `fs_readdir_stub` bounds the entry
index `< 16` before dereferencing (12305); the file-dialog 8.3→dot conversion fits its
13-byte entry exactly (`FDLG_ENTRY_SIZE` 5982, `fdlg_scan_files` 6402-6435); the icon
`.label_buf` copy is capped at 10 chars + NUL into 11 bytes (17968-18014); the window
title copy is capped at 11 + NUL into a 16-byte pool slot (17341-17350); and the INT
0x80 dispatcher validates `AH < 106` before the `*2 + table` index (247-248, 493).

---

## 3. Correctness / robustness bugs

### B1 — LOW — `fat12_read` only supports reading from file position 0
`kernel/kernel.asm:13579-13580` (`cmp bp, 0 / jne .not_supported`). Any read when the
handle's current position is non-zero returns `FS_ERR_READ_ERROR` instead of
continuing the file. It is commented "for simplicity", but it is a real functional
limitation: a caller cannot read a FAT12 file in successive chunks (open, read N,
read again) — the second read fails. The FAT16 path (`fat16_read` 14720) *does* handle
arbitrary position. **Fix:** walk the FAT chain to the cluster covering `position`
before the read loop (FAT16 already shows the pattern), or at minimum document the
"whole-file single read" contract at the API boundary so callers size one buffer.

### B2 — LOW / latent — 16-bit overflow in the FAT12 offset arithmetic
`kernel/kernel.asm:12868-12870` (`shl ax,1 / add ax,bx / shr ax,1` = `cluster*3/2`).
For a *valid* FAT12 cluster (`≤ 0xFF6`) `cluster*3 ≤ 0x2FDE` — no overflow. But because
the input cluster is not validated (see S1) a crafted value `> 0x5555` overflows the
16-bit `add`, silently yielding the wrong FAT entry. Benign today (masked, in-bounds
`fat_cache` read, walk still terminates) but a latent footgun if the cluster ever
becomes attacker-influenced. **Fix:** validate the cluster (S1) and/or widen the
multiply.

### B3 — LOW / latent — boot loaders don't bound the kernel image size against RAM
`boot/stage2_hd.asm:243-297` (`.load_cluster` follows the FAT16 chain to EOC) and
`boot/stage2.asm:98` (floppy path loads a fixed `KERNEL_SECTORS`). The HD loader reads
`kernel_size` from the directory entry (`stage2_hd.asm:229-231`) **but never uses it**
— it loads cluster-by-cluster until end-of-chain, advancing `ES:DI` with a 64 KB
segment-wrap step and **no upper limit**. On the trusted image the kernel is a fixed
~100 KB and this is fine; a corrupt/oversized `KERNEL.BIN` FAT chain would write past
the intended `0x1000:0` region into arbitrary conventional memory before the signature
check at `0x1000:0` runs. Trusted-disk → LOW. **Fix:** cap the loaded length at a
known kernel-size ceiling (or validate `kernel_size` against it) and abort past that.

### B4 — LOW — `load_settings` FAT16 close path calls `fat12_close`
`kernel/kernel.asm:2509` and `2551` (both `.ls_close16*` branches `call fat12_close`,
not a FAT16 close). Harmless in practice — file handles are generic (`file_table`) and
`fat12_close` just validates the index and clears the status byte, which is the same
teardown a FAT16 handle needs — but the routing reads as a copy-paste slip and will
mislead the next reader / break if the two closes ever diverge. **Fix:** route to the
FAT16 close (or add a comment that close is FS-agnostic and intentionally shared).

### B5 — LOW / latent — no `cluster ≥ 2` / EOC lower-bound guard before `(cluster-2)*spc`
`kernel/kernel.asm:13683` (`fat12_read` `.single_sector`) and `14792`
(`fat16_read`). Both compute `(cluster-2)*sectors_per_cluster` with no check that the
cluster is a valid data cluster (`≥ 2`). Reachable only from a corrupt chain
(get_next_cluster normally returns `≥ 2` or trips EOC); underflow yields a wild LBA →
handled disk error, not a write. Folds into S1/B2. **Fix:** validate `cluster ≥ 2`
before the multiply.

*Checked and clean:* the MBR / VBR / stage2 32-bit-LBA-as-word-pair math and the
two-chained-16-bit-DIV CHS conversion (`mbr.asm:116-146`, `stage2_hd.asm:400-427`) —
exact for all 32-bit LBAs, with geometry re-queried from INT 13h/08h and clamped
(`and al,0x3F`, zero-checks) and a floppy-default fallback; the MBR relocation to
0x0600 that keeps its data clear of the 0x7C00 VBR load (`mbr.asm:31-40`); the boot
signature checks at each hand-off (`boot.asm:99`, `mbr.asm:158`, `vbr.asm:182`,
`stage2.asm:71`, `stage2_hd.asm:309`); the keyboard ring and event-queue index math
(§2); the heap split/coalesce (§2); `util_word_to_string` (≤5 digits + NUL, caller
buffer, 7555); `alloc_file_handle` / `close_task_files` iteration bounds (12794,
12823); and the window/z-order intersection tests in `redraw_affected_windows`
(18392-18468) which clip the content clear to the damage∩content rectangle so higher-z
windows aren't erased.

---

## Suggested order of work
1. **P1** (`gfx_blit_rect` CGA hybrid) — the one real per-operation speedup in the
   default CGA mode; reuses the fill/clear masked-edge pattern already in the tree.
2. **S2** (clamp draw-string char code) — one-line-ish, closes the only
   disk-reachable OOB (read-only) and stops garbage glyphs from odd filenames.
3. **S1 + B2 + B5** (validate FAT12 cluster once, in `get_next_cluster` / on open) —
   collapses the cluster-range latent items into one guard and removes the 16-bit
   overflow footgun.
4. **B1** (FAT12 non-zero-position read) — real functional gap vs the FAT16 path.
5. **S3, B3, B4, P2-P4** — hardening, boot-load bound, the `fat12_close` routing
   tidy-up, and the remaining second-order perf items.

Building/testing note: this world assembles with NASM to a raw 1.44 MB image
(`build/unodos-144.img`) and boots on PC/XT hardware or an emulator. Nothing here was
assembled or booted during the review — the findings are proposed from static
reading, not reproduced, and the coverage boundary in the header states exactly which
kernel regions were line-audited vs deferred.
