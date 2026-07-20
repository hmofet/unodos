/* ===========================================================================
 * unomedia - AAC: the container layer.
 *
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
 * This file owns all of that: sync, box walking, the sample table, the
 * AudioSpecificConfig, duration, and handing whole raw AAC frames to the
 * decode core.
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>
#include <stdint.h>

#define AAC_MAX_FRAME  8192          /* an AAC frame is bounded well under this */
#define AAC_MAX_SAMPLES 4096         /* stsz entries we keep for a seek table   */

/* the 13 sampling frequencies an AudioSpecificConfig index can name */
static const int kAacRate[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025,  8000,  7350,     0,     0, 0
};

typedef struct {
    int  is_mp4;                     /* 0 = ADTS byte stream, 1 = MP4 boxes    */
    int  rate, channels, obj_type;   /* from ADTS header or AudioSpecificConfig */
    long pos;                        /* ADTS: byte offset of the next frame    */
    long first;                      /* ADTS: offset of the first frame        */
    /* MP4 sample table */
    int  nsamp;
    long samp_off[AAC_MAX_SAMPLES];
    int  samp_len[AAC_MAX_SAMPLES];
    int  next_samp;
    long total_frames;               /* audio frames (1024 samples each)       */
} aac_ctx;

static aac_ctx A;

static uint32_t be32(const unsigned char *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* ---- ADTS ------------------------------------------------------------------ */
/* Parse an ADTS header at `off`. Fills rate/channels/frame length. The sync
 * word is only 12 bits, which random data hits often, so a header is only
 * trusted when the frame it describes is followed by another valid one. */
static int adts_at(long off, int *rate, int *chans, int *flen, int *hdr)
{
    unsigned char h[9];
    int idx, prof, ch, len, prot;
    if (um_read(off, h, 7) != 7) return 0;
    if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) return 0;
    prot = !(h[1] & 1);                       /* protection_absent inverted    */
    prof = ((h[2] >> 6) & 3) + 1;             /* 2 = LC                        */
    idx  = (h[2] >> 2) & 0xF;
    ch   = ((h[2] & 1) << 2) | ((h[3] >> 6) & 3);
    len  = (int)(((uint32_t)(h[3] & 3) << 11) | ((uint32_t)h[4] << 3) | (h[5] >> 5));
    if (!kAacRate[idx] || len < 7) return 0;
    if (rate)  *rate  = kAacRate[idx];
    if (chans) *chans = ch;
    if (flen)  *flen  = len;
    if (hdr)   *hdr   = 7 + (prot ? 2 : 0);   /* + CRC when present            */
    (void)prof;
    return 1;
}

static long adts_find(long off)
{
    long limit = um_size();
    int guard = 0;
    while (off + 9 <= limit && guard++ < (1 << 20)) {
        int len;
        if (adts_at(off, 0, 0, &len, 0)) {
            if (off + len + 7 > limit) return off;         /* last frame       */
            if (adts_at(off + len, 0, 0, 0, 0)) return off; /* confirmed        */
        }
        off++;
    }
    return -1;
}

/* ---- AudioSpecificConfig ---------------------------------------------------- */
/* 5 bits object type, 4 bits sampling frequency index (or an explicit 24-bit
 * rate when the index is 15), 4 bits channel configuration. */
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
    A.obj_type = obj;
    A.channels = ch;
#undef GB
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
    unsigned char b[16];
    int i, samp = 0;
    long chunk, sc_i = 0, next_first = 1, per = 1;
    if (!S_stsz || !S_stco) return;
    A.nsamp = 0;
    for (chunk = 0; chunk < S_stco_n && samp < AAC_MAX_SAMPLES; chunk++) {
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
            next_first = first;
            sc_i++;
        }
        (void)next_first;
        for (i = 0; i < per && samp < AAC_MAX_SAMPLES; i++, samp++) {
            unsigned char z[4];
            long len;
            if (um_read(S_stsz + 12 + (long)samp * 4, z, 4) != 4) { samp = AAC_MAX_SAMPLES; break; }
            len = (long)be32(z);
            if (len <= 0 || len > AAC_MAX_FRAME) continue;
            A.samp_off[A.nsamp] = coff;
            A.samp_len[A.nsamp] = (int)len;
            coff += len;
            A.nsamp++;
        }
        if (sc_i >= S_stsc_n && S_stsc == 0) break;
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

