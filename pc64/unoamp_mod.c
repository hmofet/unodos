/* UnoAmp input plugins: ProTracker MOD and VGM.
 *
 * Phase 8 of docs/PLAYER-WINAMP-PLAN.md.
 *
 * These are the reason phase 2 dispatches by content rather than by a
 * hardcoded probe order: a MOD has no magic number at offset 0 (the first 20
 * bytes are the song title, which can be anything including something that
 * looks like another format), so it must be identified by the four-character
 * tag at offset 1080, and a VGM by "Vgm " at offset 0. Two decoders that
 * disagree about what a file is are exactly what the IsOurFile contract
 * exists to arbitrate.
 *
 * BOTH ARE SYNTHESISERS, not decoders. There is no compressed bitstream to
 * unpack - a MOD is a score plus eight-bit samples, a VGM is a log of writes
 * to a sound chip. That makes them cheap (no bit reader, no huffman, no
 * transform) and it makes them exact: the output is what the hardware would
 * have produced, not an approximation of a recording of it.
 */
#include "unoamp.h"
#include "pc64_fs.h"
#include <string.h>
#include <stdlib.h>

#define MIXRATE 44100

/* ===========================================================================
 * MOD - ProTracker / Amiga
 *
 * Four channels of 8-bit samples, each with its own period (pitch) and volume,
 * mixed by linear interpolation. The period table is the Amiga's, because a
 * MOD's pitch is expressed as a PAL Amiga clock divisor - the format has no
 * concept of hertz, only of "what the Paula chip does with this number".
 * ======================================================================== */
#define MOD_CH_MAX 8
#define MOD_SMP 31

typedef struct {
    long  len, loop_start, loop_len;   /* in samples                          */
    int   vol, finetune;
    long  off;                         /* into the sample pool                */
} mod_sample;

typedef struct {
    int   smp;                         /* 1..31, 0 = keep the current one     */
    int   period;
    int   vol;
    long  pos_q16;                     /* fixed-point position in the sample  */
    long  step_q16;
    int   playing;
    int   eff, param;
} mod_chan;

static unsigned char *g_mod;           /* the whole module, resident          */
static long  g_modlen;
static mod_sample g_msmp[MOD_SMP];
static long  g_pool;                   /* offset of the sample data           */
static int   g_nch, g_npat, g_songlen;
static unsigned char g_order[128];
static mod_chan g_ch[MOD_CH_MAX];
static int   g_row, g_pat_idx, g_tick, g_speed = 6, g_bpm = 125;
static long  g_tick_samples, g_tick_left;
static int   g_mod_open, g_mod_done;

/* The Amiga's PAL clock. A period of P plays a sample at 7093789.2/(P*2) Hz;
 * this is the number every MOD player has hardcoded since 1987. */
#define PAL_CLOCK 7093789L

