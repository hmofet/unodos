/* ===========================================================================
 * UNOAUTOMATE screen grab - the OUT half of URC remote desktop.
 *
 * URC already injects input (key/pointer); this is the missing other half:
 * snapshot the live UnoDOS framebuffer (fb.h `fb[]`) so a dev-PC client can
 * render it. The frame is QOI-encoded (lossless, tiny on UnoDOS's flat-colour
 * desktop, trivial to decode) and the `screen` verb in unoauto_remote.c streams
 * it base64 like `readsec`. UNO_DEBUG-only, same gate as the rest of URC.
 *
 * Two grab modes share one QOI encoder:
 *   - FULL keyframe (uno_screen_grab_qoi): the whole framebuffer, as before.
 *   - DELTA (uno_screen_grab_delta): only the TILES that changed since the last
 *     grab, so a mostly-static desktop streams at a fraction of the bytes. Dirty
 *     detection is per-tile FNV hashing against a snapshot kept here (no
 *     multi-MB previous-frame buffer - fb[] is up to 1920x1200). Every grab
 *     (full or delta) refreshes that snapshot.
 * ======================================================================== */
#ifndef UNOAUTO_SCREEN_H
#define UNOAUTO_SCREEN_H

/* Delta tile edge, in EMITTED (post-scale) pixels. The client learns it from
 * the `screen grab delta` reply header, so it need not hard-code this. */
#define UNO_SCREEN_TILE 32

/* Current desktop size (FB_W x FB_H) - for the `screen info` reply. */
void uno_screen_size(int *w, int *h);

/* Encode the current framebuffer as a QOI image into `out` (up to `cap` bytes).
 * `scale` >= 1 downsamples nearest-neighbour (2 => half width & height, a
 * quarter of the pixels), so a busy/hi-res screen can still fit the caller's
 * buffer. Writes the emitted image dimensions to ow,oh. Returns the QOI byte
 * count, or -1 if it did not fit in `cap` (the caller raises `scale` and
 * retries). fb[] pixels are 0xAABBGGRR (R,G,B,A byte order in memory), fed
 * straight into QOI's RGBA channels. Refreshes the delta snapshot. */
int  uno_screen_grab_qoi(int scale, unsigned char *out, int cap, int *ow, int *oh);

/* Delta grab. Compares the framebuffer against the snapshot from the previous
 * grab (per-tile hash) and encodes ONLY the changed tiles as one vertical QOI
 * strip (width = UNO_SCREEN_TILE, height = nch * UNO_SCREEN_TILE), then appends
 * a manifest of the changed tile indices right after the strip:
 *
 *     out = [ QOI strip: *ostrip bytes ][ manifest: *onch u16-LE tile indices ]
 *
 * so the caller stages the whole blob and the client, told `strip` and `nch`,
 * splits it without a gap. A tile index is row-major: col = idx % *ocols,
 * row = idx / ocols; its top-left emitted pixel is (col*tw, row*th). Edge
 * tiles are padded to a full cell in the strip - the client blits only the
 * valid (ew-,eh-clamped) sub-rect.
 *
 * Returns the total blob byte count (>= 0), with *onch tiles and *ostrip strip
 * bytes; a return of 0 with *onch == 0 is the static-screen win (nothing
 * changed, empty payload). Returns -1 to mean "send a full keyframe instead":
 * no matching prior snapshot (first grab, or scale/size changed), or the strip
 * did not fit `cap`. Either outcome leaves the snapshot refreshed to the current
 * frame. ow,oh = emitted dims, ocols = tiles per row, otw,oth = tile size. */
int  uno_screen_grab_delta(int scale, unsigned char *out, int cap,
                           int *ow, int *oh, int *ocols, int *otw, int *oth,
                           int *onch, int *ostrip);

/* ---- server-side session capture ----------------------------------------- *
 * The device records frames into a RAM ring on its OWN tick, decoupled from the
 * client's network poll rate, so a recording captures at a steady fps the link
 * could not sustain live. Capture keeps its own delta snapshot, independent of
 * the live-view grab, so recording and viewing never disturb each other. The
 * client drives start/stop and then pulls the ring (uno_screen_capture_read,
 * base64 like `screen read`) and reconstructs frames the same way the live view
 * composites - a keyframe replaces the canvas, a delta blits its changed tiles.
 * Ring frame format is documented at the recorder in unoauto_screen.c. */
#ifdef UNO_DEBUG
/* Start recording at `scale`/`fps` (fps clamped 1..60). Returns 1, or 0 if a
 * recording is already running. Resets the ring. */
int  uno_screen_capture_start(int scale, int fps);
/* Stop recording (the ring is retained for reading). */
void uno_screen_capture_stop(void);
/* Called every shell frame: captures a frame if `fps` says it is due. A no-op
 * unless a recording is running. Compiles away in production. */
void uno_screen_capture_tick(void);
/* Recording status + the constant geometry a client needs to reconstruct. Any
 * out pointer may be NULL. */
void uno_screen_capture_stat(int *frames, int *bytes, int *dropped, int *on,
                             int *scale, int *ew, int *eh, int *cols, int *fps);
/* Copy `len` bytes of the recorded ring at `off` into `dst`; returns the count
 * actually copied (clamped to the recording length). */
int  uno_screen_capture_read(int off, unsigned char *dst, int len);
#else
#define uno_screen_capture_tick()    ((void)0)
#endif

#endif
