/* OutLast app module (APP_OUTLAST).  Separate artifact -> app06.so.
   Pseudo-3D racer (port of apps/outlast.asm): same track table, perspective
   math, traffic and physics.  Background music goes through the kernel game-
   music engine (gm_start/gm_stop in the KernelApi); the note table travels
   with the module (the kernel only keeps a pointer). */
#include "uno_mod.h"

#define OL_W       320
#define OL_H       200
#define OLS(v)     ((short)(((v) * 3) / 2))
#define OL_HORIZON 80
#define OL_SEGLEN  80
#define OL_NSEG    32
#define OL_TRACKLEN (OL_SEGLEN * OL_NSEG)

/* Sunset Drive - the module's own copy of the game-music notes */
static const Note kDrive[] = {
    {76,10},{74,10},{72,20},{0,7},{76,10},{74,10},{72,10},{74,10},{76,20},{0,7},
    {74,10},{72,10},{71,20},{0,7},{74,10},{72,10},{71,10},{72,10},{74,20},{0,7},
    {72,20},{76,20},{79,20},{76,20},{74,20},{72,20},{71,20},{0,10},{72,20},{74,20},{76,40},{0,20}
};
#define N_KDRIVE (short)(sizeof(kDrive)/sizeof(kDrive[0]))

static const signed char kOlCurve[OL_NSEG] = {
    0,0,0,0,0,0,0,0,  5,15,25,30,  30,25,15,5,
    0,0,0,0,0,0,0,0,  -5,-15,-25,-30,  -30,-25,-15,-5
};
static const short kOlTreeZ[8] = { 200, 520, 900, 1300, 1600, 1900, 2200, 2480 };

static short gOlState = 0;
static short gOlX = 160, gOlSpeed = 0;
static long  gOlZ = 0, gOlScore = 0;
static short gOlTime = 60, gOlCrash = 0;
static long  gOlLastStep, gOlLastSec;
static long  gOlTraffic[4];
static const unsigned char kOlTrafDir[4]  = { 1, 1, 0, 0 };
static const unsigned char kOlTrafLane[4] = { 0, 1, 0, 1 };
static short gOlRoadL = 100, gOlRoadR = 220;

static void ol_new_game(void)
{
    gOlX = 160; gOlSpeed = 0; gOlZ = 0; gOlScore = 0;
    gOlTime = 60; gOlCrash = 0;
    gOlTraffic[0] = 400; gOlTraffic[1] = 1600;
    gOlTraffic[2] = 800; gOlTraffic[3] = 2000;
    gOlLastStep = gOlLastSec = TickCount();
    gOlState = 1;
    gm_start(kDrive, N_KDRIVE, APP_OUTLAST);
}

enum { OC_SKY = 0, OC_HORIZON, OC_GRASS_A, OC_GRASS_B, OC_ROAD, OC_ROAD_B,
       OC_STRIPE, OC_CAR, OC_CARWIN, OC_WHEEL, OC_TRAF_ON, OC_TRAF_SAME,
       OC_TRUNK, OC_CANOPY, OC_HUD, OC_NCOLORS };
static const GameRGB kOlRGB[OC_NCOLORS] = {
    { 110, 170, 240, C_BLUE  }, { 250, 200, 120, C_CYAN  },
    {  51, 153,  51, C_CYAN  }, {  30, 120,  40, C_MAG   },
    { 110, 110, 110, C_WHITE }, { 100, 100, 100, C_WHITE },
    { 240, 220,  60, C_BLUE  }, { 220,  40,  40, C_MAG   },
    { 140, 220, 240, C_CYAN  }, {  25,  25,  25, C_BLUE  },
    { 245, 245, 245, C_WHITE }, { 240, 200,  60, C_CYAN  },
    { 120,  80,  40, C_BLUE  }, {  30, 140,  45, C_CYAN  },
    {  10,  10,  40, C_BLUE  },
};

static void ol_vrect(UnoWin *w, short x0, short y0, short x1, short y1, short col)
{
    Rect q;
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < 0) x0 = 0;
    if (x1 > OL_W) x1 = OL_W;
    SetRect(&q, w->bounds.left + 4 + OLS(x0), w->bounds.top + TBAR_H + 2 + OLS(y0),
            w->bounds.left + 4 + OLS(x1), w->bounds.top + TBAR_H + 2 + OLS(y1));
    fill_rgb(&q, &kOlRGB[col]);
}