static unsigned rd16be(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* The four-character tag at 1080 says how many channels. M.K. is the classic
 * four; the rest are the tracker extensions that actually turn up. */
static int mod_channels(const unsigned char *t)
{
    if (!memcmp(t, "M.K.", 4) || !memcmp(t, "M!K!", 4) ||
        !memcmp(t, "FLT4", 4) || !memcmp(t, "4CHN", 4)) return 4;
    if (!memcmp(t, "6CHN", 4)) return 6;
    if (!memcmp(t, "8CHN", 4) || !memcmp(t, "FLT8", 4) || !memcmp(t, "OCTA", 4)) return 8;
    return 0;
}

static int mod_isours(const char *fn)
{
    unsigned char t[4];
    int v = unoamp_probe_volume();
    if (uno_fs_read_at(v, fn, 1080, t, 4) != 4) return 0;
    return mod_channels(t) != 0;
}

static void mod_retrigger(mod_chan *c)
{
    if (c->period <= 0) { c->playing = 0; return; }
    /* step = (sample rate implied by the period) / (our mix rate), in Q16. */
    c->step_q16 = (long)(((long long)PAL_CLOCK << 16) / ((long)c->period * 2 * MIXRATE));
    c->playing = 1;
}

static int mod_play(const char *fn)
{
    long size, need, i;
    int v = unoamp_probe_volume();
    unsigned char t[4];

    size = uno_fs_size(v, fn);
    if (size < 1084 || size > 8L * 1024 * 1024) return -1;
    free(g_mod);
    g_mod = (unsigned char *)malloc((unsigned long)size);
    if (!g_mod) return -1;
    if (uno_fs_read(v, fn, g_mod, size) != size) { free(g_mod); g_mod = 0; return -1; }
    g_modlen = size;
    memcpy(t, g_mod + 1080, 4);
    g_nch = mod_channels(t);
    if (!g_nch) { free(g_mod); g_mod = 0; return -1; }

    /* Sample headers: 30 bytes each from offset 20. Lengths and loop points
     * are in WORDS, which is the classic off-by-two that silently halves
     * every sample if you forget it. */
    need = 0;
    for (i = 0; i < MOD_SMP; i++) {
        const unsigned char *h = g_mod + 20 + i * 30;
        g_msmp[i].len        = (long)rd16be(h + 22) * 2;
        g_msmp[i].finetune   = h[24] & 15;
        g_msmp[i].vol        = h[25] > 64 ? 64 : h[25];
        g_msmp[i].loop_start = (long)rd16be(h + 26) * 2;
        g_msmp[i].loop_len   = (long)rd16be(h + 28) * 2;
        need += g_msmp[i].len;
    }
    g_songlen = g_mod[950];
    if (g_songlen > 128) g_songlen = 128;
    memcpy(g_order, g_mod + 952, 128);
    g_npat = 0;
    for (i = 0; i < 128; i++) if (g_order[i] > g_npat) g_npat = g_order[i];
    g_npat++;

    g_pool = 1084 + (long)g_npat * 64 * g_nch * 4;
    if (g_pool + need > size) {          /* truncated module: play what is there */
        long avail = size - g_pool;
        for (i = 0; i < MOD_SMP && avail > 0; i++) {
            if (g_msmp[i].len > avail) g_msmp[i].len = avail;
            avail -= g_msmp[i].len;
        }
    }
    { long at = g_pool;
      for (i = 0; i < MOD_SMP; i++) { g_msmp[i].off = at; at += g_msmp[i].len; } }

    memset(g_ch, 0, sizeof g_ch);
    g_row = 0; g_pat_idx = 0; g_tick = 0; g_speed = 6; g_bpm = 125;
    /* A "tick" is the tracker's clock: 2.5 seconds / BPM per tick, and `speed`
     * ticks per row. Both are what the module's F effect changes. */
    g_tick_samples = MIXRATE * 5 / (g_bpm * 2);
    g_tick_left = 0;
    g_mod_open = 1; g_mod_done = 0;
    return 0;
}

static void mod_stop(void)
{ free(g_mod); g_mod = 0; g_mod_open = 0; }

/* One row of the pattern: note, sample, effect for each channel. */
static void mod_step_row(void)
{
    const unsigned char *p;
    int c;
    if (g_pat_idx >= g_songlen) { g_mod_done = 1; return; }
    p = g_mod + 1084 + (long)g_order[g_pat_idx] * 64 * g_nch * 4
      + (long)g_row * g_nch * 4;
    for (c = 0; c < g_nch && c < MOD_CH_MAX; c++) {
        const unsigned char *n = p + c * 4;
        int period = ((n[0] & 0x0F) << 8) | n[1];
        int smp    = (n[0] & 0xF0) | (n[2] >> 4);
        mod_chan *ch = &g_ch[c];
        ch->eff = n[2] & 0x0F;
        ch->param = n[3];
        if (smp && smp <= MOD_SMP) { ch->smp = smp; ch->vol = g_msmp[smp - 1].vol; }
        if (period) {
            ch->period = period;
            ch->pos_q16 = 0;
            mod_retrigger(ch);
        }
        switch (ch->eff) {
        case 0xC: ch->vol = ch->param > 64 ? 64 : ch->param; break;  /* set vol */
        case 0xF:                                                    /* speed   */
            if (ch->param < 32) { if (ch->param) g_speed = ch->param; }
            else { g_bpm = ch->param; g_tick_samples = MIXRATE * 5 / (g_bpm * 2); }
            break;
        default: break;
        }
    }
    if (++g_row >= 64) { g_row = 0; g_pat_idx++; }
}

static int mod_decode(short *out, int max_frames)
{
    int done = 0;
    if (!g_mod_open || g_mod_done) return 0;
    while (done < max_frames) {
        int n, i, c;
        if (g_tick_left <= 0) {
            if (g_tick == 0) mod_step_row();
            if (g_mod_done) break;
            if (++g_tick >= g_speed) g_tick = 0;
            g_tick_left = g_tick_samples;
        }
        n = max_frames - done;
        if (n > g_tick_left) n = (int)g_tick_left;
        for (i = 0; i < n; i++) {
            int l = 0, r = 0;
            for (c = 0; c < g_nch && c < MOD_CH_MAX; c++) {
                mod_chan *ch = &g_ch[c];
                mod_sample *sm;
                long idx;
                int s;
                if (!ch->playing || !ch->smp) continue;
                sm = &g_msmp[ch->smp - 1];
                idx = ch->pos_q16 >> 16;
                if (idx >= sm->len) {
                    /* A loop length above 2 means a real loop; anything else
                     * is the format's way of saying "one shot". */
                    if (sm->loop_len > 2) {
                        ch->pos_q16 = sm->loop_start << 16;
                        idx = sm->loop_start;
                    } else { ch->playing = 0; continue; }
                }
                s = (signed char)g_mod[sm->off + idx];
                s = s * ch->vol;                        /* -64*128 .. 64*127   */
                /* Amiga panning: channels 0 and 3 left, 1 and 2 right. Hard
                 * panning is what the hardware did and what the music was
                 * written for; softening it here would be a remix. */
                if ((c & 3) == 0 || (c & 3) == 3) { l += s; r += s / 4; }
                else                              { r += s; l += s / 4; }
                ch->pos_q16 += ch->step_q16;
            }
            l = l * 4; r = r * 4;
            if (l >  32767) l =  32767;
            if (l < -32768) l = -32768;
            if (r >  32767) r =  32767;
            if (r < -32768) r = -32768;
            out[(done + i) * 2]     = (short)l;
            out[(done + i) * 2 + 1] = (short)r;
        }
        done += n;
        g_tick_left -= n;
    }
    return done;
}

static void mod_fileinfo(const char *file, char *title, int cap, int *len_ms)
{
    (void)file;
    if (title && cap > 0) {
        /* The first 20 bytes are the song name, space-padded, not always
         * NUL-terminated - which is why this copies rather than strncpy's. */
        int i, n = cap - 1 > 20 ? 20 : cap - 1;
        for (i = 0; i < n && g_mod && g_mod[i]; i++) title[i] = (char)g_mod[i];
        while (i > 0 && title[i - 1] == ' ') i--;
        title[i] = 0;
    }
    if (len_ms) *len_ms = -1;          /* a module's length is not knowable
                                          without playing it - jumps and loops */
}

static int  mod_getlen(void)  { return -1; }
static int  mod_outtime(void) { return 0; }
static void mod_setout(int ms) { (void)ms; }
static int  g_mod_paused;
static void mod_pause(void)    { g_mod_paused = 1; }
static void mod_unpause(void)  { g_mod_paused = 0; }
static int  mod_ispaused(void) { return g_mod_paused; }
static void mod_setvol(int v)  { (void)v; }
static void mod_setpan(int p)  { (void)p; }

static const unoamp_in g_in_mod = {
    UNOAMP_ABI, "ProTracker module (MOD)", 0, 0,
    "MOD\0ProTracker Module\0NST\0Noisetracker Module\0\0",
    0,                                  /* not seekable: no position index    */
    UNOAMP_CAP_PCM,
    0, 0, 0, 0,
    mod_fileinfo, mod_isours, mod_play, mod_pause, mod_unpause, mod_ispaused,
    mod_stop, mod_getlen, mod_outtime, mod_setout, mod_setvol, mod_setpan,
    mod_decode, 0
};

/* ===========================================================================
 * VGM - a log of writes to a sound chip
 *
 * A VGM is not audio, it is a recording of the register writes a game made,
 * plus wait commands. Playing it means emulating the chip. UnoDOS already has
 * an SN76489 - the Master System / Game Gear / Genesis PSG - in its own ports,
 * so THAT is the chip this supports, and a VGM using anything else is
 * recognised and refused by name rather than played back as silence.
 *
 * That refusal matters: the majority of VGMs in the wild are YM2612 (Genesis
 * FM) or YM2151, and quietly producing nothing for them would look like a bug
 * in the player rather than a missing chip.
 * ======================================================================== */
static unsigned char *g_vgm;
static long g_vgmlen, g_vgmpos, g_vgm_end;
static long g_vgm_wait;                 /* samples still to emit at 44100     */
static int  g_vgm_open, g_vgm_paused;
static const char *g_vgm_why;

/* SN76489: four channels (three square, one noise), each with a 10-bit period
 * and a 4-bit attenuation. The shift register is 16 bits, tapped at 0 and 3. */
static struct {
    unsigned short period[4], counter[4];
    unsigned char  vol[4], out[4];
    unsigned short lfsr;
    unsigned char  latch;               /* the last channel/type latched      */
} g_psg;

static const short kAtten[16] = {       /* 4-bit attenuation -> linear, Q8    */
    255, 202, 161, 128, 101, 81, 64, 51, 40, 32, 26, 20, 16, 13, 10, 0
};

static void psg_reset(void)
{
    int i;
    memset(&g_psg, 0, sizeof g_psg);
    for (i = 0; i < 4; i++) { g_psg.vol[i] = 15; g_psg.period[i] = 1; }
    g_psg.lfsr = 0x8000;
}

static void psg_write(unsigned char b)
{
    if (b & 0x80) {                     /* latch: channel + type + low bits   */
        int ch = (b >> 5) & 3;
        g_psg.latch = (unsigned char)(((b >> 4) & 1) | (ch << 1));
        if (b & 0x10) g_psg.vol[ch] = b & 15;
        else g_psg.period[ch] = (unsigned short)((g_psg.period[ch] & 0x3F0) | (b & 15));
    } else {                            /* data: the high six bits            */
        int ch = g_psg.latch >> 1;
        if (g_psg.latch & 1) g_psg.vol[ch] = b & 15;
        else g_psg.period[ch] = (unsigned short)((g_psg.period[ch] & 15) | ((b & 0x3F) << 4));
    }
}

/* The PSG clock is 3579545 Hz divided by 16, so one output sample at 44100
 * covers about 5.07 chip ticks. Accumulating that in Q16 keeps the pitch
 * right; rounding it to 5 would put every note about 1.4% flat, which is a
 * quarter tone and very audible. */
#define PSG_TICK_Q16 ((long)((3579545.0 / 16.0 / MIXRATE) * 65536.0))

static void psg_render(short *out, int n)
{
    static long acc;
    int i, c;
    for (i = 0; i < n; i++) {
        int mix = 0;
        acc += PSG_TICK_Q16;
        while (acc >= (1L << 16)) {
            acc -= (1L << 16);
            for (c = 0; c < 4; c++) {
                if (g_psg.counter[c]) g_psg.counter[c]--;
                if (g_psg.counter[c]) continue;
                g_psg.counter[c] = g_psg.period[c] ? g_psg.period[c] : 1;
                if (c < 3) g_psg.out[c] ^= 1;
                else {
                    /* Noise: white when bit 2 of the period register is set,
                     * periodic otherwise. */
                    int fb = (g_psg.period[3] & 4)
                           ? ((g_psg.lfsr & 1) ^ ((g_psg.lfsr >> 3) & 1))
                           : (g_psg.lfsr & 1);
                    g_psg.lfsr = (unsigned short)((g_psg.lfsr >> 1) | (fb << 15));
                    g_psg.out[3] = (unsigned char)(g_psg.lfsr & 1);
                }
            }
        }
        for (c = 0; c < 4; c++)
            mix += (g_psg.out[c] ? 1 : -1) * kAtten[g_psg.vol[c] & 15];
        mix *= 24;
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        out[i * 2] = out[i * 2 + 1] = (short)mix;
    }
}

static int vgm_isours(const char *fn)
{
    unsigned char h[4];
    int v = unoamp_probe_volume();
    if (uno_fs_read_at(v, fn, 0, h, 4) != 4) return 0;
    return !memcmp(h, "Vgm ", 4);
}

static unsigned rd32le(const unsigned char *p)
{ return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

static int vgm_play(const char *fn)
{
    long size;
    int v = unoamp_probe_volume();
    g_vgm_why = "";
    size = uno_fs_size(v, fn);
    if (size < 0x40 || size > 8L * 1024 * 1024) return -1;
    free(g_vgm);
    g_vgm = (unsigned char *)malloc((unsigned long)size);
    if (!g_vgm) return -1;
    if (uno_fs_read(v, fn, g_vgm, size) != size) { free(g_vgm); g_vgm = 0; return -1; }
    g_vgmlen = size;
    if (memcmp(g_vgm, "Vgm ", 4)) { free(g_vgm); g_vgm = 0; return -1; }

    /* Refuse by NAME rather than play silence. The chip clock fields say what
     * a file needs; a zero SN76489 clock with a non-zero FM clock means this
     * is a Genesis or arcade log and we do not have that chip. */
    if (!rd32le(g_vgm + 0x0C)) {
        if (rd32le(g_vgm + 0x2C)) g_vgm_why = "VGM needs the YM2612 (Genesis FM) - not emulated";
        else if (rd32le(g_vgm + 0x30)) g_vgm_why = "VGM needs the YM2151 - not emulated";
        else if (rd32le(g_vgm + 0x10)) g_vgm_why = "VGM needs the YM2413 - not emulated";
        else g_vgm_why = "VGM uses a sound chip this build does not emulate";
        free(g_vgm); g_vgm = 0;
        return -1;
    }

    /* Data offset is relative to 0x34 and only exists from version 1.50. */
    g_vgmpos = (rd32le(g_vgm + 8) >= 0x150 && rd32le(g_vgm + 0x34))
             ? 0x34 + (long)rd32le(g_vgm + 0x34) : 0x40;
    g_vgm_end = g_vgmlen;
    g_vgm_wait = 0;
    psg_reset();
    g_vgm_open = 1; g_vgm_paused = 0;
    return 0;
}

static void vgm_stop(void) { free(g_vgm); g_vgm = 0; g_vgm_open = 0; }

static int vgm_decode(short *out, int max_frames)
{
    int done = 0;
    if (!g_vgm_open) return 0;
    while (done < max_frames) {
        int n;
        while (g_vgm_wait <= 0) {
            unsigned char cmd;
            if (g_vgmpos >= g_vgm_end) { g_vgm_open = 0; return done; }
            cmd = g_vgm[g_vgmpos++];
            if (cmd == 0x50 && g_vgmpos < g_vgm_end) psg_write(g_vgm[g_vgmpos++]);
            else if (cmd == 0x61 && g_vgmpos + 1 < g_vgm_end) {
                g_vgm_wait = g_vgm[g_vgmpos] | (g_vgm[g_vgmpos + 1] << 8);
                g_vgmpos += 2;
            }
            else if (cmd == 0x62) g_vgm_wait = 735;      /* one NTSC frame     */
            else if (cmd == 0x63) g_vgm_wait = 882;      /* one PAL frame      */
            else if (cmd >= 0x70 && cmd <= 0x7F) g_vgm_wait = (cmd & 15) + 1;
            else if (cmd == 0x66) { g_vgm_open = 0; return done; }
            else if (cmd == 0x67) {                      /* a data block       */
                if (g_vgmpos + 6 <= g_vgm_end)
                    g_vgmpos += 6 + (long)rd32le(g_vgm + g_vgmpos + 2);
                else g_vgmpos = g_vgm_end;
            }
            /* Anything else is a write to a chip we do not have. Skipping its
             * operands by the command's documented width keeps the stream in
             * sync, which is what stops one unknown chip turning the rest of
             * the log into noise. */
            else if (cmd >= 0x30 && cmd <= 0x3F) g_vgmpos += 1;
            else if (cmd >= 0x40 && cmd <= 0x4E) g_vgmpos += 2;
            else if (cmd >= 0x51 && cmd <= 0x5F) g_vgmpos += 2;
            else if (cmd >= 0xA0 && cmd <= 0xBF) g_vgmpos += 2;
            else if (cmd >= 0xC0 && cmd <= 0xDF) g_vgmpos += 3;
            else if (cmd >= 0xE0) g_vgmpos += 4;    /* .. 0xFF, the top of a byte */
        }
        n = max_frames - done;
        if (n > g_vgm_wait) n = (int)g_vgm_wait;
        psg_render(out + done * 2, n);
        done += n;
        g_vgm_wait -= n;
    }
    return done;
}

static void vgm_fileinfo(const char *file, char *title, int cap, int *len_ms)
{
    (void)file;
    if (title && cap > 0) title[0] = 0;   /* the GD3 tag is UTF-16 - phase 9  */
    if (len_ms) *len_ms = (g_vgm && g_vgmlen > 0x1C)
                        ? (int)((long)rd32le(g_vgm + 0x18) * 1000 / 44100) : -1;
}
static int  vgm_getlen(void)
{ int ms = -1; vgm_fileinfo(0, 0, 0, &ms); return ms; }
static int  vgm_outtime(void) { return 0; }
static void vgm_setout(int ms) { (void)ms; }
static void vgm_pause(void)    { g_vgm_paused = 1; }
static void vgm_unpause(void)  { g_vgm_paused = 0; }
static int  vgm_ispaused(void) { return g_vgm_paused; }

static const unoamp_in g_in_vgm = {
    UNOAMP_ABI, "VGM (SN76489 PSG)", 0, 0,
    "VGM\0Video Game Music\0\0",
    0,
    UNOAMP_CAP_PCM,
    0, 0, 0, 0,
    vgm_fileinfo, vgm_isours, vgm_play, vgm_pause, vgm_unpause, vgm_ispaused,
    vgm_stop, vgm_getlen, vgm_outtime, vgm_setout, mod_setvol, mod_setpan,
    vgm_decode, 0
};

/* Why the last VGM open failed, for the host's error line. Without this a
 * refused Genesis log would be indistinguishable from a corrupt file. */
const char *unoamp_vgm_error(void) { return g_vgm_why ? g_vgm_why : ""; }

void unoamp_mod_init(void)
{
    unoamp_register_in(&g_in_mod);
    unoamp_register_in(&g_in_vgm);
}
