/* UnoAmp encoders and the transcoder that drives them.
 *
 * Phase 7 of docs/PLAYER-WINAMP-PLAN.md.
 *
 * The transcoder is the same graph as playback with the sink swapped: an input
 * plugin decodes, the DSP chain runs, and an ENCODER writes instead of the
 * DAC. That is deliberate and it is what makes the feature nearly free -
 * anything UnoAmp can play it can convert, including formats added later by a
 * dropped-in IN_*.UNO, with no work in this file.
 *
 * It also means the EQ applies to a conversion if the EQ is on, which is
 * Winamp's behaviour with a DSP plugin loaded and is what a user who has just
 * set up an EQ curve expects. The transcode entry point takes a flag to turn
 * that off for a bit-faithful copy.
 *
 * WHAT SHIPS. Lossless containers, honestly labelled:
 *
 *   WAV  - PCM, any rate, mono or stereo. The reference output.
 *   AIFF - the same samples big-endian in an IFF container, for exchange with
 *          the Mac ports, which is the one place this matters on a project
 *          that runs on a IIGS.
 *   RAW  - headerless s16, for feeding another tool.
 *
 * MP3 AND AAC ENCODING ARE NOT HERE, and that is a scope call rather than an
 * oversight. A psychoacoustic encoder is a larger body of work than all the
 * decoders in unomedia put together, and a bad one is worse than none: it
 * would silently produce files that sound wrong and be blamed on the player.
 * The encoder registry below takes them the day someone writes one, without a
 * line changing here - which is the useful half of the work, and the half
 * that was actually asked for.
 */
#include "unoamp.h"
#include "pc64_fs.h"
#include "unomedia.h"
const um_audio_info *unoamp_in_info(void);
#include <string.h>
#include <stdlib.h>

/* ---- registry -------------------------------------------------------------- */
#define ENC_MAX 8
static const unoamp_enc *g_enc[ENC_MAX];
static int g_nenc;

int unoamp_register_enc(const unoamp_enc *e)
{
    int i;
    if (!e || e->version != UNOAMP_ABI || !e->Open || !e->Write) return 0;
    for (i = 0; i < g_nenc; i++) if (g_enc[i] == e) return 0;
    if (g_nenc >= ENC_MAX) return 0;
    g_enc[g_nenc++] = e;
    return 1;
}
int unoamp_enc_count(void) { return g_nenc; }
const unoamp_enc *unoamp_enc_at(int i)
{ return (i >= 0 && i < g_nenc) ? g_enc[i] : 0; }

static int ext_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        a++; b++;
    }
    return !*a && !*b;
}
const unoamp_enc *unoamp_find_enc(const char *ext)
{
    int i;
    if (!ext) return 0;
    for (i = 0; i < g_nenc; i++)
        if (ext_eq(g_enc[i]->extension, ext)) return g_enc[i];
    return 0;
}

/* ---- the shared output buffer ----------------------------------------------
 * THE WHOLE FILE IS ASSEMBLED IN MEMORY, then written once at Close. That is
 * not a shortcut: uno_fs_write replaces a file entire and there is no append
 * or write-at-offset in the fs, so a streaming encoder would have to rewrite
 * everything it had already written on every block - quadratic, and on a real
 * disk, unusably so.
 *
 * Assembling in memory also makes the header fixup trivial. WAV and AIFF both
 * carry sizes that are only known once the last sample is in, and every other
 * implementation solves that by seeking back over a file already on disk. Here
 * the header is still in the buffer, so it is simply overwritten.
 *
 * THE COST IS A LENGTH LIMIT, stated rather than hidden: the kernel heap is
 * 32 MB and shared with Studio, so the cap below is what can be taken without
 * starving it. At 44.1 kHz stereo that is a bit over two minutes. Lifting it
 * means adding a streaming append to fat.c, which is the right follow-up and
 * is deliberately NOT bodged around here - a conversion that silently produced
 * a truncated file would be far worse than one that says it ran out of room.
 */
