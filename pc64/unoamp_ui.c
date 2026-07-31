/* UnoAmp: the Winamp 2 main window, equaliser and playlist.
 *
 * Phase 4 of docs/PLAYER-WINAMP-PLAN.md.
 *
 * Winamp 2's window is not a layout - it is a 275x116 photograph. Every
 * control sits at a fixed pixel offset and every skin in existence is drawn
 * against those offsets, so the coordinates below are not arbitrary choices we
 * could tidy up later: they ARE the skin format. Change one and every skin
 * renders wrong. That is why this file is a table of constants and a blitter
 * rather than a widget tree.
 *
 * HOSTED AS ONE CANVAS. unoui is a layout toolkit; asking it to place a
 * skinned sprite is asking the wrong question. So the window is UI_WIN_BARE
 * with a single canvas covering it, and everything - dragging the titlebar,
 * hit-testing the transport, the sliders - happens inside the canvas event
 * handler. unoui already forwards drag moves to a canvas that has the button
 * held, which is the one thing this needs from it.
 *
 * DEGRADES WITHOUT A SKIN. Every sprite goes through spr() / spr_or(), which
 * falls back to a theme-coloured rectangle when the sheet is missing. A
 * machine with no .wsz installed gets the same window in the desktop's own
 * palette rather than a black hole. That is also what makes a partial skin
 * safe: a skin with no BALANCE.BMP loses the balance sprite, not the player.
 *
 * SCALED BY AN INTEGER. 275x116 was a 640x480 window; on a 1920x1080 panel it
 * is a postage stamp. Winamp's own answer was "double size", and an integer
 * factor is the only kind of scaling that keeps a pixel-art skin crisp, so
 * that is what this does - nearest-neighbour, factor chosen from the panel.
 */
#include "unoamp.h"
#include "unoamp_skin.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_icons.h"     /* pc64_shell_theme */
void pc64_shell_dirty(void);
#include <string.h>

/* ---- the skin's sprite atlas ----------------------------------------------
 * Sheet, source x/y, width, height. Named after the skin spec, not after what
 * we happen to call the control. */
typedef struct { short sheet, sx, sy, w, h; } spr_t;

/* CBUTTONS.BMP - the transport row. Pressed frames are the same rects at
 * sy + 18 (the eject button is 16 tall and its pressed frame is at 16). */
static const spr_t kBtn[6] = {
    { UNOAMP_SHEET_CBUTTONS,   0, 0, 23, 18 },   /* previous                 */
    { UNOAMP_SHEET_CBUTTONS,  23, 0, 23, 18 },   /* play                     */
    { UNOAMP_SHEET_CBUTTONS,  46, 0, 23, 18 },   /* pause                    */
    { UNOAMP_SHEET_CBUTTONS,  69, 0, 23, 18 },   /* stop                     */
    { UNOAMP_SHEET_CBUTTONS,  92, 0, 22, 18 },   /* next                     */
    { UNOAMP_SHEET_CBUTTONS, 114, 0, 22, 16 }    /* eject                    */
};
/* Where they sit in the 275x116 window. */
static const short kBtnX[6] = { 16, 39, 62, 85, 108, 136 };
#define BTN_Y   88
#define EJECT_Y 89

/* Fixed geometry of the main window. */
#define WIN_W 275
#define WIN_H 116
#define SHADE_H 14                    /* windowshade mode is the titlebar     */
#define TITLE_H 14

#define TIME_X   36                   /* the four time digits                 */
#define TIME_Y   26
#define TEXT_X  111                   /* the scrolling track title            */
#define TEXT_Y   27
#define TEXT_W  153
#define VIS_X    24                   /* the visualiser (phase 5 draws here)  */
#define VIS_Y    43
#define VIS_W    76
#define VIS_H    16
#define VOL_X   107
#define VOL_Y    57
#define VOL_W    68
#define BAL_X   177
#define BAL_Y    57
#define BAL_W    38
#define POS_X    16
#define POS_Y    72
#define POS_W   248
#define POS_H    10
#define EQ_X    219
#define PL_X    242
#define EQPL_Y   58
#define SHUF_X  164
#define REP_X   210
#define SHUF_Y   89

enum { HIT_NONE = 0, HIT_TITLE, HIT_BTN, HIT_POS, HIT_VOL, HIT_BAL,
       HIT_EQ, HIT_PL, HIT_SHUF, HIT_REP, HIT_CLOSE, HIT_SHADE, HIT_VIS };

/* ---- state ---------------------------------------------------------------- */
static unoui_window *g_win;
static int  g_scale = 2;
static int  g_shade;                  /* windowshade mode                     */
static int  g_pressed = -1;           /* transport button being held          */
static int  g_hit;                    /* what the current drag grabbed        */
static int  g_drag_dx, g_drag_dy;
static int  g_vol = 100, g_bal = 0;   /* 0..100, -100..100                    */
static int  g_shuffle, g_repeat;
static int  g_eq_open, g_pl_open;
static int  g_scroll;                 /* title scroll offset, in characters   */
static char g_title[192] = "UnoAmp";

/* Where the window's client origin is on screen, in framebuffer pixels. */
static void origin(int *ox, int *oy)
{
    *ox = g_win ? g_win->r.x : 0;
    *oy = g_win ? g_win->r.y : 0;
}

/* ---- blitting ------------------------------------------------------------- */

/* One sprite, scaled by g_scale, clipped by fb_pixel. Nearest-neighbour on
 * purpose: a pixel-art skin bilinearly filtered is a smear. */
