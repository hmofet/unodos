/* ===========================================================================
 * unomedia - MPEG-1 Layer III (MP3) decoder.
 *
 * Written from scratch against ISO/IEC 11172-3. The only thing not written
 * here is the specification's constant data - Huffman codebooks, the 512-tap
 * synthesis window and the scalefactor-band boundaries - which no decoder can
 * invent; those live in the generated mp3_tables.h (see tools/mkmp3tables.py
 * for their provenance). Everything below is ours: the frame parser, the bit
 * reservoir, scalefactor and Huffman decoding, requantisation, stereo, the
 * IMDCT and the polyphase filterbank.
 *
 * ---- the shape of a Layer III frame ----------------------------------------
 * A frame is 1152 PCM samples as two 576-sample granules, and its bits are
 * NOT self-contained: `main_data_begin` says the granule's data starts some
 * number of bytes BEFORE this frame's header, in bytes already consumed from
 * previous frames. That is the bit reservoir, and it is why a decoder cannot
 * simply seek to a frame and decode it - it has to have been running. Here
 * the reservoir is a 512-byte ring that every frame appends to and granules
 * read backwards into.
 *
 * ---- pipeline --------------------------------------------------------------
 *   header -> side info -> scalefactors -> Huffman -> requantise
 *      -> stereo (M/S, intensity) -> reorder (short blocks)
 *      -> alias reduction (long blocks) -> IMDCT + overlap-add
 *      -> frequency inversion -> polyphase synthesis -> PCM
 *
 * ---- scope -----------------------------------------------------------------
 * MPEG-1 Layer III only: 32 / 44.1 / 48 kHz, mono, stereo, joint stereo and
 * dual channel. That is essentially every MP3 in circulation. MPEG-2 and 2.5
 * (the 8-24 kHz half- and quarter-rate extensions) use different scalefactor
 * tables and an intensity-stereo variant; they are detected and refused
 * cleanly rather than decoded into noise.
 *
 * ---- no libm ---------------------------------------------------------------
 * This is a freestanding kernel: there is no pow() or exp(). Everything that
 * looks transcendental is either a table built once at init (the x^(4/3)
 * requantisation curve, via a Newton cube root) or a power of two reached by
 * repeated multiplication from sqrtf-derived constants.
 * ======================================================================== */
#include "unomedia.h"
#include "mp3_tables.h"
#include <string.h>
#include <stdint.h>

float sinf(float x);
float cosf(float x);
float sqrtf(float x);
float tanf(float x);

#define GRANULE   576
#define NSUBBAND  32
#define NSAMP     18
/* The reservoir has to hold the 511 bytes a granule may reach BACK for plus a
 * whole frame's own main data appended on top. At 320 kbps / 32 kHz a frame is
 * 1440 bytes, so ~1915 is the true worst case; 4096 leaves the overflow path
 * unreachable in practice. Sizing this at 1024 - enough for 128 kbps and no
 * more - silently truncated the history at higher bitrates, which decodes as
 * noise the moment content gets dense enough to use the reservoir. */
#define RESERVOIR 4096
#define READ_CHUNK 4096

/* ---- stream + frame state --------------------------------------------------- */
static long  m_pos;                     /* file offset of the next frame       */
static long  m_first;                   /* offset of the first audio frame     */
static int   m_ok;
static int   m_rate, m_nch;
static int   m_srate_idx;
static long  m_total_ms;
static long  m_frames_out;              /* PCM frames emitted so far           */

static unsigned char m_buf[READ_CHUNK]; /* raw file window                     */
static long  m_buf_off;
static int   m_buf_len;

/* the bit reservoir: main data bytes, oldest first */
static unsigned char m_res[RESERVOIR];
static int   m_res_len;
static int   m_bitpos;                  /* read cursor, in bits, into m_res    */

/* ---- per-granule side info --------------------------------------------------- */
typedef struct {
    int part2_3_length, big_values, global_gain, scalefac_compress;
    int window_switching, block_type, mixed_block, preflag;
    int scalefac_scale, count1table_select;
    int table_select[3], subblock_gain[3];
    int region0_count, region1_count;
} gr_info;

static int      m_main_data_begin;
static int      m_scfsi[2][4];
static gr_info  m_gr[2][2];             /* [granule][channel]                  */
static int      m_scalefac_l[2][23];
static int      m_scalefac_s[2][14][3];

/* ---- working buffers ---------------------------------------------------------- */
static int   m_is[2][GRANULE];          /* Huffman-decoded integers            */
static float m_xr[2][GRANULE];          /* requantised / processed spectrum    */
static float m_overlap[2][NSUBBAND][NSAMP];
static float m_synth_v[2][1024];
static int   m_synth_off[2];
static short m_pcm[2][1152];            /* one frame of output per channel     */
static int   m_pcm_have, m_pcm_taken;

