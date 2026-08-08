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

#define TICK_MAX_BLOCKS 4       /* decode/filter budget per frame - see below */

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
const char *unoamp_pl_path(int i)
{ return (i >= 0 && i < g_pn) ? g_pl[i].path : ""; }
int unoamp_pl_vol(int i)
{ return (i >= 0 && i < g_pn) ? g_pl[i].vol : 0; }
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
    int room, want, got, blocks, rate = 44100, nch = 2;

    unoamp_ui_tick();
    if (!in || !out) return;
    if (in->IsPaused && in->IsPaused()) return;

    /* AT MOST THIS MANY BLOCKS PER FRAME. The file header promises that a slow
     * decode costs latency rather than stalling the desktop; without a cap it
     * does not, and that promise was worth exactly nothing.
     *
     * The sink reports how much room it has, which after a stall is the whole
     * FIFO. Draining that in one tick means decoding and filtering a second of
     * audio inside a 17 ms frame. With the EQ off that is merely slow; with it
     * on it is ten biquads over two channels over every sample, and the shell
     * simply stops responding - which is what froze the ZimaBlade the first
     * time the equaliser was switched on during playback.
     *
     * Four blocks is about 100 ms of audio a frame: comfortably faster than
     * real time, so the buffer still refills after a stall, but bounded. Going
     * over budget now shows up as an underrun you can hear, not a desktop you
     * cannot click. */
    room = out->CanWrite ? out->CanWrite() : 0;
    blocks = 0;
    while (room >= 1152 * 2 * (int)sizeof(short) && blocks < TICK_MAX_BLOCKS) {
        blocks++;
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
        /* A DSP that consumed everything leaves nothing to write. BREAK, not
         * continue: `continue` here never decrements `room`, so a plugin
         * returning zero once spins this loop forever with interrupts on and
         * the frame never ending. */
        if (got <= 0) break;
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
/* What the opening scan saw. An empty player is ambiguous - no files, or a
 * scan that never ran - and on a machine with no file manager open that
 * ambiguity is expensive to resolve. So the counts go in the title well. */
static int g_scan_vols, g_scan_files, g_scan_skins;

/* Winamp read its skin from a directory of .wsz files and remembered the last
 * one. There is no settings store here yet, so this takes the first skin it
 * finds, in volume then directory order - deterministic, and a machine with
 * exactly one skin installed does the obvious thing. */
/* What the player is currently wearing, so `skin status` can say. Empty path
 * with g_skin_vol < 0 means the built-in look. */
static char g_skin_path[80];
static int  g_skin_vol = -1;

static int is_wsz(const char *name)
{
    int k = 0;
    while (name[k]) k++;
    if (k < 5) return 0;
    return (name[k-4] == '.') &&
           (name[k-3] == 'W' || name[k-3] == 'w') &&
           (name[k-2] == 'S' || name[k-2] == 's') &&
           (name[k-1] == 'Z' || name[k-1] == 'z');
}

/* Apply one skin and remember it. The ONE place g_skin_path/g_skin_vol are
 * set, so "what is on screen" and "what status reports" cannot drift.
 *
 * A FAILED load leaves the built-in look, not the previous skin:
 * unoamp_skin_load() resets its arena before it parses, so by the time it can
 * fail the old sheets are already gone. Saying so is better than pretending
 * otherwise - and it is the safe direction, since the alternative is a
 * half-applied skin drawn from a half-filled arena. */
static int apply_skin(int vol, const char *path)
{
    if (!unoamp_skin_load(vol, path)) {
        g_skin_path[0] = 0;
        g_skin_vol = -1;
        return 0;
    }
    g_skin_path[0] = 0;
    strncpy(g_skin_path, path, sizeof g_skin_path - 1);
    g_skin_path[sizeof g_skin_path - 1] = 0;
    g_skin_vol = vol;
    return 1;
}

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
            if (!uno_fs_list_get(v, i, name, (int)sizeof name)) continue;
            if (!is_wsz(name)) continue;
            path[0] = 0;
            strncpy(path, name, sizeof path - 1);
            path[sizeof path - 1] = 0;
            g_scan_skins++;
            if (apply_skin(v, path)) return 1;
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
    g_scan_vols = nv;
    for (v = 0; v < nv && unoamp_pl_count() < 200; v++) {
        n = uno_fs_list_begin(v);
        for (i = 0; i < n && unoamp_pl_count() < 200; i++) {
            if (!uno_fs_list_get(v, i, name, (int)sizeof name)) continue;
            g_scan_files++;
            unoamp_in_set_volume_index(v);
            if (unoamp_find_in(name)) unoamp_pl_add(v, name);
        }
    }
}