static int spr(const spr_t *s, int dx, int dy)
{
    const unoamp_skin *sk = unoamp_skin_get();
    const unoamp_sheet *sh;
    int px, py, sx, sy;
    int ox, oy;
    if (!sk) return 0;
    sh = &sk->sheet[s->sheet];
    if (!sh->px) return 0;
    origin(&ox, &oy);
    for (py = 0; py < s->h; py++) {
        sy = s->sy + py;
        if (sy < 0 || sy >= sh->h) continue;
        for (px = 0; px < s->w; px++) {
            unsigned c;
            int i, j;
            sx = s->sx + px;
            if (sx < 0 || sx >= sh->w) continue;
            c = sh->px[(long)sy * sh->w + sx];
            /* Scale by replication. The inner loops are tiny (2x2 or 3x3) and
             * this runs once per dirty frame, not per sample. */
            for (j = 0; j < g_scale; j++)
                for (i = 0; i < g_scale; i++)
                    fb_pixel(ox + (dx + px) * g_scale + i,
                             oy + (dy + py) * g_scale + j, c);
        }
    }
    return 1;
}

/* A sprite, or a flat rectangle in the theme's palette when the sheet is
 * missing. This is the whole no-skin fallback: the window keeps its shape and
 * its controls stay where a Winamp user expects them. */
static void spr_or(const spr_t *s, int dx, int dy, unsigned fallback)
{
    int ox, oy;
    if (spr(s, dx, dy)) return;
    origin(&ox, &oy);
    fb_fill_rect(ox + dx * g_scale, oy + dy * g_scale,
                 s->w * g_scale, s->h * g_scale, fallback);
}

static void rect_fill(int dx, int dy, int w, int h, unsigned c)
{
    int ox, oy;
    origin(&ox, &oy);
    fb_fill_rect(ox + dx * g_scale, oy + dy * g_scale,
                 w * g_scale, h * g_scale, c);
}

/* ---- the bitmap font ------------------------------------------------------
 * TEXT.BMP is a 3-row grid of 5x6 cells. The character order is fixed by the
 * skin format; a skin author draws INTO this layout, so the string below is
 * effectively part of the file format. Space is the last cell of row 0. */
static const char *kFontRow[3] = {
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ\"@ ",
    "0123456789\x01:()-'!_+\\/[]^&%.=$#",
    "\x02\x03\x04?* "
};
#define CH_W 5
#define CH_H 6

static int font_cell(char c, int *cx, int *cy)
{
    int r, i;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (r = 0; r < 3; r++)
        for (i = 0; kFontRow[r][i]; i++)
            if (kFontRow[r][i] == c) { *cx = i * CH_W; *cy = r * CH_H; return 1; }
    return 0;
}

/* Draw `n` characters of `s` at (dx,dy). Anything the skin font has no cell
 * for becomes a space rather than a hole. */
static void font_text(const char *s, int dx, int dy, int n)
{
    const unoamp_skin *sk = unoamp_skin_get();
    int i, cx, cy;
    for (i = 0; i < n && s[i]; i++) {
        spr_t g;
        if (!font_cell(s[i], &cx, &cy)) { cx = 28 * CH_W; cy = 0; }  /* space */
        g.sheet = UNOAMP_SHEET_TEXT; g.sx = (short)cx; g.sy = (short)cy;
        g.w = CH_W; g.h = CH_H;
        if (!sk || !spr(&g, dx + i * CH_W, dy)) {
            /* No TEXT.BMP: fall back to the shell's own font, once for the
             * whole run rather than per glyph. */
            int ox, oy;
            origin(&ox, &oy);
            fb_text(ox + dx * g_scale, oy + dy * g_scale, s,
                    pc64_shell_theme()->pal.text, -1);
            return;
        }
    }
}

/* NUMBERS.BMP: ten 9x13 digits. NUMS_EX skins add a blank in cell 10, which is
 * what a leading zero should use when the track is under ten minutes; without
 * it we draw a real zero, exactly as Winamp did on old skins. */
static void font_digit(int d, int dx, int dy)
{
    spr_t g;
    g.sheet = UNOAMP_SHEET_NUMBERS; g.sy = 0; g.w = 9; g.h = 13;
    g.sx = (short)((d < 0 ? 0 : d) * 9);
    if (!spr(&g, dx, dy)) {
        /* No digit sheet: the shell font, right-sized by the scale factor. */
        char t[2]; int ox, oy;
        t[0] = (char)(d < 0 ? ' ' : '0' + d); t[1] = 0;
        origin(&ox, &oy);
        fb_text(ox + dx * g_scale, oy + dy * g_scale, t,
                pc64_shell_theme()->pal.text, -1);
    }
}

static void draw_time(int ms)
{
    int sec = ms / 1000, mn = sec / 60, sc = sec % 60;
    if (mn > 99) mn = 99;
    font_digit(mn / 10 ? mn / 10 : -1, TIME_X + 12, TIME_Y);
    font_digit(mn % 10,                TIME_X + 24, TIME_Y);
    font_digit(sc / 10,                TIME_X + 42, TIME_Y);
    font_digit(sc % 10,                TIME_X + 54, TIME_Y);
}