/* ---- derived constant tables (built once) -------------------------------------- */
static float t_pow43[8207];             /* |v|^(4/3)                           */
static float t_gain[256];               /* 2^((gg-210)/4)                      */
static float t_sfscale[2][64];          /* 2^(-(0.5|1) * n)                    */
static float t_win[4][36];              /* IMDCT windows by block type         */
static float t_imdct[36][18];           /* cos((2i+1+18)(2k+1)pi/72)           */
static float t_imdct_s[12][6];          /* the short-block 6-point core        */
static float t_synth[64][32];           /* polyphase matrixing cosines         */
static float t_cs[8], t_ca[8];          /* alias-reduction butterflies         */
static float t_is[7][2];                /* intensity-stereo position gains     */
static int   t_built;

static const int kBitrate[15] = { 0, 32, 40, 48, 56, 64, 80, 96,
                                  112, 128, 160, 192, 224, 256, 320 };
static const int kSampleRate[3] = { 44100, 48000, 32000 };
static const int kSlen1[16] = { 0,0,0,0,3,1,1,1,2,2,2,3,3,3,4,4 };
static const int kSlen2[16] = { 0,1,2,3,0,1,2,3,1,2,3,1,2,3,2,3 };
static const int kPretab[22] = { 0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2,0 };

/* cube root by Newton iteration - no libm in a freestanding kernel */
static float cbrtf_(float x)
{
    float r;
    int i;
    if (x <= 0.0f) return 0.0f;
    r = x > 1.0f ? x / 3.0f + 1.0f : x;     /* rough start, then converge      */
    for (i = 0; i < 24; i++)
        r = (2.0f * r + x / (r * r)) / 3.0f;
    return r;
}

static void build_tables(void)
{
    int i, j, k;
    if (t_built) return;
    t_built = 1;

    for (i = 0; i < 8207; i++)
        t_pow43[i] = (float)i * cbrtf_((float)i);      /* v^(4/3) = v * v^(1/3) */

    {   /* 2^((gg - 210)/4), reached by repeated multiplication by 2^(1/4) */
        float q = sqrtf(sqrtf(2.0f)), v;
        int e;
        for (i = 0; i < 256; i++) {
            e = i - 210;
            v = 1.0f;
            while (e >= 4)  { v *= 2.0f;  e -= 4; }
            while (e <= -4) { v *= 0.5f;  e += 4; }
            while (e > 0)   { v *= q;     e--; }
            while (e < 0)   { v /= q;     e++; }
            t_gain[i] = v;
        }
    }
    {   /* 2^(-scale*n) for scalefac_scale 0 (0.5) and 1 (1.0) */
        float h = sqrtf(0.5f);
        for (i = 0; i < 64; i++) {
            float a = 1.0f, b = 1.0f;
            for (j = 0; j < i; j++) { a *= h; b *= 0.5f; }
            t_sfscale[0][i] = a;
            t_sfscale[1][i] = b;
        }
    }
    {   /* the four IMDCT windows (ISO 2.4.3.4.10.3) */
        for (i = 0; i < 36; i++) t_win[0][i] = sinf(3.14159265f / 36.0f * (i + 0.5f));
        for (i = 0; i < 18; i++) t_win[1][i] = sinf(3.14159265f / 36.0f * (i + 0.5f));
        for (i = 18; i < 24; i++) t_win[1][i] = 1.0f;
        for (i = 24; i < 30; i++) t_win[1][i] = sinf(3.14159265f / 12.0f * (i + 0.5f - 18.0f));
        for (i = 30; i < 36; i++) t_win[1][i] = 0.0f;
        for (i = 0; i < 12; i++) t_win[2][i] = sinf(3.14159265f / 12.0f * (i + 0.5f));
        for (i = 12; i < 36; i++) t_win[2][i] = 0.0f;
        for (i = 0; i < 6; i++)  t_win[3][i] = 0.0f;
        for (i = 6; i < 12; i++) t_win[3][i] = sinf(3.14159265f / 12.0f * (i + 0.5f - 6.0f));
        for (i = 12; i < 18; i++) t_win[3][i] = 1.0f;
        for (i = 18; i < 36; i++) t_win[3][i] = sinf(3.14159265f / 36.0f * (i + 0.5f));
    }
    for (i = 0; i < 36; i++)
        for (k = 0; k < 18; k++)
            t_imdct[i][k] = cosf(3.14159265f / 72.0f *
                                 (2 * i + 1 + 18) * (2 * k + 1));
    for (i = 0; i < 12; i++)
        for (k = 0; k < 6; k++)
            t_imdct_s[i][k] = cosf(3.14159265f / 24.0f *
                                   (2 * i + 1 + 6) * (2 * k + 1));
    for (i = 0; i < 64; i++)
        for (k = 0; k < 32; k++)
            t_synth[i][k] = cosf(3.14159265f / 64.0f * (16 + i) * (2 * k + 1));
    {   /* alias reduction (ISO Table B.9) */
        static const float ci[8] = { -0.6f, -0.535f, -0.33f, -0.185f,
                                     -0.095f, -0.041f, -0.0142f, -0.0037f };
        for (i = 0; i < 8; i++) {
            float d = sqrtf(1.0f + ci[i] * ci[i]);
            t_cs[i] = 1.0f / d;
            t_ca[i] = ci[i] / d;
        }
    }
    for (i = 0; i < 7; i++) {           /* intensity stereo position gains */
        float t = tanf((float)i * 3.14159265f / 12.0f);
        t_is[i][0] = t / (1.0f + t);
        t_is[i][1] = 1.0f / (1.0f + t);
    }
}

