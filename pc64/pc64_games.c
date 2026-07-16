/* ===========================================================================
 * UnoDOS/pc64 - native unoui games (see pc64_games.h).
 *
 * Each game is a unoui_canvas that draws itself with fb primitives, sized from
 * the rect it is handed - so the SAME code fills a window and a full screen
 * (unoui passes the whole screen as the rect in fullscreen). No mac_compat /
 * KernelApi bridge; the game owns its pixels and reads unoui events directly.
 * ======================================================================== */
#include "pc64_games.h"
#include "fb.h"

/* ---------------------------------------------------------------- Dostris --- */
#define DT_COLS 10
#define DT_ROWS 20

static const signed char kShape[7][4][8] = {
  { {0,1,1,1,2,1,3,1}, {2,0,2,1,2,2,2,3}, {0,2,1,2,2,2,3,2}, {1,0,1,1,1,2,1,3} },
  { {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1} },
  { {1,0,0,1,1,1,2,1}, {0,0,0,1,1,1,0,2}, {0,0,1,0,2,0,1,1}, {1,0,0,1,1,1,1,2} },
  { {1,0,2,0,0,1,1,1}, {0,0,0,1,1,1,1,2}, {1,0,2,0,0,1,1,1}, {0,0,0,1,1,1,1,2} },
  { {0,0,1,0,1,1,2,1}, {1,0,0,1,1,1,0,2}, {0,0,1,0,1,1,2,1}, {1,0,0,1,1,1,0,2} },
  { {0,0,0,1,1,1,2,1}, {0,0,1,0,0,1,0,2}, {0,0,1,0,2,0,2,1}, {1,0,1,1,0,2,1,2} },
  { {2,0,0,1,1,1,2,1}, {0,0,0,1,0,2,1,2}, {0,0,1,0,2,0,0,1}, {0,0,1,0,1,1,1,2} },
};
static const fb_px kCol[7] = {
    FB_RGB(0,210,210), FB_RGB(235,215,0), FB_RGB(170,70,225), FB_RGB(50,200,80),
    FB_RGB(230,60,60), FB_RGB(70,110,240), FB_RGB(240,150,40),
};
static const long kLineScore[5] = { 0, 40, 100, 300, 1200 };

static unsigned char dtBoard[DT_ROWS][DT_COLS];
static int  dtState;                     /* 0 idle, 1 play, 3 over */
static int  dtPiece, dtRot, dtCol, dtRow, dtNext;
static long dtScore, dtLines;
static int  dtLevel, dtDropTmr;
static unsigned long dtSeed = 1;

static int dt_rand7(void){ dtSeed = dtSeed*1103515245UL + 12345UL; return (int)((dtSeed>>16)%7); }

static int dt_fits(int p, int rot, int c0, int r0)
{
    int i; const signed char *sh = kShape[p][rot];
    for (i = 0; i < 4; i++) {
        int c = c0 + sh[i*2], r = r0 + sh[i*2+1];
        if (c < 0 || c >= DT_COLS || r >= DT_ROWS) return 0;
        if (r >= 0 && dtBoard[r][c]) return 0;
    }
    return 1;
}
static void dt_spawn(void)
{
    dtPiece = dtNext; dtNext = dt_rand7(); dtRot = 0; dtCol = 3; dtRow = -1; dtDropTmr = 0;
    if (!dt_fits(dtPiece, dtRot, dtCol, dtRow+1)) dtState = 3;   /* game over */
}
static void dt_new(void)
{
    int r, c; for (r = 0; r < DT_ROWS; r++) for (c = 0; c < DT_COLS; c++) dtBoard[r][c] = 0;
    dtScore = dtLines = 0; dtLevel = 1; dtSeed = (dtSeed ^ 0x9E3779B9u) | 1;
    dtNext = dt_rand7(); dtState = 1; dt_spawn();
}
static void dt_clear_lines(void)
{
    int r, c, n = 0;
    for (r = 0; r < DT_ROWS; r++) {
        int full = 1;
        for (c = 0; c < DT_COLS; c++) if (!dtBoard[r][c]) { full = 0; break; }
        if (full) { int rr; n++;
            for (rr = r; rr > 0; rr--) for (c = 0; c < DT_COLS; c++) dtBoard[rr][c] = dtBoard[rr-1][c];
            for (c = 0; c < DT_COLS; c++) dtBoard[0][c] = 0; }
    }
    if (n) { dtScore += kLineScore[n] * (dtLevel+1); dtLines += n;
             dtLevel = (int)(dtLines/10)+1; if (dtLevel > 15) dtLevel = 15; }
}
static void dt_lock(void)
{
    int i; const signed char *sh = kShape[dtPiece][dtRot];
    for (i = 0; i < 4; i++) { int c = dtCol+sh[i*2], r = dtRow+sh[i*2+1];
        if (r >= 0 && r < DT_ROWS && c >= 0 && c < DT_COLS) dtBoard[r][c] = (unsigned char)(dtPiece+1); }
    dt_clear_lines(); dt_spawn();
}

static void num(long v, char *o){ char t[16]; int n=0,k=0; if(v<=0)t[n++]='0'; while(v){t[n++]=(char)('0'+v%10);v/=10;} while(n)o[k++]=t[--n]; o[k]=0; }

