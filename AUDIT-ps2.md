# UnoDOS `ps2` code audit — performance, security, correctness

Scope: the **ps2** world (Sony PlayStation 2, MIPS R5900 "Emotion Engine",
FreeMcBoot/uLaunchELF-launched ELF, run under PCSX2 or on metal — the `Makefile`
`all` target: `unodos.c` + Mac-compat shim + `ee_platform.c`/`ee_usb.c`/
`ee_audio.c`/`ee_modload.c`, `-DUNO_COLOR=1 -DUNO_EE`). All drawing is a software
`640×448×32` framebuffer in EE RAM (`fb.c`); the GS is a per-vsync blitter.

**Threat model.** This is firmware-hosted bare-metal: flat 32 MB, **no MMU
protection, no NX, no guard pages** — every out-of-bounds write is a direct
corruption primitive. Unlike the pc64 world there is **no network stack** and no
USB device-descriptor parsing in this port. The untrusted-input surface is narrow
and almost entirely local:

- **Memory-card files** — `ee_modload.c` reads `mc0:/UnoDOS/Apps/appNN.uno`
  (relocatable ELF) off the card and hand-parses + relocates it. On a pristine
  card these are the port's own embedded images (written on first boot), but a
  card is user-writable and portable, so a crafted `appNN.uno` is attacker-shaped
  input. **This is the one real memory-corruption surface** and it runs at every
  app launch.
- **USB HID** (`ee_usb.c`) — keycodes/mouse deltas, but every value is clamped or
  range-matched at the edge; no corruption surface.
- **FAT12 disk images** (`unodos.c`) — only reachable via a real `.Sony` MFM
  floppy (never succeeds on stock PS2 hardware) or the RAM image compiled in
  **only under `-DUNO_AUTOTEST*`**. Effectively unreachable on the shipping EE
  build; latent only.
- **libmc directory listing** (`mac_io.c`) — sizes come from libmc, copies are
  bounded.

Line numbers are exact against the current tree.

---

## 1. Performance

### P1 — The topmost window is fully repainted every single frame (biggest win)

> **✅ FIXED (`810914a`)** — the kernel-driven `task_post` no longer forces `draw_window` on the per-frame tick path (keys keep it; dynamic apps self-draw; live SysInfo/Clock via `app_secondly`). Builds + executes on PCSX2, no EE exceptions.
`unodos.c:1597` (`post_ticks()` in the main loop) → `unodos.c:984-986` →
`task_post(...,type=2,...)` → `unodos.c:979` `draw_window(&gWins[slot])`.

In the kernel-driven scheduler (`UNO_EE`), `post_ticks()` posts a frame tick to
the topmost window's slot, and `task_post` **unconditionally calls
`draw_window()`** after dispatching the tick. `draw_window` (`unodos.c:250-321`)
is the most expensive paint in the system: two drop-shadow rect fills, the
content-area fill, the titlebar fill, the window box, the **pinstripe line loop**
(`unodos.c:291-293`), the close box, centered title text, **and**
`draw_app_content()` for the whole client area. This runs **60×/second even when
the focused app is completely idle** (Sys Info, Files, Notepad, Theme, Paint at
rest all redraw their entire window every frame).

**Fix:** gate the per-frame `draw_window` on actual change. The tick handler
(`ai->tick`) already runs every frame for animation state; only games/clock need
a redraw each frame. Add a "dirty" return from `app_tick_dispatch` (or a per-app
`wants_frame` flag) and skip `draw_window` in `task_post` when nothing changed.
For static apps this removes the dominant per-frame CPU cost outright.

### P2 — Full framebuffer is DMA'd to the GS every frame, no damage tracking
`ee_platform.c:225` `gsKit_texture_upload(g_gs, &g_tex)` uploads the entire
`640×448×4 ≈ 1.1 MB` texture to GS VRAM every present, unconditionally. There is
no dirty-rect/min-max-row hint (confirmed: `fb.c` exposes no damage state), so
even a one-pixel change re-uploads the whole buffer.

Honest framing: at 60 Hz this is ~66 MB/s against a >1 GB/s path, and the README
correctly calls the upload itself hardware-cheap — so this is **lower priority
than P1**, whose cost is CPU-side pixel work. But it is genuinely O(screen) every
frame with no way to reduce it today. If P1 lands and idle frames stop changing
the FB, a "source rows dirtied this frame" hint would let `uno_ee_present` skip or
shrink the upload on idle frames.

