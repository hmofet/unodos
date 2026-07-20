/* ===========================================================================
 * unomedia - the UnoDOS media foundation: format readers (and eventually
 * writers) for images, audio and video, shared by every port.
 *
 * PHASE 1 (this header): still + animated IMAGE decoding, feeding the pc64
 * Photos app. The API shapes - a probe over magic bytes then extension, a
 * per-format decoder vtable, one global open instance, and an error surface
 * that distinguishes "that is not an image" from "that is a perfectly good
 * progressive JPEG this build does not decode" - are copied deliberately
 * from pc64_media.h (the audio layer), so migrating dec_wav/dec_midi/
 * dec_mp3 into unomedia later is a mechanical move, not a redesign.
 *
 * DESIGN RULES (the same ones the audio decoders follow):
 *   - Freestanding C, no float, no libc beyond mem-/str- (<string.h> from
 *     the port's include dir). Builds unchanged as a hosted object for the
 *     unit tests and as part of a .UNO module.
 *   - No decoder loads the whole file: bytes stream through um_read(), so
 *     the resident cost is the decode state, not the file.
 *   - Big or size-dependent buffers come from the registered allocator
 *     (um_alloc/um_free -> the kernel heap in pc64, libc malloc on the
 *     host). Static .bss stays small: a .UNO module's mem_size is arena
 *     budget, and constant tables belong in .rodata anyway.
 *   - Everything is written from scratch against the format specs - no
 *     third-party decoder code, matching the repo's uniform licensing
 *     (see AUDIO.md's AAC decision for the precedent and the reasoning).
 *
 * PIXELS: decoded output is 32-bit RGBA with R in the LOW byte of the u32
 * (byte order R,G,B,A) - exactly the fb.h fb_px layout (0xAABBGGRR), so a
 * decoded row blits straight into the framebuffer with no swizzle. Alpha is
 * always meaningful in the output: opaque pixels carry A=0xFF.
 * ======================================================================== */
#ifndef UNOMEDIA_H
#define UNOMEDIA_H

#include <stdint.h>

typedef uint32_t um_px;                     /* R,G,B,A byte order (= fb_px) */
#define UM_PX(r, g, b, a) \
    ((um_px)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
             ((uint32_t)(g) << 8)  |  (uint32_t)(r)))

/* ---- the byte source ------------------------------------------------------
 * The app hands unomedia a random-access byte source (pc64: uno_fs_read_at
 * behind a struct; host tests: pread over a FILE). Decoders never see it -
 * they stream through um_read()/um_size() below, which the core routes to
 * the open source. */
typedef struct {
    /* copy up to n bytes from byte offset off into dst; returns bytes
       copied, 0 at/past EOF, <0 on I/O error. */
    long (*read)(void *ctx, long off, unsigned char *dst, long n);
    long  size;                             /* total file size in bytes      */
    void *ctx;
} um_src;

/* decoder-facing: read from the open source (0 at EOF, <0 error) */
long um_read(long off, unsigned char *dst, long n);
long um_size(void);

/* ---- the allocator --------------------------------------------------------
 * Registered once by the host program / app before any open. um_alloc
 * returns zeroed-or-not memory (no promise); um_free tolerates NULL. */
void  um_set_alloc(void *(*a)(unsigned long), void (*f)(void *));
void *um_alloc(unsigned long n);
void  um_free(void *p);

/* ---- the error surface ----------------------------------------------------
 * Why the last open/decode failed, or "". A decoder that RECOGNISES a file
 * but cannot decode it says so precisely ("progressive JPEG - not decoded
 * in this build") - the difference between "not an image" and "a good file
 * this build declines" matters to whoever is looking at the screen. */
const char *um_error(void);
void        um_set_error(const char *why);

/* ---- image decoding (one global instance, like the audio layer) ----------- */
typedef struct {
    const char *format;      /* "PNG" / "JPEG" / "GIF" / "BMP" / ...         */
    int  w, h;               /* pixel dimensions of every frame              */
    int  bpp;                /* significant bits/pixel in the SOURCE (info)  */
    int  alpha;              /* 1 = output can carry non-opaque alpha        */
    int  frames;             /* known frame count; 1 = still, 0 = animated,
                                count unknown until played through           */
} um_image_info;