/* ---- the window ----------------------------------------------------------- */
static void draw_main(void)
{
    const unoui_theme *t = pc64_shell_theme();
    const unoamp_in *in = unoamp_playing();
    spr_t s;
    int i, playing, pos_ms = 0, len_ms = -1, frac;

    /* Background first: MAIN.BMP is the whole 275x116, so everything after it
     * is drawn ON TOP. That ordering is the format's, not a choice. */
    s.sheet = UNOAMP_SHEET_MAIN; s.sx = 0; s.sy = 0; s.w = WIN_W; s.h = WIN_H;
    spr_or(&s, 0, 0, t->pal.win_bg);

    /* Titlebar. The active/inactive pair are two strips of the same sheet. */
    s.sheet = UNOAMP_SHEET_TITLEBAR; s.sx = 27; s.w = WIN_W; s.h = TITLE_H;
    s.sy = (short)((g_win && g_win->active) ? 0 : 15);
    spr_or(&s, 0, 0, t->pal.title_bg);
    if (g_shade) return;               /* windowshade: the titlebar IS the UI */

    playing = in && in->IsPaused && !in->IsPaused() ? 1 : (in ? 2 : 0);
    if (in) {
        if (in->GetOutputTime) pos_ms = in->GetOutputTime();
        if (in->GetLength)     len_ms = in->GetLength();
    }

    /* Play / pause / stop indicator. */
    s.sheet = UNOAMP_SHEET_PLAYPAUS; s.sy = 0; s.w = 9; s.h = 9;
    s.sx = (short)(playing == 1 ? 0 : playing == 2 ? 9 : 18);
    spr_or(&s, 24, 28, t->pal.accent);

    draw_time(pos_ms);

    /* Track title. Winamp scrolls it when it overruns the display; the offset
     * advances on the frame tick, so a long title reads rather than truncates. */
    {
        int n = (int)strlen(g_title), vis = TEXT_W / CH_W;
        if (n <= vis) font_text(g_title, TEXT_X, TEXT_Y, vis);
        else {
            int off = g_scroll % (n + 3);
            char buf[64];
            int k;
            for (k = 0; k < vis && k < (int)sizeof buf - 1; k++) {
                int j = off + k;
                buf[k] = (j < n) ? g_title[j] : (j < n + 3 ? ' ' : g_title[j - n - 3]);
            }
            buf[k] = 0;
            font_text(buf, TEXT_X, TEXT_Y, vis);
        }
    }

    /* Volume and balance. Both sheets are a vertical stack of 28 frames, one
     * per position, so the "slider" is a sprite lookup rather than a drawn
     * widget - the skin decides what a half-full volume slider looks like. */
    s.sheet = UNOAMP_SHEET_VOLUME; s.sx = 0; s.w = VOL_W; s.h = 13;
    s.sy = (short)((g_vol * 27 / 100) * 15);
    spr_or(&s, VOL_X, VOL_Y, t->pal.field_bg);
    s.sheet = UNOAMP_SHEET_BALANCE; s.w = BAL_W;
    { int a = g_bal < 0 ? -g_bal : g_bal;
      s.sy = (short)((a * 27 / 100) * 15); }
    spr_or(&s, BAL_X, BAL_Y, t->pal.field_bg);

    /* Position bar: background strip plus a thumb, which is only drawn when
     * there is a length to be a fraction of. A stream has no position, and
     * showing a thumb parked at zero would be a lie about that. */
    s.sheet = UNOAMP_SHEET_POSBAR; s.sx = 0; s.sy = 0; s.w = POS_W; s.h = POS_H;
    spr_or(&s, POS_X, POS_Y, t->pal.field_bg);
    if (len_ms > 0) {
        frac = (int)((long)pos_ms * (POS_W - 29) / len_ms);
        if (frac < 0) frac = 0;
        if (frac > POS_W - 29) frac = POS_W - 29;
        s.sx = (short)(g_hit == HIT_POS ? 278 : 248); s.w = 29;
        spr_or(&s, POS_X + frac, POS_Y, t->pal.accent);
    }

    /* Transport. The held button shows its pressed frame. */
    for (i = 0; i < 6; i++) {
        s = kBtn[i];
        if (g_pressed == i) s.sy = (short)(i == 5 ? 16 : 18);
        spr_or(&s, kBtnX[i], i == 5 ? EJECT_Y : BTN_Y, t->pal.face);
    }

    /* Shuffle and repeat, each a four-frame sprite (off / off-pressed / on /
     * on-pressed). */
    s.sheet = UNOAMP_SHEET_SHUFREP; s.sx = 28; s.w = 47; s.h = 15;
    s.sy = (short)(g_shuffle ? 30 : 0);
    spr_or(&s, SHUF_X, SHUF_Y, g_shuffle ? t->pal.accent : t->pal.face);
    s.sx = 0; s.w = 28;
    s.sy = (short)(g_repeat ? 30 : 0);
    spr_or(&s, REP_X, SHUF_Y, g_repeat ? t->pal.accent : t->pal.face);

    /* EQ and PL toggles live on the same sheet, below the shuffle block. */
    s.w = 23; s.h = 12;
    s.sx = 0;  s.sy = (short)(g_eq_open ? 73 : 61);
    spr_or(&s, EQ_X, EQPL_Y, g_eq_open ? t->pal.accent : t->pal.face);
    s.sx = 23; s.sy = (short)(g_pl_open ? 73 : 61);
    spr_or(&s, PL_X, EQPL_Y, g_pl_open ? t->pal.accent : t->pal.face);

    /* The visualiser well. Phase 5 draws into it; until then it is cleared to
     * the skin's own background colour so it does not show stale pixels. */
    {
        const unoamp_skin *sk = unoamp_skin_get();
        unsigned bg = (sk && sk->have_viscolor) ? sk->viscolor[0] : t->pal.field_bg;
        int ox, oy;
        rect_fill(VIS_X, VIS_Y, VIS_W, VIS_H, bg);
        origin(&ox, &oy);
        unoamp_vis_draw(ox + VIS_X * g_scale, oy + VIS_Y * g_scale,
                        VIS_W, VIS_H, g_scale);
    }
}

static void ui_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{ (void)w; (void)r; (void)ctx; draw_main(); }

/* ---- hit testing ----------------------------------------------------------
 * Straight from the same table the drawing uses, so a control can never be
 * drawn in one place and clicked in another. */
static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{ return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }

static int hit_test(int x, int y, int *which)
{
    int i;
    *which = -1;
    if (y < TITLE_H) {
        /* The titlebar's own buttons sit at its right edge. */
        if (in_rect(x, y, WIN_W - 11, 3, 9, 9)) return HIT_CLOSE;
        if (in_rect(x, y, WIN_W - 21, 3, 9, 9)) return HIT_SHADE;
        return HIT_TITLE;
    }
    if (g_shade) return HIT_NONE;
    for (i = 0; i < 6; i++)
        if (in_rect(x, y, kBtnX[i], i == 5 ? EJECT_Y : BTN_Y,
                    kBtn[i].w, kBtn[i].h)) { *which = i; return HIT_BTN; }
    if (in_rect(x, y, POS_X, POS_Y, POS_W, POS_H))   return HIT_POS;
    if (in_rect(x, y, VOL_X, VOL_Y, VOL_W, 13))      return HIT_VOL;
    if (in_rect(x, y, BAL_X, BAL_Y, BAL_W, 13))      return HIT_BAL;
    if (in_rect(x, y, EQ_X,  EQPL_Y, 23, 12))        return HIT_EQ;
    if (in_rect(x, y, PL_X,  EQPL_Y, 23, 12))        return HIT_PL;
    if (in_rect(x, y, SHUF_X, SHUF_Y, 47, 15))       return HIT_SHUF;
    if (in_rect(x, y, REP_X,  SHUF_Y, 28, 15))       return HIT_REP;
    if (in_rect(x, y, VIS_X, VIS_Y, VIS_W, VIS_H))   return HIT_VIS;
    return HIT_NONE;
}

/* Dragging a slider: the value tracks the pointer for the whole drag, which is
 * why this is a function and not four lines inside the down handler. */
static void slider_track(int x)
{
    if (g_hit == HIT_VOL) {
        g_vol = (x - VOL_X) * 100 / (VOL_W - 14);
        if (g_vol < 0) g_vol = 0;
        if (g_vol > 100) g_vol = 100;
        unoamp_set_volume(g_vol);
    } else if (g_hit == HIT_BAL) {
        g_bal = (x - BAL_X) * 200 / (BAL_W - 14) - 100;
        if (g_bal < -100) g_bal = -100;
        if (g_bal >  100) g_bal =  100;
        /* Dead zone at centre: a balance slider you cannot centre by hand is
         * the single most complained-about thing in skinned players. */
        if (g_bal > -8 && g_bal < 8) g_bal = 0;
        unoamp_set_balance(g_bal);
    } else if (g_hit == HIT_POS) {
        const unoamp_in *in = unoamp_playing();
        int len = (in && in->GetLength) ? in->GetLength() : -1;
        if (len > 0 && in->SetOutputTime) {
            int f = (x - POS_X) * len / (POS_W - 29);
            if (f < 0) f = 0;
            if (f > len) f = len;
            in->SetOutputTime(f);
        }
    }
}

static int ui_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    int ox, oy, x, y, which;
    (void)w; (void)ctx;
    origin(&ox, &oy);
    x = (e->x - ox) / g_scale;
    y = (e->y - oy) / g_scale;

    switch (e->kind) {
    case UI_EV_MOUSE_DOWN:
        g_hit = hit_test(x, y, &which);
        switch (g_hit) {
        case HIT_TITLE: g_drag_dx = e->x - ox; g_drag_dy = e->y - oy; break;
        case HIT_BTN:   g_pressed = which; break;
        case HIT_VOL: case HIT_BAL: case HIT_POS: slider_track(x); break;
        default: break;
        }
        pc64_shell_dirty();
        return 1;

    case UI_EV_MOUSE_MOVE:
        if (g_hit == HIT_TITLE && g_win) {
            g_win->r.x = e->x - g_drag_dx;
            g_win->r.y = e->y - g_drag_dy;
            pc64_shell_dirty();
        } else if (g_hit == HIT_VOL || g_hit == HIT_BAL || g_hit == HIT_POS) {
            slider_track(x);
            pc64_shell_dirty();
        }
        return 1;

    case UI_EV_MOUSE_UP:
        /* An action fires on RELEASE, and only if the pointer is still on the
         * control - the same rule every desktop button follows, and the reason
         * a mis-click can be taken back by sliding off before letting go. */
        if (g_hit == HIT_BTN && hit_test(x, y, &which) == HIT_BTN &&
            which == g_pressed)
            unoamp_transport(g_pressed);
        else if (g_hit && hit_test(x, y, &which) == g_hit) {
            switch (g_hit) {
            case HIT_SHUF:  g_shuffle = !g_shuffle; break;
            case HIT_REP:   g_repeat  = !g_repeat;  break;
            case HIT_EQ:    unoamp_ui_show_eq(!g_eq_open); break;
            case HIT_PL:    unoamp_ui_show_pl(!g_pl_open); break;
            case HIT_SHADE: unoamp_ui_set_shade(!g_shade); break;
            case HIT_CLOSE: unoamp_ui_close(); break;
            default: break;
            }
        }
        g_pressed = -1; g_hit = HIT_NONE;
        pc64_shell_dirty();
        return 1;

    case UI_EV_WHEEL:
        /* Winamp's wheel is volume, everywhere in the window. */
        g_vol -= e->wheel * 5;
        if (g_vol < 0) g_vol = 0;
        if (g_vol > 100) g_vol = 100;
        unoamp_set_volume(g_vol);
        pc64_shell_dirty();
        return 1;

    default:
        return 0;
    }
}

static unoui_canvas g_canvas = { ui_draw, ui_event, 0 };

/* ---- host surface --------------------------------------------------------- */
void unoamp_ui_set_title(const char *s)
{
    strncpy(g_title, s ? s : "", sizeof g_title - 1);
    g_title[sizeof g_title - 1] = 0;
    g_scroll = 0;
}

void unoamp_ui_set_shade(int on)
{
    g_shade = !!on;
    if (g_win) {
        g_win->r.h = (g_shade ? SHADE_H : WIN_H) * g_scale;
        if (g_win->nw) g_win->w[0].r.h = g_win->r.h;
    }
    pc64_shell_dirty();
}
int unoamp_ui_shaded(void) { return g_shade; }

