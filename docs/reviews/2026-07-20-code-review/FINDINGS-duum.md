# Duum performance review (pc64 MicroPython) - 2026-07-20

Scope: `pc64/apps/DUUM.PY`, `pc64/apps/pyrt.c`, `pc64/upy_port/mod_uno.c`,
`pc64/upy_port/mpconfigport.h`, `pc64/upy_port/mkupy.py`, `pc64/fb.c`,
`pc64/pc64_uui.c` (shell loop + PYRT hosting), `pc64/uefi_main.c` (present +
input), `pc64/usbhid.c` / `pc64/hid_kbd.h` (key repeat), `pc64/tools/duum_host.py`
(host mirror), commit `df90641` (the cv.wall_col fast path).

## Summary

Duum is slow for four stacked reasons, and the `cv.wall_col` fast path
(df90641) only removed one layer of the problem (per-pixel texturing in
Python). What remains:

1. The geometry/column renderer (`render()`) runs fully interpreted with
   boxed single-precision floats. Under `MICROPY_OBJ_REPR_A` every float
   intermediate is a 16-byte GC heap allocation; the per-column loop makes
   roughly 20 to 30 of them per column, over hundreds of column iterations and
   about 730 seg transforms per frame. That is on the order of 10^5 heap
   allocations and around 10^6 bytecode dispatches per rendered frame. The x64
   native emitter is compiled in specifically for this renderer
   (mpconfigport.h says so) but DUUM.PY never uses `@micropython.native` or
   `@micropython.viper`.
2. The shell replays the whole scene at maximum loop rate forever: `tr_frame`
   unconditionally dirties the shell every iteration, which triggers a full
   desktop repaint plus a full Python `draw()` display-list replay (about a
   thousand Python-to-C calls and ~10^5 per-pixel writes) even when the player
   is standing still. This is also a 100 percent CPU busy loop because the
   dirty flag defeats the idle 16 ms sleep.
3. `cv.wall_col` still pays per-pixel C overhead: every pixel goes through an
   indirect thunk call to `fb_pixel`, which recomputes the clip window and the
   row address per pixel, plus a `%` division per pixel. It is also called
   once per canvas pixel column (2 to 3 times per internal column) redrawing
   identical texel columns.
4. Movement is gated on key autorepeat that mostly does not exist on device:
   the native HID keyboard path emits key-down edges only (and SET_IDLE(0)
   disables device repeat), so holding an arrow moves exactly one step per
   physical press; the PS/2 path gets hardware typematic at roughly 10.9 Hz
   after a 500 ms delay. Combined with a slow render per step, the game feels
   crawling regardless of raw render speed.

The fastest path to "playable": stop the unconditional replay (P1-2), move the
column loop to viper or C (P1-1), batch the display list into one C call
(P2-2), and add held-key continuous movement (P3-1).

## Findings

### P1-1  Interpreted, float-boxed per-column render loop
`pc64/apps/DUUM.PY:338-378` (inner column loop), `:299-337` (per-seg
transform), `pc64/upy_port/mpconfigport.h:35,38,79`.

Mechanism: `MICROPY_OBJ_REPR_A` (mpconfigport.h:79) boxes every float on the
GC heap; `MICROPY_FLOAT_IMPL_FLOAT` does not change that. The column loop at
DUUM.PY:338 does per iteration: `t`, `invd`, `dist`, `u`, `df`, `sh`, `shw`,
`dv`, `yfc`, `yff` (plus `ybc`/`ybf` on two-sided segs), each of which is one
or more float allocations and several bytecode dispatches, plus closure calls
into `rect()`/`wall()` which allocate a tuple per op. Per frame that is
hundreds of column iterations (columns are touched by multiple segs until
occluded) on top of about 730 `draw_seg` calls (E1M1), each doing a
`math.sqrt`, 8 multiplies, and the `seg_sectors` tuple dance even when the seg
is then rejected. Order of magnitude: 5x10^4 to 10^5 heap allocations and
~10^6 dispatched bytecodes per rendered frame. On a bare-metal MicroPython at
a few million boxed-float ops per second that alone is tens of ms to >100 ms
per frame, before drawing anything.

The config was written for exactly this case: mpconfigport.h:6-7 says "The
x64 native emitter is on so @micropython.viper compiles hot loops (Duum's
renderer) to machine code", and `upy/py/emitnx64.c`/`asmx64.c` are compiled
into PYRT (build.sh:214 pulls in all of `upy/py/*.c`). DUUM.PY contains zero
decorators. The intended optimization was never applied.