#define OUT_CAP (12L * 1024 * 1024)
static unsigned char *g_ob;
static long g_obn, g_obcap;
static int  g_ovol;
static char g_opath[136];
static long g_written;              /* payload bytes, excluding the header     */
static int  g_open, g_full;

static int put(const void *p, int n)
{
    const unsigned char *s = (const unsigned char *)p;
    if (g_full) return 0;
    if (g_obn + n > g_obcap) {
        long want = g_obcap ? g_obcap * 2 : (1L << 20);
        unsigned char *nb;
        while (want < g_obn + n && want < OUT_CAP) want *= 2;
        if (want > OUT_CAP) want = OUT_CAP;
        if (g_obn + n > want) { g_full = 1; return 0; }
        nb = (unsigned char *)realloc(g_ob, (unsigned long)want);
        if (!nb) { g_full = 1; return 0; }
        g_ob = nb; g_obcap = want;
    }
    memcpy(g_ob + g_obn, s, (unsigned long)n);
    g_obn += n;
    return 1;
}

/* Overwrite bytes already in the buffer - the header fixup. */
static void poke(long off, const unsigned char *p, int n)
{ if (off >= 0 && off + n <= g_obn) memcpy(g_ob + off, p, (unsigned long)n); }

static int commit(void)
{
    int ok = 0;
    if (g_ob && !g_full) ok = uno_fs_write(g_ovol, g_opath, g_ob, g_obn);
    free(g_ob);
    g_ob = 0; g_obn = 0; g_obcap = 0;
    return ok;
}

static int begin(int vol, const char *path)
{
    free(g_ob);
    g_ob = 0; g_obn = 0; g_obcap = 0; g_written = 0; g_full = 0;
    g_ovol = vol;
    strncpy(g_opath, path, sizeof g_opath - 1);
    g_opath[sizeof g_opath - 1] = 0;
    return uno_fs_writable(vol);
}
static void put32le(unsigned v)
{ unsigned char b[4]; b[0]=(unsigned char)v; b[1]=(unsigned char)(v>>8);
  b[2]=(unsigned char)(v>>16); b[3]=(unsigned char)(v>>24); put(b,4); }
static void put16le(unsigned v)
{ unsigned char b[2]; b[0]=(unsigned char)v; b[1]=(unsigned char)(v>>8); put(b,2); }
static void put32be(unsigned v)
{ unsigned char b[4]; b[0]=(unsigned char)(v>>24); b[1]=(unsigned char)(v>>16);
  b[2]=(unsigned char)(v>>8); b[3]=(unsigned char)v; put(b,4); }
static void put16be(unsigned v)
{ unsigned char b[2]; b[0]=(unsigned char)(v>>8); b[1]=(unsigned char)v; put(b,2); }

static int g_rate, g_nch;

/* ---- WAV -------------------------------------------------------------------
 * The header carries two sizes that are only known once the last sample is in,
 * so it goes down with zeroes and is poked at Close while still in the buffer.
 * That is the one real dividend of assembling in memory. */
static int wav_open(int vol, const char *path, int rate, int ch, int bps)
{
    (void)bps;
    g_rate = rate; g_nch = ch ? ch : 2;
    if (!begin(vol, path)) return -1;
    put("RIFF", 4); put32le(0); put("WAVEfmt ", 8);
    put32le(16); put16le(1); put16le((unsigned)g_nch);
    put32le((unsigned)rate);
    put32le((unsigned)(rate * g_nch * 2));               /* byte rate          */
    put16le((unsigned)(g_nch * 2));                      /* block align        */
    put16le(16);
    put("data", 4); put32le(0);
    g_open = 1;
    return 0;
}
static int wav_write(const char *buf, int len)
{
    if (!g_open) return 0;
    if (!put(buf, len)) return 0;
    g_written += len;
    return len;
}
static void wav_close(void)
{
    unsigned char b[4];
    unsigned riff, dat;
    if (!g_open) return;
    g_open = 0;
    riff = (unsigned)(g_written + 36); dat = (unsigned)g_written;
    b[0]=(unsigned char)riff; b[1]=(unsigned char)(riff>>8);
    b[2]=(unsigned char)(riff>>16); b[3]=(unsigned char)(riff>>24);
    poke(4, b, 4);
    b[0]=(unsigned char)dat; b[1]=(unsigned char)(dat>>8);
    b[2]=(unsigned char)(dat>>16); b[3]=(unsigned char)(dat>>24);
    poke(40, b, 4);
    commit();
}
static const unoamp_enc g_enc_wav = {
    UNOAMP_ABI, "WAV (PCM, 16-bit)", "WAV", wav_open, wav_write, wav_close
};

