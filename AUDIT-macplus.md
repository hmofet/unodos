# UnoDOS `macplus` code audit — performance, bugs, security

Scope: the **macplus** world — a standalone bare-metal OS for the compact
68000 Macs (Plus/SE/Classic, 512x342x1) and the Mac II class (640x480x1), the
default `./build.sh` target (`build/unodos_macplus.dsk`). Covers the boot
blocks (`boot.asm`), the kernel (`kernel.asm`) and its kernel-side includes
(`sony.i`, `fat12.i`, `snd.i`, `appsvc.i`, `diskapp.i`, `scheduler.i`), and the
disk-loaded apps (`files.app.asm`, `paint.app.asm`, `tracker.app.asm`, …).

Threat model for the security section: freestanding 68000, flat physical RAM,
**no MMU / no NX / no guard pages** — every out-of-bounds *write* is a direct
corruption primitive and even OOB *reads* can address-error the machine. But
this port has **no network stack, no USB, no removable media path other than
the boot disk itself**. The *only* external input past the trusted ROM is the
**boot floppy / disk image**: the FAT12 volume (bundled `README.TXT`/`HELLO.TXT`,
the bundled `*.APP` binaries) and user-saved files (`PAINT.UNO`, `SONG.UNO`,
edited text). That disk is simultaneously the code trust root — the boot blocks
`_Read` the kernel from it and the loader `_Read`s and *executes* `.APP`
binaries from it — so loading and running disk code is by-design trusted, not an
attack surface. The security section is therefore scoped to the one realistic
adversary: a **maliciously crafted disk image** (e.g. swapped in via a FloppyEmu)
fed to the FAT12 parser. There are no HIGH/remote findings here; the section
lists only latent/defensive items, and they are marked as such.

Three areas, most-actionable first. Line numbers are exact against the current
tree (assembly findings cite the label plus line).

---

## 1. Performance — the redraw model

### The root cause
Unlike the pc64 world, the drag path here is already damage-minimal: a
title-bar drag only moves an **XOR rubber-band outline** (`handle_drag`
`.redraw`, `kernel.asm:601-611`, via `xor_rect`/`xor_span`), so per-frame drag
cost is a few dozen byte-EORs. That part is *good* and needs no work.

The cost is the **universal full-scene repaint**. `repaint_all`
(`kernel.asm:1021`) unconditionally `clear_screen`s the whole framebuffer, then
`draw_desktop` (menu bar + footer + **all 12 icon cells**), then `draw_window`
for **every** open window. It is invoked on:

| Trigger | Site |
|---|---|
| every window **raise / focus click** | `raise_window` `kernel.asm:978` |
| every window **close** | `close_window` `kernel.asm:1009` |
| every 2nd-or-later window **open** | `win_create` `kernel.asm:889` |
| every **drag finish** (mouse-up after a move) | `handle_drag .finish` `kernel.asm:623` |

So merely clicking a background window to focus it repaints the entire desktop
and all windows — an O(screen + all-windows + 12 icons) operation for what is
only a z-order change.

### Fixes, highest impact first

**P1 — Make `raise`/focus incremental (biggest single win).**
A raise changes only two things visually: the newly-topmost window must be
redrawn on top, and the previously-topmost window's active-only title pinstripes
(`draw_window .stripe`, `kernel.asm:1125-1137`) must be cleared. The desktop,
the icons, and every non-adjacent window are unchanged. Replace the
`repaint_all` in `raise_window` (`kernel.asm:978`) with: redraw the old topmost
(now inactive) and the new topmost (now active) via `draw_window`, in z-order.
This removes the full `clear_screen` + `draw_desktop` from the most common
interaction.

**P2 — Stop repainting the 12 static icon cells on every `repaint_all`.**
`draw_desktop` (`kernel.asm:697-703`) re-blits every icon bitmap (`draw_icon16`)
and re-draws every label (`draw_string`) each call, even though icon art and
labels never change — only the selection outline moves (`select_icon` already
redraws just the two affected cells). Either cache the rendered desktop once and
`memcpy` it back, or split "repaint windows over the existing desktop" from
"repaint the desktop." Every `repaint_all` today pays the icon cost needlessly.

**P3 — Word/long-fill the middle span of `fill_rect`.**
`fill_rect` writes the full-byte interior of each row **one byte per iteration**
(`kernel.asm:1491` `.mid: move.b d6,(a0)+`). Window backgrounds, title bars,
content clears and the fault screen all go through this; a 300-px-wide row is
~37 byte writes. `clear_screen` (`kernel.asm:1414-1420`) already shows the
long-write idiom — apply the same 4-byte-run + byte-tail here (the pattern byte
is uniform per row, so a long is just the byte replicated). This speeds up every
window repaint and is independent of P1/P2.