int unoamp_ui_scale(void) { return g_scale; }
void unoamp_ui_set_scale(int s)
{
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    g_scale = s;
    if (g_win) {
        g_win->r.w = WIN_W * g_scale;
        g_win->r.h = (g_shade ? SHADE_H : WIN_H) * g_scale;
        if (g_win->nw) {
            g_win->w[0].r.w = g_win->r.w;
            g_win->w[0].r.h = g_win->r.h;
        }
    }
    pc64_shell_dirty();
}

/* One tick of animation: the title scroll. Deliberately slower than the frame
 * rate - a title that scrolls at 60 characters a second is unreadable. */
void unoamp_ui_tick(void)
{
    static int div_;
    if (++div_ >= 8) { div_ = 0; g_scroll++; pc64_shell_dirty(); }
}

void unoamp_ui_build(unoui_window *win)
{
    int sw = fb_width();
    /* Pick the scale from the panel: the classic window at 1x is unreadable on
     * anything modern, and past 3x it stops being a music player and starts
     * being wallpaper. */
    g_scale = sw >= 2400 ? 3 : sw >= 1100 ? 2 : 1;
    unoui_window_init(win, "UnoAmp", 120, 60, WIN_W * g_scale, WIN_H * g_scale);
    win->flags |= UI_WIN_BARE;         /* the skin draws its own chrome        */
    unoui_add_canvas(win, 0, 0, WIN_W * g_scale, WIN_H * g_scale, &g_canvas);
    g_win = win;
    unoamp_start();                    /* skin + playlist, both best-effort   */
}

unoui_window *unoamp_ui_window(void) { return g_win; }
void unoamp_ui_set_eq_open(int v) { g_eq_open = !!v; }
void unoamp_ui_set_pl_open(int v) { g_pl_open = !!v; }
int  unoamp_ui_volume(void)  { return g_vol; }
int  unoamp_ui_balance(void) { return g_bal; }
int  unoamp_ui_shuffle(void) { return g_shuffle; }
int  unoamp_ui_repeat(void)  { return g_repeat; }

/* ===========================================================================
 * THE EQUALISER WINDOW
 *
 * Same trick as the main window: EQMAIN.BMP is the whole 275x116 background
 * and the controls are blitted on top at fixed offsets. The band sliders are
 * drawn procedurally over the skin's own colours rather than from the skin's
 * slider strip - that strip's atlas offsets vary between skin authors far more
 * than the main window's do, and a slider drawn in the skin's palette is
 * closer to right than one blitted from the wrong rectangle.
 * ======================================================================== */
#define EQ_W 275
#define EQ_H 116
#define EQ_BANDS 10
#define EQB_X 78                       /* first band slider                    */
#define EQB_DX 18
#define EQB_Y 38
#define EQB_H 51
#define EQPRE_X 21                     /* the preamp slider                    */

static unoui_window *g_eqwin;
static int g_eq_on;
static int g_eq_gain[EQ_BANDS];        /* -100..+100, zero = flat              */
static int g_eq_pre;
static int g_eq_drag = -1;             /* -3 titlebar, -2 preamp, 0..9 band    */

/* The gains, for the DSP in phase 6. They live with the sliders that set them
 * because a second copy would drift. */
int unoamp_eq_enabled(void) { return g_eq_on; }
int unoamp_eq_band(int i) { return (i >= 0 && i < EQ_BANDS) ? g_eq_gain[i] : 0; }
int unoamp_eq_preamp(void) { return g_eq_pre; }

static void eq_origin(int *ox, int *oy)
{ *ox = g_eqwin ? g_eqwin->r.x : 0; *oy = g_eqwin ? g_eqwin->r.y : 0; }

/* One band slider: a groove and a thumb. Gain runs -100..+100 with zero in the
 * middle, and the centre line is drawn because a flat EQ should be visibly
 * flat rather than merely approximately centred. */
static void eq_slider(int dx, int gain)
{
    const unoui_theme *t = pc64_shell_theme();
    const unoamp_skin *sk = unoamp_skin_get();
    int ox, oy, ty;
    unsigned groove = t->pal.dark, thumb = t->pal.face;
    eq_origin(&ox, &oy);
    if (sk && sk->sheet[UNOAMP_SHEET_EQMAIN].px) {
        /* Borrow two pixels from the skin so the slider is in ITS palette
         * rather than the desktop's - an EQ drawn in system grey on top of a
         * black skin is the single most obvious way to look wrong. */
        const unoamp_sheet *sh = &sk->sheet[UNOAMP_SHEET_EQMAIN];
        if (sh->w > 20 && sh->h > 40) {
            groove = sh->px[(long)30 * sh->w + 15];
            thumb  = sh->px[(long)36 * sh->w + 2];
        }
    }
    fb_fill_rect(ox + (dx + 5) * g_scale, oy + EQB_Y * g_scale,
                 2 * g_scale, EQB_H * g_scale, groove);
    fb_fill_rect(ox + dx * g_scale, oy + (EQB_Y + EQB_H / 2) * g_scale,
                 12 * g_scale, g_scale, t->pal.shadow);
    ty = EQB_Y + (100 - gain) * (EQB_H - 11) / 200;
    fb_fill_rect(ox + dx * g_scale, oy + ty * g_scale,
                 11 * g_scale, 11 * g_scale, thumb);
}

/* Blit a whole sheet as a window background at an arbitrary origin. The main
 * window's spr() is deliberately main-window-relative (it is the hot path and
 * should not carry an origin argument it almost never varies), so the other
 * two windows share this instead. */
