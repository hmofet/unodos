/* ===========================================================================
 * unomedia - AAC-LC: container layer + decode core.
 *
 * ---- the two wrappers --------------------------------------------------------
 * Two very different wrappers carry AAC, and a player has to handle both:
 *
 *   ADTS  (.aac)  - a self-framing byte stream. Every frame carries a 7- or
 *                   9-byte header with a 12-bit sync word, the profile, the
 *                   sample-rate index and the frame length, so you can start
 *                   anywhere and resync. No global header, no seek table.
 *   MP4   (.m4a)  - a box tree. The audio is raw AAC frames concatenated in
 *                   `mdat` with NO framing at all; the sizes live in a
 *                   separate `stsz` table, the file offsets are reconstructed
 *                   from `stsc` + `stco`, and the decoder's setup lives in an
 *                   AudioSpecificConfig buried in `stsd`/`esds`. Lose the box
 *                   tree and the audio is unparseable - the opposite of ADTS.
 *
 * ---- the decode core ---------------------------------------------------------
 * AAC-LC (Low Complexity - object type 2, and the MPEG-2 LC profile), written
 * from scratch against ISO/IEC 13818-7 / 14496-3: raw_data_block elements,
 * ics_info, section data, scalefactors, all 11 spectral codebooks with the
 * book-11 escape, pulse data, TNS, M/S and intensity stereo, PNS, and the
 * 2048/256 IMDCT filterbank with sine AND Kaiser-Bessel-derived windows. One
 * AAC frame is 1024 PCM samples per channel; frames stream out through the
 * same take-what-you-ask surface um_mp3.c uses.
 *
 * The only part that is NOT written here is the specification's constant
 * data - the Huffman codebooks and the scalefactor-band offsets - which no
 * decoder can invent. Those live in the generated aac_tables.h, derived from
 * OpenCORE aacdec (Apache-2.0, Copyright (C) 1998-2009 PacketVideo) - see
 * tools/mkaactables.py for the exact provenance and derivation, and
 * LICENSE.APACHE-2.0 for the licence. AUDIO.md records the history of that
 * decision.
 *
 * ---- pipeline ----------------------------------------------------------------
 *   frame bytes -> element loop -> ics_info -> sections -> scalefactors
 *      -> spectral Huffman (+pulse) -> requantise (x^(4/3) * 2^(sf/4))
 *      -> PNS -> M/S -> intensity -> deinterleave (short blocks)
 *      -> TNS all-pole filter -> IMDCT (DCT-IV via N/4 complex FFT)
 *      -> window + overlap-add -> PCM
 *
 * ---- scope -------------------------------------------------------------------
 * Decoded: LC mono/stereo (SCE / CPE / LFE), 8-96 kHz, ADTS and MP4/M4A,
 * long/start/short/stop windows, both window shapes, TNS, M/S, intensity,
 * PNS (deterministic per-frame noise seed, so a decode is reproducible).
 * HE-AAC streams decode as their LC core: SBR rides in fill elements and an
 * SBR decoder is out of scope, so those elements are skipped and the output
 * is the core at the core sample rate - which is exactly what a plain LC
 * decoder is specified to do. Refused precisely: Main/SSR/LTP profiles, ER
 * object types, 960-sample framing, more than two channels.
 *
 * ---- no libm -----------------------------------------------------------------
 * Freestanding kernel: only sinf/cosf/sqrtf (pc64_math.c, same as um_mp3.c).
 * The x^(4/3) requant curve comes from a Newton cube root, 2^(n/4) from
 * repeated multiplication, and the KBD window's Bessel I0 from its power
 * series - all built once at open() into the decoder's heap block.
 * ======================================================================== */
#include "unomedia.h"
#include "aac_tables.h"
#include <string.h>
#include <stdint.h>

float sinf(float x);
float cosf(float x);
float sqrtf(float x);

#define AAC_MAX_FRAME  8192          /* an AAC frame is bounded well under this */
#define AAC_MAX_TABLE  (1L << 20)    /* stsz entries we keep for a seek table   */

#define AAC_FRAME_LEN  1024
#define AAC_MAX_SFB    51
#define AAC_MAX_ORDER  12            /* TNS order limit in LC (7 for short)     */

/* section codebook classes */
#define CB_ZERO   0
#define CB_ESC    11
#define CB_NOISE  13
#define CB_IS2    14                 /* intensity, out of phase                 */
#define CB_IS     15                 /* intensity, in phase                     */

/* window sequences */
#define SEQ_LONG   0
#define SEQ_START  1
#define SEQ_SHORT  2
#define SEQ_STOP   3

/* the 13 sampling frequencies an AudioSpecificConfig index can name */
static const int kAacRate[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025,  8000,  7350,     0,     0, 0
};

/* TNS band limits per sampling-frequency index (ISO/IEC 14496-3 table for
 * the LC profile; small spec constants like kAacRate above) */
static const unsigned char kTnsMaxLong[13] = {
    31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39
};
static const unsigned char kTnsMaxShort[13] = {
    9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14
};

typedef struct {
    int  is_mp4;                     /* 0 = ADTS byte stream, 1 = MP4 boxes    */
    int  rate, channels, obj_type;   /* from ADTS header or AudioSpecificConfig */
    int  rate_idx;                   /* index into kAacRate / kAacSfb          */
    int  sbr;                        /* explicit SBR signalled (core decoded)  */
    int  frame960;                   /* 960-sample framing flag from the ASC   */
    long pos;                        /* ADTS: byte offset of the next frame    */
    long first;                      /* ADTS: offset of the first frame        */
    long nsamp;                      /* MP4: entries in the sample table       */
    long next_samp;
    long total_frames;               /* audio frames (1024 samples each)       */
} aac_ctx;

static aac_ctx A;

/* ---- per-channel-stream parse state ---------------------------------------- */
typedef struct {
    int  seq, shape, max_sfb;
    int  num_windows, num_groups;
    unsigned char group_len[8];
    int  global_gain;
    unsigned char cb[8][AAC_MAX_SFB];       /* codebook per (group, sfb)      */
    short sf[8][AAC_MAX_SFB];               /* scalefactor / is pos / noise   */
    /* TNS, decoded to LPC at parse time */
    unsigned char n_filt[8];
    unsigned char t_order[8][3], t_dir[8][3], t_len[8][3];
    float t_lpc[8][3][AAC_MAX_ORDER + 1];
    /* pulse (long windows only) */
    int  npulse, pulse_sfb;
    unsigned char p_off[4], p_amp[4];
} aac_ics;

/* ---- the decoder state - ONE um_alloc'd block, freed in close() ------------- */
typedef struct {
    /* current frame's bitstream */
    unsigned char frame[AAC_MAX_FRAME];
    long fbits, fpos;
    int  fover;                             /* read past the end = bad frame  */

    aac_ics ics[2];
    int   quant[2][AAC_FRAME_LEN];          /* Huffman-decoded integers       */
    float coef[2][AAC_FRAME_LEN];           /* requantised, group order       */
    float spec[AAC_FRAME_LEN];              /* one channel, window order      */
    float overlap[2][AAC_FRAME_LEN];
    int   prev_shape[2];
    int   ms_mask;                          /* 0 = off, 1 = per band, 2 = all */
    unsigned char ms_used[8][AAC_MAX_SFB];
    unsigned      pns_seed[8][AAC_MAX_SFB]; /* left channel's per-band seeds  */
    unsigned      lcg;

    short pcm[2][AAC_FRAME_LEN];
    int   pcm_have, pcm_taken;
    long  frames_out;                       /* PCM frames handed out          */
    long  frame_no;

    /* ---- constant tables, built once at open ---- */
    float pow43[8192];                      /* |q|^(4/3)                      */
    float sf_gain[256];                     /* 2^((sf-100)/4)                 */
    float win_sine_l[1024], win_kbd_l[1024];
    float win_sine_s[128],  win_kbd_s[128];
    /* IMDCT: DCT-IV via H-point complex FFT (H = 512 long, 64 short) */
    float fft_c[256], fft_s[256];           /* e^(-2i.pi.k/512)               */
    float pre_c_l[512],  pre_s_l[512];      /* e^(-i.pi.p/1024)               */
    float post_c_l[512], post_s_l[512];     /* e^(-i.pi.(t+1/4)/1024)         */
    float pre_c_s[64],   pre_s_s[64];
    float post_c_s[64],  post_s_s[64];
    float zre[512], zim[512];               /* FFT work                       */
    float dct[1024];                        /* DCT-IV result                  */
    float xtime[2048];                      /* IMDCT output                   */
    float wbuf[2048];                       /* windowed frame before OLA      */

    /* MP4 sample table (tail of this same allocation) */
    unsigned int *soff;
    unsigned int *slen;
} aac_state;

