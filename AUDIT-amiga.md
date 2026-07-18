# UnoDOS `amiga` code audit — performance, bugs, security

Scope: the **amiga** world (bare-metal Motorola 68000, A500-class OCS/ECS, 512 KB
chip RAM). The bootable kernel is `amiga/kernel.asm` plus its includes
(`apps_m2.i`, `theme.i`, `games.i`, `pacman.i`, `tracker.i`, `fdd.i`, `fat12.i`,
`scheduler.i`, `paint.i`, `loader.i`, generated `gen_data.i`) and the separately
assembled disk-loaded apps (`*_app.asm`, built `-Fbin` at a fixed slot). Default
`build.sh` target (no `AUTOTEST`).

**Threat model.** This is freestanding 68000: **no MMU, no NX, no guard pages, no
supervisor/user split at runtime** — the kernel enters supervisor mode and owns the
machine. Every out-of-bounds write is a direct corruption primitive; a wild pointer
faults or silently trashes chip RAM (display, audio, code). Unlike the pc64 world
there is **no network stack, no USB, no font/JS/HTML parser** — those attack surfaces
don't exist here. The external / untrusted input is narrow and physical:

1. **The DF1 FAT12 floppy** (the "data disk"). It is user-supplied and, per the
   README, *"PC-interchangeable at the file level via mtools"* — so its BPB, FAT,
   directory entries, file bytes, **and the `.APP` binaries the kernel loads and
   executes** are externally authored, not bundled/trusted.
2. **Raw MFM off the physical DF1 media** (`fdd.i` decode path) — arbitrary/garbage
   track content.
3. Keyboard/mouse (local, trivial).

Everything else — the boot ADF kernel image, the ROM-disk fallback listing, the
bundled game tables, the extended palette — is trusted content shipped inside the
image. Note that **executing a loaded `.APP` is by design** (it's a code loader), so
"a hostile floppy runs code" is inherent, not a bug; the security section is about
memory corruption *beyond* the intended slot/buffer, reachable from a merely
oversized or malformed floppy as much as a malicious one.

Three areas, most-actionable first. Line numbers are exact against the current tree.
Because this is assembly, findings cite the routine label and line.

---

## 1. Performance

Unlike pc64, dragging is already cheap here: `handle_drag` (`kernel.asm:659`) tracks
the window with a self-erasing **XOR outline** (`xor_rect`, `kernel.asm:1558`) and
only commits on mouse-up, so a drag touches a 1px band, not the scene. The cost is
elsewhere: the **full-screen repaint** that fires on every z-order change, and the
byte-at-a-time fill/blit primitives underneath everything.

### P1 — `repaint_all` is an unconditional full-scene repaint, fired on every raise/close/create
`kernel.asm:1123-1134`. It runs `clear_screen` (wipes all 5 planes, 40000 bytes),
then `draw_desktop` (menu title + version + build strings + **all 11 icon cells**,
each an `fill_rect` + `draw_icon16` + `draw_string` — `kernel.asm:765-787`), then
**every open window's full chrome + content** bottom-up. It is invoked from
`raise_window` (`1080`), `close_window` (`1111`), `win_create` (`990`) and the drag
`finish` (`716`). So every click-to-raise repaints the entire desktop and every
window from scratch — there is no damage tracking and no cached background/scene.
**Fix (biggest single win):** cache the static desktop (background + icon grid) once
into an off-screen buffer and blit the damaged rows back, or at minimum only repaint
the windows whose stacking actually changed. `raise_window` already early-outs when
the target is already topmost (`1068`); extend that to "only the two windows that
swapped need repainting," and skip the desktop redraw entirely when no window moved.

### P2 — `fill_rect` interior is written one byte at a time
`kernel.asm:1525` (`fr5_plane` `.mid` loop: `move.b d6,(a1)+ / dbra`). `fill_rect`
is *the* workhorse primitive — window backgrounds, title bars, selection bars, icon
cells, status bars, every game tile, the notepad/paint canvas clears all go through
it — and its middle span stores 8 bits per iteration per plane. On 320-wide fills
across 5 planes this dominates redraw time. **Fix:** store `move.l` (or at least
`move.w`) for the aligned interior run and handle a 1–3 byte tail, a ~4× win on
every wide fill. The same pattern applies to `xor_span_raw`'s `.mid` (`kernel.asm:1631`,
`not.b (a2)+`) but that only runs on the thin drag outline so it is secondary.

