/* ===========================================================================
 * UnoDOS/pc64 - the media layer: open an audio file, decode it to PCM.
 *
 * One file plays at a time, so everything here is a single global instance:
 * no allocation, no ownership questions, and the decoders can keep their
 * (large) working state in .bss where the freestanding kernel wants it.
 *
 * The player never loads a whole song. `uno_src_read` streams bytes from the
 * file through a sliding window (pc64_fs's offset reads), so a 40 MB WAV
 * costs the same resident memory as a 3 MB MP3.
 *
 *   file ──► probe (magic bytes, then extension) ──► decoder ──► s16 frames
 *                                                                   │
 *                                          uno_snd_stream_write ◄────┘
 * ======================================================================== */
#ifndef PC64_MEDIA_H
#define PC64_MEDIA_H

typedef struct {
    const char *format;      /* "WAV" / "MIDI" / "MP3" / "AAC"              */
    int   rate;              /* decoder's native sample rate                */
    int   channels;          /* 1 or 2 (decoders downmix anything wider)    */
    long  duration_ms;       /* -1 when the format can't say               */
    int   bitrate;           /* kbps, 0 when not meaningful                 */
    char  title[64];         /* from metadata (ID3 / MIDI track name), or ""*/
    char  artist[64];
} uno_media_info;

/* ---- the byte source (decoders read the file through this) --------------- */
long uno_src_size(void);
long uno_src_read(long off, unsigned char *dst, long n);

/* ---- the player-facing surface -------------------------------------------- */
int  uno_media_open(int vol, const char *name, uno_media_info *info);

/* Why the last uno_media_open failed, or "" - so the player can say something
 * truer than "unsupported". A decoder that RECOGNISES a file but cannot play
 * it sets this; the difference between "that isn't audio" and "that is a
 * perfectly good AAC file this build has no decoder for" matters to whoever
 * is looking at the screen. */
const char *uno_media_error(void);
void        uno_media_set_error(const char *why);
int  uno_media_decode(short *out, int max_frames);  /* frames; 0 = end       */
int  uno_media_seek_ms(long ms);                    /* 1 if the format seeks */
long uno_media_pos_ms(void);
void uno_media_close(void);

/* 1 if `name` has an extension the media layer handles (the Music app's
 * file-list filter). */
int  uno_media_is_audio(const char *name);

/* ---- the decoder vtable (one per format, all in .bss) --------------------- */
typedef struct {
    const char *name;
    /* claim the file: `head` is the first HEAD_BYTES bytes, `ext` the
       uppercased extension without the dot. Return 1 to claim. */
    int  (*probe)(const unsigned char *head, long n, const char *ext);
    int  (*open)(uno_media_info *info);           /* 1 = ready to decode     */
    int  (*decode)(short *out, int max_frames);
    int  (*seek)(long ms);
    void (*close)(void);
    long (*pos_ms)(void);
} uno_decoder;

#define UNO_MEDIA_HEAD 512

extern const uno_decoder uno_dec_wav;
extern const uno_decoder uno_dec_midi;
extern const uno_decoder uno_dec_mp3;
extern const uno_decoder uno_dec_aac;

#endif
