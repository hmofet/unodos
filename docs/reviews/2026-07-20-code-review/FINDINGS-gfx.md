# pc64 graphics/window stack: performance review (2026-07-20)

Scope: `pc64/fb.c`, `pc64/fb.h`, `pc64/pc64_uui.c`, `pc64/pc64_uui_apps.c`,
`pc64/pc64_font.c`, `pc64/pc64_icons.c`, `unoui/unoui.c`, `unoui/unoui.h`,
`unoui/unoui_app.c`, `unoui/unoui_input.c`, `unoui/themes/theme_aurora.c`,
`pc64/uefi_main.c` (present + platform half), `pc64/mac_compat.c`,
`pc64/unodos.c` (legacy loop tail), `pc64/build.sh`. The shipped image is the
unoui shell (`build.sh` default, `-DUNO_UUI -DUNO_BG_CACHE`, `-O2`); the
legacy shell is a separate `legacy` build.

## Summary

The architecture already has the right ideas: a cached desktop background
(`unoui.c:999-1010`), a shadow-buffer diff so idle frames touch no VRAM
(`uefi_main.c:106-113`), a scene-snapshot rubber-band drag so the full-scene
painter does not run per drag frame (`pc64_uui.c:1964-1980`), and banked TTF
glyph caches (`pc64_font.c:120-168`). What defeats them on real hardware is
granularity and the final write path:

1. Damage is tracked per **row only**, with no column extent, and the drag
   outline's vertical edges touch **every row of the window's span**. So each
   drag frame rewrites roughly (window height x scale) output rows at **full
   panel width** to VRAM. The "drag only rewrites the moving window's rows"
   claim in the header comment (`pc64_uui.c:10-12`) is true row-wise, but each
   of those rows is a full 1920+ px scanline, and for a tall window that is
   most of the panel per mouse move.
2. Those VRAM writes are issued as **per-pixel 32-bit `volatile` stores**
   through a pointer whose cache attribute is never checked or programmed.
   There is no MTRR/PAT code anywhere in `pc64/` (verified by search). If the
   firmware left the GOP BAR uncacheable rather than write-combining, every
   4-byte store is a separate uncached bus transaction and a single drag frame
   costs hundreds of milliseconds. This is the one candidate that fully
   explains "VERY SLOW" by itself, and it is unverified (see Open questions).
3. Everything interactive that is not a title-bar drag (mouse motion, icon
   drags, slider drags, window resize, text selection) sets a single global
   dirty flag and repaints the **entire scene** with the alpha-heavy Aurora
   painters, then present diffs the **entire frame** to find out what changed.

Fix order that follows from this: (a) verify/force WC on the framebuffer and
write rows with wide/non-temporal stores instead of the per-pixel volatile
loop; (b) give the dirty tracker per-row x-extents (min/max column), which
collapses the drag-outline cost from full scanlines to two thin bands; (c)
stop repainting the world on bare mouse movement; (d) trim the Aurora shadow
to its visible fringe. (a)+(b) alone should make dragging feel native.

Numbers below assume the representative laptop case: 1920x1080 panel, default
desktop = half panel (960x540, `uefi_main.c:366-371`), so `fb` is 2.07 MB and
one output row is 1920 px. If the user picked a native-resolution desktop in
the Control Panel, multiply the fb-side numbers by 4.

---

## P1: dominant costs (explain the user-visible slowness)

### P1.1 Window drag rewrites ~full-panel-width scanlines for the whole window span every frame

- Where:
  - `pc64/uefi_main.c:106` (`gDirtyRow[FB_MAX_H]`, one byte per **row**; no
    column extent anywhere),
  - `pc64/uefi_main.c:1050-1058` (pass 1 marks a row dirty if *any* pixel
    differs), `1066-1088` (pass 2 rebuilds and writes the **entire** output
    row `gOutW` wide for every dirty source row),
  - `unoui/unoui.c:1102-1111` (`unoui_draw_drag_outline`: three nested
    `fb_frame_rect`s whose vertical edges put changed pixels on **every row**
    from `drag_y` to `drag_y + drag_h`),
  - `pc64/pc64_uui.c:1973-1977` (per moved frame: restore snapshot, redraw
    outline, present).
- Mechanism: after `uno_pc64_scene_restore()` the rows that held the previous
  outline differ from `gShadow`, and the rows under the new outline differ
  too. Because the outline has 3 px wide vertical edges running the full
  window height, the dirty-row set each frame is the union of two bands the
  height of the window. Pass 2 then rebuilds (per-pixel gather via `gColMap`
  plus the `gSwapRB` swizzle) and writes each such row at full panel width,
  even though only ~6 px per row actually changed. Dragging a 500 px tall
  window on a 1080p panel: ~500 source rows -> ~1000 output rows x 1920 px
  x 4 B = ~7.9 MB rebuilt and written to VRAM **per mouse move**. Dragging a
  near-maximized window rewrites essentially the whole panel per frame.
