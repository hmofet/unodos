# Bundled fonts

The pc64 TrueType engine (`pc64_font.c`, built on the vendored
`stb_truetype.h` — public domain / MIT) loads these at runtime from the ESP
(copied to `SANS.TTF` / `MONO.TTF` / `UBUNTU.TTF` by `build.sh`). All three are
open-licensed and redistributable:

| File        | Face          | License |
|-------------|---------------|---------|
| `Sans.ttf`  | DejaVu Sans   | Bitstream Vera / DejaVu license (permissive, redistributable) |
| `Mono.ttf`  | DejaVu Sans Mono | same |
| `Ubuntu.ttf`| Ubuntu        | Ubuntu Font Licence 1.0 |

These are the *optional* system/document fonts. The built-in 8×8 bitmap font
(`font_data.h`, generated at build time) remains the default and the fallback
if a TTF can't be loaded.
