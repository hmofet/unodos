/* ===========================================================================
 * UnoDOS/pc64 - Music: a real media player for the unoui shell.
 *
 * Replaces the legacy MUSIC.UNO bridge app, which drew itself with the old
 * Mac-Toolbox primitives against a fixed four-colour palette - that is where
 * the hardcoded #0000AA background came from, and why no amount of theme
 * switching ever changed it. Everything here is a unoui widget drawn by the
 * active theme, so the player matches the desktop in all ten themes.
 *
 * What it plays: whatever unomedia's audio layer decodes (WAV, MIDI, MP3,
 * AAC - see ../unomedia/README.md), reached through the pc64_media adapter's
 * windowed byte source; plus the built-in tune library, through the square
 * voice, so a machine with no files still has something to play.
 *
 * The one canvas is the level meter - genuinely per-pixel work, which is the
 * bar a canvas has to clear here. The browser, transport, sliders and status
 * are all real controls: UI_LIST, UI_BUTTON, UI_SLIDER, UI_DROPDOWN, UI_LABEL.
 *
 * Audio is pumped from tick(): decode ahead into the PCM stream FIFO only as
 * far as it has room, so a slow decode costs latency rather than stalling the
 * shell, and the DAC keeps looping whatever it has.
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "pc64_fs.h"
#include "pc64_media.h"
#include "snd_pcm.h"
#include "fat.h"
#include "pc64_icons.h"     /* pc64_shell_theme */
#include "unosound.h"
#include <string.h>

void pc64_shell_dirty(void);
int  pc64_shell_workarea_w(void);
int  pc64_shell_workarea_h(void);

/* ---- widget ids (600..639 reserved for Music) ------------------------------ */
enum {
    MID_VOL = 600, MID_UP, MID_PLAY, MID_STOP, MID_PREV, MID_NEXT,
    MID_LIST, MID_SEEK, MID_GAIN
};

#define MU_MAXE   128
#define MU_PCM    4096                  /* decode chunk, frames               */
#define VOL_TUNES 0                     /* dropdown slot 0 = the tune library */

/* ---- the built-in tunes (kept from the old app) ---------------------------- */
#define QN 30
#define EN 15
#define HN 60
#define DQ 45
static const u_seqnote_t kCanon[] = {
    {72,QN},{71,QN},{69,QN},{67,QN}, {65,QN},{64,QN},{65,QN},{67,QN},
    {72,EN},{76,EN},{71,EN},{74,EN}, {69,EN},{72,EN},{67,EN},{71,EN},
    {65,EN},{69,EN},{64,EN},{67,EN}, {65,EN},{69,EN},{67,EN},{71,EN},
};
static const u_seqnote_t kOde[] = {
    {64,QN},{64,QN},{65,QN},{67,QN},{67,QN},{65,QN},{64,QN},{62,QN},
    {60,QN},{60,QN},{62,QN},{64,QN},{64,DQ},{62,EN},{62,HN},
};
static const u_seqnote_t kTwinkle[] = {
    {60,QN},{60,QN},{67,QN},{67,QN},{69,QN},{69,QN},{67,HN},
    {65,QN},{65,QN},{64,QN},{64,QN},{62,QN},{62,QN},{60,HN},
};
static const u_seqnote_t kGreen[] = {
    {69,QN},{72,QN},{74,QN},{76,DQ},{77,EN},{76,QN},{74,QN},{71,QN},
    {67,DQ},{69,EN},{71,QN},{72,QN},{69,QN},{69,DQ},{68,EN},{69,QN},
    {71,HN},{68,QN},{64,HN},
};
static const u_seqnote_t kJingle[] = {
    {64,QN},{64,QN},{64,HN},{64,QN},{64,QN},{64,HN},
    {64,QN},{67,QN},{60,DQ},{62,EN},{64,HN},
    {65,QN},{65,QN},{65,DQ},{65,EN},{65,QN},{64,QN},{64,QN},{64,EN},{64,EN},
    {67,QN},{67,QN},{65,QN},{62,QN},{60,HN},
};
static const u_seqnote_t kSaints[] = {
    {60,QN},{64,QN},{65,QN},{67,HN},{60,QN},{64,QN},{65,QN},{67,HN},
    {60,QN},{64,QN},{65,QN},{67,QN},{64,QN},{60,QN},{64,QN},{62,HN},
    {64,QN},{64,QN},{62,QN},{60,QN},{67,HN},
};
static const u_seqnote_t kMary[] = {
    {64,QN},{62,QN},{60,QN},{62,QN},{64,QN},{64,QN},{64,HN},
    {62,QN},{62,QN},{62,HN},{64,QN},{67,QN},{67,HN},
    {64,QN},{62,QN},{60,QN},{62,QN},{64,QN},{64,QN},{64,QN},
    {64,QN},{62,QN},{62,QN},{64,QN},{62,QN},{60,HN},
};
static const u_seqnote_t kAmazing[] = {
    {67,QN},{72,HN},{76,QN},{72,QN},{76,HN},{74,QN},{72,HN},{69,QN},
    {67,HN},{72,QN},{76,HN},{74,QN},{72,QN},{69,QN},{67,HN},
};
#define NS(a) (int)(sizeof(a)/sizeof(a[0]))
typedef struct { const u_seqnote_t *n; int count; const char *title; } mutune;
static const mutune kTunes[] = {
    { kCanon,   NS(kCanon),   "Canon in D (Pachelbel)"     },
    { kOde,     NS(kOde),     "Ode to Joy (Beethoven)"     },
    { kTwinkle, NS(kTwinkle), "Twinkle Twinkle (Mozart)"   },
    { kGreen,   NS(kGreen),   "Greensleeves (Traditional)" },
    { kJingle,  NS(kJingle),  "Jingle Bells (Pierpont)"    },
    { kSaints,  NS(kSaints),  "When the Saints (Trad.)"    },
    { kMary,    NS(kMary),    "Mary Had a Little Lamb"     },
    { kAmazing, NS(kAmazing), "Amazing Grace (Trad.)"      },
};
#define NTUNES (int)(sizeof(kTunes)/sizeof(kTunes[0]))