- Fix:
  1. Track per-row dirty x-extents: replace `gDirtyRow[y]` with
     `gDirtyX0[y]/gDirtyX1[y]` filled in pass 1 (find first and last differing
     pixel; the compare loop already touches the row). Pass 2 then maps the
     source extent to an output column range and writes only that span. For
     the drag outline this reduces the written area from
     `window_h x panel_w` to two ~3 px wide vertical bands plus two short
     horizontal bands, i.e. by roughly two orders of magnitude.
  2. Cheaper still (and complementary): during `UI.drag_active`, skip the
     brute-force diff entirely; the shell knows the exact damage (old outline
     rect + new outline rect + cursor). Restore only those bands from
     `gScene` (not the full-screen memcpy at `uefi_main.c:122-123`) and
     present only those rects.

### P1.2 VRAM written with per-pixel volatile stores; framebuffer cache attribute never verified or set

- Where:
  - `pc64/uefi_main.c:95` (`static volatile UINT32 *gVram`),
  - `pc64/uefi_main.c:1085-1086` (`for (x...) dst[x] = gRow[x];` with `dst`
    volatile: the compiler must emit one scalar 4-byte MMIO store per pixel,
    no vectorization, no `rep movs`),
  - same pattern in `apply_desktop`'s clear (`uefi_main.c:356-357`) and
    `stripe_at` (`uefi_main.c:209-211`).
  - Searched for any MTRR/PAT/`wrmsr`/write-combine setup across `pc64/`:
    none exists. The mapping is whatever the firmware programmed, both before
    and after `ExitBootServices` (the port keeps the firmware page tables and
    MTRRs; `try_detach`, `uefi_main.c:595-641`, does not touch memory
    attributes).
- Mechanism: on the common path (P1.1) roughly 2M pixels are stored per drag
  frame. If the GOP BAR is mapped WC (usual, but firmware-dependent), scalar
  4-byte stores still work because the WC buffer combines them, and the cost
  is a few ms per frame. If the BAR is UC (happens when MTRRs are exhausted,
  the BAR is 64-bit/high, or firmware simply never set a WC range), each
  store is an individual uncached transaction at ~100-500 ns: 2M stores is
  **0.2-1.0 s per drag frame**. That failure mode matches "VERY SLOW on real
  pc64 hardware" exactly, and nothing in the code rules it out. This is the
  first thing to measure (see Open questions).
- Fix:
  1. Diagnose: time a fixed-size fill to `gVram` vs to `gShadow` at boot and
     log the MB/s ratio (also expose it in the System window). Read
     `IA32_PAT` (MSR 0x277) and the MTRRs (0x200..0x2FF) to see what the
     range actually is.
  2. Ensure WC: while attached, memory attributes belong to firmware, but
     after detach the port owns the machine and can set a variable-range WC
     MTRR over `FrameBufferBase..+FrameBufferSize` (or rebuild page tables
     with a PAT=WC entry). Gate it on the diagnosis from step 1.
  3. Independently of attributes, stop storing per pixel: build the row in
     `gRow` (already done) and copy it with 8/16-byte stores. Cast away
     volatile for the bulk copy (`memcpy` to `(UINT32 *)dst`; mingw emits
     `rep movsq`/SSE) or use explicit `movnti`/`_mm_stream_si128` plus one
     `sfence` per frame. On WC this alone is a several-fold win; on UC it
     raises throughput by the store-width factor.

---

## P2: large costs (make every interaction feel heavy)

### P2.1 Any input event repaints the entire scene; bare mouse motion included

- Where: `pc64/pc64_uui.c:1928` (`if (pump_input()) { g_dirty = 1; ... }`),
  `pc64/pc64_uui.c:1663-1674` (any cursor delta counts as input),
  `unoui/unoui.c:1015-1097` (`unoui_render_ui` repaints desktop + every
  window + every widget, painter's algorithm, no damage rects; occluded
  windows painted in full and then painted over).
- Mechanism: the cursor is composited at present time
  (`uefi_main.c:1024-1038`), so moving the mouse across a static desktop
  changes nothing in `fb`, yet every pointer delta triggers a full
  `unoui_render_ui` (background-cache memcpy of 2-8 MB, all window chrome,
  all widgets, all text) followed by present's full-frame diff. The same
  single global flag serves icon drags, slider drags, resize, and text
  selection, so all of them repaint the world per event. `unoui.h`/`unoui.c`
  contain no damage-rect structure at all (checked; the only clip state is
  the draw-time clip window in `fb.c:14-29`).