/* "V3 F47 S2 T9" - volumes scanned, files seen, skins found, tracks added.
 * Terse because it shares a 153-pixel well with the track title, and it is
 * only ever shown when the playlist came up empty. Four numbers separate
 * "the disk is empty" from "the scan never ran" from "nothing was playable",
 * which is three different bugs. */
static void scan_report(char *out, int cap)
{
    int vals[4], i, n = 0;
    const char *tag = "VFST";
    vals[0] = g_scan_vols; vals[1] = g_scan_files;
    vals[2] = g_scan_skins; vals[3] = unoamp_pl_count();
    for (i = 0; i < 4 && n < cap - 8; i++) {
        int v = vals[i], d = 1;
        out[n++] = tag[i];
        while (v / d >= 10) d *= 10;
        while (d) { out[n++] = (char)('0' + (v / d) % 10); d /= 10; }
        if (i < 3) out[n++] = ' ';
    }
    out[n] = 0;
}

void unoamp_start(void)
{
    if (g_started) return;
    g_started = 1;
    load_a_skin();
    seed_playlist();
    if (unoamp_pl_count() > 0) {
        unoamp_ui_set_title(g_pl[0].title);
        g_sel = 0;
    } else {
        char t[48];
        scan_report(t, (int)sizeof t);
        unoamp_ui_set_title(t);
    }
}

/* ---- re-skinning a RUNNING player -----------------------------------------
 *
 * A skin used to be chosen exactly once per boot: load_a_skin() runs from
 * unoamp_start(), behind the one-shot g_started above, and nothing ever reset
 * it. That is fine for a machine that boots with its skin already on the disk
 * and wrong for everything else - a skin copied in while the player is open
 * needed a REBOOT to be seen, and a re-skin could not be shown at all: not on
 * camera, and not in a test.
 *
 * So the swap gets one entry point here, in the lane that owns the skin, and
 * the URC `skin` verb is a thin pass-through to it (unoauto_remote.c, weak-
 * stubbed there so a build without UnoAmp still links). The contract is
 * iwl_dbg_cmd's, which is what makes that row three lines: reply length in
 * `out`, or -1 for a bad subcommand or a refused load.
 *
 * REPAINTING IS THE WHOLE TRICK, and it is free. unoamp_ui.c reads
 * unoamp_skin_get() live on every draw, and the control layout is fixed by the
 * skin FORMAT rather than by the loaded skin (see the atlas at the top of that
 * file), so no geometry is cached anywhere and one dirty flag is a complete
 * re-skin. Nothing here has to rebuild a window.
 *
 * NOT DEBUG-ONLY, deliberately. It is privilege-gated on the wire instead -
 * GATE[] gives `skin` DRIVE, alongside `launch`, `key` and `pointer` - which
 * is the honest classification, since changing what is on the screen is
 * exactly what DRIVE means. An `#ifdef UNO_DEBUG` seam would be one that
 * cannot be demonstrated on the build that actually ships.
 */
