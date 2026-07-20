/* SAMPLE.C - the "hello, UnoDOS" starter app for Studio's UnoC compiler.
 *
 * A ball bounces around the window; N restarts it, arrow-ish keys nudge it.
 * Open this in Studio, press F7 to build SAMPLE.UNO, F5 to run it.
 * The app model: define uno_app_main, hand back an AppInterface, and the
 * desktop drives you through draw/key/tick.  See DOCS\APPDEV.MD. */
#include "UNO.H"

#define BALL_R   9
#define FIELD_W  260
#define FIELD_H  160

static short bx = 40, by = 30;          /* ball centre (field coords)  */
static short vx = 3, vy = 2;            /* velocity per tick           */
static long  bounces;
static long  last;                      /* last animation tick         */

static const GameRGB kBall  = { 240, 200,  40, C_WHITE };
static const GameRGB kField = {  10,  40, 120, C_BLUE  };

static void sample_draw(UnoWin *w)
{
    Rect f, b;
    short x0 = w->bounds.left + 10;
    short y0 = w->bounds.top + TBAR_H + 8;
    char num[16];

    SetRect(&f, x0 - 2, y0 - 2, x0 + FIELD_W + 2, y0 + FIELD_H + 2);
    uno_box(&f, C_WHITE);
    SetRect(&f, x0, y0, x0 + FIELD_W, y0 + FIELD_H);
    fill_rgb(&f, &kField);

    SetRect(&b, x0 + bx - BALL_R, y0 + by - BALL_R,
            x0 + bx + BALL_R, y0 + by + BALL_R);
    fill_rgb(&b, &kBall);

    text_at(x0, y0 + FIELD_H + 16, "Bounces:", C_CYAN, C_BLUE, false);
    fmt_u(bounces, num);
    text_at(x0 + 70, y0 + FIELD_H + 16, num, C_WHITE, C_BLUE, false);
    text_at(x0 + 130, y0 + FIELD_H + 16, "N: reset", C_MAG, C_BLUE, false);
}

static void sample_redraw(void)
{
    UnoWin *w = find_app_window(APP_RUNNER);
    if (w) draw_window(w);
}

static Boolean sample_key(char ch, short code, Boolean cmd)
{
    if (cmd) return false;
    if (ch == 'n' || ch == 'N') {
        bx = 40; by = 30; vx = 3; vy = 2; bounces = 0;
        sample_redraw();
        return true;
    }
    (void)code;
    return false;
}

static void sample_tick(void)
{
    if (TickCount() - last < 2) return;
    last = TickCount();
    bx += vx; by += vy;
    if (bx < BALL_R)           { bx = BALL_R; vx = -vx; bounces++; }
    if (bx > FIELD_W - BALL_R) { bx = FIELD_W - BALL_R; vx = -vx; bounces++; }
    if (by < BALL_R)           { by = BALL_R; vy = -vy; bounces++; }
    if (by > FIELD_H - BALL_R) { by = FIELD_H - BALL_R; vy = -vy; bounces++; }
    sample_redraw();
}

static const AppInterface kIface = {
    sample_draw, sample_key, 0, sample_tick, 0, 0,
    "Sample", { 30, 30, 330, 260 }
};

const AppInterface *uno_app_main(const KernelApi *k)
{
    gK = k;
    return &kIface;
}
