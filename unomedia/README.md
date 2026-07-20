# unomedia — the UnoDOS media foundation

Format readers (and eventually writers) for **images, audio and video**,
written from scratch and shared by every port — the same role unoui plays for
widgets and uno3d for rasterisation. Phase 1 (this directory today) is
**image decoding**, feeding the pc64 Photos app.

## Why a top-level library

pc64 already had a media layer — `pc64/pc64_media.c`, the audio probe/vtable
that dec_wav/dec_midi/dec_mp3 plug into. It proved the shapes: magic-byte
probe with extension tiebreak, one decoder vtable per format, a single global
open instance, streaming byte access so a big file never loads whole, and an
error surface that distinguishes *"that is not audio"* from *"that is a
perfectly good AAC file this build declines"*. unomedia lifts exactly those
shapes out of the port so every format family can share them; the audio
decoders are slated to migrate here next (a mechanical move — the vtables
already match), after which pc64_media.c becomes a thin adapter and the other
ports get the decoders for free.

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
| **JPEG** | `um_jpg.c` | baseline sequential (SOF0/SOF1@8-bit), integer-only fixed-point IDCT, sampling h,v ∈ {1,2}, restart markers; MCU-row streaming |
| **GIF**  | `um_gif.c` | 87a/89a, streaming LZW, interlace, local palettes, full GCE (transparency, delays, disposal 0-3 incl. restore-previous), exact frame count on open |
| **BMP**  | `um_bmp.c` | CORE/INFO/V4/V5, 1/4/8-bit palette, 16/32-bit with BI_BITFIELDS, 24-bit, RLE4/RLE8, both row orders; also the bare-DIB core ICO delegates to |
| **ICO**  | `um_ico.c` | ICO/CUR directory, best-entry pick, PNG or DIB payload (delegates to the PNG/BMP decoders), AND-mask transparency |
| **TGA**  | `um_tga.c` | types 1/2/3/9/10/11, 8/15/16/24/32 bpp, palettes, RLE, origin flips (no magic — claimed by extension + header plausibility) |
| **PNM**  | `um_pnm.c` | P1..P6 ASCII + binary, comments, maxval to 65535 |
| **QOI**  | `um_qoi.c` | complete spec 1.0 |

**Identified, deliberately not decoded** (`um_stub.c`, the AAC precedent):
WebP, TIFF, AVIF, HEIC, JPEG XL, SVG — each is recognised and refused with
its name, so the viewer can say *"WebP recognised - no decoder in this
build"* instead of the false *"not an image"*. Ditto inside real decoders:
progressive/arithmetic/12-bit/CMYK JPEG and PAM (P7) refuse with precise
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
`unomedia.c` — nothing else changes. Writers (encode) will join as a
parallel vtable when the first consumer (screenshots? Paint export?) lands.