static aac_state *S;

static uint32_t be32(const unsigned char *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* ---- ADTS ------------------------------------------------------------------ */
/* Parse an ADTS header at `off`. Fills rate/channels/frame length. The sync
 * word is only 12 bits, which random data hits often, so a header is only
 * trusted when the frame it describes is followed by another valid one. */
static int adts_at(long off, int *rate, int *chans, int *flen, int *hdr,
                   int *prof, int *ridx)
{
    unsigned char h[9];
    int idx, pf, ch, len, prot;
    if (um_read(off, h, 7) != 7) return 0;
    if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) return 0;
    prot = !(h[1] & 1);                       /* protection_absent inverted    */
    pf   = ((h[2] >> 6) & 3) + 1;             /* 2 = LC                        */
    idx  = (h[2] >> 2) & 0xF;
    ch   = ((h[2] & 1) << 2) | ((h[3] >> 6) & 3);
    len  = (int)(((uint32_t)(h[3] & 3) << 11) | ((uint32_t)h[4] << 3) | (h[5] >> 5));
    if (!kAacRate[idx] || len < 7) return 0;
    if (rate)  *rate  = kAacRate[idx];
    if (chans) *chans = ch;
    if (flen)  *flen  = len;
    if (hdr)   *hdr   = 7 + (prot ? 2 : 0);   /* + CRC when present            */
    if (prof)  *prof  = pf;
    if (ridx)  *ridx  = idx;
    return 1;
}

static long adts_find(long off)
{
    long limit = um_size();
    int guard = 0;
    while (off + 9 <= limit && guard++ < (1 << 20)) {
        int len;
        if (adts_at(off, 0, 0, &len, 0, 0, 0)) {
            if (off + len + 7 > limit) return off;              /* last frame */
            if (adts_at(off + len, 0, 0, 0, 0, 0, 0)) return off; /* confirmed */
        }
        off++;
    }
    return -1;
}

/* ---- AudioSpecificConfig ---------------------------------------------------- */
/* 5 bits object type, 4 bits sampling frequency index (or an explicit 24-bit
 * rate when the index is 15), 4 bits channel configuration; object type 5/29
 * (HE-AAC) wraps the real core type behind an extension rate; then the GA
 * config's frameLengthFlag says 1024- or 960-sample frames. */
static int parse_asc(const unsigned char *p, int n)
{
    int bit = 0, obj, idx, ch;
#define GB(k) ({ int _v = 0, _i; for (_i = 0; _i < (k); _i++) { \
        _v = (_v << 1) | ((bit >> 3) < n ? ((p[bit >> 3] >> (7 - (bit & 7))) & 1) : 0); \
        bit++; } _v; })
    if (n < 2) return 0;
    obj = GB(5);
    if (obj == 31) obj = 32 + GB(6);
    idx = GB(4);
    if (idx == 15) A.rate = GB(24);
    else           A.rate = kAacRate[idx];
    ch = GB(4);
    if (obj == 5 || obj == 29) {              /* explicit SBR: unwrap the core */
        A.sbr = 1;
        if (GB(4) == 15) (void)GB(24);        /* extension (output) rate       */
        obj = GB(5);
        if (obj == 31) obj = 32 + GB(6);
    }
    A.obj_type = obj;
    A.channels = ch;
    A.rate_idx = idx;
    /* GASpecificConfig header (harmlessly reads zeros if the ASC is short) */
    A.frame960 = GB(1);
    if (GB(1)) (void)GB(14);                  /* dependsOnCoreCoder + delay    */
    (void)GB(1);                              /* extensionFlag                 */
#undef GB
    if (idx == 15) {                          /* explicit rate: find the index */
        int i;
        A.rate_idx = -1;
        for (i = 0; i < 13; i++)
            if (kAacRate[i] == A.rate) { A.rate_idx = i; break; }
    }
    return A.rate > 0;
}

/* ---- MP4 box walking --------------------------------------------------------
 * Recursive descent over the container boxes, collecting exactly what playback
 * needs: the AudioSpecificConfig, and the three tables that turn "sample N"
 * into a file offset and a length. */
static int mp4_find_asc(long off, long end, int depth);

/* stsc maps chunk ranges to samples-per-chunk; stco gives each chunk's offset.
 * Together with stsz they reconstruct every sample's position, which MP4 does
 * not store directly. */
static long S_stsz, S_stco, S_stsc, S_stts;
static long S_stsz_n, S_stco_n, S_stsc_n;

static void mp4_build_table(void)
{
    unsigned char b[16], zh[12];
    int i;
    long samp = 0, chunk, sc_i = 0, per = 1, usize = 0, cap;
    if (!S_stsz || !S_stco || !S) return;
    if (um_read(S_stsz, zh, 12) == 12) usize = (long)be32(zh + 4);
    cap = A.nsamp;                            /* capacity sized at alloc time  */
    A.nsamp = 0;
    for (chunk = 0; chunk < S_stco_n && samp < cap; chunk++) {
        long coff;
        if (um_read(S_stco + 8 + chunk * 4, b, 4) != 4) break;
        coff = (long)be32(b);
        /* which stsc run covers this chunk? */
        while (S_stsc && sc_i < S_stsc_n) {
            unsigned char e[12];
            long first;
            if (um_read(S_stsc + 8 + sc_i * 12, e, 12) != 12) break;
            first = (long)be32(e);
            if (first > chunk + 1) break;
            per = (long)be32(e + 4);
            sc_i++;
        }
        for (i = 0; i < per && samp < cap; i++, samp++) {
            long len = usize;
            if (!usize) {
                unsigned char z[4];
                if (um_read(S_stsz + 12 + samp * 4, z, 4) != 4) { samp = cap; break; }
                len = (long)be32(z);
            }
            if (len <= 0 || len > AAC_MAX_FRAME) continue;
            S->soff[A.nsamp] = (unsigned int)coff;
            S->slen[A.nsamp] = (unsigned int)len;
            coff += len;
            A.nsamp++;
        }
        if (S_stsc == 0) break;               /* no stsc: one chunk run        */
    }
}