### P3 — `draw_char` does a 5-plane read-modify-write per glyph row regardless of colour
`kernel.asm:1708-1745`. The plane loop runs all `NPLANES=5` planes for every row of
every character even when the fg/bg colour only sets bits in one or two planes (the
UI text colours are 0–3, i.e. ≤2 planes). Text is redrawn wholesale on every
`repaint_all` and on each per-second content refresh. **Fix:** skip planes where
neither `fg` nor `bg` contributes (test `btst d0,d3` / `btst d0,d4` and `bra .pnext`
before touching memory). Also `draw_string` (`kernel.asm:1751`) re-enters `draw_char`
per character with a full `movem.l d0-d7/a0-a4` save/restore each time — hoist the
save out of the per-char path.

### P4 — `clear_screen` wipes all 5 planes at the top of every `repaint_all`
`kernel.asm:1425-1436`. 40000 bytes cleared, then `draw_desktop` immediately repaints
the desktop area over most of it. It already uses `move.l`, so it is not the worst
offender, but it is pure redundant work that P1's background cache removes outright.
Standalone, restrict the clear to the union of dirty window rects.

### P5 — the main loop is a busy spin with a full 15-register context save per iteration
`kernel.asm:464-472`. `main_loop` calls `handle_clicks / handle_drag / handle_events
/ gm_tick / app_ticks / post_ticks / task_yield` back-to-back with no vblank wait, so
it burns 100 % CPU; `task_yield` (`scheduler.i:52`) pushes/pops `d0-d7/a0-a6` (15
longs) on **every** spin even when no task is ready to run. Functionally harmless
(the ISR paces the clock) but it wastes the machine and, on real hardware, power/heat.
**Fix:** wait for the vblank tick once per loop (compare `ticks`), so the loop runs
~50 Hz instead of unbounded.

*(Already good: the XOR-outline drag; the hardware-sprite mouse cursor — no software
cursor redraw; edge-only mouse events in the ISR — mouse *motion* posts nothing
(`kernel.asm:1815-1830`); Pac-Man's incremental dirty-tile rendering; the once-a-second
topmost-only content refresh, `app_ticks` `kernel.asm:740`.)*

---

## 2. Security

The reachable corruption surface is the DF1 floppy path. **S1 is the one to fix
first** — it is a real OOB write reachable from an ordinary (not even malicious)
oversized file.

### S1 — HIGH — `load_app` reads a disk file into an 8 KB slot using the file's own (untrusted) size as the budget
`amiga/loader.i:209-218`. The app's directory `size` is taken from `fat_find_file`
into `d7` (`209`) and passed **verbatim** as the read budget to `fat_read_file`
(`move.l d7,d1` `217` → `bsr fat_read_file` `218`), whose destination is the fixed
8 KB slot (`APPSLOT0 + slot*APPSLOT_SZ`, `APPSLOT_SZ = $2000`, `sysabi.i:105-106`).
There is **no clamp to `APPSLOT_SZ`**. `fat_read_file` honours the budget faithfully
(`fat12.i:202-215`), so a DF1 disk carrying a file named like an app — e.g.
`"PAINT   APP"` (`loader.i:40`) — whose directory size and cluster chain exceed 8 KB
will write the surplus **past the slot, over the adjacent window's slot and into the
bitplanes at `$60000`** and beyond. Every other consumer of `fat_read_file` caps the
budget to its buffer first — `notepad_open_fat` clamps to `NBUF-1` (`apps_m2.i:295-297`),
`notepad_open_file` likewise (`apps_m2.i:267-269`), the in-kernel `pt_load` passes the
exact `PT_W*PT_H` buffer size (`paint.i:1058`). `load_app` is the sole path that trusts
the on-disk length. **Fix:** clamp the budget to the slot before the read, and reject
overlong images, e.g.
```
    cmp.l   #APPSLOT_SZ,d7
    bhi     .fail            ; app image larger than its slot: refuse
```
(or `move.l #APPSLOT_SZ,d1` when `d7 > APPSLOT_SZ`). This also hardens the subsequent
`jsr APP_OPEN(a0)` (`227`) against jumping into a partially/garbage-loaded slot.