- Fix: on plain `UI_EV_MOUSE_MOVE` with no capture and unchanged
  hot/hover/popup state, do not set `g_dirty` (the cursor still moves because
  present composites it; present must still run, which the shadow diff makes
  cheap once P1.1's extents exist). Longer term: per-window dirty flags, and
  reuse the existing `gScene` snapshot trick for icon drags and resize.

### P2.2 Aurora theme: soft shadow alpha-blends ~6x the window area, per window, per repaint

- Where: `unoui/themes/theme_aurora.c:39-45` (`soft_shadow`: six expanding
  `fb_round_rect_a(..., alpha 8)` layers, each covering the whole expanded
  window rect), `theme_aurora.c:27-31` (`aa_border` fills the full rect
  twice), blend core `pc64/fb.c:127-157` (`blend_px`: ~6 multiplies + RMW per
  pixel).
- Mechanism: `fb_round_rect_a` at alpha < 255 alpha-blends every pixel of the
  rect (`fb.c:200-214` decomposes into `fb_blend_rect`s). For one 400x500
  window the shadow alone is 6 x ~210k = ~1.3M blended pixels (~10-15 ops +
  read-modify-write each) per repaint; the border fill doubles the face
  writes; buttons/checks/sliders each add more full-rect alpha passes
  (`theme_aurora.c:104-107` paints a button up to 3x). Multiply by every
  window (occluded ones too, P2.1) on every dirty frame. This is why even
  non-drag interactions (typing, clicking) feel slow under the default theme,
  and why the code already grew an "Aurora lite" escape hatch
  (`theme_aurora.c:33-49`).
- Fix: the interior of each shadow layer is fully overdrawn by the next layer
  and finally by the opaque window face; only a <=7 px fringe around the
  window is ever visible. Draw the shadow as four edge strips + four corner
  patches (or precompute a 1-D falloff and blend only the fringe). That takes
  the shadow from O(window area x 6) to O(perimeter x 7). Same trick for
  `aa_border`: fill the 1 px ring, not the whole rect twice.

### P2.3 Present diffs the whole frame on every present

- Where: `pc64/uefi_main.c:1050-1058` (pass 1: for all `FB_H` rows, compare
  `fb` + composited cursor against `gShadow` pixel by pixel until the first
  difference, copy the row if dirty).
- Mechanism: the renderer never reports what changed, so present reconstructs
  damage by brute force: ~2 x 2.07 MB of reads (fb + shadow) at the default
  desktop, ~2 x 8.3 MB at a native-res desktop, on **every** present,
  including drag frames where only the outline moved and 2 Hz caret blinks.
  This is a constant floor of several ms/frame at native res on top of
  everything else. (It is also what makes the design work at all, so it must
  be replaced by information, not just removed.)
- Fix: pass damage down. The shell already knows the drag rects, the caret
  rect, the hovered chip, etc. An `uno_pc64_present_rect(x,y,w,h)` (or an
  accumulated dirty-rect list filled by the `fb_*` primitives at negligible
  cost: min/max of touched extents) lets pass 1 compare only those
  rows/columns, with the current full diff kept as the fallback for `g_dirty`
  full repaints.

### P2.4 (conditional) Blt fallback presents one scanline per firmware call

- Where: `pc64/uefi_main.c:1081-1083` (`gGop->Blt(..., gOutW, 1, 0)` inside
  the per-row loop); `read_mode` at `uefi_main.c:295-297` decides `gUseBlt`.
- Mechanism: when the GOP mode has no usable linear framebuffer, each dirty
  output row is a separate firmware `Blt` call: on a full-height repaint that
  is 1080+ round trips into firmware per frame, each with call overhead.
- Fix: batch contiguous dirty output rows into one Blt call (buffer N rows of
  `gRow` and flush runs). Only matters on machines where `gUseBlt` is set;
  the System window currently gives no clue whether that is the case (see
  Open questions).

---

## P3: significant secondary costs

### P3.1 fb pixel format is opposite to the common GOP format: per-pixel swizzle on every present, and no memcpy fast path at 1:1 scale

- Where: `pc64/uefi_main.c:298-299` (`gSwapRB` is set unless the mode is
  `PixelRedGreenBlueReserved8BitPerColor`; real GOP is virtually always BGRX,
  so the swizzle branch is the common case on metal), `1072-1076` (per-pixel
  rotate + mask per dirty output pixel), `fb.h:46-47` (`FB_RGB` puts R in the
  low byte).
- Mechanism: every presented pixel pays a gather (`gColMap`) plus 3 shifts +
  3 masks + 2 ors. When the desktop resolution equals the mode (the native
  choice), `gColMap[i] == i` and rows could be `memcpy`ed directly if the
  byte orders matched; instead the full swizzle path runs. Bonus
  inefficiency: pass 2 iterates output rows, so at 2x scale each dirty source
  row is rebuilt twice into `gRow` (`uefi_main.c:1066-1079`) instead of being
  reused for the duplicated panel row.
- Fix: store `fb` pixels as 0xAARRGGBB (BGRX memory order) and swap the
  channel order inside `FB_RGB` instead; the swizzle then disappears for BGRX
  GOP modes and pass 2 becomes gather-only (memcpy at 1:1). `FB_RGB` is the
  single constructor; the PPM/host writers are the consumers that care
  (`fb.h:16-19`); the other ports keep their own layout. Also hoist the row
  build: compute `gRow` once per dirty source row and write it to each
  duplicate output row.

### P3.2 TTF text: per-pixel function calls and a per-draw subpixel filter

- Where: `pc64/pc64_font.c:231-268` (`paint_cov`), which calls
  `fb_blend_pixel_sub` (`pc64/fb.c:219-232`) for every covered pixel: a
  non-inlinable cross-TU call that recomputes clip bounds and does 6
  multiplies per pixel. In subpixel mode the 5-tap LCD filter
  (`pc64_font.c:245-253`) is recomputed per pixel on **every draw**, though
  the input coverage is cached and the pen fraction has only 3 phases.
- Mechanism: the glyph raster cache (good: `pc64_font.c:109-168`, banked per
  slot/px/mode) stops stb_truetype from re-rasterizing, but painting remains
  O(pixels) function calls: a full-UI repaint draws tens of thousands of
  glyph pixels (labels, titles, taskbar, lists) at ~20-30 ops each, on every
  dirty frame, multiplied by P2.1's repaint frequency.
- Fix: bake the filtered per-pixel R/G/B alphas (per pen phase, 3 variants)
  into the cache at `render_glyph` time, then paint with a tight local loop
  that hoists the clip test to the glyph bbox and inlines the blend. Cheap
  extra: cache a per-slot `has_kern` flag so `kern26`
  (`pc64_font.c:215-220`) skips the stb call for faces with no kern/GPOS
  data; it currently runs per glyph pair for draw *and* measure.

### P3.3 Window resize repaints the full scene per mouse move (no outline mode)

- Where: `unoui/unoui_input.c:591-604` (`UI_CAP_RESIZE` applies the new size
  and reflows immediately per move); `pc64/pc64_uui.c:1928` (each move then
  triggers the full repaint; the snapshot fast path at `1964-1980` only
  covers `UI.drag_active`, i.e. title-bar moves).
- Mechanism: every mouse move during a resize re-runs the Aurora painters for
  the whole scene (P2.2) plus the full present diff. Resizing feels as bad as
  dragging did before the snapshot trick.
- Fix: rubber-band the resize exactly like the move drag (outline +
  `drag_w/drag_h`, commit and reflow on release), reusing
  `uno_pc64_scene_save/restore` and `unoui_draw_drag_outline`.

### P3.4 Full-screen memcpys where bands would do (drag restore, background cache)

- Where: `pc64/uefi_main.c:120-123` (`uno_pc64_scene_restore`: full-screen
  memcpy per drag frame); `unoui/unoui.c:1003-1010` (`draw_desktop_cached`:
  full-screen memcpy per repaint; correct for full repaints, but it is also
  what every frame of an *icon* drag pays).
- Mechanism: at a native-res desktop these are 9.2 MB copies each; the drag
  restore only needs the rows (bands) the previous outline touched.
- Fix: restore only the previous outline's four bands from `gScene`; that
  turns the restore into a few hundred KB worst case. (With P1.1's extents,
  present stops re-diffing the untouched middle too.)

---

## P4: minor / conditional

### P4.1 Procedural icons and taskbar redrawn from scratch every repaint

- Where: `pc64/pc64_icons.c:77-104` (`disc`/`ring`/`seg` plot through
  `fb_pixel`, which re-derives clip bounds per call, `fb.c:40-44`); desktop
  icons redraw via `pc64_icon_art` on every frame the desktop layer paints;
  `pc64/pc64_uui.c:878-880` (Aurora taskbar: full-width `fb_blend_rect` per
  repaint); `pc64_uui.c:890-916` (chip layout re-measures `fb_text_w`
  several times per chip per paint).
- Mechanism: each emblem is a few thousand `fb_pixel` calls; ~10-20 icons
  plus the bar is order 100k calls per repaint. Real but dwarfed by P2.x.
- Fix: rasterize each emblem once per (icon, size, theme) into a small pixmap
  and `fb_blit` it (`fb.c:87-98` is the right shape for this already).

### P4.2 Idle repaints: caret blink and tray clock force full frames at 2 Hz

- Where: `pc64/pc64_uui.c:1929-1933` (every ~0.5 s idle sets `g_dirty = 1`
  and reformats the clock); full repaint + full present diff follow.
- Mechanism: an idle desktop still renders the whole scene and diffs the
  whole frame twice a second (VRAM writes correctly reduce to the clock text
  and caret rows thanks to the shadow). Wasted CPU/battery, not latency.
- Fix: with damage rects (P2.3) these become caret-rect and clock-chip
  updates. Until then, harmless.

### P4.3 Focused apps that tick set the global dirty flag every frame

- Where: `pc64/pc64_uui.c:1953-1956` (a focused native game sets
  `g_dirty = 1` every frame; bridge apps' `tick` reaches
  `draw_window`/`repaint_all`, which do the same,
  `pc64/pc64_uui_apps.c:113-114`); `pc64_uui.c:1957` (`UI.full` likewise, by
  design).
- Mechanism: a *windowed* focused game, or a bridge app whose tick requests a
  redraw, repaints the entire desktop at frame rate, Aurora shadows included,
  not just its canvas.
- Fix: per-window dirty (repaint just that window's rect once damage rects
  exist). Fullscreen is fine as-is.

### P4.4 mac_compat pattern/oval fills are per-pixel with per-pixel bounds tests

- Where: `pc64/mac_compat.c:149-155` (`FillRect` dither), `187-210`
  (`oval_span`: per-pixel long multiplies, plus 4 neighbor evaluations per
  pixel in frame mode), `160-168` (`put_pen` per-pixel bounds checks).
- Mechanism: only the bridge apps (Paint/Tracker/Music/Network) draw through
  these, inside their canvas rects; cost is bounded by canvas size, but
  Paint's blocking drags call them in a spin that also presents per sample
  (`uefi_main.c:761-768`).
- Fix: row-span loops with hoisted bounds (same shape as `fb_fill_rect`);
  low priority unless Paint specifically feels slow.

### P4.5 Legacy build presents unconditionally every loop iteration

- Where: `pc64/unodos.c:1646-1693` (the legacy `for(;;)` calls
  `uno_pc64_present()` every iteration, no dirty gate, no idle sleep beyond
  present's internal 1 ms).
- Mechanism: a full-frame diff (P2.3) spins continuously. Only affects
  `./build.sh legacy` images, not the shipped shell.
- Fix: gate on a dirty flag like the unoui shell does, if the legacy image is
  still exercised on metal.

---

## Open questions (verify on hardware; could not be confirmed from source)

1. **GOP framebuffer cache attribute (the P1.2 fork).** I looked for MTRR
   reads/writes, PAT (MSR 0x277) programming, `wrmsr`, page-table attribute
   setup, and any "write combining" mention across `pc64/` and found none, so
   the mapping is firmware-inherited and unknown. Verify on the affected
   machine: (a) time fixed-size writes to `gVram` vs `gShadow` and compare
   MB/s; (b) dump `IA32_PAT` and the variable MTRRs and check whether
   `FrameBufferBase` falls in a WC range. If UC, P1.2's MTRR/PAT fix is the
   single biggest win available.
2. **Which desktop resolution the user runs.** Default is half the panel
   (`uefi_main.c:366-371`); the Control Panel offers native. All fb-side
   costs (render, diff, memcpys) scale 4x between the two. Worth asking, and
   worth surfacing the active desktop/mode in the System window.
3. **Whether the slow machine is on the `gUseBlt` path** (no linear FB or an
   odd pixel format, `uefi_main.c:295-297`). Nothing currently reports it.
   If yes, P2.4 dominates. Suggest adding `linear/Blt + WxH + stride` to the
   System window readouts.
4. **Bridge-app tick behavior.** Whether Paint/Tracker/Music/Network actually
   call `draw_window` from `tick` every frame (P4.3) depends on the .UNO
   module code, which I did not audit app by app.
5. **Frame pacing.** Present paces with a 1 ms stall (`uefi_main.c:1092`) and
   the idle loop with 16 ms; there is no vsync anywhere (GOP has none), so
   the slowness is not pacing-related, but firmware `Stall` granularity on
   some boards exceeds 1 ms while attached; worth a quick TSC check if drag
   frame rate looks quantized.