/* ---- state ---------------------------------------------------------------- */
enum { ST_STOP = 0, ST_PLAY, ST_PAUSE };

static int   mu_volsel;                 /* dropdown index (0 = tunes)         */
static char  mu_path[120];              /* subdir within a FAT volume         */
static uno_fat_entry mu_e[MU_MAXE];
static const char *mu_items[MU_MAXE];
static char  mu_names[MU_MAXE][32];   /* wide enough for the full tune titles */
static int   mu_n, mu_sel;

static int   mu_state;
static int   mu_track = -1;             /* index in mu_e of what's playing    */
static um_audio_info mu_info;
static char  mu_now[80] = "Nothing playing";
static char  mu_meta[80] = "";
static char  mu_time[40] = "";
static short mu_pcm[MU_PCM * 2];
static int   mu_gain = 70;
static int   mu_level, mu_peak_hold;
static int   mu_seek_drag;

static unoui_window *mu_win;
static unoui_widget *mu_w_play, *mu_w_list, *mu_w_seek, *mu_w_vol, *mu_w_vol_dd;
static int mu_canvas_wi = -1;

static const char *mu_vols[14];
static int   mu_nvols;
static int   mu_volmap[14];             /* dropdown slot -> fs volume index   */

/* ---- small string helpers -------------------------------------------------- */
static char *mcat(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *mcat_int(char *p, long v)
{ char t[14]; int n = 0; if (v < 0) { *p++ = '-'; v = -v; }
  if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
  while (n) *p++ = t[--n]; return p; }
static char *mcat_time(char *p, long ms)
{
    long s = ms / 1000;
    p = mcat_int(p, s / 60);
    *p++ = ':';
    *p++ = (char)('0' + (s % 60) / 10);
    *p++ = (char)('0' + (s % 60) % 10);
    return p;
}

static int mu_is_tunes(void) { return mu_volsel == VOL_TUNES; }
static int mu_fsvol(void)    { return mu_volmap[mu_volsel]; }
static int mu_is_fat(void)   { return !mu_is_tunes() && uno_fs_kind(mu_fsvol()) == 1; }

/* ---- the browser ----------------------------------------------------------- */
static void mu_relist(void)
{
    int i, k = 0;
    mu_n = 0;
    if (mu_is_tunes()) {
        for (i = 0; i < NTUNES && k < MU_MAXE; i++) {
            strncpy(mu_names[k], kTunes[i].title, sizeof mu_names[0] - 1);
            mu_names[k][sizeof mu_names[0] - 1] = 0;
            mu_e[k].is_dir = 0; mu_e[k].size = -1;
            mu_items[k] = mu_names[k];
            k++;
        }
        mu_n = k;
    } else if (mu_is_fat()) {
        int fv = uno_fs_fat_index(mu_fsvol());
        uno_fat_entry tmp[MU_MAXE];
        int n = uno_fat_list_ex(fv, mu_path[0] ? mu_path : 0, tmp, MU_MAXE);
        if (n > MU_MAXE) n = MU_MAXE;
        for (i = 0; i < n && k < MU_MAXE; i++) {
            /* directories always show (you have to be able to navigate);
               files only when the media layer could actually open them */
            if (!tmp[i].is_dir && !um_audio_is(tmp[i].name)) continue;
            mu_e[k] = tmp[i];
            { char *p = mu_names[k];
              if (tmp[i].is_dir) p = mcat(p, "[");
              { const char *s = tmp[i].name; int j = 0;
                while (*s && j < 14) { *p++ = *s++; j++; } }
              if (tmp[i].is_dir) p = mcat(p, "]");
              *p = 0; }
            mu_items[k] = mu_names[k];
            k++;
        }
        mu_n = k;
    } else {
        int n = uno_fs_list_begin(mu_fsvol());
        for (i = 0; i < n && k < MU_MAXE; i++) {
            char nm[16];
            if (!uno_fs_list_get(mu_fsvol(), i, nm, sizeof nm)) continue;
            if (!um_audio_is(nm)) continue;
            strncpy(mu_e[k].name, nm, 12); mu_e[k].name[12] = 0;
            mu_e[k].is_dir = 0; mu_e[k].size = -1;
            strncpy(mu_names[k], nm, sizeof mu_names[0] - 1);
            mu_names[k][sizeof mu_names[0] - 1] = 0;
            mu_items[k] = mu_names[k];
            k++;
        }
        mu_n = k;
        mu_path[0] = 0;
    }
    if (mu_sel >= mu_n) mu_sel = mu_n - 1;
    if (mu_sel < 0) mu_sel = 0;
    if (mu_w_list) { mu_w_list->items = mu_items; mu_w_list->nitems = mu_n;
                     mu_w_list->sel = mu_sel; mu_w_list->value = mu_sel; }
}

static void join_path(const char *name, char *out, int max)
{
    char *p = out, *end = out + max - 1;
    const char *s = mu_path;
    while (*s && p < end) *p++ = *s++;
    if (p != out && p < end) *p++ = '\\';
    s = name; while (*s && p < end) *p++ = *s++;
    *p = 0;
}

/* ---- transport ------------------------------------------------------------- */
static void mu_stop(void)
{
    if (uno_snd_stream_open()) { uno_snd_stream_end(); um_audio_close(); }
    uno_seq_stop();
    mu_state = ST_STOP;
    mu_track = -1;
    mu_level = mu_peak_hold = 0;
    strcpy(mu_now, "Nothing playing");
    mu_meta[0] = 0;
    mu_time[0] = 0;
    if (mu_w_play) mu_w_play->text = "Play";
    if (mu_w_seek) { mu_w_seek->value = 0; }
}

static void mu_describe(const char *fallback)
{
    char *p;
    p = mcat(mu_now, mu_info.title[0] ? mu_info.title : fallback);
    *p = 0;
    p = mcat(mu_meta, mu_info.format ? mu_info.format : "?");
    p = mcat(p, "  ");
    p = mcat_int(p, mu_info.rate);
    p = mcat(p, " Hz  ");
    p = mcat(p, mu_info.channels >= 2 ? "stereo" : "mono");
    if (mu_info.bitrate > 0) {
        p = mcat(p, "  ");
        p = mcat_int(p, mu_info.bitrate);
        p = mcat(p, " kbps");
    }
    *p = 0;
}

static int mu_play_index(int idx)
{
    char path[136];
    if (idx < 0 || idx >= mu_n) return 0;
    mu_stop();
    if (mu_is_tunes()) {
        uno_seq_play(kTunes[idx].n, kTunes[idx].count);
        mu_track = idx;
        mu_state = ST_PLAY;
        strncpy(mu_now, kTunes[idx].title, sizeof mu_now - 1);
        mu_now[sizeof mu_now - 1] = 0;
        strcpy(mu_meta, "built-in tune  -  square voice");
        if (mu_w_play) mu_w_play->text = "Pause";
        return 1;
    }
    if (mu_e[idx].is_dir) return 0;
    join_path(mu_e[idx].name, path, sizeof path);
    if (!pc64_media_open(mu_fsvol(), path, &mu_info)) {
        /* prefer the decoder's own reason: "no decoder in this build" is a
           very different thing to tell someone than "malformed" */
        const char *why = um_error();
        strcpy(mu_now, "Cannot play that file");
        if (why && why[0]) {
            strncpy(mu_meta, why, sizeof mu_meta - 1);
            mu_meta[sizeof mu_meta - 1] = 0;
        } else {
            strcpy(mu_meta, "unsupported or malformed");
        }
        return 0;
    }
    mu_describe(mu_e[idx].name);
    uno_snd_stream_begin(mu_info.rate, mu_info.channels);
    mu_track = idx;
    mu_state = ST_PLAY;
    if (mu_w_play) mu_w_play->text = "Pause";
    return 1;
}

/* step to the next/previous PLAYABLE entry, skipping directories */
static int mu_step(int dir)
{
    int i, idx = mu_track < 0 ? mu_sel : mu_track;
    for (i = 0; i < mu_n; i++) {
        idx += dir;
        if (idx < 0) idx = mu_n - 1;
        if (idx >= mu_n) idx = 0;
        if (mu_is_tunes() || !mu_e[idx].is_dir) {
            mu_sel = idx;
            if (mu_w_list) { mu_w_list->sel = idx; mu_w_list->value = idx; }
            return mu_play_index(idx);
        }
    }
    return 0;
}

static void mu_toggle(void)
{
    if (mu_state == ST_STOP) {
        if (mu_sel >= 0 && mu_sel < mu_n) {
            if (!mu_is_tunes() && mu_e[mu_sel].is_dir) return;
            mu_play_index(mu_sel);
        }
        return;
    }
    if (mu_state == ST_PLAY) {
        mu_state = ST_PAUSE;
        if (uno_snd_stream_open()) uno_snd_stream_pause(1);
        else uno_seq_stop();
        if (mu_w_play) mu_w_play->text = "Play";
    } else {
        mu_state = ST_PLAY;
        if (uno_snd_stream_open()) uno_snd_stream_pause(0);
        else if (mu_track >= 0 && mu_track < NTUNES)
            uno_seq_play(kTunes[mu_track].n, kTunes[mu_track].count);
        if (mu_w_play) mu_w_play->text = "Pause";
    }
}

/* ---- the decode pump (called every shell tick) ------------------------------ */
void pc64_music_tick(void)
{
    int space;
    if (mu_state == ST_PLAY && uno_snd_stream_open()) {
        /* Fill only as far as the FIFO has room. This is the whole reason
           decoding can share a cooperative single-threaded shell: the work per
           tick is bounded by buffer space, not by the file. */
        while (mu_state == ST_PLAY && (space = uno_snd_stream_space()) > 0) {
            int want = space > MU_PCM ? MU_PCM : space;
            int got  = um_audio_decode(mu_pcm, want);
            if (got <= 0) {                       /* end of this track       */
                if (uno_snd_stream_queued() > 0) break;   /* let it drain    */
                if (!mu_step(1)) mu_stop();
                pc64_shell_dirty();
                return;
            }
            uno_snd_stream_write(mu_pcm, got);
            if (got < want) break;
        }
    }
    /* level meter + elapsed readout */
    {
        int lv = uno_snd_stream_open() ? uno_snd_stream_level()
                                       : (mu_state == ST_PLAY ? 40 : 0);
        if (lv > mu_level) mu_level = lv;
        else mu_level -= (mu_level - lv + 3) / 4;      /* smooth decay       */
        if (mu_level < 0) mu_level = 0;
        if (mu_level > mu_peak_hold) mu_peak_hold = mu_level;
        else if (mu_peak_hold > 0) mu_peak_hold--;
    }
    if (mu_state != ST_STOP && uno_snd_stream_open()) {
        long pos = um_audio_pos_ms();
        char *p = mcat_time(mu_time, pos);
        if (mu_info.duration_ms > 0) {
            p = mcat(p, " / ");
            p = mcat_time(p, mu_info.duration_ms);
        }
        *p = 0;
        if (mu_w_seek && !mu_seek_drag && mu_info.duration_ms > 0)
            mu_w_seek->value = (int)((pos * 1000) / mu_info.duration_ms);
    }
    if (mu_state != ST_STOP) pc64_shell_dirty();
}

/* ---- the level meter (the one legitimate canvas here) ----------------------- */
static void mu_canvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = pc64_shell_theme();
    int i, nseg = 28, gap = 2, sw;
    (void)w; (void)ctx;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.field_bg);
    sw = (r.w - (nseg - 1) * gap) / nseg;
    if (sw < 1) sw = 1;
    for (i = 0; i < nseg; i++) {
        int x = r.x + i * (sw + gap);
        int lit = (mu_level * nseg) / 100 > i;
        int pk  = (mu_peak_hold * nseg) / 100 == i + 1;
        unsigned c;
        if (lit || pk) {
            /* the top fifth reads as "hot" - the accent colour is the theme's
               own emphasis, so this stays in palette whatever theme is live */
            c = (i >= nseg - nseg / 5) ? t->pal.accent : t->pal.text;
        } else {
            c = t->pal.text_dim;
        }
        if (lit || pk)
            fb_fill_rect(x, r.y + 2, sw, r.h - 4, c);
        else
            fb_fill_rect(x, r.y + r.h / 2 - 1, sw, 2, c);
    }
}