static int mp4_walk(long off, long end, int depth)
{
    unsigned char h[8];
    while (off + 8 <= end && depth < 8) {
        long size;
        if (um_read(off, h, 8) != 8) return 0;
        size = (long)be32(h);
        if (size == 1) {                       /* 64-bit largesize */
            unsigned char e[8];
            if (um_read(off + 8, e, 8) != 8) return 0;
            size = (long)(((uint64_t)be32(e) << 32) | be32(e + 4));
            if (size < 16) return 0;
        } else if (size == 0) {
            size = end - off;                  /* to end of file */
        }
        if (size < 8 || off + size > end) return 0;

        if (!memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "trak", 4) ||
            !memcmp(h + 4, "mdia", 4) || !memcmp(h + 4, "minf", 4) ||
            !memcmp(h + 4, "stbl", 4)) {
            mp4_walk(off + 8, off + size, depth + 1);
        } else if (!memcmp(h + 4, "stsd", 4)) {
            mp4_find_asc(off + 16, off + size, depth + 1);
        } else if (!memcmp(h + 4, "stsz", 4)) {
            unsigned char b[12];
            if (um_read(off + 8, b, 12) == 12) {
                S_stsz = off + 8; S_stsz_n = (long)be32(b + 8);
            }
        } else if (!memcmp(h + 4, "stco", 4)) {
            unsigned char b[8];
            if (um_read(off + 8, b, 8) == 8) {
                S_stco = off + 8; S_stco_n = (long)be32(b + 4);
            }
        } else if (!memcmp(h + 4, "stsc", 4)) {
            unsigned char b[8];
            if (um_read(off + 8, b, 8) == 8) {
                S_stsc = off + 8; S_stsc_n = (long)be32(b + 4);
            }
        } else if (!memcmp(h + 4, "stts", 4)) {
            S_stts = off + 8;
        }
        off += size;
    }
    return 1;
}

/* Inside stsd -> mp4a -> esds, the AudioSpecificConfig sits behind the MPEG-4
 * descriptor tags (0x03 ES, 0x04 DecoderConfig, 0x05 DecoderSpecificInfo),
 * each with a variable-length size. */
static int mp4_find_asc(long off, long end, int depth)
{
    unsigned char h[8];
    while (off + 8 <= end && depth < 10) {
        long size;
        if (um_read(off, h, 8) != 8) return 0;
        size = (long)be32(h);
        if (size < 8 || off + size > end) return 0;
        if (!memcmp(h + 4, "mp4a", 4)) {
            /* skip the 28-byte AudioSampleEntry, then look for esds */
            mp4_find_asc(off + 8 + 28, off + size, depth + 1);
        } else if (!memcmp(h + 4, "esds", 4)) {
            unsigned char b[64];
            int n = (int)um_read(off + 12, b, sizeof b), i = 0;
            while (i < n) {
                int tag = b[i++], len = 0, k;
                for (k = 0; k < 4 && i < n; k++) {   /* variable-length size */
                    int c = b[i++];
                    len = (len << 7) | (c & 0x7F);
                    if (!(c & 0x80)) break;
                }
                if (tag == 0x03) { i += 3; continue; }       /* ES: skip ids   */
                if (tag == 0x04) { i += 13; continue; }      /* DecoderConfig  */
                if (tag == 0x05) return parse_asc(b + i, len < n - i ? len : n - i);
                i += len;
            }
        }
        off += size;
    }
    return 0;
}

/* ===========================================================================
 * The decode core.
 * ======================================================================== */

/* ---- frame bit reader ------------------------------------------------------- */
static unsigned agb(int n)
{
    unsigned v = 0;
    while (n-- > 0) {
        long byte = S->fpos >> 3;
        int  bit  = 7 - (int)(S->fpos & 7);
        if (S->fpos < S->fbits)
            v = (v << 1) | ((S->frame[byte] >> bit) & 1u);
        else { v <<= 1; S->fover = 1; }
        S->fpos++;
    }
    return v;
}

/* ---- Huffman (per-length runs, binary search - same scheme as um_mp3.c) ----- */
static int ahuff(const aac_htab *t)
{
    unsigned code = 0;
    int len;
    for (len = 1; len <= AAC_HUFF_MAXLEN; len++) {
        code = (code << 1) | agb(1);
        if (S->fover) return -1;
        if (t->cnt[len]) {
            int lo = t->idx[len], hi = lo + t->cnt[len] - 1;
            while (lo <= hi) {
                int mid = (lo + hi) >> 1;
                if (t->codes[mid] == code) return t->syms[mid];
                if (t->codes[mid] < code) lo = mid + 1; else hi = mid - 1;
            }
        }
    }
    return -1;
}

/* ---- derived tables (built once into the state block) ----------------------- */
static float cbrtf_(float x)                  /* Newton - no libm cbrt         */
{
    float r;
    int i;
    if (x <= 0.0f) return 0.0f;
    r = x > 1.0f ? x / 3.0f + 1.0f : x;
    for (i = 0; i < 24; i++)
        r = (2.0f * r + x / (r * r)) / 3.0f;
    return r;
}

/* 2^(n/4) by repeated multiplication - no libm pow */
static float pow2_q4(int n)
{
    float q = sqrtf(sqrtf(2.0f)), v = 1.0f;
    while (n >= 4)  { v *= 2.0f; n -= 4; }
    while (n <= -4) { v *= 0.5f; n += 4; }
    while (n > 0)   { v *= q;    n--; }
    while (n < 0)   { v /= q;    n++; }
    return v;
}

/* modified Bessel I0 by its power series - all the KBD window needs */
static float bessel_i0(float x)
{
    float sum = 1.0f, term = 1.0f, half = x * 0.5f;
    int k;
    for (k = 1; k < 64; k++) {
        term *= half / (float)k;
        sum  += term * term;
        if (term * term < 1e-12f * sum) break;
    }
    return sum;
}

/* Kaiser-Bessel-derived window, left half: w[n] = sqrt(cumsum/total) over the
 * Kaiser kernel (ISO/IEC 14496-3; alpha = 4 long, 6 short). The kernel has
 * half+1 points; scratch reuses the state's wbuf. */
static void build_kbd(float *w, int half, float alpha)
{
    float *kern = S->wbuf;                    /* scratch: half+1 <= 1025       */
    float pa = 3.14159265f * alpha, total = 0.0f, run = 0.0f;
    int j;
    for (j = 0; j <= half; j++) {
        float t = ((float)j - (float)half * 0.5f) / ((float)half * 0.5f);
        float a = 1.0f - t * t;
        kern[j] = bessel_i0(pa * sqrtf(a > 0.0f ? a : 0.0f));
        total += kern[j];
    }
    for (j = 0; j < half; j++) {
        run += kern[j];
        w[j] = sqrtf(run / total);
    }
}

static void build_tables(void)
{
    int i;
    for (i = 0; i < 8192; i++)
        S->pow43[i] = (float)i * cbrtf_((float)i);
    for (i = 0; i < 256; i++)
        S->sf_gain[i] = pow2_q4(i - 100);
    for (i = 0; i < 1024; i++)
        S->win_sine_l[i] = sinf(3.14159265f / 2048.0f * ((float)i + 0.5f));
    for (i = 0; i < 128; i++)
        S->win_sine_s[i] = sinf(3.14159265f / 256.0f * ((float)i + 0.5f));
    build_kbd(S->win_kbd_l, 1024, 4.0f);
    build_kbd(S->win_kbd_s, 128, 6.0f);
    for (i = 0; i < 256; i++) {
        S->fft_c[i] = cosf(2.0f * 3.14159265f * (float)i / 512.0f);
        S->fft_s[i] = sinf(2.0f * 3.14159265f * (float)i / 512.0f);
    }
    for (i = 0; i < 512; i++) {
        S->pre_c_l[i]  = cosf(3.14159265f * (float)i / 1024.0f);
        S->pre_s_l[i]  = sinf(3.14159265f * (float)i / 1024.0f);
        S->post_c_l[i] = cosf(3.14159265f * ((float)i + 0.25f) / 1024.0f);
        S->post_s_l[i] = sinf(3.14159265f * ((float)i + 0.25f) / 1024.0f);
    }
    for (i = 0; i < 64; i++) {
        S->pre_c_s[i]  = cosf(3.14159265f * (float)i / 128.0f);
        S->pre_s_s[i]  = sinf(3.14159265f * (float)i / 128.0f);
        S->post_c_s[i] = cosf(3.14159265f * ((float)i + 0.25f) / 128.0f);
        S->post_s_s[i] = sinf(3.14159265f * ((float)i + 0.25f) / 128.0f);
    }
}

