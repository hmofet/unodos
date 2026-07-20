Demo pictures for the Photos app (staged to build/esp/PICTURES/ by build.sh,
the same arrangement media/ uses for the Music app).

Every file here was drawn procedurally by tools/mkdemo_pics.py - no clip art,
no photographs, no third-party sources of any kind - so the set carries no
rights but this repo's own, and is dedicated to the public domain (CC0). That
unambiguity is the point: the OS image ships these files.

One file per format on purpose; together they exercise every unomedia decoder:

  SUNSET.JPG   baseline JPEG (4:2:0)         - sunset over water
  BLOOM.PNG    PNG, RGBA with real alpha      - a flower on transparency
  ORBIT.GIF    animated GIF, 10 frames        - orbiting moons
  FLAG.BMP     BMP, 24-bit                    - waving UnoDOS banner
  GRAD.TGA     TGA, 24-bit uncompressed       - RGB gradient
  MOON.PGM     PGM (netpbm grayscale)         - cratered moon
  TILES.QOI    QOI, spec 1.0                  - bevelled tiles
  LAGOON.WEB   WebP, lossy VP8 (named 8.3:    - island lagoon
               the decoder probes by RIFF
               magic, not the extension)

Regenerate with: python3 tools/mkdemo_pics.py  (needs ImageMagick `convert`
for the JPG/GIF conversions; everything else is stdlib-written bytes).
