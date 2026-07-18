# UnoDOS `pc64` code audit — performance, bugs, security

Scope: the **pc64** world (modern x86-64 / UEFI), the default `build.sh` target
(the `unoui` shell: `-DUNO_UUI -DUNO_I2C_TRACKPAD`). Threat model for the security
section: freestanding UEFI, flat identity-mapped RAM, **no NX / no guard pages /
no ASLR** — every out-of-bounds write is a direct corruption primitive and even
OOB reads can fault the machine.

Three areas, most-actionable first. Line numbers are exact against the current tree.

---

## 1. Performance — why dragging and the UI are laggy

### The root cause
The drag is a **rubber-band outline**: while you drag a window only the outline
rectangle moves; the window commits to its new position on mouse-up
(`unoui/unoui_input.c` `UI_CAP_WINDOW`, commit at ~683). *Visually* almost nothing
changes per frame. But the render path throws away that fact:

- `pump_input()` sets `g_dirty = 1` on **every** mouse-move event (`pc64_uui.c:948`).
- `g_dirty` runs `unoui_render_ui()` (`pc64_uui.c:964`), which is an
  **unconditional full-scene repaint** (`unoui/unoui.c:916`): it repaints the whole
  desktop background, every window (frame + titlebar + shadow), the taskbar, and
  *then* the one-pixel outline.
- There is **no damage/dirty-rect tracking** and **no cached background** (confirmed:
  `unoui.h` carries only the `drag_*` outline fields).

So each mouse-move during a drag re-executes the most expensive paint in the system:

| Cost per drag frame | Where |
|---|---|
| Full-screen vertical gradient (`W×H` writes + per-row math) | `theme_aurora.c:44` `fb_grad_v` |
| **Two ~0.36·W·H alpha-blended "aurora" blobs** (never change) | `theme_aurora.c:46-47` |
| Every window: **6-layer soft drop shadow**, all alpha-blended | `theme_aurora.c:34-38` |
| Border + titlebar rounded-rect alpha fills per window | `theme_aurora.c:50-84` |
| `present()` **scans the entire framebuffer** vs shadow every frame | `uefi_main.c:770-778` |
| Unconditional `Stall(8000)` (8 ms) tail on every present | `uefi_main.c:806` |

At a 1280×800 desktop the two aurora blobs alone are ~740k **alpha-blended** pixels,
and `blend_px` does **three integer divides by 255 per pixel** — recomputed every
frame for a background that is identical frame-to-frame. That is the lag.

