/* cosmo64/musprobe.c -- MUSPROBE.UNO: the audio decoders, exercised from a
 * running UnoDOS with no reflash and no hands.
 *
 * WHY. The Music slice links unomedia's WAV / MIDI / MP3 / AAC decoders into
 * this kernel for the first time on aarch64, under -mstrict-align, LLP64 and
 * clang -- three things x86's gate never asked of them. The Music app and
 * UnoAmp exercise them only through a person clicking a list, which the
 * QEMU gate cannot do and the phone should not need. This module plays every
 * audio file it finds on a volume root through the kernel's own score player
 * (uno_snd_mus_play: snd_mus.c over the same unomedia instance the Music app
 * uses) and reports, per file, whether the decoder opened it and how long it
 * played before it stopped ITSELF. On the QEMU gate afe.c's stand-in ring
 * paces the stream at 48 kHz, so a three-second file plays for three seconds
 * and a decoder that crashes, refuses, or spins is a FAIL line, not a
 * question. On the phone the same run is audible.
 *
 * WHAT A LINE MEANS.
 *   musprobe: PASS NAME  played N ms      the decoder opened it and ran to
 *                                          its end; N is wall time
 *   musprobe: FAIL NAME  refused           uno_snd_mus_play said no: no
 *                                          decoder claimed it, or no DAC, or
 *                                          the ring was busy (Music open?)
 *   musprobe: FAIL NAME  stopped in N ms   ended suspiciously early -- an
 *                                          open that decodes nothing
 *   musprobe: FAIL NAME  still playing after N ms   spinning; stopped by force
 *   musprobe: done P pass F fail           the harness's line to wait for
 *
 * THE NAME HINT. snd_mus.c opens every buffer as "score.mid", so the MIDI
 * decoder claims each file first by extension and its open() fails on the
 * bytes; unomedia then falls through to the decoder whose magic matches.
 * That is the documented probe order (um_audio.c), not luck, and WAV, MP3
 * (ID3 or frame sync) and M4A (ftyp) all carry magic. A raw ADTS .AAC does
 * too. Nothing here reads a file bigger than snd_mus.c's 1 MB limit.
 *
 * Files are found on every volume's root, by extension. No subdirectories:
 * uno_fs_list_* is root-only, and the gate pushes to the RAM disk's root.
 */
#include "uno_uuiapp.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "pc64_icons.h"     /* pc64_shell_theme */
#include "pc64_fs.h"
#include "uno_appdesc.h"
#include "unoauto.h"
#include "unolog.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pc64_shell_dirty(void);
int  uno_snd_active(void);
int  uno_snd_mus_play(const unsigned char *smf, long len, int loop);
void uno_snd_mus_stop(void);
int  uno_snd_mus_playing(void);

typedef unsigned long long u64;
static u64 cnt(void)  { u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static u64 freq(void) { u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v ? v : 13000000ull; }
static u64 ms_since(u64 t0) { return (cnt() - t0) * 1000ull / freq(); }

/* ---- output: a line buffer for the window, mirrored to the URC log -------- */
#define NLINES 40
#define LINEW  100
static char g_lines[NLINES][LINEW];
static int  g_n;

static void say(const char *fmt, ...)
{
    va_list ap;
    char *l = g_lines[g_n < NLINES ? g_n : NLINES - 1];
    va_start(ap, fmt);
    vsnprintf(l, LINEW, fmt, ap);
    va_end(ap);
    if (g_n < NLINES) g_n++;
    unoauto_log(UA_CH_SCRIPT, "musprobe: %s", l);
    unolog(LOG_NOTICE, 0, "musprobe: %s", l);
    pc64_shell_dirty();
}

/* ---- the queue ------------------------------------------------------------ */
#define MAXQ     24
#define MAX_FILE (1024L * 1024L)     /* snd_mus.c's own MUS_MAX               */
#define MIN_MS   400                 /* shorter than this and it decoded nothing */
#define CAP_MS   90000               /* longer than this and it is not stopping */

static struct { int vol; char name[32]; } g_q[MAXQ];
static int g_nq, g_i;
static int g_state;                  /* 0 idle, 1 playing, 2 done              */
static u64 g_t0;
static unsigned char *g_buf;
static int g_pass, g_fail;

static int is_audio_name(const char *n)
{
    static const char *const k[] = { ".WAV", ".MID", ".MP3", ".M4A", ".AAC", ".RMI", 0 };
    int len = 0, i;
    while (n[len]) len++;
    for (i = 0; k[i]; i++) {
        int kl = 4, j;
        if (len <= kl) continue;
        for (j = 0; j < kl; j++) {
            char c = n[len - kl + j];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c != k[i][j]) break;
        }
        if (j == kl) return 1;
    }
    return 0;
}