static int aac_open(um_audio_info *info)
{
    memset(&A, 0, sizeof A);
    S_stsz = S_stco = S_stsc = S_stts = 0;
    S_stsz_n = S_stco_n = S_stsc_n = 0;

    {   /* MP4 if it opens with an ftyp box */
        unsigned char h[12];
        if (um_read(0, h, 12) == 12 && !memcmp(h + 4, "ftyp", 4)) {
            A.is_mp4 = 1;
            mp4_walk(0, um_size(), 0);
            if (!A.rate) return 0;                    /* no AudioSpecificConfig */
            mp4_build_table();
            if (!A.nsamp) return 0;
            A.total_frames = A.nsamp;
        }
    }
    if (!A.is_mp4) {
        long off = adts_find(0);
        int rate = 0, ch = 0, flen = 0;
        if (off < 0) return 0;
        if (!adts_at(off, &rate, &ch, &flen, 0)) return 0;
        A.first = A.pos = off;
        A.rate = rate;
        A.channels = ch;
        A.obj_type = 2;
        /* frame count by average frame size - ADTS carries no duration */
        A.total_frames = flen ? (um_size() - off) / flen : 0;
    }
    if (A.channels < 1) A.channels = 2;
    if (A.channels > 2) A.channels = 2;         /* downmix wider layouts       */

    info->rate        = A.rate;
    info->channels    = A.channels;
    info->duration_ms = A.rate
        ? (long)(((int64_t)A.total_frames * 1024 * 1000) / A.rate) : -1;
    info->bitrate     = 0;

    /* ---- why there is no decode core ---------------------------------------
     * Deliberate, not unfinished.
     *
     * AAC-LC needs ISO constant tables no decoder can invent - the 11 spectral
     * Huffman codebooks, the scalefactor codebook, and the scalefactor-band
     * offsets. MP3 had a way out: PDMP3 is public domain, so mp3_tables.h
     * carries no obligations. AAC has no equivalent. Every implementation that
     * contains those tables is GPL (FAAD2, Rockbox), LGPL (FFmpeg), RPSL
     * copyleft (Helix), non-OSI with an explicit refusal of any patent grant
     * (Fraunhofer FDK), or Apache-2.0 (OpenCORE, libxaac).
     *
     * Apache-2.0 would have been legally clean, and patents are not the
     * obstacle - AAC-LC claims lapsed around 2017-2018, which is why Fedora,
     * Debian and Wikimedia all ship it. The obstacle is that Apache-2.0 is not
     * a public-domain dedication: it would have put a third-party copyright
     * and a licence file into a tree that is otherwise uniformly this
     * project's own. That trade was declined on purpose.
     *
     * So this file identifies AAC and says so plainly. Everything above -
     * ADTS framing, the MP4 box walk, the stsz/stsc/stco sample table, the
     * AudioSpecificConfig - is written here and stays useful if the decision
     * is ever revisited.
     */
    um_set_error(A.is_mp4
        ? "AAC (M4A) recognised - no decoder in this build"
        : "AAC (ADTS) recognised - no decoder in this build");
    return 0;
}

static int aac_decode(short *out, int max_frames)
{ (void)out; (void)max_frames; return 0; }
static int aac_seek(long ms) { (void)ms; return 0; }
static long aac_pos_ms(void) { return 0; }
static void aac_close(void) { }

const um_adecoder um_adec_aac = {
    "AAC", aac_probe, aac_open, aac_decode, aac_seek, aac_close, aac_pos_ms
};
