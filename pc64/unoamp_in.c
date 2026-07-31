/* UnoAmp input plugins: decoders behind Winamp's In_Module.
 *
 * Phase 2 of docs/PLAYER-WINAMP-PLAN.md. One built-in ships, wrapping
 * unomedia's um_audio_* surface, and it is registered exactly the way a
 * dropped-in IN_*.UNO will be.
 *
 * WHAT CHANGES FOR THE PLAYER. Format dispatch stops being "call
 * pc64_media_open and see if it works" and becomes the Winamp contract: ask
 * each input whether the file is ITS file, most specific answer first. That
 * matters the moment there is more than one decoder - MOD and VGM in phase 8
 * both sniff by content, and a hardcoded probe order would have to be edited
 * to add them. This one does not.
 *
 * CONTROL IS INVERTED from Winamp on purpose. There, an input plugin runs its
 * own thread and pushes into outMod; pc64's shell is a cooperative frame loop
 * with no threads, so the host pulls via Decode(). Same graph, and it is why a
 * slow decode costs latency rather than stalling the desktop.
 */
#include "unoamp.h"
#include "pc64_media.h"
#include "unomedia.h"
#include <string.h>

/* ---- registry -------------------------------------------------------------- */
#define UNOAMP_IN_MAX 8
static const unoamp_in *g_in[UNOAMP_IN_MAX];
static int g_nin;
static const unoamp_in *g_playing;

int unoamp_register_in(const unoamp_in *in)
{
    int i;
    if (!in || in->version != UNOAMP_ABI || !in->Play || !in->Decode) return 0;
    for (i = 0; i < g_nin; i++) if (g_in[i] == in) return 0;
    if (g_nin >= UNOAMP_IN_MAX) return 0;
    g_in[g_nin++] = in;
    return 1;
}
int unoamp_in_count(void) { return g_nin; }
const unoamp_in *unoamp_in_at(int i)
{ return (i >= 0 && i < g_nin) ? g_in[i] : 0; }
const unoamp_in *unoamp_playing(void) { return g_playing; }

/* Winamp's extension list is NUL-separated pairs, double-NUL terminated:
 * "MP3\0MPEG Audio\0WAV\0WAV Audio\0\0". Match the file's suffix against the
 * even-numbered entries, case-insensitively. */
static int ext_matches(const char *list, const char *fn)
{
    const char *dot = 0, *p;
    for (p = fn; *p; p++) if (*p == '.') dot = p + 1;
    if (!dot || !list) return 0;
    while (*list) {
        const char *e = list, *f = dot;
        while (*e && *f) {
            char a = *e, b = *f;
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
            e++; f++;
        }
        if (!*e && !*f) return 1;
        while (*list) list++;
        list++;                                /* skip the extension          */
        while (*list) list++;
        list++;                                /* skip its description        */
    }
    return 0;
}

/* Pick a decoder for `fn`. CONTENT SNIFF BEATS EXTENSION, because a file named
 * .DAT that is really a WAV should play and a .MP3 that is really an AAC
 * should not be handed to the MP3 decoder. Only when nobody claims it by
 * content does the extension list get a say. */
const unoamp_in *unoamp_find_in(const char *fn)
{
    int i;
    for (i = 0; i < g_nin; i++)
        if (g_in[i]->IsOurFile && g_in[i]->IsOurFile(fn)) return g_in[i];
    for (i = 0; i < g_nin; i++)
        if (ext_matches(g_in[i]->FileExtensions, fn)) return g_in[i];
    return 0;
}

/* ===========================================================================
 * IN_UNOMEDIA - WAV / MIDI / MP3 / AAC, through unomedia.
 *
 * unomedia already owns the probe (um_audio_is) and the decode loop, and it is
 * shared with the other ports, so this is a shim onto it. The one thing worth
 * noting: um_audio_* is a SINGLETON - one open stream at a time - which is why
 * Play() stops whatever was playing first rather than trying to be reentrant.
 * A second unomedia-backed input would be the same singleton, so there is
 * exactly one of these on purpose.
 * ======================================================================== */
static um_audio_info g_info;
static int  g_vol;                      /* fs volume of the open file         */
static int  g_open, g_paused;
static char g_path[136];