Fix: rewrite the column loop (and ideally `draw_seg`) as a
`@micropython.viper` function using .16 fixed-point ints and precomputed
tables (avoid recomputing `t` per column by stepping `uz` and `invd`
incrementally; the two divisions per column can become one reciprocal step).
Viper locals are unboxed machine ints, which removes the allocation storm
entirely. See the ABI caveat in Open questions before relying on viper; the
fallback is to move the loop into a C helper in mod_uno.c (pass the seg span
parameters, let C emit into the display list).

### P1-2  Unconditional full-scene replay every main-loop iteration
`pc64/apps/pyrt.c:151-154`, `pc64/pc64_uui.c:1493,1940-1941,1983-1984`,
`unoui/unoui.c:1015-1040`, `pc64/apps/DUUM.PY:403-421`.

Mechanism: `tr_frame` calls `pc64_shell_dirty()` every time the shell pumps
the Python app's frame hook, whether or not `tick()` changed anything
(pyrt.c:153). `pc64_shell_dirty` sets `g_dirty` (pc64_uui.c:1493), so the main
loop takes the `unoui_render_ui + uno_pc64_present` branch every single
iteration (pc64_uui.c:1983) and never reaches the 16 ms idle sleep on line
1984. `unoui_render_ui` repaints the desktop (alpha-heavy Aurora theme) and
every window, which invokes `py_canvas_draw` -> Duum's `draw()`, which replays
the entire cached display list: `cv.clear`, hundreds of `cv.fill_rect` calls,
and one `cv.wall_col` per wall pixel column (DUUM.PY:412-420), roughly a
thousand Python-to-C calls and ~10^5 `fb_pixel` writes. All of this runs
continuously at maximum loop rate while the player stands still and
`self.dirty` is False. The present-side shadow diff (uefi_main.c:1050-1062)
stops the VRAM write, but only after the full repaint and a full-framebuffer
compare (up to 1920x1200 = 2.3M pixel compares) have already been paid.

Cost: this is why the whole desktop feels sluggish while Duum is open, why
input-to-photon latency is bad (a keypress queues behind a full wasted
repaint), and why the machine sits at 100 percent CPU.

Fix: make the app tell the shell when it needs a repaint. Cheapest: have
`tr_frame` only call `pc64_shell_dirty()` when the Python `tick()` returns a
truthy value, and return the was-dirty state from Duum's `tick()`. (Audit the
other PYAPPs for reliance on the current always-dirty behavior; a
`cv.invalidate()` binding is the explicit alternative.) Separately, cache the
canvas pixels: render Duum's frame once into an offscreen buffer and blit it
on replay (needs an `fb_blit`-backed `cv` surface), so even legitimate desktop
repaints do not re-run the Python display list.

### P2-1  cv.wall_col pays per-pixel call, clip, multiply and modulo costs
`pc64/upy_port/mod_uno.c:129-135`, `pc64/fb.c:40-44`, `pc64/apps/DUUM.PY:416-420`.

Mechanism: the fast path's inner loop calls `fb_pixel(x, y0+i, px)` per pixel.
`fb_pixel` is a kernel export reached through a generated thunk (build.sh:237,
`pyrt_thunks.s`), so each pixel is an indirect cross-module call; inside,
`clip_bounds()` re-derives the clip rect (4 compares + 4 stores), performs a
bounds test, and computes `y * FB_W + x` from scratch. The loop also does
`(v >> 8) % th` (an integer division) per pixel. At a typical scene with walls
covering half of the 520x380 canvas that is ~10^5 pixels per replay, so
roughly 10^5 indirect calls + divisions per replay, several ms of pure
overhead, multiplied by the P1-2 replay rate.

Second layer: `draw()` calls `wall_col` once per canvas pixel column, looping
`wpx` times (DUUM.PY:417-420) where `wpx = cw // RW` is 2 at 520 wide. Each
duplicate call re-walks and re-shades an identical texel column; the C side
also redoes `texcol % tw` and argument unmarshalling (11 `mp_obj_get_int` +
2 `mp_get_buffer_raise` per call).

Fix: add a kernel export that draws the whole column run directly, for example
`fb_wall_col(x, w, y0, count, grid, tw, th, texcol, v0, dv, pal, sh)`: clip
once, take `fb + y0*FB_W + x`, step by `FB_W` per row, write `w` pixels per
row (one shade computation per row, reused across the span), and replace
`% th` with a mask when `th` is a power of two (stock Doom wall textures are
128 high) falling back to wrap-by-subtract (`if (v >= th<<8) v -= th<<8`,
valid because `dv` steps are small). Then give `cv.wall_col` the width
parameter so Python makes one call per internal column, not per pixel column.
That removes both the per-pixel indirect call and the 2-3x duplication.