/* ---- AIFF ------------------------------------------------------------------
 * Big-endian samples in an IFF container. The sample rate is an 80-bit IEEE
 * extended float, which is the single most annoying field in any audio format;
 * it is built by hand here because there is no FPU and no need for one - the
 * exponent and mantissa of an integer rate are trivial to place. */
static int aiff_open(int vol, const char *path, int rate, int ch, int bps)
{
    unsigned char ext[10];
    int e = 0; unsigned m = (unsigned)rate;
    (void)bps;
    g_rate = rate; g_nch = ch ? ch : 2;
    if (!begin(vol, path)) return -1;
    put("FORM", 4); put32be(0); put("AIFF", 4);
    put("COMM", 4); put32be(18);
    put16be((unsigned)g_nch);
    put32be(0);                              /* frame count, patched at close  */
    put16be(16);
    /* 80-bit extended: normalise the mantissa to bit 63, exponent biased 16383. */
    while (m && !(m & 0x80000000u)) { m <<= 1; e++; }
    { unsigned exp = 16383u + 31u - (unsigned)e;
      ext[0] = (unsigned char)(exp >> 8); ext[1] = (unsigned char)exp;
      ext[2] = (unsigned char)(m >> 24); ext[3] = (unsigned char)(m >> 16);
      ext[4] = (unsigned char)(m >> 8);  ext[5] = (unsigned char)m;
      ext[6] = ext[7] = ext[8] = ext[9] = 0; }
    put(ext, 10);
    put("SSND", 4); put32be(0); put32be(0); put32be(0);
    g_open = 1;
    return 0;
}
static int aiff_write(const char *buf, int len)
{
    /* s16 little-endian in, big-endian out. Done in place in a scratch block
     * rather than byte at a time, because put() is buffered and a per-sample
     * call would spend more time in the call than in the swap. */
    unsigned char t[1024];
    const unsigned char *s = (const unsigned char *)buf;
    int done = 0;
    if (!g_open) return 0;
    while (done < len) {
        int n = len - done, i;
        if (n > (int)sizeof t) n = (int)sizeof t;
        n &= ~1;
        if (!n) break;
        for (i = 0; i < n; i += 2) { t[i] = s[done + i + 1]; t[i + 1] = s[done + i]; }
        if (!put(t, n)) return 0;
        done += n;
    }
    g_written += done;
    return done;
}
static void aiff_close(void)
{
    unsigned char b[4];
    unsigned form, frames, ssnd;
    if (!g_open) return;
    g_open = 0;
    form   = (unsigned)(g_written + 46);
    frames = (unsigned)(g_written / (g_nch * 2));
    ssnd   = (unsigned)(g_written + 8);
    b[0]=(unsigned char)(form>>24); b[1]=(unsigned char)(form>>16);
    b[2]=(unsigned char)(form>>8);  b[3]=(unsigned char)form;      poke(4, b, 4);
    b[0]=(unsigned char)(frames>>24); b[1]=(unsigned char)(frames>>16);
    b[2]=(unsigned char)(frames>>8);  b[3]=(unsigned char)frames;  poke(22, b, 4);
    b[0]=(unsigned char)(ssnd>>24); b[1]=(unsigned char)(ssnd>>16);
    b[2]=(unsigned char)(ssnd>>8);  b[3]=(unsigned char)ssnd;      poke(46, b, 4);
    commit();
}
static const unoamp_enc g_enc_aiff = {
    UNOAMP_ABI, "AIFF (PCM, 16-bit)", "AIF", aiff_open, aiff_write, aiff_close
};

