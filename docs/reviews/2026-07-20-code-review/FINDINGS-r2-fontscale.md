# Round 2 - Notepad "fuzzy" text: root cause (2026-07-20)

**Not a Notepad bug and not double-scaling in Notepad. It is the general fractional
nearest-neighbour present upscale in `apply_desktop()`. Notepad merely exposes it
worst, being the app with the densest field of small body text.** Confirmed
first-hand.

## Ruled out (confirmed)
- No offscreen double-resample in Notepad: `notepad_draw()` (`apps/notepad.c:48-113`)
  draws straight into shared `fb[]`.
- Notepad uses the same font path as Files/Settings (`text_at` in
  `pc64_uui_apps.c:39-48` is `MoveTo`+`DrawText`, identical). So no Notepad-specific
  size/font bug.
- Default system font is Chicago (bitmap, AA off at 100%), crisp 1:1.

## Confirmed root cause: fractional fill-scale in `apply_desktop()`
`pc64/uefi_main.c:308-361` (verified first-hand). The desktop `fb[]` is upscaled to
the panel by a scale kept **fractional** in 16.16 (line 330:
`sc = min(modeW<<16/fbw, modeH<<16/fbh)`), then turned into per-pixel
nearest-neighbour maps `gColMap[i] = i*fbw/gOutW` (341-347), applied every present
(`1066-1088`). When `sc` is non-integer, the maps duplicate some source columns/rows
and not others; on glyph stems that reads exactly as "rendered at low res then
upscaled poorly."

When it goes fractional:
- Boot default is `apply_desktop(gModeW/2, gModeH/2)` = exact 2x, sharp. (Note: X1
  1920x1200 and Surface 1536x1024 both give exactly 2x at half-res, so at pure
  default they are sharp; the user most likely selected a non-default resolution, or
  runs a face/UI-scale that engages the AA aggravator below.)
- Any Settings resolution pick routes through `uno_pc64_res_set` -> `apply_desktop`
  (`427-435`); the comment at line 423 admits "fractional fill: no integer zoom
  label." Examples: X1 1920x1200 picking 1280x800 = 1.5x; 1600x900 = 1.2x. Surface
  1536x1024 picking 1280x800 = 1.2x; 1024x768 = 1.333x. Any 4K panel is capped to
  FB_MAX 1920x1200 so even "native" is ~1.8x.

## Secondary aggravator (bites even at 2x)
Subpixel LCD AA is on by default (`pc64_uui.c:1891`; filter `pc64_font.c:231-255`).
Coverage is computed for a 1:1 physical-subpixel grid, then the present upscale
replicates it, so at ANY scale (even 2x) the R/G/B coverage lands on the wrong
physical subpixels = colour fringing. Only active when an outline face (Sans/Mono)
or UI-scale != 100% is in use (Chicago at 100% has AA off), so it worsens fuzz on
non-default setups rather than causing it.

## Separate real Notepad bug (not the fuzz)
`TextWidth` returns hardcoded `count*8` (`mac_compat.c:225-226`) but `DrawText`
advances by real TTF/Chicago glyph widths. Notepad uses `TextWidth` for line-wrap
and caret X (`apps/notepad.c:77,92`), so the caret and wrap drift from the drawn
glyphs on any non-8px-wide font. Cosmetic/interaction bug.

## Fix (ranked)
1. **Immediate, matches the symptom: integer-only upscale.** After `sc` at
   `uefi_main.c:330`, floor to a whole number when >= 1.0:
   `if (sc >= (1ULL<<16)) sc &= ~0xFFFFULL;` Centering (`gOffX/gOffY`) and surface
   pre-clear already exist, so the only change is small centred borders instead of
   fuzz; every glyph becomes pixel-exact at every resolution. Two-line change.
2. **Long-term: run the desktop at panel resolution (1:1 present) and scale the UI
   via font px / `uno_font_set_ui_scale`.** Removes the nearest-neighbour stage
   entirely. Bigger change.
3. **Grayscale AA (not subpixel) whenever present scale != 1**, so AA stops fighting
   the upscale.