/* ---- raw file access ------------------------------------------------------------ */
static int raw_byte(long off)
{
    if (off < m_buf_off || off >= m_buf_off + m_buf_len) {
        m_buf_off = off;
        m_buf_len = (int)um_read(off, m_buf, READ_CHUNK);
        if (m_buf_len <= 0) { m_buf_len = 0; return -1; }
    }
    return m_buf[off - m_buf_off];
}

static int raw_read(long off, unsigned char *dst, int n)
{
    int i, v;
    for (i = 0; i < n; i++) {
        v = raw_byte(off + i);
        if (v < 0) return i;
        dst[i] = (unsigned char)v;
    }
    return n;
}

/* ---- the main-data bit reader ---------------------------------------------------- */
static unsigned getbits(int n)
{
    unsigned v = 0;
    while (n-- > 0) {
        int byte = m_bitpos >> 3, bit = 7 - (m_bitpos & 7);
        unsigned b = (byte < m_res_len) ? ((m_res[byte] >> bit) & 1u) : 0u;
        v = (v << 1) | b;
        m_bitpos++;
    }
    return v;
}
static unsigned getbit(void) { return getbits(1); }

/* ---- frame header ----------------------------------------------------------------- */
typedef struct {
    int ver;            /* 3 = MPEG1 (the only one we decode)                 */
    int layer;          /* 1 = Layer III                                      */
    int crc, br_idx, sr_idx, pad, mode, mode_ext;
    int size;           /* whole frame in bytes, header included              */
    int side_len;
} mp3_hdr;

static int parse_header(const unsigned char *h, mp3_hdr *o)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    o->ver      = (h[1] >> 3) & 3;
    o->layer    = (h[1] >> 1) & 3;
    o->crc      = !(h[1] & 1);
    o->br_idx   = (h[2] >> 4) & 0xF;
    o->sr_idx   = (h[2] >> 2) & 3;
    o->pad      = (h[2] >> 1) & 1;
    o->mode     = (h[3] >> 6) & 3;
    o->mode_ext = (h[3] >> 4) & 3;
    if (o->layer != 1) return 0;                 /* 01 = Layer III            */
    if (o->br_idx == 0 || o->br_idx == 15) return 0;
    if (o->sr_idx == 3) return 0;
    if (o->ver != 3) return 0;                   /* MPEG-1 only (see header)  */
    o->size = 144000 * kBitrate[o->br_idx] / kSampleRate[o->sr_idx] + o->pad;
    o->side_len = (o->mode == 3) ? 17 : 32;
    return o->size > 4;
}

/* scan forward for a header that is followed by another one where it predicts */
static long find_frame(long off, mp3_hdr *o)
{
    unsigned char h[4], h2[4];
    long limit = um_size();
    int guard = 0;
    while (off + 4 <= limit && guard++ < 1 << 20) {
        if (raw_read(off, h, 4) != 4) break;
        if (parse_header(h, o)) {
            mp3_hdr n;
            if (off + o->size + 4 > limit) return off;      /* last frame     */
            if (raw_read(off + o->size, h2, 4) == 4 && parse_header(h2, &n))
                return off;
        }
        off++;
    }
    return -1;
}