/* ---- RAW -------------------------------------------------------------------
 * No header, no fixup, nothing to get wrong. Which is exactly why it is worth
 * shipping: when a conversion sounds wrong, RAW is how you find out whether
 * the problem is the samples or the container. */
static int raw_open(int vol, const char *path, int rate, int ch, int bps)
{
    (void)bps;
    g_rate = rate; g_nch = ch ? ch : 2;
    if (!begin(vol, path)) return -1;
    g_open = 1;
    return 0;
}
static void raw_close(void) { if (g_open) { g_open = 0; commit(); } }
static const unoamp_enc g_enc_raw = {
    UNOAMP_ABI, "Raw PCM (headerless s16)", "PCM", raw_open, wav_write, raw_close
};

void unoamp_enc_init(void)
{
    unoamp_register_enc(&g_enc_wav);
    unoamp_register_enc(&g_enc_aiff);
    unoamp_register_enc(&g_enc_raw);
}

/* ---- the transcoder ---------------------------------------------------------
 * Playback's graph with the sink swapped. It runs to completion rather than a
 * frame at a time: a conversion has no reason to be interactive, and holding
 * the decoder open across frames would mean the player could not be used while
 * one ran. The shell shows progress through the callback.
 *
 * IT WILL NOT OVERWRITE ITS OWN INPUT. Converting a file onto itself would
 * truncate the source before the first block was read, and the failure would
 * be silent and total.
 */
int unoamp_transcode(int in_vol, const char *in_path,
                     int out_vol, const char *out_path,
                     const char *ext, int apply_dsp,
                     int (*progress)(int pct), const char **why)
{
    const unoamp_in *in = unoamp_find_in(in_path);
    const unoamp_enc *enc = unoamp_find_enc(ext);
    const um_audio_info *info;
    short buf[1152 * 2 * 2];
    long done_ms = 0;
    int rate = 44100, nch = 2, len_ms = -1, ok = 1;

    if (why) *why = "";
    if (!in)  { if (why) *why = "no decoder for this file";  return 0; }
    if (!enc) { if (why) *why = "no encoder for that format"; return 0; }
    if (in_vol == out_vol && !strcmp(in_path, out_path)) {
        if (why) *why = "that would overwrite the file being converted";
        return 0;
    }
    /* Converting stops playback: there is one decoder instance, and pretending
     * otherwise would have the conversion steal the playing track's position
     * halfway through. */
    unoamp_stop();

    unoamp_in_set_volume_index(in_vol);
    if (in->Play(in_path) < 0) { if (why) *why = um_error(); return 0; }
    info = unoamp_in_info();
    if (info) { rate = info->rate; nch = info->channels ? info->channels : 2; }
    if (in->GetLength) len_ms = in->GetLength();

    if (enc->Open(out_vol, out_path, rate, nch, 16) < 0) {
        in->Stop();
        if (why) *why = "could not create the output file";
        return 0;
    }

    for (;;) {
        int got = in->Decode ? in->Decode(buf, 1152) : 0;
        if (got <= 0) break;
        if (apply_dsp) got = unoamp_dsp_run(buf, got, nch, rate);
        if (got <= 0) continue;
        if (enc->Write((const char *)buf, got * nch * (int)sizeof(short)) <= 0) {
            ok = 0;
            if (why) *why = "longer than the conversion buffer allows";
            break;
        }
        done_ms += (long)got * 1000 / (rate ? rate : 44100);
        if (progress) {
            int pct = len_ms > 0 ? (int)(done_ms * 100 / len_ms) : -1;
            if (pct > 100) pct = 100;
            if (!progress(pct)) { ok = 0; if (why) *why = "cancelled"; break; }
        }
    }

    if (enc->Close) enc->Close();
    in->Stop();
    return ok;
}