/* Open `src` as an image: probe (magic bytes first, extension as the
 * tiebreak), then parse headers. 1 = ready to decode, 0 = failed (um_error
 * says why). `name` is the file name (may be "", used only for its
 * extension - TGA has no magic). On success the source stays referenced
 * until um_image_close. */
int  um_image_open(const um_src *src, const char *name, um_image_info *info);

/* Decode the NEXT frame, composited into dst (info->w * info->h um_px).
 * dst PERSISTS across calls: GIF frames compose over what is already there
 * (disposal handled inside), so the caller allocates once and never clears.
 * Returns 1 = frame produced (*delay_ms = its display time, 0 for stills),
 *         0 = no more frames (end of animation; um_image_rewind to loop),
 *        -1 = decode error (um_error says why).                            */
int  um_image_frame(um_px *dst, int *delay_ms);

/* Restart the animation from frame 0 (loop playback). Stills too. */
void um_image_rewind(void);

void um_image_close(void);

/* 1 if `name`'s extension is one the image layer handles - the Photos app's
 * file-list filter (8.3 names: PNG JPG GIF BMP TGA PPM PGM PBM QOI ICO). */
int  um_image_is(const char *name);

/* Guards every decoder applies to header dimensions before believing them:
 * a fuzzed 2-billion-pixel header must fail cleanly, not overflow w*h*4. */
#define UM_MAX_DIM     16384
#define UM_MAX_PIXELS  (64L * 1024 * 1024)

/* ---- the image decoder vtable (one per format) ---------------------------- */
#define UM_IMG_HEAD 64          /* probe sees the first 64 bytes (or fewer) */

typedef struct {
    const char *name;
    /* claim the file: head = first UM_IMG_HEAD bytes (n valid), ext = the
       uppercased extension without the dot ("" if none). Return 1 to claim.
       A probe that recognises the format but knows it cannot decode it
       should still CLAIM it and fail in open() with a precise um_set_error
       - that is how "WEBP recognised - no decoder in this build" reaches
       the user instead of "not an image". */
    int  (*probe)(const unsigned char *head, long n, const char *ext);
    int  (*open)(um_image_info *info);            /* 1 = ready              */
    int  (*frame)(um_px *dst, int *delay_ms);     /* 1/0/-1, see above      */
    void (*rewind)(void);                         /* NULL = one still frame */
    void (*close)(void);                          /* free decoder buffers   */
} um_idecoder;

extern const um_idecoder um_idec_png;
extern const um_idecoder um_idec_jpg;
extern const um_idecoder um_idec_gif;
extern const um_idecoder um_idec_bmp;   /* also DIB, and the ICO payload    */
extern const um_idecoder um_idec_tga;
extern const um_idecoder um_idec_pnm;   /* PPM/PGM/PBM, P1..P6              */
extern const um_idecoder um_idec_qoi;
extern const um_idecoder um_idec_ico;   /* ICO/CUR (BMP or PNG payload)     */
extern const um_idecoder um_idec_stub;  /* WEBP/TIFF/AVIF/HEIC/JXL: identify
                                           + refuse with the format's name  */

/* ---- inflate (RFC 1951/1950) - PNG's engine, exported because it is a
 * general facility future formats (and a future writer) will want. Streams:
 * feed compressed bytes via `in`, receive decompressed bytes via `out`
 * (called with runs as they decode; return 0 from out to abort). Returns
 * 1 = stream complete, 0 = malformed/aborted. `zlib` = 1 expects a 2-byte
 * zlib header + adler (RFC 1950), 0 = raw deflate. Working state (~44 KB,
 * incl. the 32 KB LZ77 window) comes from um_alloc. */
typedef long (*um_inf_in_fn)(void *ctx, unsigned char *dst, long max);
typedef int  (*um_inf_out_fn)(void *ctx, const unsigned char *p, long n);
int um_inflate(um_inf_in_fn in, void *in_ctx,
               um_inf_out_fn out, void *out_ctx, int zlib);

#endif /* UNOMEDIA_H */
