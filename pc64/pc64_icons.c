/* ===========================================================================
 * UnoDOS/pc64 - per-app icon artwork (see pc64_icons.h).
 *
 * Distinct, high-colour emblems drawn procedurally so every app reads at a
 * glance on the desktop, in the launcher and on the taskbar. Each emblem is
 * designed on a 32x32 grid and scaled to whatever box it is asked to fill, so
 * the same art serves a big desktop icon and a small taskbar chip.
 * ======================================================================== */
#include "pc64_icons.h"
#include "fb.h"
#include "unoui_theme.h"

/* app icon ids match the shell's app enum order */
enum { IC_CTRL, IC_EDIT, IC_FILES, IC_SYS, IC_CLOCK, IC_CANVAS, IC_SETUP,
       IC_DOSTRIS, IC_PACMAN, IC_OUTLAST, IC_MUSIC, IC_TRACKER, IC_PAINT };

/* ---- theme-aware recolouring -------------------------------------------
 * Icons adopt the ACTIVE theme so they stop looking "Aurora" on a retro
 * theme. Every colour an emblem draws is routed through IC(): full-depth
 * themes keep the rich literal art unchanged; reduced-depth themes snap each
 * colour to the nearest palette role; 1-bit themes collapse to ink/paper by
 * luminance. g_ith is refreshed from the live shell theme on each draw. */
static const unoui_theme *g_ith;

#define IR(c) ((int)((c) & 0xFF))
#define IG(c) ((int)(((c) >> 8) & 0xFF))
#define IB(c) ((int)(((c) >> 16) & 0xFF))

