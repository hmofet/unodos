/* ===========================================================================
 * UnoDOS/Dreamcast platform layer (KallistiOS) - video present + maple input.
 *
 * The portable core dreamcast/unodos.c owns main(); built with -DUNO_DC it
 * drives these three hooks instead of returning after one frame (host shim):
 *
 *   uno_dc_init()     - set the 640x480 RGB565 video mode; maple (controller,
 *                       keyboard, mouse, VMU) is brought up by KOS startup.
 *   uno_dc_poll()     - read the maple peripherals and translate to UnoDOS
 *                       events (edge-detected): the controller d-pad -> arrow
 *                       keys (desktop icon nav / in-app movement), A/Start ->
 *                       Return / Esc, the analog stick + a Dreamcast mouse ->
 *                       the pointer (with click), and a Dreamcast keyboard ->
 *                       typed characters. Posted into the shim's event queue,
 *                       so the core's normal GetNextEvent loop consumes them.
 *   uno_dc_present()  - convert the software framebuffer (fb.c, ARGB8888) to
 *                       RGB565 into the Dreamcast framebuffer (vram_s) once per
 *                       vblank, then overlay the arrow cursor. The PVR is left
 *                       idle - the DC framebuffer is the "blitter," the same
 *                       software-FB design the PS2 port uses (HANDOFF SS2), so
 *                       the whole desktop/WM/app suite the host shim renders
 *                       comes across unchanged; only present + input are DC.
 *
 * NOTE: this file needs KallistiOS (sh-elf-gcc + libkos) to build and a
 * Dreamcast / emulator (Flycast, lxdream, redream) to run - NEITHER is on the
 * dev machine that wrote it, so it is UNVERIFIED. fb.c / mac_compat.c / mac_io.c
 * / uno_splash.c / unodos.c (shared verbatim with the host shim) ARE verified
 * on the PC (build.sh host/desktop -> PNGs). Build with `./build.sh dc` once
 * KallistiOS is installed; first-run checklist in README.md.
 * ===========================================================================
 */
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/maple/mouse.h>
#include <dc/video.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <math.h>
#include <string.h>

#include "fb.h"
#include "mac_compat.h"

/* KOS init flags: default subsystems (IRQ, threads) + the standard maple
   peripheral drivers (controller, keyboard, mouse, VMU). No romdisk. */
KOS_INIT_FLAGS(INIT_DEFAULT);

/* ---- cursor state (analog stick / mouse drive a software pointer) ------- */
static int   g_cx = FB_W / 2, g_cy = FB_H / 2;
static int   g_have_pointer = 0;       /* a stick/mouse has moved the cursor */
static uint32 g_prev_cont = 0;         /* previous controller button word */
static int   g_prev_mb = 0;            /* previous mouse button mask */