static int sk_put(char *out, int cap, int n, const char *s)
{
    while (*s && n < cap - 1) out[n++] = *s++;
    if (n < cap) out[n] = 0;
    return n;
}
static int sk_puti(char *out, int cap, int n, int v)
{
    int d = 1;
    if (v < 0) { if (n < cap - 1) out[n++] = '-'; v = -v; }
    while (v / d >= 10) d *= 10;
    while (d && n < cap - 1) { out[n++] = (char)('0' + (v / d) % 10); d /= 10; }
    if (n < cap) out[n] = 0;
    return n;
}
static int sk_eq(const char *a, const char *b)
{
    int i;
    for (i = 0; a[i] || b[i]; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}
static int sk_atoi(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static int sk_status(char *out, int cap)
{
    int n;
    if (!unoamp_skin_loaded())
        return sk_put(out, cap, 0, "built-in (no skin loaded)");
    n = sk_put(out, cap, 0, "skinned vol=");
    n = sk_puti(out, cap, n, g_skin_vol);
    n = sk_put(out, cap, n, " ");
    n = sk_put(out, cap, n, g_skin_path);
    return n;
}

int unoamp_skin_cmd(char *line, char *out, int cap)
{
    char *p = line, *verb, *a1, *a2;
    static char empty[1];
    int n;

    if (!out || cap < 16) return -1;
    out[0] = 0;
    if (!p) p = empty;

    /* three tokens, cut in place - the caller's buffer is already ours */
    while (*p == ' ') p++;
    verb = p; while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    while (*p == ' ') p++;
    a1 = p;   while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    while (*p == ' ') p++;
    a2 = p;   while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;
    *p = 0;

    if (!*verb || sk_eq(verb, "status"))
        return sk_status(out, cap);

    /* Every .wsz on every volume root, so a harness can discover one rather
     * than being told where it is. Same place load_a_skin() looks. */
    if (sk_eq(verb, "list")) {
        int nv = uno_fs_volumes(), v, i, m;
        char name[64];
        n = 0;
        for (v = 0; v < nv; v++) {
            m = uno_fs_list_begin(v);
            for (i = 0; i < m; i++) {
                if (!uno_fs_list_get(v, i, name, (int)sizeof name)) continue;
                if (!is_wsz(name)) continue;
                if (n) n = sk_put(out, cap, n, " ");
                n = sk_puti(out, cap, n, v);
                n = sk_put(out, cap, n, ":");
                n = sk_put(out, cap, n, name);
            }
        }
        if (!n) n = sk_put(out, cap, 0, "(no .wsz on any volume root)");
        return n;
    }

    if (sk_eq(verb, "off")) {
        unoamp_skin_unload();
        g_skin_path[0] = 0;
        g_skin_vol = -1;
        pc64_shell_dirty();
        return sk_put(out, cap, 0, "built-in look");
    }

    /* Re-run the boot-time scan. This is the one that reaches a skin dropped
     * on the disk after the player opened. */
    if (sk_eq(verb, "scan")) {
        int got = load_a_skin();
        pc64_shell_dirty();
        if (!got) {
            sk_put(out, cap, 0, "no loadable .wsz on any volume root");
            return -1;
        }
        return sk_status(out, cap);
    }

    if (sk_eq(verb, "load")) {
        int v;
        const char *path;
        if (!*a1) {
            sk_put(out, cap, 0, "usage: skin load <vol> <file.wsz>");
            return -1;
        }
        /* "load 0 X.WSZ" and the shorthand "load X.WSZ" (volume 0) */
        if (*a2) { v = sk_atoi(a1); path = a2; }
        else     { v = 0;           path = a1; }
        if (!apply_skin(v, path)) {
            pc64_shell_dirty();       /* it dropped to built-in: show that */
            n = sk_put(out, cap, 0, "refused (not a readable .wsz, or no "
                                    "MAIN.BMP in it): ");
            n = sk_puti(out, cap, n, v);
            n = sk_put(out, cap, n, ":");
            n = sk_put(out, cap, n, path);
            return -1;
        }
        pc64_shell_dirty();
        return sk_status(out, cap);
    }

    sk_put(out, cap, 0, "bad-cmd (status/list/load/scan/off)");
    return -1;
}

