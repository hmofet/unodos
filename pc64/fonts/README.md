# Bundled fonts

The pc64 TrueType engine (`pc64_font.c`, built on the vendored
`stb_truetype.h` — public domain / MIT) loads these at runtime from the ESP
by `build.sh`. All are open-licensed and redistributable:

| File            | ESP name      | Face | License |
|-----------------|---------------|------|---------|
| `ChiKareGo2.ttf`| `CHICAGO.TTF` | ChiKareGo 2 (a Chicago-style bitmap face) by **Giles Booth** | **Creative Commons Attribution (CC BY)** — see below |
| `Sans.ttf`      | `SANS.TTF`    | DejaVu Sans | Bitstream Vera / DejaVu (permissive) |
| `Mono.ttf`      | `MONO.TTF`    | DejaVu Sans Mono | same |
| `Ubuntu.ttf`    | `UBUNTU.TTF`  | Ubuntu | Ubuntu Font Licence 1.0 |

**ChiKareGo 2 is the default UI face** (shown as "Chicago" in the Control
Panel font picker). It's a bitmap-derived TrueType, so `pc64_font.c` renders
it at its native 15 px with anti-aliasing off — crisp 1:1 pixels, the classic
chunky Mac look. The outline faces (DejaVu/Ubuntu) render at the UI px with
subpixel AA. The built-in 8×8 bitmap font (`font_data.h`) is the fallback if a
TTF can't be loaded (also selectable as "System (mono)").

## ChiKareGo 2 attribution (CC BY)

ChiKareGo 2 © Giles Booth, licensed **Creative Commons Attribution** — a
free-culture homage to Apple's *Chicago* (it is NOT Apple's Chicago, which is
proprietary and not bundled). Source: BitFontMaker2 gallery
<http://www.pentacom.jp/pentacom/bitfontmaker2/gallery/?id=3780>. CC BY permits
redistribution, modification, and commercial use with attribution; this notice
provides it.
