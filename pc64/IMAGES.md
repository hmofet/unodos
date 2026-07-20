# pc64 images — the Photos app + unomedia

The pc64 counterpart of AUDIO.md's media chain, for pictures: **Photos** is a
unoui-class `.UNO` module (`APPS\PHOTOS.UNO`, the second after Studio) and
the first consumer of **unomedia**, the top-level media-foundation library
(`../unomedia/` — see its README for the decoder inventory; since phase 2
the audio decoders live there too and Music is its second consumer).

```
file on any volume ──► um_src over uno_fs_read_at / uno_fat_read_at
        │                       (streaming - a big JPEG never loads whole)
        ▼
unomedia probe ──► PNG / JPEG / GIF / BMP / TGA / PNM / QOI / ICO decoder
        │                       (all from scratch, statically linked
        ▼                        into PHOTOS.UNO - zero kernel decoders)
w*h RGBA frame (kernel heap) ──► viewport-scaled cache ──► one fb_blit
```

## The app

- **Browser pane**: volumes (header cycles them; click also pops out of a
  subdirectory) + FAT subdirectories, filtered to what unomedia opens
  (`um_image_is`). Arrows/Enter drive it from the keyboard; `<` `>` and the
  Left/Right keys step through a folder's images directly.
- **View**: fit-to-window by default (never upscales); wheel zooms about the
  pointer, drag pans, `1:1`/`Fit`/`+`/`-` on the toolbar, 10%-800%.
- **Scaling**: box average on downscale (photos stay smooth at fit), nearest
  on upscale (pixels stay honest at 400%). The result is cached
  viewport-sized and pre-composited — alpha over a checkerboard — so a
  repaint of an unchanged view is a single clipped `fb_blit` (the one kernel
  export this feature added).
- **Animation**: GIFs play through the module `frame()` hook, paced by each
  frame's own delay against TickCount; Space or the toolbar pauses. The
  status bar shows `frame N/M`.
- **Refusals carry their reason**: a progressive JPEG or a WebP shows
  *"progressive JPEG - not decoded in this build"* / *"WebP recognised - no
  decoder in this build"* in the status bar, because "this build declines"
  and "your file is broken" are different messages (the AAC precedent,
  AUDIO.md).

## Memory shape

The decoded frame (w×h RGBA) and the viewport cache come from the kernel
heap via the module's `malloc` import; unomedia's own working state
(inflate window, Huffman tables, LZW dictionary) is `um_alloc`'d and freed
on close. Dimension guards cap at 16384² / 64 Mpx before any allocation, so
a fuzzed header fails cleanly. The module's .bss stays small — the arena
budget in pc64_modload.c is unchanged.

## Out-of-box content

`build.sh` stages `pictures/` → `ESP:\PICTURES\` — seven files, one per
decoder family, all drawn procedurally by `tools/mkdemo_pics.py` (CC0, no
third-party art; see `pictures/README.TXT`). Photos opens on the first
volume that shows anything.

## Verification

- **Decoders, host-side (the fast path)**: `python3 ../unomedia/test/run_tests.py`
  — gcc + ASan/UBSan build, pixel-exact vs ImageMagick for lossless formats,
  PSNR ≥ 40 dB for JPEG, frame-by-frame vs `-coalesce` for animated GIF,
  plus refusal/truncation cases.
- **In-OS**: `python3 tools/photos_test.py` — boots the shell in QEMU, opens
  Photos through the Start menu, walks the staged PICTURES set with the
  Right key (each format decodes on-screen), screenshots each.

## Metal-pending

Nothing hardware-specific: the app draws through fb like every other unoui
app. Worth an eyeball on a real panel: pan smoothness at native resolution
(the viewport rebuild is O(view) per drag frame) and GIF pacing under a
slow storage device.
