/* UnoAmp: the player itself - playlist, transport, mixer state.
 *
 * Phase 4 of docs/PLAYER-WINAMP-PLAN.md. unoamp_ui.c owns pixels; this owns
 * what the pixels are ABOUT. Keeping them apart is what lets the skin engine
 * be swapped for a plain window without touching playback, and what lets the
 * player be driven by the shell (or a script) with no window open at all.
 *
 * The playlist is a fixed table rather than a growing list. A player that can
 * be handed a directory of ten thousand files and then dies of allocation is
 * worse than one that says "playlist full" at a thousand - and on a machine
 * with no swap, that is not a hypothetical.
 */
#include "unoamp.h"
#include "unoamp_skin.h"
#include "pc64_fs.h"
#include "snd_pcm.h"
#include "unomedia.h"
const um_audio_info *unoamp_in_info(void);
#include <string.h>

void pc64_shell_dirty(void);

#define PL_MAX 1000
typedef struct { char path[128]; char title[96]; int vol; int len_ms; } pl_item;

static pl_item g_pl[PL_MAX];
static int g_pn, g_cur = -1, g_sel = -1;
static int g_vol = 100, g_bal;
static const char *g_err = "";

/* ---- playlist -------------------------------------------------------------- */
int unoamp_pl_count(void) { return g_pn; }
int unoamp_pl_current(void) { return g_cur; }
int unoamp_pl_selected(void) { return g_sel; }
void unoamp_pl_select(int i) { if (i >= -1 && i < g_pn) g_sel = i; }

const char *unoamp_pl_title(int i)
{ return (i >= 0 && i < g_pn) ? g_pl[i].title : ""; }
int unoamp_pl_len_ms(int i)
{ return (i >= 0 && i < g_pn) ? g_pl[i].len_ms : -1; }

/* Basename without the extension, which is what a playlist row should read as
 * before the decoder has had a chance to report a real title. */