### P2-2  About a thousand Python-to-C draw calls per replay
`pc64/apps/DUUM.PY:403-421`, `pc64/upy_port/mod_uno.c:65-71,112-138`.

Mechanism: the display list is replayed op by op from bytecode: per op a
method lookup in the canvas `locals_dict` (mod_uno.c:145), tuple unpacking of
12 fields for 'W' ops (DUUM.PY:416), a `range(wpx)` object, and per-argument
integer boxing/unboxing. With ~600-900 ops per frame this is ~10^3 crossings
plus the surrounding bytecode, every replay.

Fix: emit the display list into a preallocated `bytearray` (fixed-width
records via `struct.pack_into`, or plain int stores from viper) during
`render()`, and add one C entry point `cv.run_ops(buf, n, pal)` that walks the
records and dispatches to `fb_fill_rect` / the new `fb_wall_col` internally.
One crossing per frame; the texture `grid` objects can be registered in a
small table passed once. This also deletes the per-op tuple allocations
(DUUM.PY:281,291,296) feeding P3-3.

### P2-3  Per-frame re-derivation of static seg data (strings, tuples, dict lookups)
`pc64/apps/DUUM.PY:118-121,233-241,330-332`.

Mechanism: every `draw_seg` call, every frame, does `seg_sectors` (6 list
indexings, builds a result tuple) and three `self.tex.get(_texname(...))`
calls. `_texname` allocates a `str` via `.decode("latin1")` and `get`
allocates another via `.upper()`, then a dict probe; that is up to 6 string
allocations plus 3 dict lookups per seg per frame, times ~730 segs: ~4000
allocations and ~2200 dict probes per frame for data that never changes.

Fix: precompute a per-seg render record at level load (front/back floor and
ceiling heights, light, resolved texture tuples or None, `u1` base, `wlen`,
vertex coords, the contrast pick), stored as flat lists (or one array.array of
ints plus a parallel texture list). `draw_seg` then reads plain locals. This
is also the natural shape to hand a future C/viper seg renderer.

### P3-1  No held-key movement; step rate is hostage to autorepeat
`pc64/apps/DUUM.PY:439-462`, `pc64/hid_kbd.h:7-17`, `pc64/usbhid.c:48,111-124`,
`pc64/uefi_main.c:814-833`, `pc64/pc64_uui.c:1759-1762`.

Mechanism: Duum moves only inside `key()`, one MOVE (12 units) or one TURN
(0.2 rad) per delivered key event. On the native HID path (USB or I2C-HID
keyboards) `hid_kbd` diffs each boot report against the previous and emits
key-DOWN edges only, and `set_boot` sends SET_IDLE(0) which turns off device
auto-repeat, so holding Up yields exactly one event: one step per physical
press. On the detached PS/2 path the hardware typematic gives ~10.9 events/s
after a 500 ms delay. Either way "walkable" is a crawl, and each step then
waits on a full P1-1 render. This is very likely a large share of the
perceived slowness on device.

Fix: track held state. The platform already has it (hid_kbd_state.prev holds
the current down set; PS/2 break codes exist on that path); expose a
`uno.keys_down()` (bitmask of arrows/strafe) or per-key up/down events to
Python, then integrate movement in `tick()` scaled by measured frame dt so
speed is renderer-independent. Until the platform API exists, a Python-only
mitigation is impossible; this needs the C-side addition.

### P3-2  BSP walk visits the whole map with no occlusion early-out or bbox culling
`pc64/apps/DUUM.PY:380-395,299-325`.

Mechanism: `walk()` recurses front-to-back over every node and calls
`draw_seg` for every seg of every subsector (~730 segs, ~240 subsectors on
E1M1). Rejection happens only per seg (behind-near, backfacing via
`rx1 >= rx2`, and per-column `ct > cb`), so a fully occluded far half of the
map still pays the transform + `seg_sectors` + texture lookup cost per seg.
Doom's own renderer stops when all columns are closed and culls node bounding
boxes; the NODES bboxes are already parsed (`<hhhh8hHH`, DUUM.PY:106,
`nd[4..11]`) and unused.

Fix: keep a count of open columns (decrement when a column closes at
DUUM.PY:360/378); return from `walk` when it hits zero. Optionally add the
node bbox vs view frustum test before recursing into a child. Both are cheap
in Python and typically cut seg work by 2-4x in indoor scenes.

