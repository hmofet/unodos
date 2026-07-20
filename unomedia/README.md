# unomedia — the UnoDOS media foundation

Format readers (and eventually writers) for **images, audio and video**,
shared by every port — the same role unoui plays for widgets and uno3d for
rasterisation. Phase 1 delivered **image decoding** (the pc64 Photos app);
phase 2 moved the **audio decoders** in and put the Music app on them.

## The shape

pc64's original media layer (`pc64/pc64_media.c`) proved the shapes: a
magic-byte probe with extension tiebreak, one decoder vtable per format, a
single open instance, streaming byte access so a big file never loads whole,
and an error surface that distinguishes *"that is not audio"* from *"that is
a perfectly good progressive JPEG this build declines"*. unomedia is those
shapes, made shared: a small core (allocator, error surface, owner-tagged
byte source) plus one dispatcher per family — `um_image.c` and `um_audio.c`
— so a build links only what it uses. pc64's kernel takes core+audio for
Music; PHOTOS.UNO carries its own private core+image instance; the two never
collide. `pc64_media.c` remains as that port's adapter: a 64 KB
sliding-window source over `uno_fs_read_at` and one open call.

## The image surface (`unomedia.h`)

```
um_src (random-access bytes)          um_image_open(src, name, &info)
        │  um_read(off, dst, n)               │
        ▼                                     ▼
   probe: magic bytes, ext tiebreak ──► decoder vtable ──► um_image_frame(dst)
                                                           w*h RGBA (= fb_px),
                                                           composited frames,
                                                           delay for animation
```

- **Pixels**: 32-bit RGBA with R in the low byte (`0xAABBGGRR`) — exactly
  fb.h's `fb_px`, so a decoded row blits with no swizzle.
- **Animation**: `um_image_frame` returns each frame composited into the
  caller's persistent buffer plus its delay; `um_image_rewind` loops.
- **Memory**: decoders stream through `um_read` and take working buffers from
  the registered allocator (`um_set_alloc` — kernel heap in pc64, libc malloc
  in the host tests). Static state stays tiny: the whole library rides inside
  PHOTOS.UNO, where .bss is module-arena budget.

## Decoders (all from scratch, no third-party code)

| Format | File | Notes |
|---|---|---|
| **PNG**  | `um_png.c` + `um_inflate.c` | own RFC 1950/1951 inflate (32 KB window, streaming); color types 0/2/3/4/6, depths 1/2/4/8/16 (16 folds to 8), Adam7, tRNS |
| **JPEG** | `um_jpg.c` | baseline + extended-sequential + full **progressive** (spectral selection, successive approximation, EOB runs; a truncated progressive file renders the coarse image it carries), integer-only IDCT, sampling h,v ∈ {1,2}, restart markers |
| **WebP** | `um_webp.c` + `um_vp8.c` | lossy (complete RFC 6386 VP8 key-frame decode incl. the in-loop deblocking filter — RGBA bit-identical to libwebp), lossless (full VP8L: all transforms, meta-prefix groups, color cache), ALPH alpha on lossy frames, animated WebP with GIF-style compositing |
| **GIF**  | `um_gif.c` | 87a/89a, streaming LZW, interlace, local palettes, full GCE (transparency, delays, disposal 0-3 incl. restore-previous), exact frame count on open |
| **BMP**  | `um_bmp.c` | CORE/INFO/V4/V5, 1/4/8-bit palette, 16/32-bit with BI_BITFIELDS, 24-bit, RLE4/RLE8, both row orders; also the bare-DIB core ICO delegates to |
| **ICO**  | `um_ico.c` | ICO/CUR directory, best-entry pick, PNG or DIB payload (delegates to the PNG/BMP decoders), AND-mask transparency |
| **TGA**  | `um_tga.c` | types 1/2/3/9/10/11, 8/15/16/24/32 bpp, palettes, RLE, origin flips (no magic — claimed by extension + header plausibility) |
| **PNM**  | `um_pnm.c` | P1..P6 ASCII + binary, comments, maxval to 65535 |
| **QOI**  | `um_qoi.c` | complete spec 1.0 |

## Audio decoders

| Format | File | Notes |
|---|---|---|
| **WAV**  | `um_wav.c` | PCM 8/16/24/32 + IEEE float, any rate, extensible headers |
| **MIDI** | `um_midi.c` | SMF 0/1/2 + RMI; a 48-voice synthesiser (no soundfont) |
| **MP3**  | `um_mp3.c` | MPEG-1 Layer III complete (tables: `mp3_tables.h`, public-domain-derived, see `tools/mkmp3tables.py`) |
| **AAC**  | `um_aac.c` | AAC-LC, ADTS + MP4/M4A. The ISO constant tables (`aac_tables.h`) derive from OpenCORE aacdec, **Apache-2.0** — see `tools/mkaactables.py`, `LICENSE.APACHE-2.0`, and the shipped `DOCS\LICENSES.MD`; the decoder code itself is this project's own |

Verify with `python3 test/run_audio_tests.py` — WAV sample-exact vs ffmpeg,
MP3/AAC at aligned PSNR vs ffmpeg's decode of the same files, MIDI synth
sanity. The pc64 in-OS pass is `pc64/tools/music_test.py`.

**Identified, deliberately not decoded** (`um_stub.c`, the AAC-container precedent):
TIFF, AVIF, HEIC, JPEG XL, SVG — each is recognised and refused with its
name, so the viewer can say *"HEIC recognised - no decoder in this build"*
instead of the false *"not an image"*. Ditto inside real decoders:
arithmetic/12-bit/lossless/CMYK JPEG and PAM (P7) refuse with precise
reasons through `um_error()`.

## Verifying

```
python3 test/run_tests.py            # everything
python3 test/run_tests.py png       # one family
```

Builds the decoders **hosted** (gcc + ASan/UBSan) into `test/imgtest`,
generates references with ImageMagick, and compares: lossless formats
pixel-exact against ImageMagick's decode of the same file, JPEG at
PSNR ≥ 40 dB (both decoders follow the spec; only rounding differs),
animated GIF frame-by-frame against `-coalesce`, plus refusal and
truncation cases. The hosted build is the fast path when changing a decoder
— it isolates codec bugs from OS plumbing, exactly like the audio decoders'
host harness.

Freestanding compile check (what the .UNO module build does):

```
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
    -nostdinc -I../pc64/include -I. -c um_png.c
```

## Adding a format

One new `um_<fmt>.c` defining a `um_idecoder`, plus a roster line in
`um_image.c` / `um_audio.c` — nothing else changes. Writers (encode) will
join as a parallel vtable when the first consumer (screenshots? Paint
export?) lands.

## What unomedia is NOT: unodoc

SVG stays refused here on purpose: it is a *document*, not a raster
bitstream — rendering it means XML, paths, CSS and text layout, a different
kind of machine than a decoder. It is earmarked for **unodoc**, the planned
sibling library for document formats (Word, Excel, PDF, SVG), which will
stand next to unomedia the way unoui stands next to uno3d. Media bitstreams
decode here; documents will render there.