### P3 — `oval_span` is ~10 long-multiplies per pixel with a 4-neighbour edge test
`mac_compat.c:187-210`. `PaintOval`/`FrameOval` run an O(w·h) double loop, and for
**every** pixel compute `nx*nx + ny*ny` plus, in the frame case, **four more full
neighbour evaluations** (`mac_compat.c:199-203`), each a pair of `long`
multiplies. This is recomputed on every draw. Pac-Man draws pac + dots as ovals
inside its per-frame `pacman_draw`, and the Clock face redraws on the second — so
this lands on the hot path. **Fix:** replace the per-pixel implicit-equation test
with a midpoint/Bresenham circle (span fill for `PaintOval`, outline for
`FrameOval`); drop the 4-neighbour edge probe entirely.

### P4 — LOW — `fb_big_glyph` issues up to 64 `fb_fill_rect` calls per glyph
`fb.c:98-99`. Each lit font pixel becomes a separate `fb_fill_rect`, and each of
those re-runs `clip()` (`fb.c:9-16`). Only `fb_big_text` uses it, and only the M0
`uno_splash.c` reference target calls `fb_big_text` — the shipping desktop draws
its splash through the Toolbox `DrawText` path — so impact is one-time at most.
Noted for completeness; low priority.

*(Already good: `fb_fill_rect`/`fb_invert_rect` are tight word-write loops with a
single up-front clip; the drag is a real XOR rubber-band outline — `drag_update`
`unodos.c:1256-1284` — that only touches the outline rows, not a full repaint; the
main loop is vsync-bound via `gsKit_sync_flip` (`ee_platform.c:232`) with no busy
spin; TTF-style glyph work isn't re-rasterised — the 8×8 font is a static table.)*

---

## 2. Security

### S1 — MEDIUM (HIGH impact) — memory-card ELF overlay loader trusts every offset/size/index
`ee_modload.c` `overlay_load()` (`342-466`) parses and relocates an `ET_REL` ELF
read straight off `mc0:/UnoDOS/Apps/appNN.uno` (`mc_read_module`, `284-317`). The
image is fully attacker-shaped if the card file is replaced, and `overlay_load`
runs at **every app launch** (`uno_load_module`, `470-509`, `UNO_EE_OVERLAY=1`
default). Structural fields are validated only weakly (`e_type`, `e_shnum ≤
MAX_SHDR`), while nearly every offset, size and index is trusted:

- **Arbitrary relative write (worst primitive).** `ee_modload.c:413`
  `loc = (unsigned int *)(base + rel[j].r_offset)` then `ee_modload.c:423-424`
  `*loc += value` (and the `R_MIPS_26/HI16/LO16` cases at `426-439`). `r_offset`
  is never bounded against the target section's `sh_size`, so a crafted relocation
  writes a controlled 32-bit value at `base + attacker_offset` — an arbitrary
  write anywhere in EE RAM, i.e. code execution with no MMU to stop it.
- **`e_shoff` unchecked** — `ee_modload.c:358` `sh = (Elf32_Shdr *)(img +
  eh->e_shoff)`. A large `e_shoff` makes every subsequent `sh[i]` access read past
  the malloc'd image.
- **`e_shstrndx` unchecked** — `ee_modload.c:359` indexes `sh[eh->e_shstrndx]`
  with no `< shnum` bound.
- **Section copy over-read** — `ee_modload.c:390` `memcpy(blob + off, img +
  sh[i].sh_offset, sh[i].sh_size)`: `sh_offset`/`sh_size` are not validated
  against `len`, so the source can lie far beyond the image (and the copy can be
  huge).
- **Symtab/strtab/rel bases unchecked** — `ee_modload.c:394-395` and `407-408`
  take `sh_offset` for the symbol table, string table and relocation table with no
  bound.
- **Symbol index + name over-read** — `resolve_sym` (`323-339`) indexes
  `&syms[idx]` with `idx = ELF32_R_SYM(r_info)` (`ee_modload.c:412`) unbounded
  against the symbol count, and `strtab + s->st_name` (`ee_modload.c:328`)
  unbounded against the string-table size, then `strcmp`/`printf("%s")` walk it.

**Reachability caveat (why MEDIUM, not HIGH):** there is no network or removable
auto-run path here — exploitation needs an attacker-authored `appNN.uno` already
present on the inserted memory card (a shared/homebrew card image, or a save-file
swap). Given that, the impact is a clean arbitrary write. **Fix:** before use,
validate `e_shoff + e_shnum*sizeof(Elf32_Shdr) ≤ len` and `e_shstrndx < shnum`;
for each section, `sh_offset + sh_size ≤ len`; for each reloc, `r_offset + 4 ≤`
target-section `sh_size` and `ELF32_R_SYM(r_info) < (symtab sh_size /
sizeof(Elf32_Sym))`; bound `st_name < strtab sh_size`. Any failure → free + return
NULL (the caller already falls back to the linked-in registry, so failing closed
is free).

### S2 — LOW (latent) — FAT12 BPB fully trusted, but unreachable on the stock EE build
`unodos.c:490-518` (`fat12_mount_dev`). Geometry (`gFatSecPerClus`, `gFatRsvd`,
`gFatFatSz`, `gFatRootEnts`, `gFatTotSec`) is read from an untrusted boot sector;
`gFatCache = NewPtr(gFatFatSz*512)` can be driven to a 32 MB allocation, and
`gFatRootStart`/`gFatDataStart` are computed and stored as `short`
(`unodos.c:502-503`), so a large-but-legal BPB truncates/overflows the sector
math. This is real code but the only device that can feed it on a shipping EE
build is `fat_dev_sony`, which always fails (no `.Sony` floppy — `mac_io.c`
`PBReadSync` rejects `ioRefNum < 0`), and the RAM image is compiled in **only
under `-DUNO_AUTOTEST*`** (`unodos.c:526-532`). So it is latent/defensive on the
default build. **Fix (when a `fat_dev_mc` backend lands):** validate the BPB
(`bytes/sector == 512`, sane cluster/FAT/root counts, `TotSec` fits the device)
and use `long` for `gFatRootStart`/`gFatDataStart`.

### S3 — LOW (defensive) — libmc dir names bounded, but only by the 32-byte loop cap
`mac_io.c:220-227` (EE `PBGetCatInfoSync`) copies `gMcDir[i].EntryName` into
`char nm[34]` with `for (j = 0; j < 32 && e[j]; j++)` then `nm[j] = 0` — safe
(max `j == 32`, `nm[32]=0`). Called out only to record that the enumeration path
was checked and the cap is what makes it safe; `EntryName` is otherwise trusted
libmc output. No change needed.

*Checked and clean:* `ee_usb.c` — `hid_translate` (`68-106`) range-matches every
HID usage, the modifier shift `1 << (rk.key - 0xE0)` is bounded by the `0xE0..0xE7`
guard (`140`), and `poll_mouse` clamps `g_mx/g_my` to the framebuffer
(`170-173`); `ee_audio.c` — `uno_audio_note` clamps `chan` and `midi`
(`86-95`), the pump bounds `bytes ≤ sizeof(g_buf)` (`113`); `mac_io.c` `p2c`/`c2p`
(`28-43`) cap by both the Pascal length byte and the destination `cap`; the SIF
lock discipline (`ee_platform.c:41-58`) keeps audio-pump / USB-poll RPC mutually
exclusive; `ee_modload.c` fails closed to the linked-in registry on any parse
error.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `fmt_u` stack buffer is sized for 32-bit `long` only
`unodos.c:193-200`. `char tmp[12]` holds at most 10 digits + NUL. On the **EE**
target (`mips64r5900el`, n32 ABI) `long` is 32-bit, so ≤10 digits — safe, and the
`num[12]`/`num[16]` call-site buffers (`notepad.c:40`, `theme.c:26`,
`dostris.c:78`, `pacman.c:133`, etc.) also fit. But `unodos.c` is shared with the
**host** shim, built by WSL gcc where `long` is **64-bit**; a value ≥10¹² (13+
digits) overflows both `tmp[12]` and the `num[12]` buffers. Pac-Man's score
(`pacman.c:123` `gPmScore += 200L << gPmKills`) is `long` and unbounded across
sessions, so it is the reachable path on the host build. **Fix:** `char tmp[24]`
and widen the `num[12]` buffers to `num[24]`. (Same defect as pc64 `B1`; here the
on-target ABI happens to make the EE build safe, so it is LOW rather than MEDIUM.)

### B2 — LOW — overlay loader silently can't relocate `SHN_COMMON` symbols
`ee_modload.c:323-339` (`resolve_sym`). `SHN_UNDEF` and `SHN_ABS` are handled, but
`SHN_COMMON` (`0xFFF2`, `#define`d at line 234 yet never checked) falls through to
`if (shndx >= MAX_SHDR) return NULL` (`336`), so a reloc against a COMMON symbol
makes `overlay_load` bail to the linked-in fallback (`417-420`). Fails safe, but
means a legitimately compiled module using a common/tentative-definition symbol
never loads from the card. **Fix:** either allocate COMMON symbols into the blob,
or document them as unsupported so the packaging step rejects them.

### B3 — LOW — `mc_read_module` treats a short read as EOF
`ee_modload.c:299-312`. The read loop breaks on `got < want` (`311`) as "short
read = EOF". libmc's `mcRead` returns the full requested count for a valid
save-file, so this is fine in practice, but if a future backend ever returns a
partial transfer mid-file the image is silently truncated and `overlay_load`
rejects it (falling back to the registry). Robust-but-surprising; prefer looping
until `got <= 0`, using `got == 0` as the only EOF signal.

### B4 — LOW — `put2` writes 3 bytes with no size contract at call sites
`unodos.c:201`. `put2(long v, char *out)` always writes `out[0..2]` (two digits +
NUL). Callers must supply ≥3 bytes; there is no guard. No current caller is
under-sized, but it's an unchecked-width helper worth a comment or a `char out[3]`
convention. Latent.

*Checked and clean:* `notepad.c` — the insert path is guarded (`notepad.c:166`
`if (gNLen < NBUF-1)`), backspace guards `gNCaret > 0` (`161`), `notepad_load`
clamps `got` via `fat12_read(..., NBUF-1)` and casts into `gNLen`
(`101-107`); `pacman.c` — all board indexing goes through `pm_walkable`'s
`ty<0||ty>=PM_ROWS` bound and `tx` wrap (`58-62`), tile writes use masked
coordinates (`107-113`), `kPmModeDur` is indexed with an explicit
`gPmMode>7?7:gPmMode` clamp (`104`); `mac_compat.c` — every pixel writer
(`put_pen` `160-169`, `oval_span` `204`, `FillRect` `149-154`, `fb_glyph`
`fb.c:62-72`) bounds-checks `xx`/`yy` before the store, and `clip()` (`fb.c:9-16`)
gates the rect fills; the event ring (`mac_compat.c:232-253`) is a standard
head/tail ring with a full-drop guard; `ee_audio.c` phase-accumulator mixing is
bounded by `frames`/`rem`.

---

## Suggested order of work
1. **P1** — stop the unconditional per-frame `draw_window` of the topmost window
   (`unodos.c:979`); this is the single largest CPU win and needs only a dirty
   signal from the tick dispatch.
2. **S1** — bound every offset/size/index in `overlay_load` (`ee_modload.c`); the
   arbitrary-write reloc is the one genuine corruption primitive in the port, and
   it fails closed for free, so hardening is cheap.
3. **P3** — replace `oval_span`'s per-pixel implicit-equation + 4-neighbour test
   with a midpoint circle (`mac_compat.c:187-210`).
4. **P2** — add a damage-row hint so idle frames shrink/skip the GS upload
   (`ee_platform.c:225`), once P1 makes idle frames actually idle.
5. **B1-B4, S2-S3, P4** — the latent/defensive fixes: widen `fmt_u`'s buffer for
   the shared host build, validate the FAT12 BPB before a real `fat_dev_mc`
   backend lands, and the loader's COMMON/short-read edge cases.

Building/testing note: the EE target builds with `mips64r5900el-ps2-elf-gcc`
(ps2dev v2.0.0 under WSL) and boots under PCSX2 v2.6.3 + a 4 MB PS2 BIOS
(`build.sh ee`, `tools/run_pcsx2.ps1`). That toolchain is a WSL-only prerequisite
and none of the above was compiled while auditing — the fixes are proposed, not
applied. The memory-card overlay path (S1) and SPU2 audio are additionally
hardware/real-card only to exercise; PCSX2 fastboot HLE does not drive them.