/* ---- IMDCT ------------------------------------------------------------------
 * x[n] = (2/N) sum X[k] cos(2pi/N (n + N/4 + 1/2)(k + 1/2)), N = 2048 or 256.
 * Computed as a DCT-IV of size M = N/2 (an index shuffle with signs maps it
 * onto the IMDCT), and the DCT-IV via an H = M/2 point complex FFT:
 *   z[p] = (X[2p] + i X[M-1-2p]) e^(-i.pi.p/M)
 *   Z    = FFT_H(z)
 *   w[t] = Z[t] e^(-i.pi.(t+1/4)/M)
 *   y[2t] = Re w[t],  y[M-1-2t] = -Im w[t]
 * (derived from the DFT split of the DCT-IV kernel; verified against the
 * direct O(N^2) transform during development). */
static void cfft(float *re, float *im, int h)
{
    int i, j, len;
    /* bit-reverse permute */
    for (i = 1, j = 0; i < h; i++) {
        int bit = h >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= h; len <<= 1) {
        int half = len >> 1, tstep = (512 / len);
        for (i = 0; i < h; i += len) {
            for (j = 0; j < half; j++) {
                int ti = j * tstep;
                float c = S->fft_c[ti], s = S->fft_s[ti];
                float ur = re[i + j], ui = im[i + j];
                float vr = re[i + j + half] * c + im[i + j + half] * s;
                float vi = im[i + j + half] * c - re[i + j + half] * s;
                re[i + j] = ur + vr;  im[i + j] = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
            }
        }
    }
}

static void imdct_do(const float *X, int is_long, float *out)
{
    int N = is_long ? 2048 : 256, M = N >> 1, H = M >> 1, N4 = N >> 2;
    const float *pc = is_long ? S->pre_c_l  : S->pre_c_s;
    const float *ps = is_long ? S->pre_s_l  : S->pre_s_s;
    const float *qc = is_long ? S->post_c_l : S->post_c_s;
    const float *qs = is_long ? S->post_s_l : S->post_s_s;
    float scale = 2.0f / (float)N;
    int p, t, n;

    for (p = 0; p < H; p++) {
        float a = X[2 * p], b = X[M - 1 - 2 * p];
        S->zre[p] = a * pc[p] + b * ps[p];
        S->zim[p] = b * pc[p] - a * ps[p];
    }
    cfft(S->zre, S->zim, H);
    for (t = 0; t < H; t++) {
        float wr = S->zre[t] * qc[t] + S->zim[t] * qs[t];
        float wi = S->zim[t] * qc[t] - S->zre[t] * qs[t];
        S->dct[2 * t] = wr;
        S->dct[M - 1 - 2 * t] = -wi;
    }
    for (n = 0; n < N; n++) {
        int m = n + N4;
        float v;
        if (m < M)          v =  S->dct[m];
        else if (m < 2 * M) v = -S->dct[2 * M - 1 - m];
        else                v = -S->dct[m - 2 * M];
        out[n] = v * scale;
    }
}

/* ---- ics_info / grouping ----------------------------------------------------- */
static int ics_nsfb(const aac_ics *ics)
{
    const aac_sfb_info *si = &kAacSfb[A.rate_idx];
    return ics->seq == SEQ_SHORT ? si->nshort : si->nlong;
}

static const unsigned short *ics_swb(const aac_ics *ics)
{
    const aac_sfb_info *si = &kAacSfb[A.rate_idx];
    return ics->seq == SEQ_SHORT ? si->swb_short : si->swb_long;
}

/* band b starts at swb[b-1] (the tables store exclusive tops) */
static int swb_lo(const unsigned short *swb, int b) { return b ? swb[b - 1] : 0; }

static int parse_ics_info(aac_ics *ics)
{
    (void)agb(1);                              /* ics_reserved_bit             */
    ics->seq   = (int)agb(2);
    ics->shape = (int)agb(1);
    if (ics->seq == SEQ_SHORT) {
        unsigned grouping;
        int b, g;
        ics->max_sfb = (int)agb(4);
        grouping = agb(7);
        ics->num_windows = 8;
        ics->num_groups = 1;
        ics->group_len[0] = 1;
        for (b = 6; b >= 0; b--) {
            if ((grouping >> b) & 1)
                ics->group_len[ics->num_groups - 1]++;
            else {
                g = ics->num_groups++;
                ics->group_len[g] = 1;
            }
        }
    } else {
        ics->max_sfb = (int)agb(6);
        if (agb(1)) return 0;                  /* prediction / LTP: not LC     */
        ics->num_windows = 1;
        ics->num_groups = 1;
        ics->group_len[0] = 1;
    }
    if (ics->max_sfb > ics_nsfb(ics)) return 0;
    return !S->fover;
}

/* ---- section data ------------------------------------------------------------ */
static int parse_sections(aac_ics *ics)
{
    int bits = ics->seq == SEQ_SHORT ? 3 : 5;
    int esc = (1 << bits) - 1;
    int g, k;
    for (g = 0; g < ics->num_groups; g++) {
        k = 0;
        while (k < ics->max_sfb) {
            int cb = (int)agb(4), len = 0, l, i;
            if (cb == 12) return 0;            /* reserved                     */
            while ((l = (int)agb(bits)) == esc && !S->fover) len += esc;
            len += l;
            if (len <= 0 || k + len > ics->max_sfb || S->fover) return 0;
            for (i = 0; i < len; i++)
                ics->cb[g][k + i] = (unsigned char)cb;
            k += len;
        }
    }
    return 1;
}

/* ---- scalefactor data ---------------------------------------------------------
 * Three independent DPCM chains share the scalefactor codebook: scalefactors
 * (start = global_gain), intensity positions (start = 0) and PNS energies
 * (start = global_gain - 90, with the FIRST noise band sent as a 9-bit PCM
 * delta instead of a codeword). */
static int parse_scalefactors(aac_ics *ics)
{
    int sf = ics->global_gain, is = 0, nrg = ics->global_gain - 90;
    int noise_pcm = 1;
    int g, b;
    for (g = 0; g < ics->num_groups; g++) {
        for (b = 0; b < ics->max_sfb; b++) {
            int cb = ics->cb[g][b], d;
            if (cb == CB_ZERO) { ics->sf[g][b] = 0; continue; }
            if (cb == CB_IS || cb == CB_IS2) {
                d = ahuff(&kAacScl);
                if (d < 0) return 0;
                is += d - 60;
                ics->sf[g][b] = (short)is;
            } else if (cb == CB_NOISE) {
                if (noise_pcm) { noise_pcm = 0; d = (int)agb(9) - 256; }
                else {
                    d = ahuff(&kAacScl);
                    if (d < 0) return 0;
                    d -= 60;
                }
                nrg += d;
                ics->sf[g][b] = (short)nrg;
            } else {
                d = ahuff(&kAacScl);
                if (d < 0) return 0;
                sf += d - 60;
                if (sf < 0 || sf > 255) return 0;
                ics->sf[g][b] = (short)sf;
            }
        }
    }
    return !S->fover;
}

/* ---- pulse + TNS -------------------------------------------------------------- */
static int parse_pulse(aac_ics *ics)
{
    int i;
    if (ics->seq == SEQ_SHORT) return 0;       /* pulse is long-window only    */
    ics->npulse = (int)agb(2) + 1;
    ics->pulse_sfb = (int)agb(6);
    if (ics->pulse_sfb >= ics_nsfb(ics)) return 0;
    for (i = 0; i < ics->npulse; i++) {
        ics->p_off[i] = (unsigned char)agb(5);
        ics->p_amp[i] = (unsigned char)agb(4);
    }
    return !S->fover;
}