static int mu_canvas_event(struct unoui_widget *w, const void *evp, void *ctx)
{ (void)w; (void)evp; (void)ctx; return 0; }

static unoui_canvas mu_canvas = { mu_canvas_draw, mu_canvas_event, 0 };

/* ---- actions ---------------------------------------------------------------- */
int pc64_music_action(const unoui_action *a)
{
    if (a->id < 600 || a->id >= 640) return 0;
    switch (a->id) {
    case MID_VOL:
        if (a->value >= 0 && a->value < mu_nvols && a->value != mu_volsel) {
            mu_stop();
            mu_volsel = a->value;
            mu_path[0] = 0; mu_sel = 0;
            mu_relist();
        }
        break;
    case MID_UP: {
        int i = (int)strlen(mu_path);
        if (!i) break;
        while (i > 0 && mu_path[i - 1] != '\\') i--;
        if (i > 0) i--;
        mu_path[i] = 0;
        mu_sel = 0;
        mu_relist();
        break;
    }
    case MID_LIST:
        if (a->value >= 0 && a->value < mu_n) {
            int same = (a->value == mu_sel);
            mu_sel = a->value;
            /* a click on the already-selected row commits: enter a directory
               or start the track. First click just selects. */
            if (same) {
                if (!mu_is_tunes() && mu_e[mu_sel].is_dir) {
                    char np[120];
                    join_path(mu_e[mu_sel].name, np, sizeof np);
                    strcpy(mu_path, np);
                    mu_sel = 0;
                    mu_relist();
                } else {
                    mu_play_index(mu_sel);
                }
            }
        }
        break;
    case MID_PLAY: mu_toggle(); break;
    case MID_STOP: mu_stop(); break;
    case MID_PREV: mu_step(-1); break;
    case MID_NEXT: mu_step(1); break;
    case MID_SEEK:
        if (uno_snd_stream_open() && mu_info.duration_ms > 0) {
            long ms = (long)(((long long)a->value * mu_info.duration_ms) / 1000);
            if (um_audio_seek_ms(ms)) uno_snd_stream_flush();
        }
        break;
    case MID_GAIN:
        mu_gain = a->value;
        uno_snd_volume(mu_gain);
        break;
    default: return 0;
    }
    pc64_shell_dirty();
    return 1;
}