static void sheet_bg(int sheet, int ox, int oy, int w, int h, unsigned fallback)
{
    const unoamp_skin *sk = unoamp_skin_get();
    const unoamp_sheet *sh = sk ? &sk->sheet[sheet] : 0;
    int px, py, ii, jj;
    if (!sh || !sh->px) {
        fb_fill_rect(ox, oy, w * g_scale, h * g_scale, fallback);
        return;
    }
    for (py = 0; py < h && py < sh->h; py++)
        for (px = 0; px < w && px < sh->w; px++) {
            unsigned c = sh->px[(long)py * sh->w + px];
            for (jj = 0; jj < g_scale; jj++)
                for (ii = 0; ii < g_scale; ii++)
                    fb_pixel(ox + px * g_scale + ii, oy + py * g_scale + jj, c);
        }
}

static void eq_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = pc64_shell_theme();
    int i, ox, oy;
    (void)w; (void)r; (void)ctx;
    eq_origin(&ox, &oy);
    sheet_bg(UNOAMP_SHEET_EQMAIN, ox, oy, EQ_W, EQ_H, t->pal.win_bg);
    eq_slider(EQPRE_X, g_eq_pre);
    for (i = 0; i < EQ_BANDS; i++) eq_slider(EQB_X + i * EQB_DX, g_eq_gain[i]);
    /* The ON button, top left under the titlebar. */
    fb_fill_rect(ox + 14 * g_scale, oy + 18 * g_scale,
                 26 * g_scale, 12 * g_scale,
                 g_eq_on ? t->pal.accent : t->pal.face);
}

static int eq_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    int ox, oy, x, y, i;
    (void)w; (void)ctx;
    eq_origin(&ox, &oy);
    x = (e->x - ox) / g_scale;
    y = (e->y - oy) / g_scale;

    if (e->kind == UI_EV_MOUSE_DOWN) {
        if (y < TITLE_H) {
            if (in_rect(x, y, EQ_W - 11, 3, 9, 9)) { unoamp_ui_show_eq(0); return 1; }
            g_eq_drag = -3;
            g_drag_dx = e->x - ox; g_drag_dy = e->y - oy;
            return 1;
        }
        if (in_rect(x, y, 14, 18, 26, 12)) {
            g_eq_on = !g_eq_on; pc64_shell_dirty(); return 1;
        }
        if (in_rect(x, y, EQPRE_X, EQB_Y, 12, EQB_H)) g_eq_drag = -2;
        for (i = 0; i < EQ_BANDS; i++)
            if (in_rect(x, y, EQB_X + i * EQB_DX, EQB_Y, 12, EQB_H)) g_eq_drag = i;
    }
    if (e->kind == UI_EV_MOUSE_DOWN || e->kind == UI_EV_MOUSE_MOVE) {
        if (g_eq_drag == -3 && g_eqwin) {
            g_eqwin->r.x = e->x - g_drag_dx;
            g_eqwin->r.y = e->y - g_drag_dy;
            pc64_shell_dirty();
            return 1;
        }
        if (g_eq_drag >= -2) {
            int g = 100 - (y - EQB_Y) * 200 / (EQB_H - 11);
            if (g < -100) g = -100;
            if (g >  100) g =  100;
            if (g > -6 && g < 6) g = 0;           /* snap to flat              */
            if (g_eq_drag == -2) g_eq_pre = g; else g_eq_gain[g_eq_drag] = g;
            pc64_shell_dirty();
            return 1;
        }
    }
    if (e->kind == UI_EV_MOUSE_UP) { g_eq_drag = -1; return 1; }
    return 0;
}

static unoui_canvas g_eq_canvas = { eq_draw, eq_event, 0 };

/* ===========================================================================
 * THE PLAYLIST WINDOW
 *
 * PLEDIT.BMP is a nine-slice: corners, tiled edges and a tiled interior, so
 * the playlist RESIZES - the one classic Winamp window that does. It resizes
 * in whole tiles because a partial tile is exactly what makes a hand-rolled
 * nine-slice look broken.
 * ======================================================================== */
#define PLW_MIN_W 275
#define PLW_MIN_H 174
#define PL_ROW_H 6                     /* the bitmap font's line height       */

static unoui_window *g_plwin;
static int g_pl_top;                   /* first visible row                   */
static int g_pl_drag;
static int g_pl_last = -1;             /* for click-again-to-play             */

static void pl_origin(int *ox, int *oy)
{ *ox = g_plwin ? g_plwin->r.x : 0; *oy = g_plwin ? g_plwin->r.y : 0; }

/* A PLEDIT sprite at the playlist window's origin. */
static void pl_spr(int sx, int sy, int w, int h, int dx, int dy, unsigned fallback)
{
    const unoamp_skin *sk = unoamp_skin_get();
    const unoamp_sheet *sh = sk ? &sk->sheet[UNOAMP_SHEET_PLEDIT] : 0;
    int ox, oy, px, py, ii, jj;
    pl_origin(&ox, &oy);
    if (!sh || !sh->px) {
        fb_fill_rect(ox + dx * g_scale, oy + dy * g_scale,
                     w * g_scale, h * g_scale, fallback);
        return;
    }
    for (py = 0; py < h; py++) {
        if (sy + py >= sh->h) break;
        for (px = 0; px < w; px++) {
            unsigned c;
            if (sx + px >= sh->w) break;
            c = sh->px[(long)(sy + py) * sh->w + sx + px];
            for (jj = 0; jj < g_scale; jj++)
                for (ii = 0; ii < g_scale; ii++)
                    fb_pixel(ox + (dx + px) * g_scale + ii,
                             oy + (dy + py) * g_scale + jj, c);
        }
    }
}

/* Text at the playlist origin in the skin's bitmap font. font_text() is
 * main-window-relative for the same reason spr() is. */