static int parse_tns(aac_ics *ics)
{
    int is_short = ics->seq == SEQ_SHORT;
    int w, f, i;
    for (w = 0; w < ics->num_windows; w++) {
        int nf = (int)agb(is_short ? 1 : 2);
        int coef_res;
        ics->n_filt[w] = (unsigned char)nf;
        if (!nf) continue;
        coef_res = (int)agb(1);
        for (f = 0; f < nf; f++) {
            int order, compress = 0, cbits, m;
            float tmp[AAC_MAX_ORDER], b[AAC_MAX_ORDER + 1], *a;
            ics->t_len[w][f] = (unsigned char)agb(is_short ? 4 : 6);
            order = (int)agb(is_short ? 3 : 5);
            if (order > (is_short ? 7 : AAC_MAX_ORDER)) return 0; /* not LC   */
            ics->t_order[w][f] = (unsigned char)order;
            if (!order) continue;
            ics->t_dir[w][f] = (unsigned char)agb(1);
            compress = (int)agb(1);
            cbits = coef_res + 3 - compress;
            /* dequantise the sent reflection-ish coefficients (ISO's sin
               reconstruction), then run the standard recursion to LPC */
            {
                float iqf  = (((float)(1 << (coef_res + 2))) - 0.5f) /
                             (3.14159265f / 2.0f);
                float iqfm = (((float)(1 << (coef_res + 2))) + 0.5f) /
                             (3.14159265f / 2.0f);
                for (i = 0; i < order; i++) {
                    int v = (int)agb(cbits);
                    if (v >= (1 << (cbits - 1))) v -= 1 << cbits; /* sign ext */
                    tmp[i] = sinf((float)v / (v >= 0 ? iqf : iqfm));
                }
            }
            a = ics->t_lpc[w][f];
            a[0] = 1.0f;
            for (m = 1; m <= order; m++) {
                float t = tmp[m - 1];
                for (i = 1; i < m; i++) b[i] = a[i] + t * a[m - i];
                for (i = 1; i < m; i++) a[i] = b[i];
                a[m] = t;
            }
        }
    }
    return !S->fover;
}

/* ---- spectral data ------------------------------------------------------------ */
static int esc_value(void)
{
    int n = 0, off;
    while (agb(1)) { n++; if (n > 8 || S->fover) return -1; }
    off = (int)agb(n + 4);
    return (1 << (n + 4)) + off;               /* 16 .. 8191                   */
}

static int dec_spec_chunk(int book, int *out, int n)
{
    const aac_htab *t = &kAacSpec[book];
    int i, k;
    if (book <= 4) {                           /* quads                        */
        for (i = 0; i + 3 < n; i += 4) {
            int s = ahuff(t), v[4];
            if (s < 0) return 0;
            if (book <= 2) {                   /* signed, -1..1                */
                v[0] = s / 27 - 1; v[1] = (s / 9) % 3 - 1;
                v[2] = (s / 3) % 3 - 1; v[3] = s % 3 - 1;
            } else {                           /* unsigned 0..2 + sign bits    */
                v[0] = s / 27; v[1] = (s / 9) % 3;
                v[2] = (s / 3) % 3; v[3] = s % 3;
                for (k = 0; k < 4; k++)
                    if (v[k] && agb(1)) v[k] = -v[k];
            }
            for (k = 0; k < 4; k++) out[i + k] = v[k];
        }
    } else {                                   /* pairs                        */
        int mod = book <= 6 ? 9 : (book <= 8 ? 8 : (book <= 10 ? 13 : 17));
        int off = book <= 6 ? 4 : 0;
        for (i = 0; i + 1 < n; i += 2) {
            int s = ahuff(t), x, y, sx = 1, sy = 1;
            if (s < 0) return 0;
            x = s / mod - off;
            y = s % mod - off;
            if (!off) {                        /* unsigned books: sign bits    */
                if (x && agb(1)) sx = -1;
                if (y && agb(1)) sy = -1;
            }
            if (book == CB_ESC) {              /* 16 marks an escape          */
                if (x == 16) { x = esc_value(); if (x < 0) return 0; }
                if (y == 16) { y = esc_value(); if (y < 0) return 0; }
            }
            out[i] = sx * x;
            out[i + 1] = sy * y;
        }
    }
    return !S->fover;
}

static int parse_spectral(aac_ics *ics, int ch)
{
    const unsigned short *swb = ics_swb(ics);
    int *q = S->quant[ch];
    int g, b, pos = 0;
    memset(q, 0, sizeof S->quant[0]);
    for (g = 0; g < ics->num_groups; g++) {
        int glen = ics->group_len[g];
        int gsize = ics->seq == SEQ_SHORT ? 128 * glen : 1024;
        int base = pos;
        for (b = 0; b < ics->max_sfb; b++) {
            int width = (swb[b] - swb_lo(swb, b)) *
                        (ics->seq == SEQ_SHORT ? glen : 1);
            int cb = ics->cb[g][b];
            if (pos + width > base + gsize || pos + width > 1024) return 0;
            if (cb >= 1 && cb <= 11) {
                if (!dec_spec_chunk(cb, q + pos, width)) return 0;
            }
            pos += width;
        }
        pos = base + gsize;
        if (pos > 1024) return 0;
    }
    /* pulse data adds to the quantised magnitudes (long windows only) */
    if (ics->npulse) {
        int k = swb_lo(swb, ics->pulse_sfb), i;        /* start of that band  */
        for (i = 0; i < ics->npulse; i++) {
            k += ics->p_off[i];
            if (k >= 1024) break;
            if (q[k] > 0)      q[k] += ics->p_amp[i];
            else if (q[k] < 0) q[k] -= ics->p_amp[i];
            else               q[k]  = ics->p_amp[i];
        }
    }
    return 1;
}

/* ---- requantise ---------------------------------------------------------------- */
static float requant_one(int v, float gain)
{
    int a = v < 0 ? -v : v;
    float r;
    if (a > 8191) a = 8191;
    r = S->pow43[a] * gain;
    return v < 0 ? -r : r;
}

static void requantise(aac_ics *ics, int ch)
{
    const unsigned short *swb = ics_swb(ics);
    const int *q = S->quant[ch];
    float *x = S->coef[ch];
    int g, b, i, pos = 0;
    memset(x, 0, sizeof S->coef[0]);
    for (g = 0; g < ics->num_groups; g++) {
        int glen = ics->group_len[g];
        int gsize = ics->seq == SEQ_SHORT ? 128 * glen : 1024;
        int base = pos;
        for (b = 0; b < ics->max_sfb; b++) {
            int width = (swb[b] - swb_lo(swb, b)) *
                        (ics->seq == SEQ_SHORT ? glen : 1);
            int cb = ics->cb[g][b];
            if (cb >= 1 && cb <= 11) {
                float gain = S->sf_gain[ics->sf[g][b] & 255];
                for (i = 0; i < width; i++)
                    x[pos + i] = requant_one(q[pos + i], gain);
            }
            pos += width;
        }
        pos = base + gsize;
    }
}

/* ---- PNS ------------------------------------------------------------------------
 * Noise bands get a pseudo-random vector normalised per window to the coded
 * energy. The LCG reseeds deterministically each frame, so a given file
 * always decodes to the same samples. When M/S marks a band and BOTH
 * channels carry noise there, the right channel reuses the left's seed -
 * that is the spec's correlated-noise case. */
static unsigned lcg_next(void)
{
    S->lcg = S->lcg * 1664525u + 1013904223u;
    return S->lcg;
}