/* ---- 565 helpers ------------------------------------------------------- */
static inline uint16 to565(fb_px p)
{
    unsigned r = (p) & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
    return (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ---- AICA square-wave synth (M3) --------------------------------------- *
 * One 8-bit signed square-wave period (SQ_PERIOD samples) is uploaded to the
 * AICA once; each voice loops it. Output pitch = playback rate / SQ_PERIOD, so
 * a note's frequency is set via the play-data freq field = hz * SQ_PERIOD. The
 * core replaces the note on a channel by re-playing (AICA restarts the voice);
 * quietCmd stops it. Mirrors the square-wave model the other ports use. */
#define SQ_PERIOD 32
static sfxhnd_t g_sq = 0;
static int      g_snd_ready = 0;

static void snd_bringup(void)
{
    static char wave[SQ_PERIOD];
    int i;
    if (g_snd_ready) return;
    snd_init();
    for (i = 0; i < SQ_PERIOD; i++) wave[i] = (i < SQ_PERIOD / 2) ? 100 : -100;
    /* 8-bit mono; the nominal rate is overridden per-note by the freq field */
    g_sq = snd_sfx_load_raw_buf(wave, SQ_PERIOD, 44100, 8, 1);
    g_snd_ready = 1;
}

static int midi_hz(int midi)
{
    /* equal temperament, A4 (MIDI 69) = 440 Hz */
    return (int)(440.0 * pow(2.0, (midi - 69) / 12.0) + 0.5);
}

void uno_dc_snd_note(int chan, int midi, int amp)
{
    sfx_play_data_t d;
    if (!g_snd_ready || !g_sq || midi <= 0) return;
    memset(&d, 0, sizeof d);
    d.chn = chan;                 /* fixed AICA channel per Sound Manager id */
    d.idx = g_sq;
    d.vol = (amp > 255) ? 255 : (amp < 0 ? 0 : amp);
    d.pan = 128;                  /* centre */
    d.loop = 1;
    d.freq = midi_hz(midi) * SQ_PERIOD;
    snd_sfx_play_ex(&d);
}

void uno_dc_snd_quiet(int chan)
{
    if (g_snd_ready) snd_sfx_stop(chan);
}

void uno_dc_init(void)
{
    /* 640x480, 16bpp RGB565. KOS auto-selects VGA vs NTSC/PAL by the cable. */
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_empty();
    snd_bringup();                /* AICA up for Music / Tracker / Dostris */
}

/* post a keyDown into the shim queue: message = (keycode<<8)|charcode, exactly
   the encoding the core's main loop decodes (ch = msg & 0xFF, code = msg>>8). */
static void post_key(short keycode, char ch)
{
    Point p = { 0, 0 };
    long msg = ((long)(keycode & 0xFF) << 8) | (unsigned char)ch;
    uno_post_event(keyDown, msg, p, 0);
}

/* ---- controller: d-pad -> arrows, A/Start -> Return/Esc ----------------- */
static void poll_controller(void)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st;
    uint32 now, edge;
    if (!dev) return;
    st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return;
    now = st->buttons;
    edge = now & ~g_prev_cont;             /* newly-pressed this frame */
    g_prev_cont = now;

    /* d-pad -> arrow keys (Mac keycode + classic arrow ASCII, as the EE port) */
    if (edge & CONT_DPAD_LEFT)  post_key(0x7B, 0x1C);
    if (edge & CONT_DPAD_RIGHT) post_key(0x7C, 0x1D);
    if (edge & CONT_DPAD_UP)    post_key(0x7E, 0x1E);
    if (edge & CONT_DPAD_DOWN)  post_key(0x7D, 0x1F);
    /* A / B -> Return (launch icon / confirm) */
    if (edge & CONT_A)     post_key(0x24, 0x0D);
    if (edge & CONT_B)     post_key(0x24, 0x0D);
    /* Start -> Esc (close focused window) */
    if (edge & CONT_START) post_key(0x35, 0x1B);

    /* left analog stick -> move the pointer (joyx/joyy are signed -128..127) */
    {
        int jx = st->joyx, jy = st->joyy;
        if (jx > 32 || jx < -32) { g_cx += jx / 24; g_have_pointer = 1; }
        if (jy > 32 || jy < -32) { g_cy += jy / 24; g_have_pointer = 1; }
        if (g_cx < 0) g_cx = 0; if (g_cx > FB_W - 1) g_cx = FB_W - 1;
        if (g_cy < 0) g_cy = 0; if (g_cy > FB_H - 1) g_cy = FB_H - 1;
        /* the trigger acts as the analog-pointer's click button */
        {
            int down = (st->rtrig > 64);
            static int prev_trig = 0;
            uno_set_mouse((short)g_cx, (short)g_cy, (Boolean)down);
            if (down != prev_trig) {
                Point p; p.h = (short)g_cx; p.v = (short)g_cy;
                uno_post_event(down ? mouseDown : mouseUp, 0, p, 0);
                prev_trig = down;
            }
        }
    }
}

/* ---- Dreamcast mouse: relative motion -> pointer, buttons -> clicks ------ */
static void poll_mouse(void)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_MOUSE);
    mouse_state_t *ms;
    int mb;
    if (!dev) return;
    ms = (mouse_state_t *)maple_dev_status(dev);
    if (!ms) return;

    if (ms->dx || ms->dy) {
        g_cx += ms->dx; g_cy += ms->dy; g_have_pointer = 1;
        if (g_cx < 0) g_cx = 0; if (g_cx > FB_W - 1) g_cx = FB_W - 1;
        if (g_cy < 0) g_cy = 0; if (g_cy > FB_H - 1) g_cy = FB_H - 1;
    }
    mb = (ms->buttons & MOUSE_LEFTBUTTON) ? 1 : 0;
    uno_set_mouse((short)g_cx, (short)g_cy, (Boolean)mb);
    if (mb != g_prev_mb) {
        Point p; p.h = (short)g_cx; p.v = (short)g_cy;
        uno_post_event(mb ? mouseDown : mouseUp, 0, p, 0);
        g_prev_mb = mb;
    }
}

/* ---- Dreamcast keyboard: typed characters ------------------------------ */
static void poll_keyboard(void)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    int k;
    if (!dev) return;
    /* drain the cooked key queue (xlat=1 => ASCII for printable keys). Arrow
       keys are driven by the controller d-pad; this path covers text entry
       (Notepad) plus Enter / Esc / Backspace / Tab. */
    while ((k = kbd_queue_pop(dev, 1)) > 0) {
        if (k == 27)                 post_key(0x35, 0x1B);          /* Esc   */
        else if (k == 13 || k == 10) post_key(0x24, 0x0D);          /* Enter */
        else if (k == 8)             post_key(0x33, 0x08);          /* Bksp  */
        else if (k == 9)             post_key(0x30, 0x09);          /* Tab   */
        else if (k >= 32 && k < 127) post_key(0, (char)k);          /* printable */
    }
}

void uno_dc_poll(void)
{
    poll_controller();
    poll_mouse();
    poll_keyboard();
}

/* ---- arrow cursor drawn directly into the 565 framebuffer (overlay) ----- */
static const char *kCursor[] = {
    "B","BB","BWB","BWWB","BWWWB","BWWWWB","BWWWWWB","BWWWWWWB",
    "BWWWWBBBB","BWWBWB","BWB BWB","BB  BWB","B    BWB","      BWB","       BB", 0
};
static void overlay_cursor(uint16 *vram)
{
    int r, c;
    if (!g_have_pointer) return;       /* keyboard/d-pad-only session: no pointer */
    for (r = 0; kCursor[r]; r++) {
        const char *row = kCursor[r];
        int yy = g_cy + r;
        if (yy < 0 || yy >= FB_H) continue;
        for (c = 0; row[c]; c++) {
            int xx = g_cx + c;
            if (xx < 0 || xx >= FB_W) continue;
            if (row[c] == 'B')      vram[yy * FB_W + xx] = 0x0000;   /* black */
            else if (row[c] == 'W') vram[yy * FB_W + xx] = 0xFFFF;   /* white */
        }
    }
}

void uno_dc_present(void)
{
    uint16 *vram = (uint16 *)vram_s;
    int i, n = FB_W * FB_H;
    vid_waitvbl();                      /* present at vblank to limit tearing */
    for (i = 0; i < n; i++) vram[i] = to565(fb[i]);
    overlay_cursor(vram);
}