static void pl_text(const char *s, int dx, int dy, int maxch, unsigned fallback)
{
    const unoamp_skin *sk = unoamp_skin_get();
    const unoamp_sheet *sh = sk ? &sk->sheet[UNOAMP_SHEET_TEXT] : 0;
    int ox, oy, i, cx, cy, px, py, ii, jj;
    pl_origin(&ox, &oy);
    if (!sh || !sh->px) {
        fb_text(ox + dx * g_scale, oy + dy * g_scale, s, fallback, -1);
        return;
    }
    for (i = 0; i < maxch && s[i]; i++) {
        if (!font_cell(s[i], &cx, &cy)) { cx = 28 * CH_W; cy = 0; }
        for (py = 0; py < CH_H; py++)
            for (px = 0; px < CH_W; px++) {
                unsigned c;
                if (cx + px >= sh->w || cy + py >= sh->h) continue;
                c = sh->px[(long)(cy + py) * sh->w + cx + px];
                for (jj = 0; jj < g_scale; jj++)
                    for (ii = 0; ii < g_scale; ii++)
                        fb_pixel(ox + (dx + i * CH_W + px) * g_scale + ii,
                                 oy + (dy + py) * g_scale + jj, c);
            }
    }
}

static void pl_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = pc64_shell_theme();
    const unoamp_skin *sk = unoamp_skin_get();
    int ww = g_plwin ? g_plwin->r.w / g_scale : PLW_MIN_W;
    int wh = g_plwin ? g_plwin->r.h / g_scale : PLW_MIN_H;
    unsigned c_norm, c_cur, c_bg;
    int i, rows, x, ox, oy;
    (void)w; (void)r; (void)ctx;

    c_norm = (sk && sk->pl_normal)  ? sk->pl_normal  : t->pal.field_text;
    c_cur  = (sk && sk->pl_current) ? sk->pl_current : t->pal.accent;
    c_bg   = (sk && sk->pl_bg)      ? sk->pl_bg      : t->pal.field_bg;

    /* Interior first, then the frame on top: the nine-slice edges overlap the
     * fill by design, which is how the skin's inner shadow lands. */
    pl_origin(&ox, &oy);
    fb_fill_rect(ox, oy + 20 * g_scale, ww * g_scale, (wh - 58) * g_scale, c_bg);

    pl_spr(0, 0, 25, 20, 0, 0, t->pal.title_bg);                 /* top-left  */
    for (x = 25; x < ww - 25; x += 25)
        pl_spr(127, 0, 25, 20, x, 0, t->pal.title_bg);           /* top tile  */
    pl_spr(153, 0, 25, 20, ww - 25, 0, t->pal.title_bg);         /* top-right */
    pl_spr(26, 0, 100, 20, (ww - 100) / 2, 0, t->pal.title_bg);  /* the title */
    for (i = 20; i < wh - 38; i += 29) {
        pl_spr(0, 42, 12, 29, 0, i, t->pal.win_frame);           /* left edge */
        pl_spr(31, 42, 20, 29, ww - 20, i, t->pal.win_frame);    /* right     */
    }
    pl_spr(0, 72, 125, 38, 0, wh - 38, t->pal.face);             /* bottom-l  */
    pl_spr(126, 72, 150, 38, ww - 150, wh - 38, t->pal.face);    /* bottom-r  */

    rows = (wh - 58) / (PL_ROW_H + 2);
    for (i = 0; i < rows; i++) {
        int idx = g_pl_top + i, y = 22 + i * (PL_ROW_H + 2), n;
        char line[80];
        if (idx >= unoamp_pl_count()) break;
        /* "12. Title" - the number is what makes a playlist navigable by
         * keyboard, which is why Winamp drew it too. */
        n = 0;
        if (idx + 1 >= 100) line[n++] = (char)('0' + (idx + 1) / 100 % 10);
        if (idx + 1 >= 10)  line[n++] = (char)('0' + (idx + 1) / 10 % 10);
        line[n++] = (char)('0' + (idx + 1) % 10);
        line[n++] = '.'; line[n++] = ' ';
        {
            const char *ti = unoamp_pl_title(idx);
            while (*ti && n < (int)sizeof line - 1) line[n++] = *ti++;
        }
        line[n] = 0;
        if (idx == unoamp_pl_selected())
            fb_fill_rect(ox + 12 * g_scale, oy + (y - 1) * g_scale,
                         (ww - 32) * g_scale, (PL_ROW_H + 2) * g_scale,
                         t->pal.accent);
        pl_text(line, 12, y, (ww - 40) / CH_W,
                idx == unoamp_pl_current() ? c_cur : c_norm);
    }
}