int pc64_music_canvas_index(void) { return mu_canvas_wi; }

/* ---- keyboard drive ---------------------------------------------------------
 * The whole player is reachable without a pointer. That is not just harness
 * convenience: this OS runs on laptops where the trackpad may not have bound
 * (the X1 Carbon's I2C-HID pad being the standing example), and an app you
 * cannot operate from the keyboard is an app that machine cannot use at all.
 * The Install window already works this way for the same reason.
 *
 * Up/Down select, Enter opens a folder or starts a track, Space is
 * play/pause, S stops, N/P skip, V cycles the source, Left/Right seek. */
int pc64_music_key(int uni, int scan)
{
    int used = 1;
    switch (scan) {
    case 0x01:                                            /* up              */
        if (mu_sel > 0) mu_sel--;
        if (mu_w_list) { mu_w_list->sel = mu_sel; mu_w_list->value = mu_sel; }
        break;
    case 0x02:                                            /* down            */
        if (mu_sel < mu_n - 1) mu_sel++;
        if (mu_w_list) { mu_w_list->sel = mu_sel; mu_w_list->value = mu_sel; }
        break;
    case 0x04:                                            /* left  - seek -5 */
    case 0x03: {                                          /* right - seek +5 */
        long pos = um_audio_pos_ms() + (scan == 0x03 ? 5000 : -5000);
        if (pos < 0) pos = 0;
        if (uno_snd_stream_open() && um_audio_seek_ms(pos)) uno_snd_stream_flush();
        break;
    }
    default:
        used = 0;
        break;
    }
    if (!used) {
        used = 1;
        if (uni == '\r' || uni == '\n') {                 /* enter / open     */
            if (!mu_is_tunes() && mu_sel >= 0 && mu_sel < mu_n && mu_e[mu_sel].is_dir) {
                char np[120];
                join_path(mu_e[mu_sel].name, np, sizeof np);
                strcpy(mu_path, np);
                mu_sel = 0;
                mu_relist();
            } else {
                mu_play_index(mu_sel);
            }
        }
        else if (uni == ' ')                    mu_toggle();
        else if (uni == 's' || uni == 'S')      mu_stop();
        else if (uni == 'n' || uni == 'N')      mu_step(1);
        else if (uni == 'p' || uni == 'P')      mu_step(-1);
        else if (uni == 'v' || uni == 'V') {              /* cycle the source */
            mu_stop();
            mu_volsel = (mu_volsel + 1) % (mu_nvols > 0 ? mu_nvols : 1);
            mu_path[0] = 0; mu_sel = 0;
            mu_relist();
            if (mu_w_vol_dd) mu_w_vol_dd->sel = mu_volsel;
        }
        else if (uni == 'u' || uni == 'U') {              /* up a directory   */
            int i = (int)strlen(mu_path);
            if (i) {
                while (i > 0 && mu_path[i - 1] != '\\') i--;
                if (i > 0) i--;
                mu_path[i] = 0; mu_sel = 0;
                mu_relist();
            }
        }
        else used = 0;
    }
    if (used) pc64_shell_dirty();
    return used;
}