static void pns_fill(aac_ics *ics, int ch, int is_right_cpe)
{
    const unsigned short *swb = ics_swb(ics);
    float *x = S->coef[ch];
    int g, b, w, i, pos = 0;
    for (g = 0; g < ics->num_groups; g++) {
        int glen = ics->group_len[g];
        int gsize = ics->seq == SEQ_SHORT ? 128 * glen : 1024;
        int nwin = ics->seq == SEQ_SHORT ? glen : 1;
        int base = pos;
        for (b = 0; b < ics->max_sfb; b++) {
            int ws = swb[b] - swb_lo(swb, b);
            if (ics->cb[g][b] == CB_NOISE) {
                int nrg = ics->sf[g][b];
                int corr = is_right_cpe && S->ms_mask &&
                           (S->ms_mask == 2 || S->ms_used[g][b]) &&
                           S->ics[0].cb[g][b] == CB_NOISE;
                unsigned saved = S->lcg;
                float gain;
                if (nrg < 0) nrg = 0;
                if (nrg > 255) nrg = 255;
                gain = S->sf_gain[nrg];
                if (corr) S->lcg = S->pns_seed[g][b];
                else if (!is_right_cpe) S->pns_seed[g][b] = S->lcg;
                for (w = 0; w < nwin; w++) {
                    float e = 0.0f, sc;
                    float *dst = x + pos + w * ws;
                    for (i = 0; i < ws; i++) {
                        dst[i] = (float)(int)lcg_next() * (1.0f / 2147483648.0f);
                        e += dst[i] * dst[i];
                    }
                    sc = e > 0.0f ? gain / sqrtf(e) : 0.0f;
                    for (i = 0; i < ws; i++) dst[i] *= sc;
                }
                if (corr) S->lcg = saved;
            }
            pos += ws * nwin;
        }
        pos = base + gsize;
    }
}

/* ---- stereo tools (CPE, group-order spectra) ------------------------------------ */
static void apply_ms(void)
{
    aac_ics *l = &S->ics[0], *r = &S->ics[1];
    const unsigned short *swb = ics_swb(l);
    int g, b, i, pos = 0;
    if (!S->ms_mask) return;
    for (g = 0; g < l->num_groups; g++) {
        int glen = l->group_len[g];
        int gsize = l->seq == SEQ_SHORT ? 128 * glen : 1024;
        int base = pos;
        for (b = 0; b < l->max_sfb; b++) {
            int width = (swb[b] - swb_lo(swb, b)) *
                        (l->seq == SEQ_SHORT ? glen : 1);
            int used = S->ms_mask == 2 || S->ms_used[g][b];
            if (used && l->cb[g][b] <= 11 && r->cb[g][b] <= 11) {
                for (i = pos; i < pos + width; i++) {
                    float m = S->coef[0][i], s = S->coef[1][i];
                    S->coef[0][i] = m + s;
                    S->coef[1][i] = m - s;
                }
            }
            pos += width;
        }
        pos = base + gsize;
    }
}

static void apply_intensity(void)
{
    aac_ics *l = &S->ics[0], *r = &S->ics[1];
    const unsigned short *swb = ics_swb(r);
    int g, b, i, pos = 0;
    for (g = 0; g < r->num_groups; g++) {
        int glen = r->group_len[g];
        int gsize = r->seq == SEQ_SHORT ? 128 * glen : 1024;
        int base = pos;
        for (b = 0; b < r->max_sfb; b++) {
            int width = (swb[b] - swb_lo(swb, b)) *
                        (r->seq == SEQ_SHORT ? glen : 1);
            int cb = r->cb[g][b];
            if (cb == CB_IS || cb == CB_IS2) {
                int p = r->sf[g][b];
                float dir = cb == CB_IS2 ? -1.0f : 1.0f;
                float gain;
                if (S->ms_mask == 1 && S->ms_used[g][b]) dir = -dir;
                if (p < -155) p = -155;
                if (p > 100)  p = 100;
                gain = S->sf_gain[100 - p] * dir;   /* 2^(-p/4), signed       */
                for (i = pos; i < pos + width; i++)
                    S->coef[1][i] = S->coef[0][i] * gain;
            }
            pos += width;
        }
        pos = base + gsize;
    }
    (void)l;
}

/* ---- deinterleave (short blocks arrive band-major within each group) ------------ */
static void deinterleave(aac_ics *ics, int ch)
{
    const unsigned short *swb = ics_swb(ics);
    const float *x = S->coef[ch];
    float *y = S->spec;
    int g, b, w, i, pos = 0, win0 = 0;
    if (ics->seq != SEQ_SHORT) {
        memcpy(y, x, sizeof S->spec);
        return;
    }
    memset(y, 0, sizeof S->spec);
    for (g = 0; g < ics->num_groups; g++) {
        int glen = ics->group_len[g];
        int base = pos;
        for (b = 0; b < ics->max_sfb; b++) {
            int lo = swb_lo(swb, b), ws = swb[b] - lo;
            for (w = 0; w < glen; w++)
                for (i = 0; i < ws; i++)
                    y[(win0 + w) * 128 + lo + i] = x[pos + w * ws + i];
            pos += ws * glen;
        }
        pos = base + 128 * glen;
        win0 += glen;
    }
}

/* ---- TNS (all-pole filter over each window's spectrum) -------------------------- */
static void apply_tns(aac_ics *ics)
{
    const unsigned short *swb = ics_swb(ics);
    int is_short = ics->seq == SEQ_SHORT;
    int wsize = is_short ? 128 : 1024;
    int nb = ics_nsfb(ics);
    int mmax = is_short ? kTnsMaxShort[A.rate_idx] : kTnsMaxLong[A.rate_idx];
    int w, f, i, k;
    if (mmax > nb) mmax = nb;
    if (mmax > ics->max_sfb) mmax = ics->max_sfb;
    for (w = 0; w < ics->num_windows; w++) {
        int bottom = nb;
        for (f = 0; f < ics->n_filt[w]; f++) {
            int top = bottom, order = ics->t_order[w][f];
            int lo, hi, start, end;
            const float *a = ics->t_lpc[w][f];
            float *x = S->spec + w * wsize;
            bottom = top - ics->t_len[w][f];
            if (bottom < 0) bottom = 0;
            if (!order) continue;
            lo = bottom < mmax ? bottom : mmax;
            hi = top < mmax ? top : mmax;
            start = swb_lo(swb, lo);
            end = swb_lo(swb, hi);
            if (end > wsize) end = wsize;
            if (start >= end) continue;
            if (!ics->t_dir[w][f]) {
                for (i = start; i < end; i++) {
                    float acc = x[i];
                    for (k = 1; k <= order && i - k >= start; k++)
                        acc -= a[k] * x[i - k];
                    x[i] = acc;
                }
            } else {
                for (i = end - 1; i >= start; i--) {
                    float acc = x[i];
                    for (k = 1; k <= order && i + k < end; k++)
                        acc -= a[k] * x[i + k];
                    x[i] = acc;
                }
            }
        }
    }
}

