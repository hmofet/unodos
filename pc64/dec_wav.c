/* ===========================================================================
 * UnoDOS/pc64 - the WAV decoder (RIFF/WAVE), written from scratch.
 *
 * Handles what real files actually contain: linear PCM at 8 / 16 / 24 / 32
 * bits, IEEE float at 32 / 64 bits, any sample rate, any channel count
 * (anything wider than stereo is folded to the front pair), and
 * WAVE_FORMAT_EXTENSIBLE, whose real format tag hides in the first two bytes
 * of the SubFormat GUID.
 *
 * The chunk walk is deliberately tolerant: chunk order is not guaranteed by
 * the spec and writers put LIST/fact/bext/junk chunks wherever they like, so
 * this scans until it has both `fmt ` and `data` rather than assuming the
 * canonical 44-byte layout. Odd-sized chunks are word-padded, which several
 * popular encoders get wrong in the length field but right on disk.
 *
 * Nothing is loaded up front - samples stream through uno_src_read as they
 * are converted, so file size is irrelevant to memory use.
 * ======================================================================== */
#include "pc64_media.h"
#include <string.h>
#include <stdint.h>

#define WF_PCM        0x0001
#define WF_FLOAT      0x0003
#define WF_EXTENSIBLE 0xFFFE

#define RAW_BUF 16384

static int   w_ok;
static int   w_fmt;                     /* WF_PCM / WF_FLOAT                  */
static int   w_bits, w_nch, w_rate;
static int   w_align;                   /* bytes per frame on disk            */
static long  w_data_off, w_data_len;
static long  w_cur;                     /* bytes consumed from the data chunk */
static int   w_out_ch;                  /* 1 or 2 - what decode() emits       */
static unsigned char w_raw[RAW_BUF];

static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

static int wav_probe(const unsigned char *head, long n, const char *ext)
{
    (void)ext;
    if (n < 12) return 0;
    return !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4);
}

/* walk the chunk list collecting `fmt ` and `data` */
static int wav_open(uno_media_info *info)
{
    long off = 12, size = uno_src_size();
    int have_fmt = 0, have_data = 0;
    unsigned char hdr[8], fmt[40];

    w_ok = 0; w_cur = 0;
    while (off + 8 <= size && !(have_fmt && have_data)) {
        uint32_t clen;
        if (uno_src_read(off, hdr, 8) != 8) break;
        clen = rd32(hdr + 4);
        if (!memcmp(hdr, "fmt ", 4)) {
            long want = clen > sizeof fmt ? (long)sizeof fmt : (long)clen;
            if (want < 16 || uno_src_read(off + 8, fmt, want) != want) return 0;
            w_fmt   = rd16(fmt);
            w_nch   = rd16(fmt + 2);
            w_rate  = (int)rd32(fmt + 4);
            w_align = rd16(fmt + 12);
            w_bits  = rd16(fmt + 14);
            if (w_fmt == WF_EXTENSIBLE) {
                /* the true tag is the first 2 bytes of the SubFormat GUID */
                if (want < 26) return 0;
                w_fmt = rd16(fmt + 24);
            }
            have_fmt = 1;
        } else if (!memcmp(hdr, "data", 4)) {
            w_data_off = off + 8;
            w_data_len = (long)clen;
            /* a streamed writer can leave 0 or 0xFFFFFFFF here; trust the file */
            if (w_data_len <= 0 || w_data_off + w_data_len > size)
                w_data_len = size - w_data_off;
            have_data = 1;
        }
        off += 8 + (long)clen + (clen & 1);      /* chunks are word-aligned */
    }
    if (!have_fmt || !have_data) return 0;
    if (w_nch < 1 || w_nch > 32) return 0;
    if (w_rate < 1000 || w_rate > 192000) return 0;
    if (w_fmt != WF_PCM && w_fmt != WF_FLOAT) return 0;
    if (w_fmt == WF_PCM   && w_bits != 8 && w_bits != 16 && w_bits != 24 && w_bits != 32) return 0;
    if (w_fmt == WF_FLOAT && w_bits != 32 && w_bits != 64) return 0;
    if (w_align <= 0) w_align = w_nch * (w_bits / 8);   /* repair a bad header */
    if (w_align != w_nch * (w_bits / 8)) return 0;

    w_out_ch = w_nch >= 2 ? 2 : 1;
    info->rate        = w_rate;
    info->channels    = w_out_ch;
    info->duration_ms = (long)(((int64_t)(w_data_len / w_align) * 1000) / w_rate);
    info->bitrate     = (int)((int64_t)w_rate * w_nch * w_bits / 1000);
    w_ok = 1;
    return 1;
}

/* one sample -> s16, from whatever the file stores */
static int sample_at(const unsigned char *p)
{
    if (w_fmt == WF_PCM) {
        switch (w_bits) {
        case 8:  return ((int)p[0] - 128) << 8;          /* 8-bit WAV is unsigned */
        case 16: return (int16_t)rd16(p);
        case 24: return (int16_t)(((uint32_t)p[1]) | ((uint32_t)p[2] << 8));
        default: return (int)((int32_t)rd32(p) >> 16);
        }
    }
    if (w_bits == 32) {
        union { uint32_t u; float f; } c;
        float v;
        c.u = rd32(p);
        v = c.f * 32767.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        return (int)v;
    } else {
        union { uint64_t u; double d; } c;
        double v;
        c.u = (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
        v = c.d * 32767.0;
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        return (int)v;
    }
}

static int wav_decode(short *out, int max_frames)
{
    int bps = w_bits / 8, done = 0;
    if (!w_ok) return 0;
    while (done < max_frames) {
        long left = w_data_len - w_cur;
        long want, got;
        int nf, i;
        if (left < w_align) break;                       /* end of the data   */
        want = (long)(max_frames - done) * w_align;
        if (want > RAW_BUF) want = RAW_BUF - (RAW_BUF % w_align);
        if (want > left)    want = left - (left % w_align);
        if (want < w_align) break;
        got = uno_src_read(w_data_off + w_cur, w_raw, want);
        if (got < w_align) break;                        /* short read = stop */
        nf = (int)(got / w_align);
        for (i = 0; i < nf; i++) {
            const unsigned char *f = w_raw + (long)i * w_align;
            out[(done + i) * w_out_ch] = (short)sample_at(f);
            if (w_out_ch == 2)
                out[(done + i) * 2 + 1] = (short)sample_at(f + bps);
        }
        w_cur += (long)nf * w_align;
        done  += nf;
    }
    return done;
}

static int wav_seek(long ms)
{
    long frame;
    if (!w_ok) return 0;
    if (ms < 0) ms = 0;
    frame = (long)(((int64_t)ms * w_rate) / 1000);
    w_cur = frame * w_align;
    if (w_cur > w_data_len) w_cur = w_data_len - (w_data_len % w_align);
    if (w_cur < 0) w_cur = 0;
    return 1;
}

static long wav_pos_ms(void)
{
    if (!w_ok || !w_rate) return 0;
    return (long)(((int64_t)(w_cur / w_align) * 1000) / w_rate);
}

static void wav_close(void) { w_ok = 0; w_cur = 0; }

const uno_decoder uno_dec_wav = {
    "WAV", wav_probe, wav_open, wav_decode, wav_seek, wav_close, wav_pos_ms
};