static void scan(void)
{
    int v, nv = uno_fs_volumes();
    g_nq = 0;
    for (v = 0; v < nv && g_nq < MAXQ; v++) {
        int n = uno_fs_list_begin(v), i;
        for (i = 0; i < n && g_nq < MAXQ; i++) {
            char nm[32];
            if (!uno_fs_list_get(v, i, nm, (int)sizeof nm)) continue;
            if (!is_audio_name(nm)) continue;
            g_q[g_nq].vol = v;
            strncpy(g_q[g_nq].name, nm, sizeof g_q[g_nq].name - 1);
            g_q[g_nq].name[sizeof g_q[g_nq].name - 1] = 0;
            g_nq++;
        }
    }
}

static void finish(void)
{
    g_state = 2;
    free(g_buf); g_buf = 0;
    say("done %d pass %d fail", g_pass, g_fail);
}

/* start the next queued file, or finish */
static void next(void)
{
    while (g_i < g_nq) {
        int vol = g_q[g_i].vol;
        const char *nm = g_q[g_i].name;
        long size = uno_fs_size(vol, nm), got;
        g_i++;
        free(g_buf); g_buf = 0;
        if (size <= 0 || size > MAX_FILE) {
            say("FAIL %s  %ld bytes: not readable, or over the 1 MB score limit", nm, size);
            g_fail++; continue;
        }
        g_buf = (unsigned char *)malloc((size_t)size);
        if (!g_buf) { say("FAIL %s  no memory for %ld bytes", nm, size); g_fail++; continue; }
        got = uno_fs_read(vol, nm, g_buf, size);
        if (got != size) {
            say("FAIL %s  read %ld of %ld bytes", nm, got, size);
            g_fail++; continue;
        }
        if (!uno_snd_mus_play(g_buf, size, 0)) {
            say("FAIL %s  refused (%ld bytes): no decoder claimed it, no DAC, or the ring is busy",
                nm, size);
            g_fail++; continue;
        }
        g_t0 = cnt();
        g_state = 1;
        say("playing %s (vol %d, %ld bytes)", nm, vol, size);
        return;
    }
    finish();
}

static void frame(void)
{
    u64 el;
    if (g_state != 1) return;
    el = ms_since(g_t0);
    if (uno_snd_mus_playing()) {
        if (el < CAP_MS) return;
        uno_snd_mus_stop();
        say("FAIL %s  still playing after %llu ms -- stopped by force",
            g_q[g_i - 1].name, (unsigned long long)el);
        g_fail++;
    } else if (el < MIN_MS) {
        say("FAIL %s  stopped in %llu ms -- opened, decoded nothing",
            g_q[g_i - 1].name, (unsigned long long)el);
        g_fail++;
    } else {
        say("PASS %s  played %llu ms", g_q[g_i - 1].name, (unsigned long long)el);
        g_pass++;
    }
    g_state = 0;
    next();
}

/* ---- the window ---------------------------------------------------------- */
static void draw(unoui_widget *w, unoui_rect c, void *ctx)
{
    const unoui_palette *p = &pc64_shell_theme()->pal;
    int i, y, h = uno_font_height_px(0, 12) + 2;
    (void)w; (void)ctx;
    fb_fill_rect(c.x, c.y, c.w, c.h, p->win_bg);
    y = c.y + 4;
    for (i = 0; i < g_n && y + h <= c.y + c.h; i++, y += h)
        fb_text(c.x + 6, y, g_lines[i], p->text, -1);
}

static unoui_canvas g_canvas = { draw, 0, 0 };

static void build(unoui_window *win)
{
    const unoui_metrics *m = &pc64_shell_theme()->m;
    int aw = fb_width() - 80, ah = fb_height() - 90;
    if (aw > 900) aw = 900;
    if (ah > 500) ah = 500;
    unoui_window_init(win, "Decoder probe", 30, 24,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      ah + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(win, 0, 0, aw, ah, &g_canvas);
}

static void opened(void)
{
    g_n = 0; g_i = 0; g_pass = g_fail = 0; g_state = 0;
    if (!uno_snd_active()) {
        say("no PCM device -- uno_snd_active() is 0, nothing to decode into");
        finish();
        return;
    }
    scan();
    say("%d audio file%s on the volume roots", g_nq, g_nq == 1 ? "" : "s");
    if (!g_nq) { finish(); return; }
    next();
}

static void closed(void)
{
    if (g_state == 1) { uno_snd_mus_stop(); g_state = 0; }
    free(g_buf); g_buf = 0;
}

static int canvas_index(void) { return 0; }

UNO_APP_DESC("id: musprobe\n"
             "name: Decoder probe\n"
             "short: Decode\n"
             "icon: sys\n"
             "cat: system\n"
             "rank: 99\n");

static const UnoUuiApp kApp = {
    UNO_UUIAPP_ABI, "Decoder probe", build, 0, 0, frame, opened, closed, canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved) { (void)reserved; return &kApp; }
