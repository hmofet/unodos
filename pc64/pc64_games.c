/* ===========================================================================
 * UnoDOS/pc64 - the native unoui game (see pc64_games.h).
 *
 * Runner3D is a unoui_canvas that draws itself with fb primitives, sized from
 * the rect it is handed - so the SAME code fills a window and a full screen
 * (unoui passes the whole screen as the rect in fullscreen). No mac_compat /
 * KernelApi bridge; the game owns its pixels and reads unoui events directly.
 *
 * Runner3D is the ONLY game here, because it is the only one with no module
 * counterpart - it drives uno3d directly. Dostris, Pac-Man and OutLast ship as
 * .UNO modules (apps/dostris.c, apps/pacman.c, apps/outlast.c) so that ALL apps
 * load from storage, per the decoupling contract. Native copies of those three
 * used to live here as well, unreachable: app_game() in pc64_uui.c has mapped
 * only EX_RUNNER onto a native canvas for some time, so nothing could call
 * them. Two compiling copies of one game hide divergence indefinitely - the
 * native Pac-Man was the only one with sounds, so the Pac-Man users actually
 * played measured at digital silence for weeks, and the module Dostris had
 * likewise lost its line-clear blip. Both were recovered into the modules; the
 * dead copies are gone. Do not add a second copy of a module game here.
 * ======================================================================== */
#include "pc64_games.h"
#include "fb.h"
#include "uno3d.h"
#include "uno3d_game.h"
#include "uno3d_backend.h"
#include "mac_compat.h"      /* uno_pc64_lowres */

static void num(long v, char *o){ char t[16]; int n=0,k=0; if(v<=0)t[n++]='0'; while(v){t[n++]=(char)('0'+v%10);v/=10;} while(n)o[k++]=t[--n]; o[k]=0; }

/* On-canvas GUI button (games are canvases, so their controls live on the
 * pixels). Draw one and remember its rect; hit_btn() tests a click against it,
 * so every key command (restart) also has a mouse-reachable control. */
static unoui_rect rnBtn;
static int draw_btn(unoui_rect *out, int x, int y, const char *label, fb_px col)
{
    int w = fb_text_w(label) + 18, h = 17;
    fb_round_rect(x, y, w, h, 5, col);
    fb_round_rect_a(x, y, w, h, 5, FB_RGB(255,255,255), 26, FB_CORNER_ALL);   /* soft top light */
    fb_text(x + 9, y + 5, label, FB_RGB(255,255,255), -1);
    out->x = x; out->y = y; out->w = w; out->h = h;
    return w;
}
static int hit_btn(const unoui_rect *b, const unoui_event *e)
{
    return e->kind == UI_EV_MOUSE_DOWN && b->w > 0 &&
           e->x >= b->x && e->x < b->x + b->w && e->y >= b->y && e->y < b->y + b->h;
}

/* --------------------------------------------------------------- Runner3D --- *
 * The uno3d "UnoDOS Runner" game. It renders the 3D corridor straight into fb
 * at the desktop resolution (fullscreen), so it runs as a native game that
 * auto-fullscreens. uno_pc64_lowres(1) drops the render res for a playable
 * frame-rate on the software rasteriser; open/close manage uno3d + the res. */
static int gRnInit, gRnL, gRnR;
/* (Re)build uno3d for the CURRENT framebuffer size.  The mode switch is NOT
 * done here any more - the shell owns it through pc64_game_fullscreen below,
 * because entering and leaving fullscreen are the two moments the render size
 * changes and only the shell knows about both.  This runs lazily from
 * rn_frame, so it always initialises at whatever size is live by then. */
/* The rasteriser Runner3D draws with.  pc64 on a PC takes the Intel backend
 * (which falls back to software itself when no iGPU is mapped); a platform
 * with no PCI GPU at all names the software one at compile time, so this
 * file compiles unchanged there without dragging uno3d_intel.c along. */