static fb_px IC(fb_px c)
{
    const unoui_theme *t = g_ith;
    const unoui_palette *p;
    fb_px cand[15];
    int i, best;
    long bd;
    if (!t || t->m.depth == UNOUI_DEPTH_FULL) return c;   /* keep rich art */
    p = &t->pal;
    if (t->m.depth == UNOUI_DEPTH_1) {                    /* 1-bit: ink/paper */
        int y = (IR(c) * 77 + IG(c) * 150 + IB(c) * 29) >> 8;
        return y < 128 ? p->text : p->win_bg;
    }
    /* reduced depth: snap to the nearest palette colour */
    cand[0]=p->desktop; cand[1]=p->win_bg; cand[2]=p->text;  cand[3]=p->text_dim;
    cand[4]=p->face;    cand[5]=p->face_text; cand[6]=p->light; cand[7]=p->shadow;
    cand[8]=p->dark;    cand[9]=p->accent;  cand[10]=p->accent_text;
    cand[11]=p->field_bg; cand[12]=p->field_text; cand[13]=p->title_bg; cand[14]=p->title_fg;
    best = 0; bd = -1;
    for (i = 0; i < 15; i++) {
        long dr=IR(c)-IR(cand[i]), dg=IG(c)-IG(cand[i]), db=IB(c)-IB(cand[i]);
        long d = dr*dr + dg*dg + db*db;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return cand[best];
}

/* icon draw primitives - all route colour through IC() so a themed emblem
 * only ever paints in the active palette. */
static void rr(int x, int y, int w, int h, fb_px c) { if (w > 0 && h > 0) fb_fill_rect(x, y, w, h, IC(c)); }
static void frame(int x, int y, int w, int h, fb_px c) { fb_frame_rect(x, y, w, h, IC(c)); }
static void hln(int x, int y, int w, fb_px c) { fb_hline(x, y, w, IC(c)); }

static void disc(int cx, int cy, int rad, fb_px c)
{
    int x, y;
    c = IC(c);
    for (y = -rad; y <= rad; y++)
        for (x = -rad; x <= rad; x++)
            if (x * x + y * y <= rad * rad) fb_pixel(cx + x, cy + y, c);
}
static void ring(int cx, int cy, int rad, int th, fb_px c)
{
    int x, y, in = (rad - th) * (rad - th);
    c = IC(c);
    for (y = -rad; y <= rad; y++)
        for (x = -rad; x <= rad; x++) {
            int d = x * x + y * y;
            if (d <= rad * rad && d >= in) fb_pixel(cx + x, cy + y, c);
        }
}
static void seg(int x0, int y0, int x1, int y1, fb_px c)
{
    int dx = x1 - x0, dy = y1 - y0, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int n = adx > ady ? adx : ady, i;
    c = IC(c);
    if (n < 1) n = 1;
    for (i = 0; i <= n; i++) fb_pixel(x0 + dx * i / n, y0 + dy * i / n, c);
}
static void thick(int x0, int y0, int x1, int y1, int w, fb_px c)
{ int k; for (k = 0; k < w; k++) seg(x0 + k, y0, x1 + k, y1, c); }

void pc64_icon_emblem(int icon, unoui_rect box)
{
    int s  = (box.w < box.h ? box.w : box.h);
    int ox, oy, cx, cy;
    g_ith = pc64_shell_theme();                      /* recolour to live theme */
    if (s < 8) s = 8;
    ox = box.x + (box.w - s) / 2;
    oy = box.y + (box.h - s) / 2;
    cx = ox + s / 2; cy = oy + s / 2;
#define G(a) ((a) * s / 32)

    switch (icon) {
    case IC_CTRL: {                                  /* settings: sliders */
        fb_px trk = FB_RGB(90, 100, 120), kn = FB_RGB(235, 235, 245);
        int r, ys[3] = { G(8), G(16), G(24) }, kx[3] = { G(20), G(10), G(24) };
        for (r = 0; r < 3; r++) {
            rr(ox + G(4), oy + ys[r] - G(1), G(24), G(2), trk);
            rr(ox + kx[r] - G(2), oy + ys[r] - G(4), G(5), G(8), kn);
            frame(ox + kx[r] - G(2), oy + ys[r] - G(4), G(5), G(8), FB_RGB(40, 45, 60));
        }
        break; }
    case IC_EDIT: {                                  /* document + fold + pencil */
        fb_px paper = FB_RGB(250, 250, 244), ink = FB_RGB(90, 100, 125);
        int i;
        rr(ox + G(7), oy + G(4), G(16), G(24), paper);
        frame(ox + G(7), oy + G(4), G(16), G(24), FB_RGB(120, 125, 140));
        rr(ox + G(17), oy + G(4), G(6), G(6), FB_RGB(210, 212, 205));   /* fold */
        seg(ox + G(17), oy + G(10), ox + G(23), oy + G(4), FB_RGB(120,125,140));
        for (i = 0; i < 4; i++) rr(ox + G(10), oy + G(12) + i * G(4), G(10), G(1), ink);
        thick(ox + G(18), oy + G(22), ox + G(26), oy + G(14), G(2), FB_RGB(240, 190, 60)); /* pencil */
        disc(ox + G(26), oy + G(14), G(2), FB_RGB(60, 60, 60));
        break; }
    case IC_FILES: {                                 /* manila folder */
        fb_px body = FB_RGB(244, 200, 92), tab = FB_RGB(224, 176, 60), edge = FB_RGB(150, 110, 30);
        rr(ox + G(5), oy + G(7), G(10), G(4), tab);
        rr(ox + G(4), oy + G(10), G(24), G(16), body);
        frame(ox + G(4), oy + G(10), G(24), G(16), edge);
        rr(ox + G(4), oy + G(13), G(24), G(1), FB_RGB(255, 225, 140));
        break; }
    case IC_SYS: {                                   /* monitor */
        rr(ox + G(4), oy + G(5), G(24), G(18), FB_RGB(55, 58, 70));
        rr(ox + G(7), oy + G(8), G(18), G(12), FB_RGB(40, 120, 210));
        rr(ox + G(8), oy + G(9), G(16), G(4), FB_RGB(90, 160, 235));    /* screen sheen */
        rr(ox + G(13), oy + G(23), G(6), G(3), FB_RGB(80, 84, 96));     /* stand */
        rr(ox + G(9), oy + G(26), G(14), G(2), FB_RGB(80, 84, 96));
        break; }
    case IC_CLOCK: {                                 /* clock face */
        disc(cx, cy, G(13), FB_RGB(250, 250, 250));
        ring(cx, cy, G(13), G(2), FB_RGB(60, 70, 90));
        seg(cx, cy, cx, cy - G(8), FB_RGB(30, 30, 45));                 /* minute */
        seg(cx, cy, cx + G(6), cy + G(3), FB_RGB(200, 60, 60));         /* hour */
        disc(cx, cy, G(2), FB_RGB(30, 30, 45));
        break; }
    case IC_CANVAS: {                                /* artist palette */
        disc(cx, cy, G(13), FB_RGB(222, 192, 150));
        ring(cx, cy, G(13), G(1), FB_RGB(150, 120, 80));
        disc(cx + G(5), cy + G(5), G(3), FB_RGB(0, 0, 0));              /* thumb hole */
        disc(cx - G(5), cy - G(4), G(2), FB_RGB(220, 60, 60));
        disc(cx + G(1), cy - G(6), G(2), FB_RGB(60, 180, 70));
        disc(cx + G(6), cy - G(2), G(2), FB_RGB(60, 110, 230));
        disc(cx - G(6), cy + G(3), G(2), FB_RGB(240, 205, 60));
        break; }
    case IC_SETUP: {                                 /* install: disk + down arrow */
        fb_px platter = FB_RGB(70, 76, 92), sheen = FB_RGB(105, 112, 130);
        fb_px arrow = FB_RGB(70, 190, 110);
        rr(ox + G(4), oy + G(18), G(24), G(9), platter);     /* drive body     */
        frame(ox + G(4), oy + G(18), G(24), G(9), FB_RGB(35, 40, 52));
        rr(ox + G(6), oy + G(20), G(20), G(2), sheen);
        disc(ox + G(24), oy + G(24), G(1), FB_RGB(90, 230, 120));  /* LED      */
        thick(ox + G(15), oy + G(3), ox + G(15), oy + G(12), G(3), arrow);
        seg(ox + G(10), oy + G(10), ox + G(16), oy + G(16), arrow); /* head    */
        seg(ox + G(11), oy + G(10), ox + G(16), oy + G(15), arrow);
        seg(ox + G(21), oy + G(10), ox + G(16), oy + G(16), arrow);
        seg(ox + G(20), oy + G(10), ox + G(16), oy + G(15), arrow);
        break; }
    case IC_DOSTRIS: {                               /* S-tetromino */
        fb_px a = FB_RGB(60, 200, 90), b = FB_RGB(0, 200, 200);
        int u = G(7);
        rr(ox + G(11), oy + G(6), u, u, a); rr(ox + G(18), oy + G(6), u, u, a);
        rr(ox + G(4),  oy + G(13), u, u, b); rr(ox + G(11), oy + G(13), u, u, b);
        rr(ox + G(4),  oy + G(20), u, u, a); rr(ox + G(11), oy + G(20), u, u, a);
        break; }
    case IC_PACMAN: {                                /* pac + pellet */
        disc(cx - G(2), cy, G(11), FB_RGB(255, 216, 0));
        seg(cx - G(2), cy, ox + G(30), oy + G(6), 0);                   /* mouth wedge */
        { int i; for (i = 0; i < G(11); i++)                            /* wedge fill */
            seg(cx - G(2), cy, ox + G(28), oy + G(9) + i, FB_RGB(0,0,0)); }
        disc(ox + G(26), cy, G(2), FB_RGB(255, 240, 180));             /* pellet */
        break; }
    case IC_OUTLAST: {                               /* ghost */
        fb_px g = FB_RGB(235, 80, 100);
        disc(cx, oy + G(13), G(10), g);
        rr(ox + G(6), oy + G(13), G(20), G(11), g);
        { int i; for (i = 0; i < 4; i++) disc(ox + G(8) + i * G(5), oy + G(24), G(2), g); }
        disc(cx - G(4), oy + G(12), G(3), FB_RGB(255,255,255));
        disc(cx + G(4), oy + G(12), G(3), FB_RGB(255,255,255));
        disc(cx - G(4), oy + G(12), G(1), FB_RGB(40,60,180));
        disc(cx + G(4), oy + G(12), G(1), FB_RGB(40,60,180));
        break; }
    case IC_MUSIC: {                                 /* eighth note */
        fb_px n = FB_RGB(30, 40, 64);
        disc(ox + G(11), oy + G(24), G(5), n);
        rr(ox + G(15), oy + G(6), G(2), G(18), n);
        rr(ox + G(15), oy + G(6), G(9), G(2), n);                       /* flag */
        rr(ox + G(21), oy + G(6), G(2), G(6), n);
        break; }
    case IC_TRACKER: {                               /* equaliser bars */
        static const int h[6] = { 10, 20, 14, 26, 8, 18 };
        fb_px col[6] = { FB_RGB(60,180,230), FB_RGB(80,200,120), FB_RGB(240,200,60),
                         FB_RGB(230,90,90), FB_RGB(160,110,230), FB_RGB(60,180,230) };
        int i;
        for (i = 0; i < 6; i++)
            rr(ox + G(4) + i * G(4), oy + G(28) - G(h[i]), G(3), G(h[i]), col[i]);
        break; }
    case IC_PAINT: {                                 /* paintbrush + dab */
        thick(ox + G(20), oy + G(6), ox + G(10), oy + G(20), G(3), FB_RGB(165, 105, 55));
        thick(ox + G(9),  oy + G(19), ox + G(6),  oy + G(24), G(3), FB_RGB(185, 185, 195));
        disc(ox + G(6), oy + G(25), G(3), FB_RGB(220, 60, 60));         /* bristle paint */
        disc(ox + G(24), oy + G(24), G(4), FB_RGB(60, 120, 230));       /* colour dab */
        break; }
    case IC_PAINT + 1: {                             /* Network: connected nodes */
        disc(ox+G(8),  oy+G(8),  G(3), FB_RGB(70,180,230));
        disc(ox+G(24), oy+G(10), G(3), FB_RGB(80,200,120));
        disc(ox+G(14), oy+G(24), G(3), FB_RGB(240,200,60));
        seg(ox+G(8), oy+G(8), ox+G(24), oy+G(10), FB_RGB(160,175,200));
        seg(ox+G(8), oy+G(8), ox+G(14), oy+G(24), FB_RGB(160,175,200));
        seg(ox+G(24), oy+G(10), ox+G(14), oy+G(24), FB_RGB(160,175,200));
        break; }
    case IC_PAINT + 3: {                             /* Browser: window + globe */
        rr(ox+G(4), oy+G(5), G(24), G(22), FB_RGB(245,246,250));
        frame(ox+G(4), oy+G(5), G(24), G(22), FB_RGB(70,80,110));
        rr(ox+G(4), oy+G(5), G(24), G(6), FB_RGB(70,120,210));   /* address bar */
        rr(ox+G(7), oy+G(6), G(16), G(4), FB_RGB(230,238,250));
        disc(cx, oy+G(18), G(6), FB_RGB(70,170,90));             /* globe */
        ring(cx, oy+G(18), G(6), G(1), FB_RGB(40,110,60));
        seg(cx-G(6), oy+G(18), cx+G(6), oy+G(18), FB_RGB(230,245,235));
        seg(cx, oy+G(12), cx, oy+G(24), FB_RGB(230,245,235));
        break; }
    case IC_PAINT + 2: {                             /* Runner3D: perspective road */
        rr(ox + G(2), oy + G(3), G(28), G(12), FB_RGB(90,150,225));    /* sky */
        rr(ox + G(2), oy + G(15), G(28), G(14), FB_RGB(45,140,55));    /* grass */
        { int yy; for (yy = 0; yy < G(14); yy++) {                     /* road wedge */
            int w = G(4) + yy*G(20)/G(14), xx = cx - w/2;
            hln(xx, oy + G(15) + yy, w, FB_RGB(105,105,105)); } }
        { int yy; for (yy = 0; yy < G(14); yy += G(3))                 /* centre line */
            hln(cx-1, oy + G(15) + yy, 2, FB_RGB(240,220,60)); }
        break; }
    default:
        rr(ox + G(6), oy + G(6), G(20), G(20), FB_RGB(160, 170, 190));
        frame(ox + G(6), oy + G(6), G(20), G(20), FB_RGB(60, 70, 90));
        break;
    }
#undef G
}

void pc64_icon_art(int icon, unoui_rect r, const char *label, int flags)
{
    unoui_rect emb = { r.x, r.y, r.w, r.h - 12 };
    const unoui_theme *t;
    if (emb.h > r.w) emb.h = r.w;                    /* keep the emblem square-ish */
    pc64_icon_emblem(icon, emb);                     /* also refreshes g_ith */
    t = g_ith;
    if (label) {
        int lx = r.x + (r.w - (int)(fb_text_w(label))) / 2;
        int ly = r.y + r.h - 10;
        if (flags & UI_F_FOCUS) {                    /* selected: theme accent */
            fb_px fbg = t ? t->pal.accent      : FB_RGB(40, 90, 200);
            fb_px ftx = t ? t->pal.accent_text : FB_RGB(255, 255, 255);
            fb_fill_rect(lx - 3, ly - 1, fb_text_w(label) + 6, 10, fbg);
            fb_text(lx, ly, label, ftx, -1);
        } else {                                     /* label contrasts the desktop */
            fb_px dt = t ? t->pal.desktop : FB_RGB(30, 40, 70);
            int  dl  = (IR(dt) * 77 + IG(dt) * 150 + IB(dt) * 29) >> 8;
            fb_px txt = dl < 128 ? FB_RGB(240, 244, 252) : FB_RGB(20, 24, 36);
            fb_px sh  = dl < 128 ? FB_RGB(10, 15, 30)    : FB_RGB(232, 236, 245);
            fb_text(lx + 1, ly + 1, label, sh, -1);
            fb_text(lx, ly, label, txt, -1);
        }
    }
}