static void outlast_draw(UnoWin *w)
{
    short y;
    long dx = 0;
    char num[16], hud[48];

    if (gOlState == 0) {
        ol_vrect(w, 0, 0, OL_W, 100, OC_SKY);
        ol_vrect(w, 0, 100, OL_W, 102, OC_HORIZON);
        ol_vrect(w, 0, 102, OL_W, OL_H, OC_GRASS_A);
        for (y = 0; y < 10; y++) {
            short t = (short)(102 + y * 10);
            short hw2 = (short)(8 + y * 14);
            ol_vrect(w, 160 - hw2, t, 160 + hw2, t + 10, OC_ROAD);
        }
        ol_vrect(w, 140, 150, 180, 176, OC_CAR);
        ol_vrect(w, 146, 154, 174, 162, OC_CARWIN);
        ol_vrect(w, 136, 170, 144, 178, OC_WHEEL);
        ol_vrect(w, 176, 170, 184, 178, OC_WHEEL);
        text_at(w->bounds.left + 4 + OLS(120), w->bounds.top + TBAR_H + 2 + OLS(30),
                "O U T L A S T", C_WHITE, C_BLUE, false);
        text_at(w->bounds.left + 4 + OLS(112), w->bounds.top + TBAR_H + 2 + OLS(190),
                "Press N to drive", C_CYAN, C_BLUE, false);
        return;
    }

    ol_vrect(w, 0, 12, OL_W, OL_HORIZON, OC_SKY);
    ol_vrect(w, 0, OL_HORIZON, OL_W, OL_HORIZON + 2, OC_HORIZON);

    for (y = OL_H - 1; y > OL_HORIZON + 1; y -= 2) {
        long z = 4800L / (y - OL_HORIZON);
        long hw = (16L * 256L) / z;
        long worldz = gOlZ + z * 4;
        short seg = (short)((worldz / OL_SEGLEN) & (OL_NSEG - 1));
        short center, l, rgt;
        dx += kOlCurve[seg];
        center = (short)(160 + (dx >> 5));
        l = (short)(center - hw); rgt = (short)(center + hw);
        ol_vrect(w, 0, y - 2, l, y, (seg & 1) ? OC_GRASS_B : OC_GRASS_A);
        ol_vrect(w, l, y - 2, rgt, y, (seg & 1) ? OC_ROAD_B : OC_ROAD);
        ol_vrect(w, rgt, y - 2, OL_W, y, (seg & 1) ? OC_GRASS_B : OC_GRASS_A);
        if (seg & 1)
            ol_vrect(w, center - 2, y - 2, center + 2, y, OC_STRIPE);
        if (y >= OL_H - 4) { gOlRoadL = l; gOlRoadR = rgt; }
        {
            short t;
            for (t = 0; t < 8; t++) {
                long rel = kOlTreeZ[t] - (gOlZ % OL_TRACKLEN);
                if (rel < 0) rel += OL_TRACKLEN;
                if (rel < 30 || rel > 400) continue;
                if (rel >= z * 4 - 8 && rel < z * 4 + 8) {
                    short th = (short)(1600 / (rel ? rel : 1));
                    short tw = (short)(800 / (rel ? rel : 1));
                    short tx;
                    if (th < 2) th = 2; if (th > 40) th = 40;
                    if (tw < 2) tw = 2; if (tw > 24) tw = 24;
                    tx = (t & 1) ? (short)(rgt + 4) : (short)(l - 4 - tw);
                    ol_vrect(w, (short)(tx + tw / 2 - tw / 8), (short)(y - th / 2), (short)(tx + tw / 2 + tw / 8), y, OC_TRUNK);
                    ol_vrect(w, tx, (short)(y - th), (short)(tx + tw), (short)(y - th / 2), OC_CANOPY);
                }
            }
        }
    }

    {
        short t;
        for (t = 0; t < 4; t++) {
            long rel = gOlTraffic[t] - (gOlZ % OL_TRACKLEN);
            if (rel < 0) rel += OL_TRACKLEN;
            if (rel >= 10 && rel <= 400) {
                short ch2 = (short)(1500 / rel), cw = (short)(2100 / rel);
                short cy, cx;
                if (ch2 < 2) ch2 = 2; if (ch2 > 40) ch2 = 40;
                if (cw < 3) cw = 3; if (cw > 50) cw = 50;
                cy = (short)(OL_HORIZON + 4800 / (rel / 4 + 25));
                if (cy > OL_H - 6) cy = OL_H - 6;
                cx = (short)(160 + (kOlTrafLane[t] ? 30 : -30) * (200 - (short)(rel / 2)) / 200);
                ol_vrect(w, (short)(cx - cw / 2), (short)(cy - ch2), (short)(cx + cw / 2), cy,
                         kOlTrafDir[t] ? OC_TRAF_SAME : OC_TRAF_ON);
                ol_vrect(w, (short)(cx - cw / 2 + 1), (short)(cy - ch2), (short)(cx + cw / 2 - 1),
                         (short)(cy - ch2 + (ch2 / 4 ? ch2 / 4 : 1)), OC_WHEEL);
            }
        }
    }

    if (!(gOlCrash & 4)) {
        ol_vrect(w, gOlX - 14, 168, gOlX + 14, 186, OC_CAR);
        ol_vrect(w, gOlX - 10, 171, gOlX + 10, 177, OC_CARWIN);
        ol_vrect(w, gOlX - 16, 182, gOlX - 10, 190, OC_WHEEL);
        ol_vrect(w, gOlX + 10, 182, gOlX + 16, 190, OC_WHEEL);
    }

    ol_vrect(w, 0, 0, OL_W, 12, OC_HUD);
    strcpy(hud, "Speed ");  fmt_u(gOlSpeed, num); strcat(hud, num);
    strcat(hud, "  Score "); fmt_u(gOlScore, num); strcat(hud, num);
    strcat(hud, "  Time ");  fmt_u(gOlTime, num);  strcat(hud, num);
    text_at(w->bounds.left + 8, w->bounds.top + TBAR_H + 14, hud, C_WHITE, C_BLUE, false);

    if (gOlState == 2) {
        ol_vrect(w, 80, 70, 240, 130, OC_HUD);
        text_at(w->bounds.left + 4 + OLS(124), w->bounds.top + TBAR_H + 2 + OLS(92),
                "GAME OVER", C_WHITE, C_BLUE, false);
        strcpy(hud, "Final score "); fmt_u(gOlScore, num); strcat(hud, num);
        text_at(w->bounds.left + 4 + OLS(104), w->bounds.top + TBAR_H + 2 + OLS(108),
                hud, C_CYAN, C_BLUE, false);
        text_at(w->bounds.left + 4 + OLS(110), w->bounds.top + TBAR_H + 2 + OLS(122),
                "N: new game", C_CYAN, C_BLUE, false);
    }
}