/* ---- ID3 / VBR headers -------------------------------------------------------------- */
static long skip_id3(void)
{
    unsigned char h[10];
    if (raw_read(0, h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
        long sz = ((long)(h[6] & 0x7F) << 21) | ((long)(h[7] & 0x7F) << 14) |
                  ((long)(h[8] & 0x7F) << 7)  |  (long)(h[9] & 0x7F);
        return 10 + sz + ((h[5] & 0x10) ? 10 : 0);          /* + footer       */
    }
    return 0;
}

/* Xing/Info/VBRI give the true frame count, which is the only way to state a
 * VBR file's length without decoding all of it. */
static long xing_frames(long frame_off, const mp3_hdr *h)
{
    unsigned char b[200];
    int off = 4 + (h->crc ? 2 : 0) + h->side_len;
    int n = raw_read(frame_off + off, b, 16);
    if (n >= 8 && (!memcmp(b, "Xing", 4) || !memcmp(b, "Info", 4))) {
        unsigned flags = ((unsigned)b[4] << 24) | ((unsigned)b[5] << 16) |
                         ((unsigned)b[6] << 8) | b[7];
        if (flags & 1)
            return ((long)b[8] << 24) | ((long)b[9] << 16) |
                   ((long)b[10] << 8) | b[11];
    }
    n = raw_read(frame_off + 4 + 32, b, 32);
    if (n >= 32 && !memcmp(b, "VBRI", 4))
        return ((long)b[14] << 24) | ((long)b[15] << 16) |
               ((long)b[16] << 8) | b[17];
    return 0;
}

/* ---- side info ----------------------------------------------------------------------- */
/* side info is its own little bitstream, separate from the reservoir */
static unsigned side_bits(const unsigned char *s, int *p, int n)
{
    unsigned v = 0;
    while (n-- > 0) {
        v = (v << 1) | ((s[*p >> 3] >> (7 - (*p & 7))) & 1u);
        (*p)++;
    }
    return v;
}

static int read_side_info(const unsigned char *s, int nch)
{
    int gr, ch, i, p = 0;
#define SB(n) side_bits(s, &p, (n))
    m_main_data_begin = SB(9);
    p += (nch == 1) ? 5 : 3;                       /* private_bits            */
    for (ch = 0; ch < nch; ch++)
        for (i = 0; i < 4; i++) m_scfsi[ch][i] = SB(1);
    for (gr = 0; gr < 2; gr++)
        for (ch = 0; ch < nch; ch++) {
            gr_info *g = &m_gr[gr][ch];
            g->part2_3_length    = SB(12);
            g->big_values        = SB(9);
            g->global_gain       = SB(8);
            g->scalefac_compress = SB(4);
            g->window_switching  = SB(1);
            if (g->window_switching) {
                g->block_type   = SB(2);
                g->mixed_block  = SB(1);
                for (i = 0; i < 2; i++) g->table_select[i] = SB(5);
                g->table_select[2] = 0;
                for (i = 0; i < 3; i++) g->subblock_gain[i] = SB(3);
                /* the standard fixes the region split when switching */
                g->region0_count = (g->block_type == 2 && !g->mixed_block) ? 8 : 7;
                g->region1_count = 20 - g->region0_count;
            } else {
                for (i = 0; i < 3; i++) g->table_select[i] = SB(5);
                g->region0_count = SB(4);
                g->region1_count = SB(3);
                g->block_type = 0; g->mixed_block = 0;
                for (i = 0; i < 3; i++) g->subblock_gain[i] = 0;
            }
            g->preflag           = SB(1);
            g->scalefac_scale    = SB(1);
            g->count1table_select = SB(1);
            if (g->big_values > 288) return 0;      /* malformed             */
        }
#undef SB
    return 1;
}

/* ---- scalefactors ---------------------------------------------------------------------- */
static int read_scalefactors(int gr, int ch)
{
    gr_info *g = &m_gr[gr][ch];
    int slen1 = kSlen1[g->scalefac_compress];
    int slen2 = kSlen2[g->scalefac_compress];
    int start = m_bitpos, sfb, w;

    if (g->window_switching && g->block_type == 2) {
        if (g->mixed_block) {
            for (sfb = 0; sfb < 8; sfb++)  m_scalefac_l[ch][sfb] = getbits(slen1);
            for (sfb = 3; sfb < 6; sfb++)
                for (w = 0; w < 3; w++) m_scalefac_s[ch][sfb][w] = getbits(slen1);
            for (sfb = 6; sfb < 12; sfb++)
                for (w = 0; w < 3; w++) m_scalefac_s[ch][sfb][w] = getbits(slen2);
        } else {
            for (sfb = 0; sfb < 6; sfb++)
                for (w = 0; w < 3; w++) m_scalefac_s[ch][sfb][w] = getbits(slen1);
            for (sfb = 6; sfb < 12; sfb++)
                for (w = 0; w < 3; w++) m_scalefac_s[ch][sfb][w] = getbits(slen2);
        }
        for (w = 0; w < 3; w++) m_scalefac_s[ch][12][w] = 0;
    } else {
        /* scfsi lets granule 1 inherit granule 0's bands, in four groups */
        static const int lo[4] = { 0, 6, 11, 16 }, hi[4] = { 6, 11, 16, 21 };
        int grp;
        for (grp = 0; grp < 4; grp++) {
            if (gr == 1 && m_scfsi[ch][grp]) continue;   /* keep granule 0's  */
            for (sfb = lo[grp]; sfb < hi[grp]; sfb++)
                m_scalefac_l[ch][sfb] = getbits(grp < 2 ? slen1 : slen2);
        }
        m_scalefac_l[ch][21] = m_scalefac_l[ch][22] = 0;
    }
    return m_bitpos - start;
}

/* ---- Huffman ----------------------------------------------------------------------------- */
#ifdef MP3_STATS
int g_huff_fail, g_huff_ok;
#endif
static int huff_sym(const mp3_htab *t)
{
    unsigned code = 0;
    int len;
    for (len = 1; len <= MP3_HUFF_MAXLEN; len++) {
        code = (code << 1) | getbit();
        if (t->cnt[len]) {
            /* binary search this length's run: Layer III's codes are not
               canonical, so we cannot index by (code - first) */
            int lo = t->idx[len], hi = lo + t->cnt[len] - 1;
            while (lo <= hi) {
                int mid = (lo + hi) >> 1;
                if (t->codes[mid] == code) {
#ifdef MP3_STATS
                g_huff_ok++;
#endif
                return t->syms[mid]; }
                if (t->codes[mid] < code) lo = mid + 1; else hi = mid - 1;
            }
        }
    }
#ifdef MP3_STATS
    g_huff_fail++;
#endif
    return -1;
}

static void huff_pair(const mp3_htab *t, int *x, int *y)
{
    int s = huff_sym(t);
    if (s < 0) { *x = *y = 0; return; }
    *x = (s >> 4) & 0xF;
    *y = s & 0xF;
    if (t->linbits && *x == 15) *x += (int)getbits(t->linbits);
    if (*x && getbit()) *x = -*x;
    if (t->linbits && *y == 15) *y += (int)getbits(t->linbits);
    if (*y && getbit()) *y = -*y;
}

static int decode_huffman(int gr, int ch, int part2_bits)
{
    gr_info *g = &m_gr[gr][ch];
    int end = m_bitpos + g->part2_3_length - part2_bits;
    int n = 0, i, r0, r1, reg[3];
    const unsigned short *sfb = kSfbLong[m_srate_idx];

    memset(m_is[ch], 0, sizeof m_is[ch]);

    /* Region boundaries, in spectral lines. A window-switched granule has only
     * TWO regions (table_select[2] is not even transmitted), and only a pure
     * short block gets the fixed 36-line region0 - start and stop windows
     * (block_type 1 and 3), which bracket every transient, still take the
     * normal scalefactor-band boundary. Treating all window-switched granules
     * as the 36-line case decodes those with the wrong Huffman table. */
    if (g->window_switching) {
        reg[0] = (g->block_type == 2 && !g->mixed_block) ? 36 : sfb[8];
        reg[1] = 576;
    } else {
        r0 = g->region0_count + 1;
        r1 = r0 + g->region1_count + 1;
        if (r0 > 22) r0 = 22;
        if (r1 > 22) r1 = 22;
        reg[0] = sfb[r0];
        reg[1] = sfb[r1];
    }
    if (reg[0] > 576) reg[0] = 576;
    if (reg[1] > 576) reg[1] = 576;
    reg[2] = 576;

    {   /* big_values: pairs, split across three regions with their own tables */
        int bv = g->big_values * 2, region = 0;
        if (bv > 576) bv = 576;
        for (i = 0; i < bv && m_bitpos < end; i += 2) {
            int x, y;
            while (region < 2 && i >= reg[region]) region++;
            {
                const mp3_htab *t = &kHuff[g->table_select[region]];
                if (!t->cnt) { x = y = 0; }
                else huff_pair(t, &x, &y);
            }
            m_is[ch][i] = x;
            m_is[ch][i + 1] = y;
            n = i + 2;
        }
    }
    {   /* count1: quadruples until the granule's bits run out */
        const mp3_htab *t = &kHuff[g->count1table_select ? 33 : 32];
        while (m_bitpos < end && n <= 572) {
            int s = huff_sym(t), v[4], k;
            if (s < 0) break;
            v[0] = (s >> 3) & 1; v[1] = (s >> 2) & 1;
            v[2] = (s >> 1) & 1; v[3] = s & 1;
            for (k = 0; k < 4; k++)
                if (v[k] && getbit()) v[k] = -v[k];
            for (k = 0; k < 4; k++) m_is[ch][n + k] = v[k];
            n += 4;
        }
    }
    /* the granule owns a fixed bit count whatever we consumed */
    m_bitpos = end;
    return n;
}

/* ---- requantisation ------------------------------------------------------------------------ */
static float pow43(int v)
{
    int a = v < 0 ? -v : v;
    float r;
    if (a > 8206) a = 8206;
    r = t_pow43[a];
    return v < 0 ? -r : r;
}

static void requantise(int gr, int ch)
{
    gr_info *g = &m_gr[gr][ch];
    const unsigned short *sl = kSfbLong[m_srate_idx];
    const unsigned short *ss = kSfbShort[m_srate_idx];
    float gain = t_gain[g->global_gain];
    const float *sfs = t_sfscale[g->scalefac_scale];
    int i, sfb, w;

    if (g->window_switching && g->block_type == 2) {
        int startsfb = g->mixed_block ? 8 : 0;
        if (g->mixed_block) {
            for (sfb = 0; sfb < 8; sfb++) {
                int n = m_scalefac_l[ch][sfb];
                float s = gain * sfs[n];
                for (i = sl[sfb]; i < sl[sfb + 1] && i < 576; i++)
                    m_xr[ch][i] = pow43(m_is[ch][i]) * s;
            }
        }
        for (sfb = (g->mixed_block ? 3 : 0); sfb < 13; sfb++) {
            int width = ss[sfb + 1] - ss[sfb];
            for (w = 0; w < 3; w++) {
                int n = m_scalefac_s[ch][sfb][w];
                float s = gain * sfs[n];
                int sg = g->subblock_gain[w];
                while (sg--) s *= 0.25f;              /* 2^(-8*sg/4) */
                for (i = 0; i < width; i++) {
                    int k = ss[sfb] * 3 + w * width + i;
                    if (k >= 576) break;
                    if (g->mixed_block && k < sl[startsfb]) continue;
                    m_xr[ch][k] = pow43(m_is[ch][k]) * s;
                }
            }
        }
    } else {
        for (sfb = 0; sfb < 22; sfb++) {
            int n = m_scalefac_l[ch][sfb] + (g->preflag ? kPretab[sfb] : 0);
            float s = gain * sfs[n];
            for (i = sl[sfb]; i < sl[sfb + 1] && i < 576; i++)
                m_xr[ch][i] = pow43(m_is[ch][i]) * s;
        }
        for (i = sl[22]; i < 576; i++) m_xr[ch][i] = 0.0f;
    }
}

/* ---- stereo ------------------------------------------------------------------------------- */
static void stereo(int gr, int mode, int mode_ext)
{
    gr_info *g = &m_gr[gr][0];
    int i;
    if (mode != 1) return;                       /* only joint stereo         */

    if (mode_ext & 1) {                          /* intensity stereo          */
        const unsigned short *sl = kSfbLong[m_srate_idx];
        /* the intensity region starts after the last non-zero line of ch 1 */
        int last = 575;
        while (last >= 0 && m_is[1][last] == 0) last--;
        {
            int sfb = 0;
            while (sfb < 21 && sl[sfb + 1] <= last + 1) sfb++;
            for (; sfb < 21; sfb++) {
                int pos = m_scalefac_l[1][sfb];
                if (pos >= 7) continue;          /* 7 = "illegal", leave as is */
                for (i = sl[sfb]; i < sl[sfb + 1] && i < 576; i++) {
                    float v = m_xr[0][i];
                    m_xr[0][i] = v * t_is[pos][1];
                    m_xr[1][i] = v * t_is[pos][0];
                }
            }
        }
    }
    if (mode_ext & 2) {                          /* middle/side               */
        const float inv = 0.70710678f;
        int upto = 576;
        if (mode_ext & 1) {                      /* MS only below intensity   */
            int last = 575;
            while (last >= 0 && m_is[1][last] == 0) last--;
            upto = last + 1;
        }
        for (i = 0; i < upto; i++) {
            float m = m_xr[0][i], s = m_xr[1][i];
            m_xr[0][i] = (m + s) * inv;
            m_xr[1][i] = (m - s) * inv;
        }
    }
    (void)g;
}

/* ---- reorder (short blocks are stored window-major) ----------------------------------------- */
static void reorder(int gr, int ch)
{
    gr_info *g = &m_gr[gr][ch];
    const unsigned short *ss = kSfbShort[m_srate_idx];
    static float tmp[576];
    int sfb, w, i, k = 0, start;
    if (!(g->window_switching && g->block_type == 2)) return;
    start = g->mixed_block ? 3 : 0;
    if (g->mixed_block) k = ss[3] * 3;
    for (sfb = start; sfb < 13; sfb++) {
        int width = ss[sfb + 1] - ss[sfb];
        for (i = 0; i < width; i++)
            for (w = 0; w < 3; w++) {
                int src = ss[sfb] * 3 + w * width + i;
                if (k < 576 && src < 576) tmp[k++] = m_xr[ch][src];
            }
    }
    for (i = (g->mixed_block ? ss[3] * 3 : 0); i < k; i++) m_xr[ch][i] = tmp[i];
}

/* ---- alias reduction (long blocks only) ------------------------------------------------------ */
static void antialias(int gr, int ch)
{
    gr_info *g = &m_gr[gr][ch];
    int sb, i, nsb;
    if (g->window_switching && g->block_type == 2 && !g->mixed_block) return;
    nsb = (g->window_switching && g->block_type == 2 && g->mixed_block) ? 1 : 31;
    for (sb = 0; sb < nsb; sb++)
        for (i = 0; i < 8; i++) {
            int a = sb * 18 + 17 - i, b = sb * 18 + 18 + i;
            float x = m_xr[ch][a], y = m_xr[ch][b];
            m_xr[ch][a] = x * t_cs[i] - y * t_ca[i];
            m_xr[ch][b] = y * t_cs[i] + x * t_ca[i];
        }
}

/* ---- IMDCT + overlap-add ----------------------------------------------------------------------- */
static void imdct_sb(int ch, int sb, int block_type, float *out)
{
    const float *in = &m_xr[ch][sb * 18];
    float raw[36];
    int i, k;

    if (block_type == 2) {
        /* Three 6-point transforms, each windowed to 12 samples and laid into
           the 36-sample frame at 6, 12 and 18 - so consecutive short windows
           overlap by 6 and the first/last six samples stay zero. Placing them
           at 0/6/12 instead (the obvious-looking k*6) shifts every short block
           a third of a window early, which sounds like smeared transients. */
        for (i = 0; i < 36; i++) raw[i] = 0.0f;
        for (k = 0; k < 3; k++) {
            float sub[12];
            for (i = 0; i < 12; i++) {
                float s = 0.0f;
                int j;
                for (j = 0; j < 6; j++) s += in[k + 3 * j] * t_imdct_s[i][j];
                sub[i] = s * t_win[2][i];
            }
            for (i = 0; i < 12; i++) raw[6 + k * 6 + i] += sub[i];
        }
    } else {
        for (i = 0; i < 36; i++) {
            float s = 0.0f;
            for (k = 0; k < 18; k++) s += in[k] * t_imdct[i][k];
            raw[i] = s * t_win[block_type][i];
        }
    }
    for (i = 0; i < 18; i++) {
        out[i] = raw[i] + m_overlap[ch][sb][i];
        m_overlap[ch][sb][i] = raw[i + 18];
    }
}

/* ---- polyphase synthesis --------------------------------------------------------------------- */
static void synth_sb(int ch, const float *sb32, short *pcm, int stride)
{
    float *v = m_synth_v[ch];
    float u[512];
    int i, j;

    m_synth_off[ch] = (m_synth_off[ch] - 64) & 1023;
    {
        int off = m_synth_off[ch];
        for (i = 0; i < 64; i++) {
            float s = 0.0f;
            for (j = 0; j < 32; j++) s += sb32[j] * t_synth[i][j];
            v[off + i] = s;
        }
    }
    for (i = 0; i < 8; i++)
        for (j = 0; j < 32; j++) {
            int a = (m_synth_off[ch] + i * 128 + j) & 1023;
            int b = (m_synth_off[ch] + i * 128 + 96 + j) & 1023;
            u[i * 64 + j] = v[a];
            u[i * 64 + 32 + j] = v[b];
        }
    for (j = 0; j < 32; j++) {
        float s = 0.0f;
        int k;
        for (k = 0; k < 16; k++) s += u[k * 32 + j] * kSynthWin[k * 32 + j];
        {
            int q = (int)(s * 32767.0f);
            if (q > 32767) q = 32767; else if (q < -32768) q = -32768;
            pcm[j * stride] = (short)q;
        }
    }
}

/* ---- one frame ------------------------------------------------------------------------------- */
static int decode_frame(void)
{
    unsigned char hdr[4], side[32];
    mp3_hdr h;
    long off = m_pos;
    int nch, gr, ch, i, sb, need;

    off = find_frame(off, &h);
    if (off < 0) return 0;
    if (raw_read(off, hdr, 4) != 4) return 0;
    nch = (h.mode == 3) ? 1 : 2;
    m_srate_idx = h.sr_idx;

    {   /* pull the side info, then append this frame's main data to the ring */
        int soff = 4 + (h.crc ? 2 : 0);
        if (raw_read(off + soff, side, h.side_len) != h.side_len) return 0;
        if (!read_side_info(side, nch)) { m_pos = off + h.size; return -1; }
        need = h.size - soff - h.side_len;
        if (need < 0) { m_pos = off + h.size; return -1; }
        if (m_main_data_begin > m_res_len) {     /* reservoir not filled yet  */
            /* keep what we have and append; this granule will be wrong, which
               is exactly what happens when you start mid-stream */
            m_res_len = 0;
        } else if (m_main_data_begin < m_res_len) {
            int drop = m_res_len - m_main_data_begin;
            memmove(m_res, m_res + drop, (unsigned)m_main_data_begin);
            m_res_len = m_main_data_begin;
        }
        if (m_res_len + need > RESERVOIR) {
            int drop = m_res_len + need - RESERVOIR;
            if (drop > m_res_len) drop = m_res_len;
            memmove(m_res, m_res + drop, (unsigned)(m_res_len - drop));
            m_res_len -= drop;
        }
        if (need > 0) {
            int got = raw_read(off + soff + h.side_len, m_res + m_res_len, need);
            m_res_len += got;
            if (got < need) { m_pos = off + h.size; return -1; }
        }
        m_bitpos = 0;
    }
    m_pos = off + h.size;

    for (gr = 0; gr < 2; gr++) {
        for (ch = 0; ch < nch; ch++) {
            int p2 = read_scalefactors(gr, ch);
            decode_huffman(gr, ch, p2);
            requantise(gr, ch);
#ifdef MP3_STATS
            {   /* build-time diagnostics only; never in the kernel image */
                extern void mp3_stat(int bt, int mixed, int mdb, int pre, int sc);
                mp3_stat(m_gr[gr][ch].window_switching ? m_gr[gr][ch].block_type : -1,
                         m_gr[gr][ch].mixed_block, m_main_data_begin,
                         m_gr[gr][ch].preflag, m_gr[gr][ch].scalefac_scale);
            }
#endif
        }
        if (nch == 2) stereo(gr, h.mode, h.mode_ext);
        for (ch = 0; ch < nch; ch++) {
            reorder(gr, ch);
            antialias(gr, ch);
            {
                float sub[18][32];
                for (sb = 0; sb < NSUBBAND; sb++) {
                    float o[18];
                    gr_info *g = &m_gr[gr][ch];
                    /* In a mixed block the two lowest subbands stay LONG and
                       use the normal window; only subbands 2..31 are short. */
                    int bt = g->block_type;
                    if (bt == 2 && g->mixed_block && sb < 2) bt = 0;
                    imdct_sb(ch, sb, bt, o);
                    for (i = 0; i < 18; i++) {
                        /* frequency inversion: every other sample of every
                           other subband is negated before the filterbank */
                        sub[i][sb] = ((sb & 1) && (i & 1)) ? -o[i] : o[i];
                    }
                }
                for (i = 0; i < 18; i++)
                    synth_sb(ch, sub[i], &m_pcm[ch][gr * 576 + i * 32], 1);
            }
        }
        if (nch == 1)
            memcpy(m_pcm[1] + gr * 576, m_pcm[0] + gr * 576, 576 * sizeof(short));
    }
    m_pcm_have = 1152;
    m_pcm_taken = 0;
    return 1;
}

/* ---- decoder interface -------------------------------------------------------------------------- */
static int mp3_probe(const unsigned char *head, long n, const char *ext)
{
    if (n >= 3 && head[0] == 'I' && head[1] == 'D' && head[2] == '3') return 1;
    if (n >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0) return 1;
    return !strcmp(ext, "MP3");
}

static void reset_state(void)
{
    memset(m_overlap, 0, sizeof m_overlap);
    memset(m_synth_v, 0, sizeof m_synth_v);
    memset(m_scalefac_l, 0, sizeof m_scalefac_l);
    memset(m_scalefac_s, 0, sizeof m_scalefac_s);
    m_synth_off[0] = m_synth_off[1] = 0;
    m_res_len = 0; m_bitpos = 0;
    m_pcm_have = m_pcm_taken = 0;
}

static int mp3_open(um_audio_info *info)
{
    mp3_hdr h;
    long off;
    m_ok = 0;
    m_buf_off = -1; m_buf_len = 0;
    build_tables();

    off = find_frame(skip_id3(), &h);
    if (off < 0) {
        /* Might be a rate we do not decode: say so rather than fail silently */
        unsigned char b[4];
        if (raw_read(0, b, 4) == 4 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) {
            int ver = (b[1] >> 3) & 3;
            if (ver != 3) return 0;                 /* MPEG-2 / 2.5          */
        }
        return 0;
    }
    m_first = off;
    m_pos = off;
    m_rate = kSampleRate[h.sr_idx];
    m_nch = (h.mode == 3) ? 1 : 2;
    m_srate_idx = h.sr_idx;
    reset_state();

    {   /* duration: prefer a VBR header's frame count, else assume CBR */
        long frames = xing_frames(off, &h);
        long total;
        if (frames > 0) {
            m_total_ms = (long)(((int64_t)frames * 1152 * 1000) / m_rate);
            m_pos = off + h.size;                  /* skip the header frame  */
        } else {
            total = um_size() - off;
            m_total_ms = (long)(((int64_t)total * 8) / kBitrate[h.br_idx]);
        }
    }
    info->rate        = m_rate;
    info->channels    = m_nch;
    info->duration_ms = m_total_ms;
    info->bitrate     = kBitrate[h.br_idx];
    m_frames_out = 0;
    m_ok = 1;
    return 1;
}

static int mp3_decode(short *out, int max_frames)
{
    int done = 0;
    if (!m_ok) return 0;
    while (done < max_frames) {
        int avail, take, i;
        if (m_pcm_taken >= m_pcm_have) {
            int r = decode_frame();
            if (r == 0) break;                     /* end of stream          */
            if (r < 0) continue;                   /* bad frame: skip it     */
        }
        avail = m_pcm_have - m_pcm_taken;
        take = max_frames - done;
        if (take > avail) take = avail;
        for (i = 0; i < take; i++) {
            out[(done + i) * m_nch] = m_pcm[0][m_pcm_taken + i];
            if (m_nch == 2)
                out[(done + i) * 2 + 1] = m_pcm[1][m_pcm_taken + i];
        }
        m_pcm_taken += take;
        done += take;
    }
    m_frames_out += done;
    return done;
}

static int mp3_seek(long ms)
{
    long target, bytes;
    if (!m_ok) return 0;
    if (ms < 0) ms = 0;
    if (m_total_ms <= 0) return 0;
    /* Byte-proportional seek. Layer III's bit reservoir means the first
       granule or two after a seek can be wrong (they reference data we
       skipped), so state is reset and the stream re-syncs on the next frame
       header - the same one-frame settle every decoder has here. */
    target = ms;
    if (target > m_total_ms) target = m_total_ms;
    bytes = m_first + (long)(((int64_t)(um_size() - m_first) * target) /
                             (m_total_ms ? m_total_ms : 1));
    reset_state();
    m_pos = bytes;
    m_frames_out = (long)(((int64_t)target * m_rate) / 1000);
    return 1;
}

static long mp3_pos_ms(void)
{
    if (!m_ok || !m_rate) return 0;
    return (long)(((int64_t)m_frames_out * 1000) / m_rate);
}

static void mp3_close(void) { m_ok = 0; }

const um_adecoder um_adec_mp3 = {
    "MP3", mp3_probe, mp3_open, mp3_decode, mp3_seek, mp3_close, mp3_pos_ms
};