**P4 — Idle loop busy-spins.**
When nothing has changed the main loop tight-loops (`kernel.asm:350`
`bra main_loop`) with no `stop`, holding the 68000 at 100% (power/heat on a
battery-less desktop, but still wasteful and unlike the pc64 idle delay). A
`stop #$2000` in the idle branch would sleep until the next VBL/keyboard/mouse
interrupt with zero added latency (the 60 Hz tick wakes it). Minor.

*(Already good, no work needed: the rubber-band drag is outline-only — the
exact optimization the pc64 audit had to *add* as its P2 is already present
here; the cursor is a 3-byte×14-row save-under, negligible; the once-per-second
content refresh (`app_ticks`, `kernel.asm:644`) touches only the topmost window;
`clear_screen` is already long-at-a-time.)*

---

## 2. Security — memory corruption reachable from a crafted disk

No remote or device-reachable surface exists (no net/USB/ADB-data path into
memory writes). Under the disk-only threat model above, both items are
**latent** and read-only; neither is a write primitive.

### S1 — LOW / latent — `fat_next_cluster` over-reads `fat_buf` on a corrupt FAT chain
`fat12.i:97` (`fat_next_cluster`), deref at `fat12.i:105-107`. The 12-bit FAT
entry offset is `offset = cluster + cluster/2` (1.5·cluster), and `fat_buf` is
only **`$600` = 1536 bytes** (three sectors — `kernel.asm:46`, and `fat_mount`
hard-clamps the FAT to 3 sectors at `fat12.i:72-75`). The chain-follow loops
(`fat_read_file` `fat12.i:186-229`, `fat_free_chain`, `load_app_slot`) only stop
a chain at `cluster >= $0FF8` or `< 2`, so a crafted chain entry in
`1024 … 4087` yields `offset` in `1536 … 6130` — up to ~4.6 KB past `fat_buf`.
The read stays **inside the KBSS arena** (`fat_buf`+6130 ≈ `KBSS+$2213`, well
under `KBSS_END = KBSS+$AC00`, `kernel.asm:55`), so it does not fault; it merely
interprets adjacent kernel BSS (`FATSECBUF`, `sony_pb`, game boards) as a
cluster number. Read-only and self-limiting: the bogus cluster becomes a bogus
LBA, which the `.Sony` `_Read` rejects, so `fat_read_file` returns `-1`. Reachable
only from a maliciously crafted disk image.
**Fix:** before indexing, bound `cluster < (fat_buf_bytes*2)/3` (i.e. `< 1024`
for the current buffer) and treat an out-of-range link as end-of-chain; or size
`fat_buf` for the full 12-bit FAT if larger media is ever supported.

### S2 — none beyond S1, justified
Everything else the disk feeds is size-clamped before it reaches memory — see
the *Checked and clean* list (Notepad/Paint/Tracker loads, the `.APP` slot
loader, the keyboard table index, the ROM event pool). Executing a `.APP` by
absolute vector with no bound on the vector target (`app_disp_draw`
`diskapp.i:184-185` `jsr (a0)`) is **by design**: the same disk supplies the
kernel, so an attacker who can rewrite a `.APP` can already rewrite the kernel —
no privilege boundary is crossed. (The *robustness* corner of this — a truncated
0-byte `.APP` — is tracked as B2 below, not as an escalation.)

---

## 3. Correctness / robustness bugs

### B1 — LOW — uptime `divu` overflows after ~18 hours
`kernel.asm:1293` (`clock_draw`) and `kernel.asm:1271` (`sysinfo_draw`) both do
`divu #TICKS_SEC,d0` with `d0` = the **32-bit** `ticks` counter. `divu` requires
the quotient to fit 16 bits; once uptime passes `65535·60 = 3,932,100` ticks
(~18.2 h) the quotient overflows (V set) and the result is undefined, so the
Clock window and SysInfo "Uptime" show garbage (and Clock's cascaded
`divu #60`s compound it). **Fix:** do the seconds division as a real 32-bit
divide (high-word `divu` then low-word `divu` with carry, or a shift/subtract),
then the `/60`/`/60` on the ≤16-bit seconds value are safe.