static Boolean outlast_key(char ch, short code, Boolean cmd)
{
    if (cmd) return false;
    if (ch == 'n' || ch == 'N') { ol_new_game(); return true; }
    if (gOlState != 1 || gOlCrash) return false;
    if (code == 0x7B || ch == 0x1C) { gOlX -= 9; if (gOlX < 40) gOlX = 40; return true; }
    if (code == 0x7C || ch == 0x1D) { gOlX += 9; if (gOlX > 280) gOlX = 280; return true; }
    if (code == 0x7E || ch == 0x1E) { gOlSpeed += 4; if (gOlSpeed > 60) gOlSpeed = 60; return true; }
    if (code == 0x7D || ch == 0x1F) { gOlSpeed -= 8; if (gOlSpeed < 0) gOlSpeed = 0; return true; }
    return false;
}

static void outlast_tick(void)
{
    UnoWin *w;
    long now = TickCount();
    if (gOlState != 1) return;
    if (now - gOlLastStep < 4) return;
    gOlLastStep = now;

    if (gOlCrash) {
        gOlCrash--;
        if (!gOlCrash) { gOlX = 160; gOlSpeed = 5; }
    } else {
        short seg = (short)((gOlZ / OL_SEGLEN) & (OL_NSEG - 1));
        if (gOlSpeed < 60) gOlSpeed++;
        if (gOlX < gOlRoadL || gOlX > gOlRoadR) {
            gOlSpeed -= 2;
            if (gOlSpeed < 5) gOlSpeed = 5;
        }
        gOlX -= kOlCurve[seg] / 8;
        if (gOlX < 40) gOlX = 40;
        if (gOlX > 280) gOlX = 280;
        gOlZ += gOlSpeed;
        gOlScore += gOlSpeed >> 2;
        {
            short t;
            for (t = 0; t < 4; t++) {
                if (kOlTrafDir[t]) gOlTraffic[t] += 5;
                else               gOlTraffic[t] -= 5;
                if (gOlTraffic[t] < 0) gOlTraffic[t] += OL_TRACKLEN;
                if (gOlTraffic[t] >= OL_TRACKLEN) gOlTraffic[t] -= OL_TRACKLEN;
                {
                    long rel = gOlTraffic[t] - (gOlZ % OL_TRACKLEN);
                    if (rel < 0) rel += OL_TRACKLEN;
                    if (rel < 15) {
                        short cx = (short)(160 + (kOlTrafLane[t] ? 30 : -30));
                        if (gOlX > cx - 25 && gOlX < cx + 25) gOlCrash = 30;
                    }
                }
            }
        }
        {
            short t;
            for (t = 0; t < 8; t++) {
                long rel = kOlTreeZ[t] - (gOlZ % OL_TRACKLEN);
                if (rel < 0) rel += OL_TRACKLEN;
                if (rel < 12 &&
                    ((t & 1) ? (gOlX > gOlRoadR - 5) : (gOlX < gOlRoadL + 5)))
                    gOlCrash = 30;
            }
        }
        if (gOlCrash) gOlSpeed = 0;
    }

    if (now - gOlLastSec >= 60) {
        gOlLastSec = now;
        if (--gOlTime <= 0) { gOlTime = 0; gOlState = 2; gm_stop(); }
    }

    w = find_app_window(APP_OUTLAST);
    if (w) draw_window(w);
}

static void outlast_closed(void){ gm_stop(); }

static const AppInterface kIface = {
    outlast_draw, outlast_key, 0, outlast_tick, 0, outlast_closed,
    "OutLast", { 70, 40, 562, 384 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