### S2 — MEDIUM — `fat_next_cluster` indexes a 1536-byte FAT buffer with a 12-bit cluster number
`amiga/fat12.i:96-103`. `offset = cl + cl/2` and the code reads `1(a0,d0.w)` /
`(a0,d0.w)` into `fat_buf`, which is only **1536 bytes** (`fat_buf: ds.b 1536`,
`kernel.asm:2366` — 3 FAT sectors, the mount clamps `fat_spf` to 3, `fat12.i:69-71`).
The cluster number comes from untrusted input — a directory entry's first-cluster
(`fat12.i:161-164`) and each FAT link followed by `fat_read_file`/`fat_write_data`.
A valid 880 KB volume tops out near cluster ~880 (`offset` ~1320, in bounds), but a
cluster value in ~1024…4095 makes `offset` reach ~6142, an **over-read of up to ~4.6 KB
past `fat_buf`**. It is read-only, but the garbage "next cluster" then drives an
arbitrary-LBA disk seek/read (`fat_read_file` `fat12.i:189-192`, and `fdd_seek` never
clamps the target cylinder, `fdd.i:134-141`). Reachable from a crafted or corrupt
FAT12 disk. **Fix:** in `fat_next_cluster` (and the `2 <= cl < maxclusters` guards in
`fat_read_file`/`fat_write_data`/`fat_free_chain`) reject `cl` beyond the FAT the
buffer actually holds — `if (offset+1) >= 1536 return end-of-chain`, or validate
`cl < (fat_bytes*2)/3`.

### S3 — LOW — `fat_mount` trusts BPB geometry with minimal validation
`amiga/fat12.i:44-66`. `fat_spc` (sectors/cluster) is read as a raw byte (`13(a0)`,
`fat12.i:44`) and used directly in the LBA multiply (`mulu fat_spc`, `fat12.i:191`,
`367`); reserved-sector count, root-entry count and FAT-copy count are taken as-is to
derive the region starts, with only the 512-byte-per-sector check (`fat12.i:38-40`)
and the `fat_spf ≤ 3` buffer clamp guarding anything. A malformed BPB (e.g. `spc = 0`,
or region starts that push clusters off the 1760-sector medium) yields wild LBAs into
`fdd_read_sector`/`fdd_write_sector`. All disk I/O is sector-granular into fixed 512 B
buffers so this is **not a direct memory-corruption primitive** (it collapses to bad
reads / mispositioned writes), which is why it is LOW, but it is defensive debt.
**Fix:** validate `fat_spc ∈ {1,2}`, `fat_rootents` sane, and
`fat_datastart + clusters*spc ≤ 1760` before marking the volume mounted.

*Genuinely no other untrusted-input corruption surface exists.* The MFM decoder is
clean (see below), and executing a loaded `.APP` is the loader's designed behaviour,
not a vulnerability — a floppy you boot with can already run code by construction.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `npstat` scratch line is 48 bytes but the ABI documents it as 64
`kernel.asm:2358` allocates `npstat: ds.b 48`; `sysabi.i:74` publishes it to every
disk-loaded app as *"shared 64-byte scratch line"* (`KD_npstat`). A disk app built
against the ABI comment that writes up to 64 bytes overruns `npstat` by 16 bytes into
`dt_board` (`kernel.asm:2359`, the Dostris board). No in-tree app does today, but the
two numbers must not disagree in a published contract. **Fix:** size it `ds.b 64` (or
correct the comment to 48 and keep them in lockstep — the former is safer).