static int um_isours(const char *fn)
{
    /* Content sniffing needs the file open, which is the expensive thing we
     * are trying to avoid doing twice. um_audio_is() answers from the NAME
     * only, so this stays a cheap pre-filter and the real answer comes from
     * Play() failing - which the host reports with um_error(). */
    return um_audio_is(fn);
}

static int um_play(const char *fn)
{
    if (g_open) { um_audio_close(); g_open = 0; }
    strncpy(g_path, fn, sizeof g_path - 1);
    g_path[sizeof g_path - 1] = 0;
    if (!pc64_media_open(g_vol, g_path, &g_info)) return -1;
    g_open = 1; g_paused = 0;
    return 0;
}
static void um_stop(void) { if (g_open) { um_audio_close(); g_open = 0; } }
static void um_pause_(void)   { g_paused = 1; }
static void um_unpause(void)  { g_paused = 0; }
static int  um_ispaused(void) { return g_paused; }
static int  um_getlen(void)   { return g_open ? (int)g_info.duration_ms : -1; }
static int  um_outtime(void)  { return g_open ? (int)um_audio_pos_ms() : 0; }
static void um_setouttime(int ms) { if (g_open) um_audio_seek_ms(ms); }
static void um_setvol(int v)  { (void)v; }   /* the sink owns volume          */
static void um_setpan(int p)  { (void)p; }

static void um_fileinfo(const char *file, char *title, int cap, int *len_ms)
{
    (void)file;
    if (title && cap > 0) {
        if (g_open && g_info.title[0]) { strncpy(title, g_info.title, (unsigned)cap - 1);
                                         title[cap - 1] = 0; }
        else title[0] = 0;
    }
    if (len_ms) *len_ms = g_open ? (int)g_info.duration_ms : -1;
}

static int um_decode(short *out, int max_frames)
{
    if (!g_open || g_paused) return 0;
    return um_audio_decode(out, max_frames);
}

static const unoamp_in g_in_unomedia = {
    UNOAMP_ABI, "unomedia (WAV / MIDI / MP3 / AAC)", 0, 0,
    "WAV\0WAV Audio\0MID\0MIDI\0MIDI\0MIDI\0MP3\0MPEG Audio\0"
    "AAC\0AAC Audio\0M4A\0MPEG-4 Audio\0\0",
    1,                                  /* seekable                           */
    UNOAMP_CAP_PCM,                     /* needs a sink that carries samples  */
    0, 0, 0, 0,
    um_fileinfo, um_isours, um_play, um_pause_, um_unpause, um_ispaused,
    um_stop, um_getlen, um_outtime, um_setouttime, um_setvol, um_setpan,
    um_decode, 0
};

/* The fs volume is per-open state the Winamp ABI has no field for (its Play
 * takes a path and nothing else). The host sets it before Play. */
void unoamp_in_set_volume_index(int vol) { g_vol = vol; }

/* What the open stream is, for the host's status line. */
const um_audio_info *unoamp_in_info(void) { return g_open ? &g_info : 0; }

void unoamp_in_init(void)
{
    unoamp_register_in(&g_in_unomedia);
}

/* ---- host-side play, the bit that makes the graph a graph ------------------
 * Find a decoder, check the SINK can carry what it produces, then open both.
 * The caps check is the honest-refusal rule from phase 1 applied at the one
 * point where both halves are known. */
int unoamp_play(int vol, const char *fn, const char **why)
{
    const unoamp_in *in = unoamp_find_in(fn);
    const unoamp_out *out = unoamp_current_out();
    if (why) *why = "";
    if (!in)  { if (why) *why = "no decoder for this file"; return 0; }
    if (!out) { if (why) *why = "no audio hardware found";  return 0; }
    if ((in->needs_caps & out->caps) != in->needs_caps) {
        if (why) *why = "the selected output cannot carry this format";
        return 0;
    }
    unoamp_in_set_volume_index(vol);
    if (in->Play(fn) < 0) { if (why) *why = um_error(); return 0; }
    g_playing = in;
    return 1;
}

void unoamp_stop(void)
{
    if (g_playing && g_playing->Stop) g_playing->Stop();
    g_playing = 0;
}