static void dt_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    int boardW = r.w * 2 / 3, cell, bw, bh, bx, by, rr, cc, i, sx, ty, sc;
    char b[16];
    (void)w; (void)ctx;
    cell = boardW / DT_COLS; { int ch = r.h / DT_ROWS; if (ch < cell) cell = ch; }
    if (cell < 4) cell = 4;
    bw = cell*DT_COLS; bh = cell*DT_ROWS;
    bx = r.x + (boardW - bw)/2; if (bx < r.x+2) bx = r.x+2;
    by = r.y + (r.h - bh)/2;    if (by < r.y+2) by = r.y+2;
    fb_fill_rect(r.x, r.y, r.w, r.h, FB_RGB(8,10,20));
    fb_frame_rect(bx-2, by-2, bw+4, bh+4, FB_RGB(230,230,240));
    for (rr = 0; rr < DT_ROWS; rr++) for (cc = 0; cc < DT_COLS; cc++) if (dtBoard[rr][cc])
        fb_fill_rect(bx+cc*cell, by+rr*cell, cell-1, cell-1, kCol[dtBoard[rr][cc]-1]);
    if (dtState == 1) { const signed char *sh = kShape[dtPiece][dtRot];
        for (i = 0; i < 4; i++) { int c = dtCol+sh[i*2], ro = dtRow+sh[i*2+1];
            if (ro >= 0) fb_fill_rect(bx+c*cell, by+ro*cell, cell-1, cell-1, kCol[dtPiece]); } }
    /* sidebar */
    sx = r.x + boardW + 8; ty = by; sc = cell >= 14 ? 2 : 1;
    fb_big_text(sx, ty, "DOSTRIS", FB_RGB(180,90,230), -1, sc); ty += 12*sc + 8;
    fb_text(sx, ty, "Score", FB_RGB(0,200,200), -1); num(dtScore,b); fb_text(sx+56, ty, b, FB_RGB(255,255,255), -1); ty += 16;
    fb_text(sx, ty, "Lines", FB_RGB(0,200,200), -1); num(dtLines,b); fb_text(sx+56, ty, b, FB_RGB(255,255,255), -1); ty += 16;
    fb_text(sx, ty, "Level", FB_RGB(0,200,200), -1); num(dtLevel,b); fb_text(sx+56, ty, b, FB_RGB(255,255,255), -1); ty += 24;
    if (dtState == 0)      fb_text(sx, ty, "N: new game", FB_RGB(255,255,255), -1);
    else if (dtState == 3) { fb_text(sx, ty, "GAME OVER", FB_RGB(240,80,80), -1);
                             fb_text(sx, ty+16, "N: retry", FB_RGB(200,200,210), -1); }
    else { fb_text(sx, ty, "<- -> move", FB_RGB(160,170,200), -1);
           fb_text(sx, ty+14, "up: rotate", FB_RGB(160,170,200), -1);
           fb_text(sx, ty+28, "dn: drop", FB_RGB(160,170,200), -1);
           fb_text(sx, ty+42, "spc: slam", FB_RGB(160,170,200), -1); }
}

static int dt_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w; (void)ctx;
    if (e->kind == UI_EV_CHAR) {
        if (e->ch == 'n' || e->ch == 'N') { dt_new(); return 1; }
        if (e->ch == ' ' && dtState == 1) {
            while (dt_fits(dtPiece, dtRot, dtCol, dtRow+1)) { dtRow++; dtScore += 2; }
            dt_lock(); return 1;
        }
        return 0;
    }
    if (e->kind != UI_EV_KEY || dtState != 1) return 0;
    switch (e->key) {
    case UI_KEY_LEFT:  if (dt_fits(dtPiece, dtRot, dtCol-1, dtRow)) dtCol--; return 1;
    case UI_KEY_RIGHT: if (dt_fits(dtPiece, dtRot, dtCol+1, dtRow)) dtCol++; return 1;
    case UI_KEY_UP:    { int nr = (dtRot+1)&3; if (dt_fits(dtPiece, nr, dtCol, dtRow)) dtRot = nr; } return 1;
    case UI_KEY_DOWN:  if (dt_fits(dtPiece, dtRot, dtCol, dtRow+1)) { dtRow++; dtScore++; dtDropTmr = 0; } else dt_lock(); return 1;
    default: return 0;
    }
}
static void dt_tick(void)
{
    int iv;
    if (dtState != 1) return;
    iv = 32 - dtLevel*2; if (iv < 4) iv = 4;
    if (++dtDropTmr < iv) return;
    dtDropTmr = 0;
    if (dt_fits(dtPiece, dtRot, dtCol, dtRow+1)) dtRow++; else dt_lock();
}

/* ------------------------------------------------------ registry ----------- */
static unoui_canvas g_dostris = { dt_draw, dt_event, 0 };

unoui_canvas *pc64_game_canvas(int game)
{
    switch (game) {
    case GAME_DOSTRIS: return &g_dostris;
    default:           return 0;   /* not yet native -> shell uses the bridge */
    }
}
void pc64_game_open(int game) { if (game == GAME_DOSTRIS) { dtState = 0; dt_new(); } }
void pc64_game_tick(int game) { if (game == GAME_DOSTRIS) dt_tick(); }
const char *pc64_game_name(int game)
{
    static const char *n[PC64_NGAMES] = { "Dostris", "Pac-Man", "OutLast" };
    return (game >= 0 && game < PC64_NGAMES) ? n[game] : "Game";
}
