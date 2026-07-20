# Round 2 - pointer accel: X1 "wacky" + Surface "floaty" (2026-07-20)

Both pads are HID-over-I2C, but the cursor can take two very different paths, which
is why the two machines show different symptoms:
- **Native I2C-HID** (`uefi_main.c:889-913`): abs pad coords -> `rel_from_abs()`
  deltas -> integer sub-pixel accumulator. No EMA. (X1 is here.)
- **Firmware EFI_ABSOLUTE_POINTER fallback** (`uefi_main.c:918-948`): used when native
  did not bind. Maps pad position absolutely onto the screen, then a 0.75 EMA filter.
  (Surface is here.)

## X1 Carbon - "acceleration completely wacky"

### A1 (CONFIRMED, top cause) - signed int32 overflow in the delta scaler
`pc64/uefi_main.c:901-902`:
```c
accx += dx * uno_fb_w * g_ptr_speed / 100;
```
All operands are `int`, so `dx * uno_fb_w * g_ptr_speed` is evaluated in 32-bit
before `/100`. With `dx` clamped to +-4000, `g_ptr_speed` default 300, `uno_fb_w`
capped at FB_MAX_W=1920: `4000 * 1920 * 300 = 2,304,000,000 > INT_MAX
(2,147,483,647)` -> signed overflow (UB), wraps large-negative -> the cursor lurches
backward hundreds of px. Verified arithmetic first-hand.

Machine-specific: overflow onset at 1920-wide desktop is `dx >= floor(2^31 /
(1920*300)) = 3729` (about 11.4% of the pad, inside the 4000 clamp). The Surface's
desktop is <=1536 wide, giving a threshold of 4660 which is ABOVE the 4000 clamp, so
the Surface never overflows. That is why only the X1 is wacky: fast/large flicks
fling the cursor the wrong way while slow moves are fine.
**Fix:** 64-bit intermediate: `accx += (int)((long long)dx * uno_fb_w * g_ptr_speed
/ 100);` (accumulated result is small, only the product overflows).

### A2 (CONFIRMED, contributing) - +-4000 delta clamp drops fast moves
`pc64/i2c_hid.c:559-562` zeroes the whole delta when any axis exceeds 4000
pad-units (12.2% of pad). Polling is synchronous in the main loop, so when the frame
loop is busy the per-poll displacement grows and a brisk flick is discarded -> cursor
stalls on fast moves. This clamp is also exactly what lets the overflowing dx range
(3729-4000) reach A1. Fix: lower the clamp and fix A1, or scale before clamping.

### A3 (SUSPECTED, minor) - anisotropic per-axis normalization
`i2c_hid.c:589-590` normalizes X and Y independently to 0..32767, then
`uefi_main.c:901-902` scales dx by `uno_fb_w` and dy by `uno_fb_h`. With the pad's
X/Y maxima and physical dims differing and fb_w != fb_h, horizontal vs vertical gain
differ -> diagonal motion curves. Adds to the wacky feel, not the primary defect.

## Surface Laptop Go - "unusably floaty / drifts / overshoots"

### B1 (top cause) - a relative touchpad mapped ABSOLUTELY, then lag-filtered
`pc64/uefi_main.c:927-930` maps the pad's absolute position straight onto the screen
(`sx = (CurrentX-MinX)*gModeW/rx`, then panel->fb). A precision touchpad is a
relative device; absolute mapping makes the small pad span the whole display
(hypersensitive) and teleports the cursor to wherever the finger lands. The native
path deliberately avoids this via `rel_from_abs()` (`i2c_hid.c:546-564`), but the
firmware-Abs path never got that treatment. Then `uefi_main.c:940-943` runs a 0.75
EMA (`g_cx = (g_cx + tx*3)/4`) = visible glide/lag + a permanent ~1px integer-trunc
offset, alternating with a 24px snap = imprecise + overshoot. The author's own
comment (931-937) already attributes "floaty on the Surface's precision touchpad" to
this path. **Fix:** convert firmware-Abs to relative deltas like `rel_from_abs()` and
drop the EMA, or make native I2C-HID actually bind on the Surface.

Caveat: the Surface Laptop Go also has a touchscreen. If `gAbs[i]` is the touchscreen
(genuinely absolute), the mapping is correct and the fault is the EMA alone (or B2).
The author's comment identifies gAbs as the touchpad, so B1 leads; confirm on metal
which device gAbs is.

### B2 (ALTERNATIVE, if native I2C-HID does bind on the Surface) - tip-switch not parsed -> resting drift
`i2c_hid.c:588`: when the parser does not locate a Digitizer Tip Switch
(`tip_off < 0`), `tip` is forced to 1 (always touching). `rel_from_abs()` then never
resets `have_last` on lift, so a resting finger's micro-jitter (+-1-3 units) keeps
producing tiny deltas the accumulator integrates -> slow drift while resting. Verify
on metal whether `parsed==1` and `tip_off>=0` for the Surface pad (via
`uno_i2c_hid_status`).

## Why the symptoms are cleanly disjoint
- The overflow (A1) cannot occur on the Surface (narrower desktop keeps the product
  < INT_MAX) -> Surface is not wacky.
- The EMA/absolute-map (B1) never runs on the native path -> X1 is not floaty.

## Confirmed vs suspected
- Confirmed: A1 (arithmetic exact), A2 (clamp discards fast moves), B1 (absolute
  map + EMA, plus the author's own comment).
- Suspected / metal-dependent: A3 magnitude, B2 (depends on native binding + tip
  switch), and the gAbs touchpad-vs-touchscreen identity on the Surface.