/* ---- filterbank: IMDCT + window + overlap-add ----------------------------------- */
static void filterbank(aac_ics *ics, int ch)
{
    const float *lw_prev = S->prev_shape[ch] ? S->win_kbd_l : S->win_sine_l;
    const float *sw_prev = S->prev_shape[ch] ? S->win_kbd_s : S->win_sine_s;
    const float *lw_cur  = ics->shape ? S->win_kbd_l : S->win_sine_l;
    const float *sw_cur  = ics->shape ? S->win_kbd_s : S->win_sine_s;
    float *ov = S->overlap[ch];
    float *wb = S->wbuf;
    int i, k;

    if (ics->seq == SEQ_SHORT) {
        memset(wb, 0, 2048 * sizeof(float));
        for (k = 0; k < 8; k++) {
            const float *lw = k == 0 ? sw_prev : sw_cur;
            imdct_do(S->spec + k * 128, 0, S->xtime);
            for (i = 0; i < 128; i++)
                wb[448 + k * 128 + i] += S->xtime[i] * lw[i];
            for (i = 128; i < 256; i++)
                wb[448 + k * 128 + i] += S->xtime[i] * sw_cur[255 - i];
        }
    } else {
        imdct_do(S->spec, 1, S->xtime);
        /* left half: how this frame fades in over the previous one */
        if (ics->seq == SEQ_STOP) {
            for (i = 0; i < 448; i++)    wb[i] = 0.0f;
            for (i = 448; i < 576; i++)  wb[i] = S->xtime[i] * sw_prev[i - 448];
            for (i = 576; i < 1024; i++) wb[i] = S->xtime[i];
        } else {
            for (i = 0; i < 1024; i++)   wb[i] = S->xtime[i] * lw_prev[i];
        }
        /* right half: what this frame leaves for the next */
        if (ics->seq == SEQ_START) {
            for (i = 0; i < 448; i++)    wb[1024 + i] = S->xtime[1024 + i];
            for (i = 448; i < 576; i++)
                wb[1024 + i] = S->xtime[1024 + i] * sw_cur[127 - (i - 448)];
            for (i = 576; i < 1024; i++) wb[1024 + i] = 0.0f;
        } else {
            for (i = 0; i < 1024; i++)
                wb[1024 + i] = S->xtime[1024 + i] * lw_cur[1023 - i];
        }
    }

    /* AAC codes PCM at integer scale (+-32768 is full scale), so the
       filterbank output IS the sample value - no 32767 rescale like MP3 */
    for (i = 0; i < 1024; i++) {
        float v = ov[i] + wb[i];
        int q = (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
        if (q > 32767) q = 32767; else if (q < -32768) q = -32768;
        S->pcm[ch][i] = (short)q;
        ov[i] = wb[1024 + i];
    }
    S->prev_shape[ch] = ics->shape;
}

/* ---- individual channel stream --------------------------------------------------- */
static int parse_ics(aac_ics *ics, int ch, int have_info)
{
    ics->global_gain = (int)agb(8);
    ics->npulse = 0;
    memset(ics->n_filt, 0, sizeof ics->n_filt);
    if (!have_info && !parse_ics_info(ics)) return 0;
    if (!parse_sections(ics)) return 0;
    if (!parse_scalefactors(ics)) return 0;
    if (agb(1) && !parse_pulse(ics)) return 0;         /* pulse_data_present  */
    if (agb(1) && !parse_tns(ics)) return 0;           /* tns_data_present    */
    if (agb(1)) return 0;                              /* SSR gain control    */
    if (!parse_spectral(ics, ch)) return 0;
    return !S->fover;
}

/* everything after the stereo tools is per channel */
static void process_channel(aac_ics *ics, int ch)
{
    deinterleave(ics, ch);
    apply_tns(ics);
    filterbank(ics, ch);
}

/* ---- the element loop (one raw_data_block = one 1024-sample frame) --------------- */
static void skip_pce(void)
{
    int i, n_front, n_side, n_back, n_lfe, n_data, n_cc, n;
    (void)agb(4); (void)agb(2); (void)agb(4);
    n_front = (int)agb(4); n_side = (int)agb(4); n_back = (int)agb(4);
    n_lfe = (int)agb(2); n_data = (int)agb(3); n_cc = (int)agb(4);
    if (agb(1)) (void)agb(4);                          /* mono mixdown        */
    if (agb(1)) (void)agb(4);                          /* stereo mixdown      */
    if (agb(1)) (void)agb(3);                          /* matrix mixdown      */
    for (i = 0; i < n_front + n_side + n_back; i++) { (void)agb(1); (void)agb(4); }
    for (i = 0; i < n_lfe + n_data; i++) (void)agb(4);
    for (i = 0; i < n_cc; i++) { (void)agb(1); (void)agb(4); }
    S->fpos = (S->fpos + 7) & ~7L;                     /* byte_align          */
    n = (int)agb(8);                                   /* comment bytes       */
    S->fpos += 8L * n;
}

static int parse_frame(void)
{
    int got = 0;
    S->lcg = 0x2545F491u ^ ((unsigned)S->frame_no * 2654435761u);
    while (!S->fover && S->fpos + 3 <= S->fbits) {
        int id = (int)agb(3);
        if (id == 7) break;                            /* END                 */
        switch (id) {
        case 0: case 3: {                              /* SCE / LFE           */
            int keep = !got;
            aac_ics *ics = &S->ics[0];
            (void)agb(4);                              /* instance tag        */
            if (!parse_ics(ics, 0, 0)) return -1;
            if (keep) {
                requantise(ics, 0);
                pns_fill(ics, 0, 0);
                process_channel(ics, 0);
                got = 1;
                if (A.channels == 2) {                 /* dual-mono fallback  */
                    memcpy(S->pcm[1], S->pcm[0], sizeof S->pcm[0]);
                    got = 2;
                }
            }
            break;
        }
        case 1: {                                      /* CPE                 */
            int keep = !got && A.channels == 2;
            int common;
            (void)agb(4);
            common = (int)agb(1);
            S->ms_mask = 0;
            if (common) {
                if (!parse_ics_info(&S->ics[0])) return -1;
                S->ms_mask = (int)agb(2);
                if (S->ms_mask == 3) return -1;
                if (S->ms_mask == 1) {
                    int g, b;
                    for (g = 0; g < S->ics[0].num_groups; g++)
                        for (b = 0; b < S->ics[0].max_sfb; b++)
                            S->ms_used[g][b] = (unsigned char)agb(1);
                }
                S->ics[1] = S->ics[0];
            }
            if (!parse_ics(&S->ics[0], 0, common)) return -1;
            if (!parse_ics(&S->ics[1], 1, common)) return -1;
            if (!common) S->ms_mask = 0;
            if (keep) {
                requantise(&S->ics[0], 0);
                requantise(&S->ics[1], 1);
                pns_fill(&S->ics[0], 0, 0);
                pns_fill(&S->ics[1], 1, 1);
                apply_ms();
                apply_intensity();
                process_channel(&S->ics[0], 0);
                process_channel(&S->ics[1], 1);
                got = 2;
            }
            break;
        }
        case 2:                                        /* CCE: not decoded    */
            return -1;
        case 4: {                                      /* DSE                 */
            int align, n;
            (void)agb(4);
            align = (int)agb(1);
            n = (int)agb(8);
            if (n == 255) n += (int)agb(8);
            if (align) S->fpos = (S->fpos + 7) & ~7L;
            S->fpos += 8L * n;
            break;
        }
        case 5:                                        /* PCE                 */
            skip_pce();
            break;
        case 6: {                                      /* FIL - SBR lives here
                                                          and is skipped (the
                                                          LC core is the
                                                          output)             */
            int n = (int)agb(4);
            if (n == 15) n += (int)agb(8) - 1;
            S->fpos += 8L * n;
            break;
        }
        default:
            return -1;
        }
    }
    if (S->fover || !got) return -1;
    return got;
}

/* ---- frame fetch ------------------------------------------------------------------ */
static int fetch_frame(void)
{
    if (A.is_mp4) {
        long len, off;
        if (A.next_samp >= A.nsamp) return 0;
        off = (long)S->soff[A.next_samp];
        len = (long)S->slen[A.next_samp];
        A.next_samp++;
        if (len <= 0 || len > AAC_MAX_FRAME) return -1;
        if (um_read(off, S->frame, len) != len) return 0;   /* truncated: end */
        S->fbits = 8L * len;
    } else {
        int flen = 0, hdr = 0;
        long payload;
        if (A.pos + 7 > um_size()) return 0;
        if (!adts_at(A.pos, 0, 0, &flen, &hdr, 0, 0)) {
            long p = adts_find(A.pos);
            if (p < 0) return 0;
            A.pos = p;
            if (!adts_at(A.pos, 0, 0, &flen, &hdr, 0, 0)) return 0;
        }
        payload = flen - hdr;
        if (payload <= 0 || payload > AAC_MAX_FRAME) { A.pos += flen; return -1; }
        if (um_read(A.pos + hdr, S->frame, payload) != payload) return 0;
        A.pos += flen;
        S->fbits = 8L * payload;
    }
    S->fpos = 0;
    S->fover = 0;
    return 1;
}

static int decode_frame(void)
{
    int r = fetch_frame();
    if (r <= 0) return r;
    S->frame_no++;
    if (parse_frame() < 0) return -1;
    S->pcm_have = AAC_FRAME_LEN;
    S->pcm_taken = 0;
    return 1;
}

/* ---- decoder interface ------------------------------------------------------ */
static int aac_probe(const unsigned char *head, long n, const char *ext)
{
    if (n >= 12 && !memcmp(head + 4, "ftyp", 4)) {
        /* an MP4 family file - only claim the audio-ish brands */
        if (!memcmp(head + 8, "M4A ", 4) || !memcmp(head + 8, "mp42", 4) ||
            !memcmp(head + 8, "isom", 4) || !memcmp(head + 8, "M4B ", 4))
            return 1;
    }
    if (n >= 2 && head[0] == 0xFF && (head[1] & 0xF0) == 0xF0) return 1;  /* ADTS */
    return !strcmp(ext, "AAC") || !strcmp(ext, "M4A") || !strcmp(ext, "ADT");
}

static void aac_close(void)
{
    um_free(S);
    S = 0;
    memset(&A, 0, sizeof A);
}

static int aac_open(um_audio_info *info)
{
    long nsamp_cap = 0;

    aac_close();                                    /* idempotent reset       */
    S_stsz = S_stco = S_stsc = S_stts = 0;
    S_stsz_n = S_stco_n = S_stsc_n = 0;

    {   /* MP4 if it opens with an ftyp box */
        unsigned char h[12];
        if (um_read(0, h, 12) == 12 && !memcmp(h + 4, "ftyp", 4)) {
            A.is_mp4 = 1;
            mp4_walk(0, um_size(), 0);
            if (!A.rate) {                            /* no AudioSpecificConfig */
                um_set_error("AAC (M4A) - no decoder config found (truncated file?)");
                return 0;
            }
            nsamp_cap = S_stsz_n;
            if (nsamp_cap < 1) {
                um_set_error("AAC (M4A) - no sample table");
                return 0;
            }
            if (nsamp_cap > AAC_MAX_TABLE) nsamp_cap = AAC_MAX_TABLE;
        }
    }
    if (!A.is_mp4) {
        long off = adts_find(0);
        int rate = 0, ch = 0, flen = 0, prof = 0, ridx = 0;
        if (off < 0) return 0;
        if (!adts_at(off, &rate, &ch, &flen, 0, &prof, &ridx)) return 0;
        A.first = A.pos = off;
        A.rate = rate;
        A.rate_idx = ridx;
        A.channels = ch;
        A.obj_type = prof;                            /* ADTS profile + 1     */
        /* frame count by average frame size - ADTS carries no duration */
        A.total_frames = flen ? (um_size() - off) / flen : 0;
    }

    /* ---- precise refusals (the container recognised the file fine) ---- */
    if (A.obj_type == 1) {
        um_set_error("AAC Main profile - not decoded (LC only)");
        return 0;
    }
    if (A.obj_type == 3) {
        um_set_error("AAC SSR profile - not decoded (LC only)");
        return 0;
    }
    if (A.obj_type == 4) {
        um_set_error("AAC LTP profile - not decoded (LC only)");
        return 0;
    }
    if (A.obj_type != 2) {
        um_set_error("AAC object type not LC - not decoded (LC only)");
        return 0;
    }
    if (A.frame960) {
        um_set_error("AAC 960-sample framing - not decoded");
        return 0;
    }
    if (A.channels > 2) {
        um_set_error("AAC multichannel - not decoded (mono/stereo only)");
        return 0;
    }
    if (A.rate_idx < 0 || A.rate_idx > 12 || A.rate <= 0) {
        um_set_error("AAC nonstandard sample rate - not decoded");
        return 0;
    }
    if (A.channels < 1) A.channels = 2;               /* cfg 0: PCE-defined   */

    S = um_alloc(sizeof *S + (unsigned long)nsamp_cap * 8u);
    if (!S) {
        um_set_error("AAC: out of memory");
        return 0;
    }
    memset(S, 0, sizeof *S);
    S->soff = (unsigned int *)(void *)((char *)S + sizeof *S);
    S->slen = S->soff + nsamp_cap;
    build_tables();

    if (A.is_mp4) {
        A.nsamp = nsamp_cap;
        mp4_build_table();
        if (!A.nsamp) {
            um_set_error("AAC (M4A) - unreadable sample table");
            aac_close();
            return 0;
        }
        A.total_frames = A.nsamp;
        A.next_samp = 0;
    }

    info->rate        = A.rate;
    info->channels    = A.channels;
    info->duration_ms = A.rate
        ? (long)(((int64_t)A.total_frames * 1024 * 1000) / A.rate) : -1;
    info->bitrate     = (A.total_frames > 0 && A.rate)
        ? (int)(((int64_t)um_size() * 8 * A.rate) /
                ((int64_t)A.total_frames * 1024 * 1000))
        : 0;
    return 1;
}

static int aac_decode(short *out, int max_frames)
{
    int done = 0;
    if (!S) return 0;
    while (done < max_frames) {
        int avail, take, i;
        if (S->pcm_taken >= S->pcm_have) {
            int r = decode_frame();
            if (r == 0) break;                     /* end of stream          */
            if (r < 0) continue;                   /* bad frame: skip it     */
        }
        avail = S->pcm_have - S->pcm_taken;
        take = max_frames - done;
        if (take > avail) take = avail;
        for (i = 0; i < take; i++) {
            out[(done + i) * A.channels] = S->pcm[0][S->pcm_taken + i];
            if (A.channels == 2)
                out[(done + i) * 2 + 1] = S->pcm[1][S->pcm_taken + i];
        }
        S->pcm_taken += take;
        done += take;
    }
    S->frames_out += done;
    return done;
}

static int aac_seek(long ms)
{
    long target;
    if (!S || !A.rate) return 0;
    if (ms < 0) ms = 0;
    target = (long)((((int64_t)ms * A.rate) / 1000) / 1024);
    memset(S->overlap, 0, sizeof S->overlap);
    S->pcm_have = S->pcm_taken = 0;
    if (A.is_mp4) {
        if (target > A.nsamp) target = A.nsamp;
        A.next_samp = target;
    } else {
        /* walk the self-framing headers - exact, and fast enough: it reads
           7 bytes per frame, no payloads */
        long off = A.first, f = 0;
        while (f < target) {
            int flen = 0;
            if (!adts_at(off, 0, 0, &flen, 0, 0, 0)) {
                off = adts_find(off);
                if (off < 0) break;
                continue;
            }
            off += flen;
            f++;
            if (off + 7 > um_size()) break;
        }
        A.pos = off;
        target = f;
    }
    S->frames_out = target * 1024;
    return 1;
}

static long aac_pos_ms(void)
{
    if (!S || !A.rate) return 0;
    return (long)(((int64_t)S->frames_out * 1000) / A.rate);
}

const um_adecoder um_adec_aac = {
    "AAC", aac_probe, aac_open, aac_decode, aac_seek, aac_close, aac_pos_ms
};