static void nice_title(const char *path, char *out, int cap)
{
    const char *b = path, *p, *dot = 0;
    int i;
    for (p = path; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    for (p = b; *p; p++) if (*p == '.') dot = p;
    for (i = 0; i < cap - 1 && b[i] && (!dot || b + i < dot); i++) out[i] = b[i];
    out[i] = 0;
}

int unoamp_pl_add(int vol, const char *path)
{
    pl_item *it;
    if (g_pn >= PL_MAX || !path) return 0;
    it = &g_pl[g_pn];
    strncpy(it->path, path, sizeof it->path - 1);
    it->path[sizeof it->path - 1] = 0;
    it->vol = vol;
    it->len_ms = -1;               /* unknown until it has been opened once   */
    nice_title(it->path, it->title, sizeof it->title);
    g_pn++;
    if (g_sel < 0) g_sel = 0;
    pc64_shell_dirty();
    return 1;
}

void unoamp_pl_clear(void)
{
    unoamp_stop();
    g_pn = 0; g_cur = -1; g_sel = -1;
    pc64_shell_dirty();
}

void unoamp_pl_remove(int i)
{
    int k;
    if (i < 0 || i >= g_pn) return;
    if (i == g_cur) { unoamp_stop(); g_cur = -1; }
    else if (i < g_cur) g_cur--;
    for (k = i; k < g_pn - 1; k++) g_pl[k] = g_pl[k + 1];
    g_pn--;
    if (g_sel >= g_pn) g_sel = g_pn - 1;
    pc64_shell_dirty();
}

/* ---- playback -------------------------------------------------------------- */

/* Play entry `i`. The decoder's own title wins once it has one - an MP3's ID3
 * name is better than its filename - but the filename stays until then rather
 * than showing an empty row. */
int unoamp_play_index(int i)
{
    const char *why = "";
    const unoamp_in *in;
    if (i < 0 || i >= g_pn) return 0;
    if (!unoamp_play(g_pl[i].vol, g_pl[i].path, &why)) {
        g_err = why;
        pc64_shell_dirty();
        return 0;
    }
    g_err = "";
    g_cur = i;
    in = unoamp_playing();
    if (in && in->GetFileInfo) {
        char t[96]; int len = -1;
        in->GetFileInfo(g_pl[i].path, t, (int)sizeof t, &len);
        if (t[0]) { strncpy(g_pl[i].title, t, sizeof g_pl[i].title - 1);
                    g_pl[i].title[sizeof g_pl[i].title - 1] = 0; }
        g_pl[i].len_ms = len;
    }
    unoamp_ui_set_title(g_pl[i].title);
    pc64_shell_dirty();
    return 1;
}

const char *unoamp_last_error(void) { return g_err; }

/* Advance. Shuffle uses the sample position as its entropy - there is no RNG
 * in the kernel and a player does not need a good one, only an unpredictable
 * one, which "whatever the DAC has consumed by now" is. */
static int next_index(int dir)
{
    if (!g_pn) return -1;
    if (unoamp_ui_shuffle() && g_pn > 1) {
        int n = (int)(uno_snd_stream_played() % (long)g_pn);
        if (n == g_cur) n = (n + 1) % g_pn;
        return n;
    }
    if (g_cur < 0) return 0;
    if (dir > 0) {
        if (g_cur + 1 < g_pn) return g_cur + 1;
        return unoamp_ui_repeat() ? 0 : -1;
    }
    if (g_cur > 0) return g_cur - 1;
    return unoamp_ui_repeat() ? g_pn - 1 : -1;
}

void unoamp_next(void) { int n = next_index(+1); if (n >= 0) unoamp_play_index(n); else unoamp_stop(); }
void unoamp_prev(void) { int n = next_index(-1); if (n >= 0) unoamp_play_index(n); }

/* The transport row, in CBUTTONS order. One switch so the buttons, a keyboard
 * shortcut and a script verb all reach playback the same way. */
void unoamp_transport(int which)
{
    const unoamp_in *in = unoamp_playing();
    switch (which) {
    case UNOAMP_T_PREV:  unoamp_prev(); break;
    case UNOAMP_T_PLAY:
        if (in && in->IsPaused && in->IsPaused()) { if (in->UnPause) in->UnPause(); }
        else if (g_sel >= 0) unoamp_play_index(g_sel);
        else if (g_pn)       unoamp_play_index(0);
        break;
    case UNOAMP_T_PAUSE:
        if (in) {
            if (in->IsPaused && in->IsPaused()) { if (in->UnPause) in->UnPause(); }
            else if (in->Pause) in->Pause();
        }
        break;
    case UNOAMP_T_STOP:  unoamp_stop(); g_cur = -1; break;
    case UNOAMP_T_NEXT:  unoamp_next(); break;
    case UNOAMP_T_EJECT: unoamp_ui_show_pl(1); break;   /* "open file" = the list */
    default: break;
    }
    pc64_shell_dirty();
}

/* ---- mixer ------------------------------------------------------------------
 * Volume and balance are the SINK's job, not the decoder's: a decoder that
 * scaled its own samples would quantise them twice and would be wrong the
 * moment a DSP sat between the two. */
void unoamp_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_vol = v;
    { const unoamp_out *o = unoamp_current_out();
      if (o && o->SetVolume) o->SetVolume(v * 255 / 100); }
}
void unoamp_set_balance(int b)
{
    if (b < -100) b = -100;
    if (b >  100) b =  100;
    g_bal = b;
    { const unoamp_out *o = unoamp_current_out();
      if (o && o->SetPan) o->SetPan(b); }
}
int unoamp_volume(void)  { return g_vol; }
int unoamp_balance(void) { return g_bal; }

/* ---- the frame tick ---------------------------------------------------------
 * Pull from the decoder into the sink, and notice when a track has run out.
 * Winamp's input plugin would call outMod->Write from its own thread; here the
 * host does it, once per frame, which is why this function is the whole
 * playback engine. */
