# uno3d performance review: findings

Scope, read first-hand: `uno3d/uno3d.c`, `uno3d_soft.c`, `uno3d_intel.c`,
`uno3d_game.c`, `uno3d_demo.c`, `uno3d_int.h`, `uno3d_backend.h`, `uno3d.h`; pc64
consumers `pc64/apps/runner.c`, `pc64/pc64_games.c` (rn_* at 505-553),
`pc64/pc64_uui.c` (main loop 1907-1988), `pc64/uefi_main.c` (present 1040-1093,
lowres 440-451), `pc64/fb.c/.h`, `pc64/pc64_math.c`, `pc64/build.sh`, `unoui/unoui.c`.

## Summary

pc64 has no hardware 3D path: `u3d_backend_intel` is an honest soft fallback
(uno3d_intel.c:31-49), so every Runner frame is CPU-rasterised into the RAM
backbuffer `fb[]` at 1/4 panel resolution (uno_pc64_lowres, uefi_main.c:444), then
upscaled and written to the panel at FULL panel resolution every frame by
`uno_pc64_present()`. The two dominant per-frame costs are (1) the present pass,
which builds and stores about 2M panel pixels per frame at 1920x1080 into VRAM whose
caching attribute the OS never configures, and (2) the brute-force rasteriser inner
loop, which evaluates three float edge functions from scratch for every pixel of
every triangle bounding box. On top of that, the near-plane REJECT (there is no near
clip) produces two symptoms: the corridor floor/ceiling slabs likely render nothing
at all, and triangles that barely survive the reject project to near-infinite
coordinates whose clamped bounding box is the whole screen, causing frame-time
spikes exactly when a wall reaches the player. Finally, the game has no delta-time,
so any slowness also plays in slow motion, which makes it feel far worse. PS2/DC
avoid all of this because the GS/PVR rasterise and present is a flip.

## Non-problems (credit where due, do not "fix")

- No per-pixel divide: the only perspective divide is per vertex (uno3d.c:155).
- Back-face cull exists (uno3d.c:168-170); depth test rejects before the colour
  math (uno3d_soft.c:76).
- No per-frame heap allocation, no sorting, no O(n^2) geometry anywhere in uno3d/.
- On pc64, float is hardware SSE (mingw x86-64 ABI, OVMF guarantees SSE enabled).
- uno3d draws into `fb[]`, plain cached RAM, never into GOP MMIO directly.

## Findings

### P1-1: full-panel present every frame, VRAM memory type never configured
`pc64/uefi_main.c:1066-1088`, geometry `apply_desktop` (308-349), lowres 440-451.

Runner renders at fb = panel/4, but present pass 2 iterates OUTPUT rows (full panel
height) and writes gOutW pixels per row, so every 3D frame builds and stores the
full panel (1920x1080 = 2.07M px). Three compounding problems:

1. Memory type: nothing in pc64 programs MTRRs or PAT (grep for mtrr/pat/
   write-combin across pc64/ finds nothing), so the GOP framebuffer keeps whatever
   attribute the firmware left. If that is UC (common on real laptops), each of the
   2.07M stores is a serialised uncacheable transaction: order 100 to 500 ms/frame
   (2 to 8 fps, a slideshow). If WC, this pass is a tolerable 3 to 8 ms. This single
   unknown spans "fine" to "unusable" and is the first thing to measure.
2. Redundant row builds: consecutive output rows map to the same source row at 4x
   upscale, yet gRow is rebuilt per OUTPUT row. Reusing gRow while `sy` is unchanged
   cuts the build side by ~4x for free.
3. `gUseBlt` machines issue one firmware `gGop->Blt()` per OUTPUT ROW (1082): ~1080
   protocol calls per frame. Catastrophic on such a machine.

Fix: (a) add a TSC frame-time readout to the Runner HUD to split raster vs present
cost on metal; (b) reuse gRow across output rows with the same source row; (c) map
or MTRR the GOP framebuffer as WC after detach (the kernel owns the machine then);
(d) batch contiguous rows into one Blt on the gUseBlt path.

### P1-2: rasteriser recomputes three full edge functions per pixel
`uno3d/uno3d_soft.c:64-88`, `edge()` at 37-40.

For every pixel in the triangle bounding box the loop calls `edge()` three times
from scratch (per pixel: ~6 mul + 12 add/sub for coverage, + 3 mul for `* inv`
normalisation, + 3 mul for z, + 9 mul for colour when the depth test passes, all
scalar float). The edge functions are affine in px/py, so w0/w1/w2/z/r/g/b can all
be stepped by a constant add per pixel and per row. `* inv` (68-70) is wasted on the
coverage test since only the sign matters. Runner draws ~450 tris/frame with heavy
overdraw. This is the second dominant term and the reason lowres was needed at all.