### B2 — LOW — `divu` overflow garbles uptime/clock after long sessions
`kernel.asm:1319-1320` (SysInfo) and `1341-1342` (Clock): `move.l ticks(pc),d0 /
divu #TICKS_SEC,d0`. `divu` is a 32÷16→16-bit quotient; once uptime exceeds 65535
seconds the quotient overflows (V set, result undefined) and the seconds/clock
display corrupts. At `TICKS_SEC=50` that is ~18 h of real vblanks, and the README
notes WinUAE paces this config ~4× fast, so it arrives sooner. Cosmetic (display
only), hence LOW. **Fix:** stage the divide (divide `ticks` by e.g. 50 in two 16-bit
steps) or hold seconds in a separate counter incremented in the vblank ISR.

### B3 — LOW / latent — per-task 2 KB stacks have no guard
`scheduler.i:14-15` (`TASKSTK=$70000`, `TSTK_SZ=2048`) and `task_spawn`
(`scheduler.i:75-95`) hand each app task a fixed 2 KB stack with no canary or guard
page (there can be none — no MMU). A disk app that recurses deeply or holds a large
local frame overflows into the neighbouring task's stack, silently corrupting another
window's context. Trusted app code today, so latent. The slot geometry itself is
sound: `MAXWIN=6` (`sysabi_gen.i:14`) → slots span `$50000…$5C000`, clear of the
`$60000` bitplanes, and the 7 task stacks span `$70000…$73800`, clear of the copper
list at `$76000`. **Fix:** bound app call depth (the flood-fill already caps its
explicit stack, `paint.i:366,380`); optionally stamp a canary at each stack base and
check it in debug builds.

*Checked and clean:* the MFM decoder `fdd_decode_track` (`fdd.i:183-246`) bounds every
`TRKCACHE` write by the header sector number (`cmp.w #FDD_SPT,d5 / bge .scan`,
`fdd.i:215-216`) and keeps its raw-buffer scan below `RAWBUF+RAWWORDS*2-1200`
(`fdd.i:187`); `fat_read_file`/`fat_write_data` honour the caller's byte budget
(`fat12.i:202-215`, `370-380`); `fat_list_root` caps at `FAT_MAXFILES=16` fixed 18-byte
entries (`fat12.i:138-140`); `fat_alloc_chain` scans within the 872-cluster ceiling so
`fat_set_entry` stays inside `fat_buf` (`fat12.i:295`); the Paint flood fill bounds
both the pixel coords (`cmp #PT_W`/`#PT_H`) and the explicit work stack (`cmp #PT_STKN,d6`,
`paint.i:366,380`); `notepad` insert/backspace shifts are `np_len`/`NBUF`-bounded
(`apps_m2.i:681-707`); Tracker pattern indexing is masked to `TK_ROWS`/`TK_CHANS`
(`tracker.i:150,169,384,395`); `fmt_dec`/`fmt_u16`/`put2dig` build into 16-byte
`numbuf`/`clkbuf` within range (word-limited inputs); the event queue is a power-of-two
ring with drop-when-full (`ev_post` `kernel.asm:1897-1912`); and `find_window_at`'s
"fall through with the hit's compare flags" trick (`kernel.asm:1058`) does mirror the
result correctly (hit → `d2 ≥ 0` → `N=0`; miss → `d2 = -1` → `N=1`).

---

## Suggested order of work
1. **S1** (`load_app` slot-budget clamp) — one comparison, closes the only reachable
   OOB write; also hardens the `APP_OPEN` jump.
2. **P1 + P2** — cache the desktop / damage-rect `repaint_all`, and widen the
   `fill_rect` interior store. Together these are the visible-redraw win.
3. **S2, S3** — bound the FAT cluster index and validate the BPB (crafted/corrupt
   floppy hardening).
4. **P3 + P4 + P5** — plane-skip in `draw_char`, restrict `clear_screen`, pace the
   main loop to vblank.
5. **B1–B3** — align the `npstat` size to the ABI, stage the uptime divide, add a
   task-stack canary in debug builds.

Building/testing note: the amiga target assembles with `vasmm68k_mot` + `exe2adf` and
runs under WinUAE with the AROS ROM (`amiga/build.sh`, `uae/*.uae`). That toolchain is
not installed on this machine, so none of the above was assembled or run here — the
fixes are proposed, not applied.