### P3-3  GC churn and full-heap collection pauses
`pc64/pc64_uui.c:154`, `pc64/upy_port/mpconfigport.h:21-22`, findings P1-1/P2-3.

Mechanism: the 16 MB GC heap (`g_pyrt_gc`) with no runtime threshold set means
collections happen only when the heap is exhausted. At ~10^5 allocations per
rendered frame (floats, op tuples, strings) the heap fills after some tens of
frames, then a full conservative mark/sweep walks the ~1M-block allocation
table in one go: a periodic hitch plausibly in the tens of ms that reads as
stutter. `MICROPY_GC_ALLOC_THRESHOLD (1)` only compiles the feature in; nobody
calls `gc.threshold()`.

Fix: the real fix is removing the churn (P1-1, P2-2, P2-3). If any churn
remains, set a threshold (e.g. `gc.threshold(2*1024*1024)` after build) so
collections are smaller and more frequent, and consider whether 16 MB of
kernel BSS for the heap is even needed once the renderer stops allocating.

### P4-1  Column-granular rect ops for ceiling/floor/sky
`pc64/apps/DUUM.PY:354-357,397-400`.

Ceiling, floor, and sky are emitted as one 2-3 px wide fill_rect per internal
column, so flat areas that could be a handful of horizontal spans become
hundreds of ops (each a Python-to-C crossing on replay and a tuple in the
list). Merging adjacent columns with equal color and y-extent at emit time
would shrink the display list severalfold. Subsumed by a C display-list
executor (P2-2) but worth doing even with one.

### P4-2  Full-framebuffer diff scan per present
`pc64/uefi_main.c:1050-1062`.

`uno_pc64_present` pass 1 compares every framebuffer pixel against the shadow
copy on every present (up to 1920x1200 = 2.3M compares at native panel
resolution). Well-designed for idle frames, but under P1-2's forced repaint it
runs at maximum loop rate. Once P1-2 is fixed this cost mostly disappears; a
dirty-rect hint from the render path would be the further step if needed.

## Assessment of df90641 (the cv.wall_col fast path)

The commit moved the per-pixel texture sample + shade + palette + write out of
bytecode, which was necessary and real: before it, texturing would have been
~10^5 interpreted iterations per frame. But it addressed only the innermost
layer. It left (a) per-pixel cost inside C via the `fb_pixel` thunk, clip
recompute and divide (P2-1), (b) 2-3x duplicate column draws from the `wpx`
loop (P2-1), (c) the entire interpreted geometry/column pass with float boxing
(P1-1), and (d) the unconditional full replay each loop iteration (P1-2),
which multiplies whatever draw cost remains. Net: the bottleneck moved, it did
not go away.

## Open questions

1. Does the x64 native emitter actually work on this build? emitnx64/asmx64
   generate code with the SysV AMD64 calling convention (args in
   rdi/rsi/rdx/...), but PYRT is compiled with x86_64-w64-mingw32-gcc
   (MS x64 ABI: rcx/rdx/r8/r9). Generated native code calls C helpers through
   mp_fun_table; if those calls are emitted SysV against MS-ABI-compiled
   helpers they will misplace arguments. Additionally, viper/native code is
   allocated from the GC heap (`g_pyrt_gc`, kernel BSS): it must be mapped
   executable. Both need an on-device smoke test (a trivial
   `@micropython.viper` add function) before investing in the viper route. If
   either fails, the C-helper route (P2-1/P2-2 exports) is the safe path and
   delivers most of the same win.
2. No on-device timing exists: nothing measures where a frame's time actually
   goes (render vs replay vs shell repaint vs present). A TSC-based
   `uno.ticks_us()` export plus three timestamps would confirm the split this
   review estimates; recommended before and after each fix lands.
3. Seg/subsector counts (~730 segs, ~240 ssectors for E1M1) are the shareware
   WAD's known figures, not counted from the file on the test disk; freedoom1
   differs somewhat. Does not change any conclusion.
4. Whether the slowness report is from QEMU TCG or bare metal. Under TCG the
   interpreter is easily another order of magnitude slower, which would make
   the P1 items look even worse than the estimates here; on metal (X1 Carbon
   class) the estimates above should hold.
5. Behavior of other PYAPPs if `tr_frame` stops dirtying unconditionally
   (P1-2 fix): any Python app that animates in `draw()` without a `tick()`
   signal would stop animating. Needs a quick audit of shipped .PY apps and
   the Studio-run path before changing the contract.