void unoamp_tick(void)
{
    const unoamp_in *in = unoamp_playing();
    const unoamp_out *out = unoamp_current_out();
    /* Headroom: a DSP plugin may hand back MORE frames than it was given
     * (Winamp's ABI allows time-stretching), so the buffer is twice what is
     * ever decoded into it. */
    short buf[1152 * 2 * 2];
    int room, want, got, rate = 44100, nch = 2;

    unoamp_ui_tick();
    if (!in || !out) return;
    if (in->IsPaused && in->IsPaused()) return;

    room = out->CanWrite ? out->CanWrite() : 0;
    while (room >= 1152 * 2 * (int)sizeof(short)) {
        want = 1152;                   /* half the buffer: DSP headroom      */
        got = in->Decode ? in->Decode(buf, want) : 0;
        if (got <= 0) {
            /* End of stream. Auto-advance is the behaviour a playlist implies;
             * with repeat off and nothing after it, playback stops. */
            unoamp_next();
            return;
        }
        /* The visualiser sees the audio AFTER the DSP chain, because that is
         * what will be heard - a spectrum showing the pre-EQ signal would
         * contradict the EQ sliders sitting next to it. */
        { const um_audio_info *inf = unoamp_in_info();
          if (inf) { rate = inf->rate; nch = inf->channels ? inf->channels : 2; } }
        got = unoamp_dsp_run(buf, got, nch, rate);
        if (got <= 0) continue;
        unoamp_vis_feed(buf, got);
        if (out->Write) out->Write((const char *)buf, got * nch * (int)sizeof(short));
        room -= got * nch * (int)sizeof(short);
    }
}

/* ---- opening the player -----------------------------------------------------
 * Two things have to happen before the window is worth looking at: a skin if
 * the machine has one, and something in the playlist.
 *
 * BOTH ARE BEST-EFFORT AND NEITHER IS FATAL. A machine with no \SKINS gets the
 * theme-coloured fallback (see unoamp_ui.c), and one with no audio files gets
 * an empty list rather than a refusal to open. A media player that will not
 * start because it found nothing to play is a player that cannot be used to
 * find something to play.
 */
static int g_started;

/* Winamp read its skin from a directory of .wsz files and remembered the last
 * one. There is no settings store here yet, so this takes the first skin it
 * finds, in volume then directory order - deterministic, and a machine with
 * exactly one skin installed does the obvious thing. */
static int load_a_skin(void)
{
    int nv = uno_fs_volumes(), v, i, n;
    char name[64];
    char path[80];
    for (v = 0; v < nv; v++) {
        /* uno_fs lists a volume's ROOT only, so a skin lives at the root as
         * NAME.WSZ. Nesting them under \SKINS would need the FAT layer
         * directly, whose volume indices are NOT uno_fs's - a mismatch that
         * has already cost this repo one silent wrong-disk read. */
        n = uno_fs_list_begin(v);
        for (i = 0; i < n; i++) {
            int k;
            if (!uno_fs_list_get(v, i, name, (int)sizeof name)) continue;
            k = 0; while (name[k]) k++;
            if (k < 5) continue;
            if (!((name[k-4] == '.') &&
                  (name[k-3] == 'W' || name[k-3] == 'w') &&
                  (name[k-2] == 'S' || name[k-2] == 's') &&
                  (name[k-1] == 'Z' || name[k-1] == 'z'))) continue;
            path[0] = 0;
            strncpy(path, name, sizeof path - 1);
            path[sizeof path - 1] = 0;
            if (unoamp_skin_load(v, path)) return 1;
        }
    }
    return 0;
}

/* Fill the playlist from every volume's root. Winamp opened empty and made you
 * find files; this is a machine where "find files" means a file manager in
 * another window, so seeding what is obviously playable is the kinder default.
 * Anything no decoder claims is skipped, which is the same test that would
 * have refused it at play time. */
static void seed_playlist(void)
{
    int nv = uno_fs_volumes(), v, i, n;
    char name[64];
    for (v = 0; v < nv && unoamp_pl_count() < 200; v++) {
        n = uno_fs_list_begin(v);
        for (i = 0; i < n && unoamp_pl_count() < 200; i++) {
            if (!uno_fs_list_get(v, i, name, (int)sizeof name)) continue;
            unoamp_in_set_volume_index(v);
            if (unoamp_find_in(name)) unoamp_pl_add(v, name);
        }
    }
}

void unoamp_start(void)
{
    if (g_started) return;
    g_started = 1;
    load_a_skin();
    seed_playlist();
}