> Note: the `pc64_uui.c:8-12` header comment ("a drag only rewrites the moving
> window's rows — no full-screen churn") describes the `present()` VRAM-upload
> optimization, **not** the CPU repaint. The repaint is full-screen. The comment
> is misleading and masks the real cost.

### Fixes, highest impact first

**P1 — Cache the static desktop background (biggest single win).**
The desktop (gradient + two aurora blobs) only changes on theme change or resolution
change. Render it once into a `bg[FB_BUF_PIX]` buffer; each frame `memcpy` the rows
you need instead of recomputing gradient + two large alpha blends. Invalidate the
cache in `unoui_ui_theme()` and `uno_screen_changed()`. This removes ~90% of the
per-frame pixel work by itself.

**P2 — Damage-rect the rubber-band drag (direct fix for the reported symptom).**
While `ui->drag_active`, the only thing that changed is `union(old_outline,
new_outline)`. Instead of `unoui_render_ui()`, restore those rows from the cached
background/scene and draw the new outline. Even a coarse version — "if `drag_active`,
only repaint the band of rows the outline touches" — makes dragging feel instant.
The general form is a damage list the widgets append to; the drag case is the
80/20 subset.

**P3 — Kill the `/255` divides in the blend hot path.**
`fb.c` divides by 255 three times per pixel in `blend_px` (97-99),
`fb_blend_pixel_sub` (191-193), `fb_grad_v` (134-136), and `round_corner` (155-157).
Replace `x / 255` with the exact-enough `(x * 257 + 257) >> 16` (or
`(x + 1 + (x >> 8)) >> 8`). The whole Aurora theme is alpha-heavy, so this is a
~3-5× speedup on *all* blended drawing, independent of P1/P2.

**P4 — Stop the full-framebuffer dirty scan in `present()`.**
`uefi_main.c:770-778` compares all `FB_W×FB_H` pixels to `gShadow` every present even
when the renderer already knows what changed. Track a min/max dirty source-row hint
(or a damage rect) from the renderer and only scan/upload those rows. Secondary to
P1/P2 but O(screen) every frame today.

**P5 — Drop the unconditional 8 ms present stall for interactive frames.**
`gBS->Stall(8000)` (`uefi_main.c:806`) adds fixed 8 ms latency to *every* present,
including each drag frame, and is redundant with the main loop's idle
`uno_pc64_delay_ms(16)` (`pc64_uui.c:965`). Pace only idle frames; let active frames
present as fast as they render.

**P6 — Word-at-a-time `memcpy`/`memset`.**
`pc64_libc.c:19-41` copies/sets one byte at a time. Under `-ffreestanding` the
compiler emits real calls to these, so every `fb_clear`, background blit (P1), and
present upload pays 4-8× overhead. Do 8-byte-aligned copies with a byte tail.

**P7 — `soft_shadow` = 6 alpha layers per window per frame** (`theme_aurora.c:34`).
Largely subsumed by P1/P2 (windows stop repainting mid-drag), but for normal repaints
consider caching each window's chrome or cutting the layer count.

*(Already good: TTF glyphs are cached — `pc64_font.c` ASCII glyph cache — so text is
not re-rasterized per frame.)*

Expected result: P1+P2+P3 together should take a full-desktop drag from "repaint
everything every frame" to "touch a few hundred rows," which is the difference
between visibly laggy and smooth.

---

## 2. Security — remotely / device reachable memory corruption

Ordered by reachability × impact. **S1 is the one to fix first.**

### S1 — HIGH — IP total-length trusted; over-read leaks adjacent memory to the network
`pc64/net.c:501-515` (`ip_recv`). `plen = rd16(ip + 2) - hl` is taken from the
attacker-controlled IP header and only checked for `plen < 0`, never against the
actual received frame length `len`. The inflated `plen` is passed as authoritative to
every L4 handler:
- `tcp_recv` (`net.c:376-383`) `memcpy`s up to `sizeof(tcp.rxq)`=2048 bytes from
  `seg + hl`, reading ~450 bytes **past the 1600-byte static `rx[]`** into the TCP
  receive queue — which is then delivered to the app / browser as page content. A
  **remote info-leak of adjacent `.bss` to any HTTP server the browser visits.**
- Same root cause drives OOB scans in `dhcp_input`/`dhcp_opt` (`net.c:462-498`, XID is
  a fixed constant so it's reachable) and stale-byte over-reads in
  `udp_recv`/`icmp_recv`.

**Fix (one line, closes all of the above):** in `ip_recv`, clamp to the real frame:
```c
int tot = rd16(ip + 2);
if (tot < hl || tot > len) return;   /* or: if (tot > len) tot = len; */
int plen = tot - hl;
```

### S2 — HIGH (default build) — I2C-HID report-descriptor OOB read
`pc64/i2c_hid.c:315-323` (`getbits`) + `347-349`. Bit offsets/sizes
(`g_x_off/g_y_off/g_tip_off`, `g_x_bits/g_y_bits`) come from the device's HID report
descriptor via `parse_rd` (`251-261`) with **no bound relative to the 64-byte `rep[]`
buffer**. A device advertising X/Y at a large bit offset makes `getbits` read far past
`rep[64]` on the stack; a `Report Size ≥ 32` also makes `v |= (1 << i)` an undefined
shift. **`build.sh` enables `-DUNO_I2C_TRACKPAD` by default**, so this is in the
shipping image (inert on QEMU; triggers with a real/emulated I2C-HID pad).
**Fix:** in `parse_rd` reject `rsize > 32` and `bitpos` beyond `g_max_input*8`; in
`uno_i2c_hid_poll` verify `g_x_off + g_x_bits ≤ (len-base)*8` (and Y) before
`getbits`; make `getbits` use `unsigned` and bound `nbits ≤ 32`.

### S3 — HIGH — xHCI input-context OOB write from a device-supplied endpoint number
`pc64/xhci.c:349-356` (`setup_ep`) via `uno_usb_setup_bulk` (`376-393`). The bulk
endpoint address is read from the device config descriptor; `dci = (addr & 0xF)*2 + 1`
reaches 31. With 64-byte contexts (`g_csz==1`), `setup_ep` writes u32s at offsets
2052-2067 — **past the 2048-byte `g_inctx[..]` row**, into the adjacent context. The
buffer is undersized even for a legitimate high-DCI device: `(32+1)*64 = 2112 > 2048`.
A malicious USB device (VID 0x0b95 to match `ax88179`) with `bEndpointAddress=0x8F`
triggers it. **Fix:** size `g_inctx` rows to ≥2112 (round to 4096) **and** reject
`maxdci > 31` / validate endpoint addresses in `find_bulk_eps` (`ax88179.c:70-91`).

### S4 — HIGH — HTTP request assembly overflows a stack buffer
`pc64/pc64_http.c:101-107` (`pc64_http_get`). `path` and `host` copies into `req[720]`
are length-checked, but the two fixed header strings `b` (17 B) and `c` (~92 B) are
`memcpy`'d **unconditionally**. With a near-max URL (host ≤127, path ≤511) `rn` reaches
~659 before the final 92-byte copy → ~31-byte **stack OOB write**. URLs come from the
address bar *and from links in untrusted pages*. Overflow bytes are constant header
text (so realistically a stack-smash/DoS), but it is a genuine OOB write.
**Fix:** bounds-check every append against `sizeof req`, or size `req` to fit the
worst case (4 + 512 + 17 + 128 + 92 + slack ≈ 768 → use `req[1024]`).

### S5 — HIGH — Unbounded native recursion in the JS interpreter
`pc64/js.c` recursive-descent parser (`parse_expr`→…→`parse_primary`→`parse_expr`) and
tree-walking `ev()` have **no depth guard** (the `guard++ < 1000000` counters only
cover `while`/`for`). A `<script>` with deeply nested `(((…)))`, `!!!!…`, nested array
literals, or recursive calls (`function f(n){return f(n+1)} f(0)`) overflows the C
stack; with no guard page this **corrupts adjacent RAM**, a controllable write.
Reachable from any fetched or local HTML page. **Fix:** add explicit parse-depth and
call-depth counters that trip `perr`/`g_err`.

### S6 — MEDIUM — ax88179 RX aggregation: only the first descriptor is bounds-checked
`pc64/ax88179.c:143-162` (`ax_recv`). `g_rx_cnt`/`g_rx_hdroff` come from the device's
trailing header; the bound check runs only on refill. Each subsequent pop advances
`g_rx_hdroff += 4` and dereferences `*(u32*)(g_rx + g_rx_hdroff)` with **no re-check**,
walking up to ~256 KB past `g_rx[4096]`. Read-only (the payload `memcpy` is separately
guarded). **Fix:** re-validate `g_rx_hdroff + 4 ≤ g_rx_len` (and `g_rx_off`) on every
pop; reset `g_rx_cnt = 0` on failure.

### S7 — MEDIUM — Untrusted TrueType into fixed arenas
`pc64/pc64_font.c:36-42, 79-91`. Fonts are read into `g_data[slot][FONT_BUF]`; an
over-size file is silently truncated then handed to `stbtt_InitFont`/rasterization,
which trust internal table offsets and can read past the truncated buffer. stb's arena
`fa_alloc` returns 0 on exhaustion and stb doesn't consistently null-check → possible
NULL deref. Semi-trusted (needs control of the ESP/RAM-disk font). **Fix:** reject
fonts whose on-disk size hits `FONT_BUF` (treat `n == FONT_BUF` as "too big"); ensure
`fa_alloc` failures propagate.

### S8 — LOW — DNS answer-name walk over-reads the stack buffer
`pc64/net.c:580-589` (`net_dns_query`). The `i + 12 ≤ n` guard is checked before the
name-skip advances `i`; `r[i]`, `r[i+1]`, `r[i+8]`, `r[i+9]` are then read without
re-validation, up to ~70 bytes past `r[512]`. Read-only; spoofable (fixed txid
`0x5153`, fixed src port). **Fix:** verify `i + 10 ≤ n` after the name-skip and bound
the uncompressed-name walk so `i` can't exceed `n`.

### S9 — LOW — JS statement loops can hang (DoS)
`pc64/js.c:318, 532`. The statement loops advance the cursor only inside the
capacity-guarded branch, so a script with more top-level (>512) or per-block (>256)
statements than capacity spins forever. **Fix:** always advance past a statement even
when the table is full (or error out).

*Checked and clean:* `e1000_recv` (clamps to `cap`), the HTML/Markdown parsers in
`pc64_browser.c` (all fixed buffers capped, style stack depth-guarded), the JS lexer /
array / arg buffers, `pc64_fs.c`/`uno_efifs_*` reads (respect caller `max`), the module/
app loader (static range-checked registry), and TLS parsing (delegated to BearSSL; its
non-RDRAND entropy fallback is documented demo-grade, not a memory bug).

---

## 3. Correctness / robustness bugs

### B1 — MEDIUM — `fmt_u` stack buffer too small for 64-bit values
`pc64/unodos.c:193-200`. `char tmp[12]` but `v` is a 64-bit `long`; a value ≥10¹²
(13+ digits) overflows the buffer. Game scores are unbounded `long`
(`gPmScore += 200L << gPmKills`, `gDtScore`, `gOlScore`), so it is reachable, not just
theoretical. **Fix:** `char tmp[24]`. (Note: `unodos.c` is the legacy Mac core, only in
the `build.sh legacy` target — fix anyway, it's shared code.)

### B2 — LOW — `calloc` multiply overflow
`pc64/pc64_libc.c:177-183`. `size_t n = nm * sz;` with no overflow check → under-alloc
+ `memset` past the block if it wraps. No call sites today (latent), but a footgun for
any future parser that adopts it. **Fix:** `if (sz && nm > (size_t)-1 / sz) return 0;`.

### B3 — LOW — allocator coalesces forward only → long-run fragmentation
`pc64/pc64_libc.c:148-160`. `free()` merges with `b->next` when physically adjacent but
never backward, so a block freed before its lower neighbour never re-merges. Over a
long session of window open/close and RAM-disk churn the single first-fit arena
fragments and large allocations can spuriously fail. Not memory-unsafe. **Fix:**
address-order the free list and coalesce with the previous block too.

### B4 — LOW — `free`/`realloc` do no pointer/double-free validation
`pc64/pc64_libc.c:148-175`. `free(p)` blindly treats `p - HDR` as a `Blk`; a bad/double
free corrupts adjacent metadata. Low given trusted callers. **Fix:** a debug-build
magic/`used`-already-clear guard.

### B5 — LOW — negative `max` mishandled in RAM-FS accessors
`pc64/pc64_fs.c:39` and `pc64/pc64_io.c:108-120`. `uno_fs_list_get`/`uno_ramfs_name`
still write `name[0]=0` when `max==0` (into a zero-length buffer); `uno_ramfs_read`
with negative `max` makes `n` negative → cast to a huge `size_t` in `memcpy`. Callers
pass positive sizes today (latent). **Fix:** early-return on `max <= 0`.

### B6 — LOW — `floorf`/`ceilf` UB for large/NaN inputs
`pc64/pc64_math.c:16-27`. `(int)x` overflows for `|x| > INT_MAX`. Only fed demo 3D/font
values. **Fix:** range-guard before the cast.

*Checked and clean:* `notepad.c` insert (guarded `memmove`), `paint.c` flood fill
(stack-depth guarded, pixel writes bounds-checked), tracker/dostris/pacman/outlast
board indexing (all masked or bounds-tested), FAT12 read/write paths (offset-bounded),
`network.c` echo buffers (loop-bounded), and `mac_compat.c` `p2c`/`c2p` (length-capped).

---

## Suggested order of work
1. **S1** (`ip_recv` clamp) — one line, closes the remote info-leak + several OOB reads.
2. **P1 + P2 + P3** — the drag/UI lag the report was asked about.
3. **S2, S3, S4, S5** — the reachable corruption primitives (HID, xHCI, HTTP, JS).
4. **P4-P6** — the remaining perf wins.
5. **S6-S9, B1-B6** — hardening and latent fixes.

Building/testing note: the pc64 target builds with `x86_64-w64-mingw32-gcc` and boots
under QEMU + OVMF (`build.sh run`, driven by `harness.py`). That toolchain isn't
installed on this Mac, so none of the above has been compiled here — the fixes are
proposed, not applied.