void pc64_music_closed(void) { mu_stop(); }

/* ---- window build ------------------------------------------------------------ */
void pc64_music_build(unoui_window *w)
{
    unoui_widget *x;
    const unoui_theme *t = pc64_shell_theme();
    int fh = fb_text_h(), ch = ui_field_h(), bh = ui_ctl_h();
    int waw = pc64_shell_workarea_w(), wah = pc64_shell_workarea_h();
    int ww, wh, cw, y, bx, i, need, listh, chrome;

    /* Lay the window out from its parts rather than picking a size and hoping.
     * Every row below has a known height, so the list gets whatever is left
     * and the window is exactly tall enough - no widget can be pushed past
     * the bottom edge when the font or theme metrics change.
     *
     * Note there is deliberately no UI_WIN_RESIZE / fill widget here: fill
     * stretches a widget to the content edge, which is only correct for the
     * LAST widget in a window. The list sits in the middle, so filling it
     * would swallow the transport controls underneath. */
    listh  = 8 * (fh + 6) + 8;                    /* about eight visible rows  */
    chrome = 2                                     /* top margin               */
           + (bh + 6)                              /* source row               */
           + listh + 6                             /* list + separator         */
           + 2 * (fh + 2) + 4                      /* now-playing + meta       */
           + (ch + 6)                              /* seek row                 */
           + (bh + 6)                              /* transport row            */
           + 18 + 2;                               /* level meter              */
    ww = 460;
    wh = chrome + t->m.title_h + t->m.frame_w + 2 * t->m.pad;
    if (ww > waw - 8)  ww = waw - 8;
    if (wh > wah - 24) { listh -= (wh - (wah - 24)); wh = wah - 24; }
    if (listh < 3 * (fh + 6)) listh = 3 * (fh + 6);
    unoui_window_init(w, "Music", 140, 70, ww, wh);
    mu_win = w; (void)mu_win;

    /* volume list: slot 0 is the built-in tunes, then every mounted volume */
    mu_nvols = 0;
    mu_vols[mu_nvols] = "Tunes"; mu_volmap[mu_nvols] = -1; mu_nvols++;
    { int nv = uno_fs_volumes();
      for (i = 0; i < nv && mu_nvols < 14; i++) {
          mu_vols[mu_nvols] = uno_fs_volume_name(i);
          mu_volmap[mu_nvols] = i;
          mu_nvols++;
      } }
    if (mu_volsel >= mu_nvols) mu_volsel = 0;

#define MBTN(label, wid, keep) do { \
        int bw_ = fb_text_w(label) + 16; \
        x = unoui_add_button(w, bx, y, bw_, label, 0); x->id = (wid); \
        keep; bx += bw_ + 4; } while (0)

    /* row 1: source picker + directory navigation */
    y = 2; bx = 0;
    { int dw = fb_text_w("MMMMMM") + 26;
      x = unoui_add_dropdown(w, bx, y, dw, mu_vols, mu_nvols, mu_volsel);
      x->id = MID_VOL; mu_w_vol_dd = x; bx += dw + 4; }
    MBTN("Up", MID_UP, (void)0);
    need = bx;
    y += bh + 6;

    cw = ww - 2 * t->m.frame_w - 2 * t->m.pad;

    /* the browser: a real list widget, scrolling and hit-testing itself */
    x = unoui_add_list(w, 0, y, cw, listh, mu_items, mu_n, mu_sel);
    x->id = MID_LIST; mu_w_list = x;
    y += listh + 4;

    /* now playing */
    unoui_add_sep(w, 0, y, cw); y += 6;
    unoui_add_label(w, 0, y, mu_now);  y += fh + 2;
    unoui_add_label(w, 0, y, mu_meta); y += fh + 4;

    /* seek + elapsed */
    x = unoui_add_slider(w, 0, y, cw - fb_text_w("00:00 / 00:00") - 8, 0, 1000, 0);
    x->id = MID_SEEK; mu_w_seek = x;
    unoui_add_label(w, cw - fb_text_w("00:00 / 00:00"), y + (ch - fh) / 2, mu_time);
    y += ch + 6;

    /* transport */
    bx = 0;
    MBTN("Play", MID_PLAY, mu_w_play = x);
    MBTN("Stop", MID_STOP, (void)0);
    MBTN("Prev", MID_PREV, (void)0);
    MBTN("Next", MID_NEXT, (void)0);
    if (bx > need) need = bx;
    unoui_add_label(w, bx + 4, y + (bh - fh) / 2, "Vol");
    bx += fb_text_w("Vol") + 8;
    { int sw = cw - bx; if (sw < 60) sw = 60;
      x = unoui_add_slider(w, bx, y, sw, 0, 100, mu_gain);
      x->id = MID_GAIN; mu_w_vol = x; (void)mu_w_vol; }
    y += bh + 6;

    /* the level meter */
    x = unoui_add_canvas(w, 0, y, cw, 18, &mu_canvas);
    mu_canvas_wi = w->nw - 1;

#undef MBTN
    w->min_w = need + 2 * t->m.frame_w + 2 * t->m.pad;
    w->min_h = wh;
    mu_relist();
    uno_snd_volume(mu_gain);
}