#ifndef UNO_U3D_BACKEND
#define UNO_U3D_BACKEND u3d_backend_intel
#endif
static void rn_start(void)
{
    u3d_use_backend(&UNO_U3D_BACKEND);
    game_init(FB_W, FB_H); u3d_init(FB_W, FB_H);
    gRnInit = 1; gRnL = gRnR = 0;
}
static void rn_frame(void)
{
    game_input in; char line[48], nb[12]; const char *s; char *p; long sc;
    if (!gRnInit) rn_start();
    in.left = gRnL > 0; in.right = gRnR > 0; in.up = in.down = 0;
    in.fire = in.start = 0;
    if (gRnL > 0) gRnL--; if (gRnR > 0) gRnR--;
    game_update(&in); game_render();
    p = line; s = "UnoDOS Runner 3D   score "; while (*s) *p++ = *s++;
    sc = game_score(); num(sc, nb); { char *q = nb; while (*q) *p++ = *q++; } *p = 0;
    fb_text(12, 10, line, FB_RGB(255,255,255), -1);
    fb_text(12, 22, u3d_backend_name(), FB_RGB(100,255,255), -1);
    if (game_over()) { fb_text(FB_W/2 - 40, FB_H/2 - 18, "CRASH", FB_RGB(255,110,110), -1);
                       draw_btn(&rnBtn, FB_W/2 - 62, FB_H/2, "Restart (Space)", FB_RGB(40,120,220)); }
    else             { rnBtn.w = 0; fb_text(12, FB_H - 16, "Left/Right steer   Esc quit", FB_RGB(170,170,170), -1); }
}
static void rn_draw(struct unoui_widget *w, unoui_rect r, void *ctx) { (void)w;(void)r;(void)ctx; rn_frame(); }
static int rn_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w;(void)ctx;
    if (hit_btn(&rnBtn, e)) { game_input in = {0,0,0,0,0,1}; game_update(&in); return 1; }
    if (e->kind == UI_EV_KEY) {
        if (e->key == UI_KEY_LEFT)  { gRnL = 6; return 1; }
        if (e->key == UI_KEY_RIGHT) { gRnR = 6; return 1; }
        return 0;
    }
    if (e->kind == UI_EV_CHAR && (e->ch == ' ' || e->ch == '\r')) {
        game_input in = {0,0,0,0,0,1}; game_update(&in); return 1;   /* start/restart */
    }
    return 0;
}
static void rn_close(void)
{
    if (gRnInit) { u3d_shutdown(); gRnInit = 0; }
    uno_pc64_lowres(0);      /* belt and braces: lowres is idempotent, and a
                                close that arrives without a fullscreen-leave
                                must still hand the resolution back */
}

/* ------------------------------------------------------ registry ----------- */
static unoui_canvas g_runner = { rn_draw, rn_event, 0 };

unoui_canvas *pc64_game_canvas(int game)
{
    switch (game) {
    case GAME_RUNNER:  return &g_runner;
    default:           return 0;
    }
}
void pc64_game_open(int game)
{
    if (game == GAME_RUNNER) { if (gRnInit) { u3d_shutdown(); gRnInit = 0; } }
                             /* built lazily, AFTER the shell has taken the
                                screen and set the render size */
}
/* No uno_seq_stop() here any more: the only sequencer users in this file were
 * the native Dostris/OutLast songs, and they are gone. Runner3D is silent, so
 * the call could only reach across and cut a MODULE game's music short. */
void pc64_game_close(int game) { if (game == GAME_RUNNER) rn_close(); }

/* THE RESOLUTION FOLLOWS FULLSCREEN, not the app's lifetime.
 *
 * uno_pc64_lowres(1) used to be called from rn_start and undone only in
 * rn_close, so the low render mode was tied to Runner3D being OPEN.  But the
 * shell drops a window out of fullscreen for eight different reasons and only
 * one of them closes it - Esc, Alt+D, a virtual-desktop switch, minimize,
 * "minimize all", moving the window to another desktop, F12, and finally
 * close.  Every route but the last left the desktop at a QUARTER of its
 * resolution with the game still running.  Tying the mode to fullscreen
 * instead means there is no route that can miss it.
 *
 * The uno3d pipeline is torn down on both edges: it is built for one fixed
 * framebuffer size, so a mode change under it must be a rebuild, not a resize.
 * rn_frame does that on the next frame. */
void pc64_game_fullscreen(int game, int on)
{
    if (game != GAME_RUNNER) return;
    uno_pc64_lowres(on ? 1 : 0);
    if (gRnInit) { u3d_shutdown(); gRnInit = 0; }
}
void pc64_game_tick(int game)
{
    (void)game;
    /* GAME_RUNNER: update+render happen together in rn_draw (every frame) */
}
const char *pc64_game_name(int game)
{
    static const char *n[PC64_NGAMES] = { "Runner3D" };
    return (game >= 0 && game < PC64_NGAMES) ? n[game] : "Game";
}