### B2 — LOW / latent — a 0-byte `.APP` is marked resident, then jumped through
`diskapp.i:114-118`. `load_app_slot` only treats `fat_read_file` as failed when
it returns **negative** (`bmi .fail`); a return of **0** (empty file, or a
directory entry whose first cluster is out of range) falls through to
`app_set_resident`. The slot region at `$40000+` is **not** zeroed at boot (only
`[KBSS,KBSS_END)` is cleared — `kernel.asm:221-225`), so the subsequent
`jsr (vector)` in `app_disp_draw`/`_key`/`_tick` (`diskapp.i:184-233`) reads
uninitialised slot memory as a code pointer and wild-jumps → crash screen.
Only reachable with a crafted/empty `.APP`; the bundled apps are all non-empty.
**Fix:** `tst.l d0 / ble .fail` (reject 0 as well as negative), or require a
minimum image size (≥16 bytes for the vector table).

### B3 — LOW / latent — `fat_mount` silently caps the FAT at 3 sectors
`fat12.i:72-75`. `sectors-per-FAT` is read from the (untrusted) BPB and clamped
to 3 with no diagnostic; `fat_buf` holds only those 3 sectors (~1024 addressable
clusters). Correct for the fixed 800K/880K media this port ships, but any larger
image is mis-parsed (clusters beyond the cached FAT read stale zero → chains
terminate early / wrongly). Not a memory-safety issue (bounded read), but an
undocumented hard limit shared with S1's root cause. **Fix:** reject a volume
whose `spf` or cluster count exceeds what `fat_buf` can hold, rather than
silently truncating.

*Checked and clean:*
- **Notepad** edit buffer (`files.app.asm`): open clamps the read to `NBUF-1`
  before `fat_read_file` (`198-201`), insert checks `NBUF-1` before shifting
  (`527`), backspace/insert shifts are length-bounded (`502-549`).
- **Paint** `pt_load`/`pt_save` clamp to exactly `PT_W*PT_H` = the `pt_canvas`
  size (`paint.app.asm:949-951`, `932`); `pt_clear` covers the same extent.
- **Tracker** load/save clamp to exactly `TK_ROWS*TK_CHANS*2` = the `tk_pat`
  size (`tracker.app.asm:216-217`, `205`); `tk_load_demo` copies a fixed table.
- **`fat_read_file`** honours the caller's byte budget on every sector copy
  (`fat12.i:206-219`), so no dest overrun regardless of chain length.
- **App loader** clamps the read to `APP_SLOT_SZ` (`diskapp.i:106-108`) and
  bounds `proc < 12` / rejects in-kernel slots (`app_slot_for` `diskapp.i:70-80`).
- **Keyboard** scan→table index is masked to 0..127 (`kb_byte` `kernel.asm:2010-2020`;
  `kb_os_poll` masks `$7F` `kernel.asm:386-388`) into the 128-byte `kb_ascii`/`kb_raw`.
- **ROM OS-event pool** is sized exactly for the `EvtBufCnt` it advertises
  (`osevt_buf` = `OSEVT_N*22` = 440 B, `kernel.asm:2595`, `194-199`).
- **Window / zlist** indexing is bounded by `MAXWIN`; `win_create` rejects a full
  table (`kernel.asm:864-867`); `close`/`raise` shift within `zcount`.
- **Fault handler** frame offsets match the 68000 group-0 (bus/addr) and group-2
  (illegal) exception stack frames (`berr_h`/`ill_h` `kernel.asm:2221-2244`).
- **Event ring** masks head/tail with `EVQ_SIZE-1` (`kernel.asm:2363,2387`).
- The legacy in-kernel game/app sources `games.i`, `pacman.i`, `paint.i`,
  `tracker.i`, `music.i`, `theme.i` are **not in any include or assembly path**
  (superseded by the `*.app.asm` disk binaries) — dead files, not shipped, not
  audited as reachable.

---

## Suggested order of work
1. **P1 + P2** — kill the full-desktop repaint on focus/raise and the redundant
   icon re-blit; this is the biggest responsiveness win and the analog of the
   pc64 report's headline drag fix.
2. **P3** — long-fill `fill_rect`'s interior; helps every remaining repaint.
3. **B1** — 32-bit uptime division (visible wrong output after ~18 h).
4. **B2 + S1 + B3** — the crafted-disk hardening: reject 0-byte `.APP`s, bound
   the FAT cluster index against `fat_buf`, and fail loudly on an over-large FAT.
5. **P4** — `stop` the idle loop (power).

Building/testing note: the macplus target assembles with `vasmm68k_mot` and is
exercised ROM-free by `harness.py` (Unicorn 68000 + emulated `.Sony`/VIA/SCC).
That toolchain is not installed on this machine, so none of the above has been
assembled or run here — the findings are from source review and the fixes are
proposed, not applied.