Fix: incremental half-space rasterisation (precompute dE/dx, dE/dy per edge once;
inner loop becomes three adds + sign test); keep z and r/g/b as stepped
accumulators (fold `inv` into the deltas). Expected 5 to 10x. Add a per-row
early-out once the span exits the triangle.

### P2-1: near-plane reject, no clip: missing geometry AND full-screen sliver bboxes
`uno3d/uno3d.c:153` (reject), `uno3d_game.c:191-192` (floor/ceiling slabs).

Correctness half: any triangle with ANY vertex at cw < 0.0001 is dropped whole.
Every face of the floor/ceiling slabs except the far -Z face contains a vertex
behind the camera, and the far face is back-face culled, so the slabs likely
contribute zero pixels on every backend (the corridor floor/ceiling is silently
lost). Walls also pop out entirely a little before reaching the camera.

Performance half: a vertex that barely survives (cw just over 0.0001) gets inv up to
1e4, so sx/sy explode to 1e5-1e6. The soft backend clamps the bbox to the screen
(uno3d_soft.c:59-62), so such a sliver triangle costs a FULL-SCREEN bbox scan
(~130k px x 3 edge evaluations) while covering almost nothing. Walls crossing the
player plane generate dozens of these for a few frames, so frame time spikes exactly
at the gameplay-critical moment.

Fix: real near-plane clipping (Sutherland-Hodgman against w = eps) in u3d_triangles
before the divide. Fixes both halves. Cheaper stopgap for the spike only: reject
triangles whose screen-space bbox area far exceeds their signed area.

### P2-2: four redundant full-frame passes around each 3D frame
- `unoui/unoui.c:1026`: fullscreen render fills the whole fb black, then `u3d_begin`
  immediately repaints every pixel via `soft_clear` (uno3d_soft.c:30-35).
- `pc64/uefi_main.c:1050-1058`: present pass 1 compares the entire fb against
  gShadow every frame to discover what a fullscreen 3D client already knows:
  everything is dirty. (Matches G-LEAD-1.)

~4 extra full-fb passes = ~0.5M pixel touches per frame at lowres, 1 to 3 ms.

Fix: skip the black fill when the fullscreen canvas is opaque (a canvas flag); add a
full-invalidate present hint that skips pass 1 and marks every row dirty; split
soft_clear into two tight loops so both vectorise.

### P2-3: all-float pipeline on FPU-less family ports
`uno3d_int.h:10-13`. On Amiga/MacPlus/8088-class targets every per-pixel float op is
a soft-float call. Not the pc64 problem, but the same inner loop is the family-wide
fallback. Fix: a fixed-point variant (x/y in 28.4, z/colour in 16.16) composes with
the P1-2 stepping; gate per backend so pc64/PS2/DC keep float if faster.

### P3 minor
- No hardware backend on pc64 (uno3d_intel.c:31-49 delegates to soft); architectural.
- No delta time (uno3d_game.c:147-158): game speed is frame-rate coupled, so slowness
  reads as slow motion. Fix: fixed-timestep updates.
- No x/y frustum reject before queueing (uno3d.c:135-178); MVP recomputed per call
  (~10k mults/frame, negligible).
- `soft_clear` interleaves fb and z stores (defeats vectorisation); float z-buffer
  could be u16; 1 ms present tail delay (uefi_main.c:1092).

## Suggested order
1. Measure (TSC frame-time + present-time HUD) to split P1-1 from P1-2 on metal.
2. P1-1 b/c: gRow reuse, then confirm/force WC on the framebuffer.
3. P1-2: incremental edge stepping + stepped z/colour + row early-out.
4. P2-1: near clipping (fixes the spikes and the missing floor).
5. P2-2: opaque-canvas + full-invalidate hints. 6. P3-2 delta time, then cleanup.

## Open questions (need metal, do not treat as confirmed)
- VRAM caching attribute on the real laptops (UC vs WC decides 5 ms vs 500 ms
  present). No MTRR/PAT code exists to force it.
- Where was "very slow" observed? `pc64/harness.py` runs QEMU with no accel flag
  (pure TCG), guaranteeing single-digit fps regardless of any fix. Metal numbers may
  be substantially better.
- Does any supported machine actually take the gUseBlt per-row path?
- Floor/ceiling invisibility (P2-1): verify against a PS2/DC screenshot before
  treating as confirmed.