static int pl_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    int ox, oy, x, y, ww, wh;
    (void)w; (void)ctx;
    pl_origin(&ox, &oy);
    x = (e->x - ox) / g_scale;
    y = (e->y - oy) / g_scale;
    ww = g_plwin ? g_plwin->r.w / g_scale : PLW_MIN_W;
    wh = g_plwin ? g_plwin->r.h / g_scale : PLW_MIN_H;

    switch (e->kind) {
    case UI_EV_MOUSE_DOWN:
        if (y < 20) {
            if (in_rect(x, y, ww - 11, 3, 9, 9)) { unoamp_ui_show_pl(0); return 1; }
            g_pl_drag = 1; g_drag_dx = e->x - ox; g_drag_dy = e->y - oy;
            return 1;
        }
        /* The bottom-right corner is the resize grip, as it was in Winamp. */
        if (x > ww - 20 && y > wh - 20) { g_pl_drag = 2; return 1; }
        if (y < wh - 38) {
            int row = (y - 22) / (PL_ROW_H + 2) + g_pl_top;
            if (row >= 0 && row < unoamp_pl_count()) {
                unoamp_pl_select(row);
                /* A second click on the already-selected row plays it. The
                 * shell has no double-click event, and select-then-commit is
                 * the behaviour that survives without one. */
                if (g_pl_last == row) unoamp_play_index(row);
                g_pl_last = row;
            }
            pc64_shell_dirty();
        }
        return 1;

    case UI_EV_MOUSE_MOVE:
        if (g_pl_drag == 1 && g_plwin) {
            g_plwin->r.x = e->x - g_drag_dx;
            g_plwin->r.y = e->y - g_drag_dy;
            pc64_shell_dirty();
        } else if (g_pl_drag == 2 && g_plwin) {
            /* Snap to the tile grid, or the nine-slice shows a partial tile. */
            int nw = ((x + 12) / 25) * 25, nh = ((y - 58 + 14) / 29) * 29 + 58;
            if (nw < PLW_MIN_W) nw = PLW_MIN_W;
            if (nh < PLW_MIN_H) nh = PLW_MIN_H;
            g_plwin->r.w = nw * g_scale;
            g_plwin->r.h = nh * g_scale;
            if (g_plwin->nw) {
                g_plwin->w[0].r.w = g_plwin->r.w;
                g_plwin->w[0].r.h = g_plwin->r.h;
            }
            pc64_shell_dirty();
        }
        return 1;

    case UI_EV_MOUSE_UP: g_pl_drag = 0; return 1;

    case UI_EV_WHEEL: {
        int rows = (wh - 58) / (PL_ROW_H + 2);
        g_pl_top += e->wheel * 3;
        if (g_pl_top > unoamp_pl_count() - rows) g_pl_top = unoamp_pl_count() - rows;
        if (g_pl_top < 0) g_pl_top = 0;
        pc64_shell_dirty();
        return 1;
    }

    case UI_EV_KEY:
        if (e->key == UI_KEY_DELETE) { unoamp_pl_remove(unoamp_pl_selected()); return 1; }
        if (e->key == UI_KEY_ENTER)  { unoamp_play_index(unoamp_pl_selected()); return 1; }
        if (e->key == UI_KEY_UP)     { unoamp_pl_select(unoamp_pl_selected() - 1);
                                       pc64_shell_dirty(); return 1; }
        if (e->key == UI_KEY_DOWN)   { unoamp_pl_select(unoamp_pl_selected() + 1);
                                       pc64_shell_dirty(); return 1; }
        return 0;

    default:
        return 0;
    }
}

static unoui_canvas g_pl_canvas = { pl_draw, pl_event, 0 };

/* ---- docking ---------------------------------------------------------------
 * Winamp's windows snapped to each other's edges, and that is not decoration:
 * the three are meant to read as one object, and without snapping they drift
 * apart the first time the main window moves. The EQ and playlist sit below
 * the main window and follow it. */
void unoamp_ui_dock(void)
{
    int below;
    if (!g_win) return;
    below = g_win->r.y + (g_shade ? SHADE_H : WIN_H) * g_scale;
    if (g_eqwin) { g_eqwin->r.x = g_win->r.x; g_eqwin->r.y = below; }
    if (g_plwin) {
        g_plwin->r.x = g_win->r.x;
        g_plwin->r.y = below + (g_eq_open ? EQ_H * g_scale : 0);
    }
}

unoui_window *unoamp_ui_eq_window(void) { return g_eqwin; }
unoui_window *unoamp_ui_pl_window(void) { return g_plwin; }

void unoamp_ui_build_eq(unoui_window *win)
{
    unoui_window_init(win, "UnoAmp EQ", 120, 60, EQ_W * g_scale, EQ_H * g_scale);
    win->flags |= UI_WIN_BARE;
    unoui_add_canvas(win, 0, 0, EQ_W * g_scale, EQ_H * g_scale, &g_eq_canvas);
    g_eqwin = win;
    unoamp_ui_dock();
}
void unoamp_ui_build_pl(unoui_window *win)
{
    unoui_window_init(win, "UnoAmp Playlist", 120, 60,
                      PLW_MIN_W * g_scale, PLW_MIN_H * g_scale);
    win->flags |= UI_WIN_BARE;
    unoui_add_canvas(win, 0, 0, win->r.w, win->r.h, &g_pl_canvas);
    g_plwin = win;
    unoamp_ui_dock();
}

/* ---- window lifecycle -------------------------------------------------------
 * The three windows are OWNED HERE, not by the shell's app table. Winamp's EQ
 * and playlist are not separate applications - they are parts of one player
 * that happen to be separately positionable - so they open and close with it
 * and the taskbar shows one entry, not three.
 *
 * They are added to the shell with pc64_shell_add_window, the same door the
 * standalone Write and Files windows use. */
void pc64_shell_add_window(unoui_window *w);
void pc64_shell_remove_window(unoui_window *w);

static unoui_window g_eq_storage, g_pl_storage;

void unoamp_ui_show_eq(int on)
{
    if (!!on == g_eq_open) return;
    g_eq_open = !!on;
    if (on) { unoamp_ui_build_eq(&g_eq_storage);
              pc64_shell_add_window(&g_eq_storage); }
    else    { pc64_shell_remove_window(&g_eq_storage); g_eqwin = 0; }
    unoamp_ui_dock();
    pc64_shell_dirty();
}

void unoamp_ui_show_pl(int on)
{
    if (!!on == g_pl_open) return;
    g_pl_open = !!on;
    if (on) { unoamp_ui_build_pl(&g_pl_storage);
              pc64_shell_add_window(&g_pl_storage); }
    else    { pc64_shell_remove_window(&g_pl_storage); g_plwin = 0; }
    unoamp_ui_dock();
    pc64_shell_dirty();
}

/* Closing the player closes all three and stops playback. A music player that
 * kept playing after its window went away would be a bug report, not a
 * feature. */
void unoamp_ui_close(void)
{
    unoamp_stop();
    unoamp_ui_show_eq(0);
    unoamp_ui_show_pl(0);
    if (g_win) { pc64_shell_remove_window(g_win); g_win = 0; }
    pc64_shell_dirty();
}
