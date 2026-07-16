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

/* ---------------------------------------------------------------- Pac-Man --- *
 * Logic is the faithful maze + ghost-AI port (native 8px-tile coordinate space,
 * TickCount replaced with a frame counter); only the drawing is rewritten to
 * scale to the rect. */
#define PM_COLS 28
#define PM_ROWS 25
#define PM_TILE 8
static const unsigned char kPmMaze[PM_ROWS][PM_COLS] = {
 {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
 {1,3,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,3,1},
 {1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1},
 {1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1},
 {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
 {1,2,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,2,1},
 {1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1},
 {1,1,1,1,1,2,1,1,1,1,0,1,0,0,0,0,1,0,1,1,1,1,2,1,1,1,1,1},
 {0,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0},
 {0,0,0,0,1,2,1,0,1,1,1,1,5,0,0,5,1,1,1,1,0,1,2,1,0,0,0,0},
 {1,1,1,1,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,1,1,1,1},
 {0,0,0,0,0,2,0,0,1,4,4,4,4,4,4,4,4,4,4,1,0,0,2,0,0,0,0,0},
 {0,0,0,0,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,0,0,0,0},
 {0,0,0,0,1,2,1,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1,2,1,0,0,0,0},
 {1,1,1,1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,1,1,1,1},
 {1,2,2,2,2,2,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,2,2,2,2,2,1},
 {1,2,1,1,1,2,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,2,1,1,1,2,1},
 {1,3,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,3,1},
 {1,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,1},
 {1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1},
 {1,2,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,2,1},
 {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
 {1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1},
 {1,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,1},
 {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};
enum { PM_TITLE=0, PM_READY, PM_PLAY, PM_DEAD, PM_OVER };
enum { GH_HOUSE=0, GH_SCATTER, GH_CHASE, GH_FRIGHT, GH_EATEN };
enum { D_UP=0, D_LEFT, D_DOWN, D_RIGHT, D_NONE };
typedef struct { short x,y,dir,state,timer; } PmGhost;
static unsigned char gPmMaze[PM_ROWS][PM_COLS];
static short gPmState=PM_TITLE, gPmX,gPmY,gPmDir,gPmNextDir;
static PmGhost gPmGh[3];
static long gPmScore,gPmHi; static short gPmLives,gPmLevel,gPmDots;
static short gPmMode,gPmFright,gPmKills; static long gPmModeT,gPmLastStep,gPmStateT;
static unsigned long gPmSeed=7, pmFrame;
static const short kPmModeDur[8]={127,364,127,364,91,364,91,0x7FFF};
static const short kPmDX[4]={0,-1,0,1}, kPmDY[4]={-1,0,1,0};
static const short kPmCornX[3]={26,1,1}, kPmCornY[3]={1,1,23};

static short pm_rand(short n){ gPmSeed=gPmSeed*1103515245UL+12345UL; return (short)((gPmSeed>>16)%n); }
static int pm_walkable(short tx,short ty,short forGhost,short eaten){
    unsigned char t; if(ty<0||ty>=PM_ROWS) return 0;
    if(tx<0)tx+=PM_COLS; if(tx>=PM_COLS)tx-=PM_COLS; t=gPmMaze[ty][tx];
    if(t==1) return 0; if(t==4||t==5) return forGhost&&eaten; return 1;
}
static void pm_reset_actors(void){
    gPmX=14*PM_TILE; gPmY=19*PM_TILE; gPmDir=D_LEFT; gPmNextDir=D_LEFT;
    gPmGh[0].x=14*PM_TILE; gPmGh[0].y=10*PM_TILE; gPmGh[0].dir=D_LEFT; gPmGh[0].state=GH_SCATTER; gPmGh[0].timer=0;
    gPmGh[1].x=13*PM_TILE; gPmGh[1].y=12*PM_TILE; gPmGh[1].dir=D_UP; gPmGh[1].state=GH_HOUSE; gPmGh[1].timer=100;
    gPmGh[2].x=15*PM_TILE; gPmGh[2].y=12*PM_TILE; gPmGh[2].dir=D_UP; gPmGh[2].state=GH_HOUSE; gPmGh[2].timer=200;
    gPmFright=0; gPmKills=0;
}
static void pm_load_maze(void){ short r,c; gPmDots=0;
    for(r=0;r<PM_ROWS;r++) for(c=0;c<PM_COLS;c++){ gPmMaze[r][c]=kPmMaze[r][c];
        if(kPmMaze[r][c]==2||kPmMaze[r][c]==3) gPmDots++; } }
static void pm_new_game(void){ gPmScore=0; gPmLives=3; gPmLevel=1; gPmMode=0; gPmModeT=0;
    pm_load_maze(); pm_reset_actors(); gPmState=PM_READY; gPmStateT=pmFrame+40;
    gPmLastStep=pmFrame; gPmSeed=(pmFrame^0x1234567u)|1; }
static short pm_mode_state(void){ return (gPmMode&1)?GH_CHASE:GH_SCATTER; }
static void pm_ghost_steer(short gi){
    PmGhost *g=&gPmGh[gi]; short gtx=g->x/PM_TILE,gty=g->y/PM_TILE;
    short ptx=gPmX/PM_TILE,pty=gPmY/PM_TILE; short tx,ty,best=-1,bestd=0x7FFF,d,rev;
    if(g->state==GH_FRIGHT){ short tries=8; rev=g->dir^2;
        while(tries--){ d=pm_rand(4); if(d==rev) continue;
            if(pm_walkable(gtx+kPmDX[d],gty+kPmDY[d],1,0)){g->dir=d;return;} } return; }
    if(g->state==GH_EATEN){ tx=14; ty=10; }
    else if(g->state==GH_CHASE){
        if(gi==0){tx=ptx;ty=pty;}
        else if(gi==1){ tx=ptx+kPmDX[gPmDir]*4; ty=pty+kPmDY[gPmDir]*4; }
        else { short md=(short)((gtx>ptx?gtx-ptx:ptx-gtx)+(gty>pty?gty-pty:pty-gty));
            if(md<=8){tx=1;ty=1;} else {tx=ptx;ty=pty;} }
    } else { tx=kPmCornX[gi]; ty=kPmCornY[gi]; }
    rev=g->dir^2;
    for(d=0;d<4;d++){ short nx,ny; if(d==rev) continue; nx=gtx+kPmDX[d]; ny=gty+kPmDY[d];
        if(!pm_walkable(nx,ny,1,g->state==GH_EATEN)) continue;
        if(nx<0)nx+=PM_COLS; if(nx>=PM_COLS)nx-=PM_COLS;
        { short ddx=nx>tx?nx-tx:tx-nx, ddy=ny>ty?ny-ty:ty-ny, dist=(short)(ddx+ddy);
          if(dist<bestd){bestd=dist;best=d;} } }
    if(best>=0) g->dir=best;
}
static void pm_kill_pac(void){ gPmLives--;
    if(gPmLives<=0){ gPmState=PM_OVER; if(gPmScore>gPmHi)gPmHi=gPmScore; }
    else { pm_reset_actors(); gPmState=PM_READY; gPmStateT=pmFrame+40; } }
static void pm_step(void){
    short i,sub;
    if(gPmFright>0){ gPmFright--; if(!gPmFright) for(i=0;i<3;i++) if(gPmGh[i].state==GH_FRIGHT) gPmGh[i].state=pm_mode_state(); }
    else { gPmModeT++; if(gPmModeT>=kPmModeDur[gPmMode>7?7:gPmMode]){ gPmModeT=0; if(gPmMode<7)gPmMode++;
        for(i=0;i<3;i++) if(gPmGh[i].state==GH_SCATTER||gPmGh[i].state==GH_CHASE){ gPmGh[i].state=pm_mode_state(); gPmGh[i].dir^=2; } } }
    for(sub=0;sub<2;sub++){
        if((gPmX%PM_TILE)==0&&(gPmY%PM_TILE)==0){ short tx=gPmX/PM_TILE,ty=gPmY/PM_TILE; unsigned char *t=&gPmMaze[ty][tx];
            if(*t==2){*t=0;gPmScore+=10;gPmDots--;}
            else if(*t==3){*t=0;gPmScore+=50;gPmDots--;gPmFright=200;gPmKills=0;
                for(i=0;i<3;i++) if(gPmGh[i].state==GH_SCATTER||gPmGh[i].state==GH_CHASE){gPmGh[i].state=GH_FRIGHT;gPmGh[i].dir^=2;} }
            if(!gPmDots){ gPmLevel++; pm_load_maze(); pm_reset_actors(); gPmState=PM_READY; gPmStateT=pmFrame+40; return; }
            if(pm_walkable(tx+kPmDX[gPmNextDir],ty+kPmDY[gPmNextDir],0,0)) gPmDir=gPmNextDir;
            if(pm_walkable(tx+kPmDX[gPmDir],ty+kPmDY[gPmDir],0,0)){ gPmX+=kPmDX[gPmDir]; gPmY+=kPmDY[gPmDir]; }
        } else { gPmX+=kPmDX[gPmDir]; gPmY+=kPmDY[gPmDir]; }
        if(gPmX<0)gPmX=(PM_COLS-1)*PM_TILE; if(gPmX>(PM_COLS-1)*PM_TILE)gPmX=0;
        for(i=0;i<3;i++){ PmGhost *g=&gPmGh[i];
            if(g->state==GH_HOUSE){ if(sub==0&&--g->timer<=0){ g->x=14*PM_TILE; g->y=10*PM_TILE; g->dir=D_LEFT; g->state=pm_mode_state(); } continue; }
            if((g->x%PM_TILE)==0&&(g->y%PM_TILE)==0){ if(g->state==GH_EATEN&&g->x==14*PM_TILE&&g->y==10*PM_TILE) g->state=pm_mode_state(); pm_ghost_steer(i); }
            if(g->state==GH_FRIGHT&&sub==1) continue;
            if(pm_walkable((g->x+kPmDX[g->dir]*PM_TILE)/PM_TILE,(g->y+kPmDY[g->dir]*PM_TILE)/PM_TILE,1,g->state==GH_EATEN)||(g->x%PM_TILE)||(g->y%PM_TILE)){ g->x+=kPmDX[g->dir]; g->y+=kPmDY[g->dir]; }
            if(g->x<0)g->x=(PM_COLS-1)*PM_TILE; if(g->x>(PM_COLS-1)*PM_TILE)g->x=0;
            { short dx=g->x>gPmX?g->x-gPmX:gPmX-g->x, dy=g->y>gPmY?g->y-gPmY:gPmY-g->y;
              if(dx<6&&dy<6){ if(g->state==GH_FRIGHT){ g->state=GH_EATEN; gPmScore+=200L<<gPmKills; if(gPmKills<3)gPmKills++; }
                else if(g->state!=GH_EATEN){ pm_kill_pac(); return; } } }
        }
    }
}
static void pm_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    int mazePx = r.w*3/4, ts, mw, mh, bx, by, rr, cc, i, sx, ty; char b[16];
    fb_px pcol[3] = { FB_RGB(230,40,30), FB_RGB(250,150,200), FB_RGB(245,160,50) };
    (void)w; (void)ctx;
    ts = mazePx/PM_COLS; { int th = r.h/PM_ROWS; if (th < ts) ts = th; } if (ts < 2) ts = 2;
    mw = ts*PM_COLS; mh = ts*PM_ROWS;
    bx = r.x + (mazePx - mw)/2; if (bx < r.x) bx = r.x;
    by = r.y + (r.h - mh)/2;    if (by < r.y) by = r.y;
    fb_fill_rect(r.x, r.y, r.w, r.h, FB_RGB(0,0,0));
#define PSX(px) (bx + (px)*ts/PM_TILE)
#define PSY(py) (by + (py)*ts/PM_TILE)
    if (gPmState==PM_TITLE) {
        fb_big_text(bx+mw/2-84, by+mh/3, "PAC-MAN", FB_RGB(240,230,60), -1, 2);
        fb_text(bx+mw/2-44, by+mh/3+40, "N: new game", FB_RGB(0,200,200), -1);
        return;
    }
    for (rr=0; rr<PM_ROWS; rr++) for (cc=0; cc<PM_COLS; cc++) { unsigned char t=gPmMaze[rr][cc];
        int x=bx+cc*ts, y=by+rr*ts;
        if (t==1) fb_fill_rect(x, y, ts-1, ts-1, FB_RGB(30,50,220));
        else if (t==2) fb_fill_rect(x+ts/2-1, y+ts/2-1, ts>4?2:1, ts>4?2:1, FB_RGB(250,240,200));
        else if (t==3) fb_fill_rect(x+ts/4, y+ts/4, ts/2, ts/2, FB_RGB(255,255,255));
        else if (t==5) fb_fill_rect(x, y+ts/3, ts, ts/3, FB_RGB(230,120,220)); }
    if (gPmState!=PM_DEAD) fb_fill_rect(PSX(gPmX), PSY(gPmY), ts, ts, FB_RGB(245,230,40));  /* pac */
    for (i=0; i<3; i++) { PmGhost *g=&gPmGh[i]; fb_px c=pcol[i];
        if (g->state==GH_FRIGHT) c=(gPmFright<70&&(gPmFright&8))?FB_RGB(240,240,240):FB_RGB(40,40,210);
        else if (g->state==GH_EATEN) c=FB_RGB(200,200,220);
        fb_fill_rect(PSX(g->x), PSY(g->y), ts, ts, c);
        fb_fill_rect(PSX(g->x)+ts/4, PSY(g->y)+ts/4, 2, 2, FB_RGB(255,255,255)); }
    /* sidebar */
    sx = r.x + mazePx + 8; ty = by;
    fb_text(sx, ty, "SCORE", FB_RGB(0,200,200), -1); num(gPmScore,b); fb_text(sx, ty+12, b, FB_RGB(255,255,255), -1); ty += 34;
    fb_text(sx, ty, "LIVES", FB_RGB(0,200,200), -1); num(gPmLives,b); fb_text(sx, ty+12, b, FB_RGB(255,255,255), -1); ty += 34;
    fb_text(sx, ty, "LEVEL", FB_RGB(0,200,200), -1); num(gPmLevel,b); fb_text(sx, ty+12, b, FB_RGB(255,255,255), -1); ty += 34;
    if (gPmState==PM_READY)     fb_text(bx+mw/2-24, by+mh/2, "READY!", FB_RGB(240,230,60), -1);
    else if (gPmState==PM_OVER) fb_text(bx+mw/2-40, by+mh/2, "GAME OVER", FB_RGB(240,80,80), -1);
#undef PSX
#undef PSY
}
static int pm_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e=(const unoui_event*)ev; (void)w;(void)ctx;
    if (e->kind==UI_EV_CHAR) { if (e->ch=='n'||e->ch=='N') { pm_new_game(); return 1; } return 0; }
    if (e->kind!=UI_EV_KEY || (gPmState!=PM_PLAY && gPmState!=PM_READY)) return 0;
    switch (e->key) {
    case UI_KEY_UP:    gPmNextDir=D_UP;    return 1;
    case UI_KEY_DOWN:  gPmNextDir=D_DOWN;  return 1;
    case UI_KEY_LEFT:  gPmNextDir=D_LEFT;  return 1;
    case UI_KEY_RIGHT: gPmNextDir=D_RIGHT; return 1;
    default: return 0;
    }
}
static void pm_tick(void)
{
    pmFrame++;
    if (gPmState==PM_TITLE || gPmState==PM_OVER) return;
    if (gPmState==PM_READY) { if (pmFrame>=(unsigned long)gPmStateT) gPmState=PM_PLAY; else return; }
    if (pmFrame - (unsigned long)gPmLastStep < 2) return;   /* ~30 Hz */
    gPmLastStep=pmFrame; pm_step();
}

/* ---------------------------------------------------------------- OutLast --- *
 * Pseudo-3D racer. Virtual 320x200 space; the native ol_vrect maps it to the
 * canvas rect, so the same perspective code fills a window or the full screen.
 * Frame-count timing; music dropped (native games are silent for now). */
#define OL_W 320
#define OL_H 200
#define OL_HORIZON 80
#define OL_SEGLEN 80
#define OL_NSEG 32
#define OL_TRACKLEN (OL_SEGLEN*OL_NSEG)
enum { OC_SKY,OC_HORIZON,OC_GRASS_A,OC_GRASS_B,OC_ROAD,OC_ROAD_B,OC_STRIPE,OC_CAR,
       OC_CARWIN,OC_WHEEL,OC_TRAF_ON,OC_TRAF_SAME,OC_TRUNK,OC_CANOPY,OC_HUD,OC_N };
static const fb_px kOlCol[OC_N] = {
    FB_RGB(110,170,240), FB_RGB(250,200,120), FB_RGB(51,153,51), FB_RGB(30,120,40),
    FB_RGB(110,110,110), FB_RGB(100,100,100), FB_RGB(240,220,60), FB_RGB(220,40,40),
    FB_RGB(140,220,240), FB_RGB(25,25,25), FB_RGB(245,245,245), FB_RGB(240,200,60),
    FB_RGB(120,80,40), FB_RGB(30,140,45), FB_RGB(10,10,40),
};
static const signed char kOlCurve[OL_NSEG] = {
    0,0,0,0,0,0,0,0, 5,15,25,30, 30,25,15,5,
    0,0,0,0,0,0,0,0, -5,-15,-25,-30, -30,-25,-15,-5 };
static const short kOlTreeZ[8] = { 200,520,900,1300,1600,1900,2200,2480 };
static short gOlState, gOlX=160, gOlSpeed; static long gOlZ, gOlScore;
static short gOlTime=60, gOlCrash; static long gOlLastStep, gOlLastSec, gOlTraffic[4];
static const unsigned char kOlTrafDir[4]={1,1,0,0}, kOlTrafLane[4]={0,1,0,1};
static short gOlRoadL=100, gOlRoadR=220; static unsigned long olFrame;
static unoui_rect g_olR;

static void ol_v(short x0, short y0, short x1, short y1, int col)
{
    int sx0, sy0, sx1, sy1;
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < 0) x0 = 0; if (x1 > OL_W) x1 = OL_W;
    if (y0 < 0) y0 = 0; if (y1 > OL_H) y1 = OL_H;
    sx0 = g_olR.x + x0*g_olR.w/OL_W; sx1 = g_olR.x + x1*g_olR.w/OL_W;
    sy0 = g_olR.y + y0*g_olR.h/OL_H; sy1 = g_olR.y + y1*g_olR.h/OL_H;
    fb_fill_rect(sx0, sy0, sx1-sx0 > 0 ? sx1-sx0 : 1, sy1-sy0 > 0 ? sy1-sy0 : 1, kOlCol[col]);
}
static int ol_tx(int vx){ return g_olR.x + vx*g_olR.w/OL_W; }
static int ol_ty(int vy){ return g_olR.y + vy*g_olR.h/OL_H; }

static void ol_new(void)
{
    gOlX=160; gOlSpeed=0; gOlZ=0; gOlScore=0; gOlTime=60; gOlCrash=0;
    gOlTraffic[0]=400; gOlTraffic[1]=1600; gOlTraffic[2]=800; gOlTraffic[3]=2000;
    gOlLastStep=gOlLastSec=olFrame; gOlState=1;
}
static void ol_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    short y; long dx=0; char hud[64];
    (void)w; (void)ctx; g_olR = r;
    if (gOlState==0) {
        ol_v(0,0,OL_W,100,OC_SKY); ol_v(0,100,OL_W,102,OC_HORIZON); ol_v(0,102,OL_W,OL_H,OC_GRASS_A);
        for (y=0;y<10;y++){ short t=(short)(102+y*10), hw2=(short)(8+y*14); ol_v(160-hw2,t,160+hw2,t+10,OC_ROAD); }
        ol_v(140,150,180,176,OC_CAR); ol_v(146,154,174,162,OC_CARWIN);
        fb_big_text(ol_tx(112), ol_ty(30), "OUTLAST", FB_RGB(255,255,255), -1, 2);
        fb_text(ol_tx(112), ol_ty(180), "Press N to drive", FB_RGB(0,200,200), -1);
        return;
    }
    ol_v(0,12,OL_W,OL_HORIZON,OC_SKY); ol_v(0,OL_HORIZON,OL_W,OL_HORIZON+2,OC_HORIZON);
    for (y=OL_H-1; y>OL_HORIZON+1; y-=2) {
        long z=4800L/(y-OL_HORIZON), worldz=gOlZ+z*4; long hw=(16L*256L)/z;
        short seg=(short)((worldz/OL_SEGLEN)&(OL_NSEG-1)), center, l, rgt;
        dx += kOlCurve[seg]; center=(short)(160+(dx>>5));
        l=(short)(center-hw); rgt=(short)(center+hw);
        ol_v(0,y-2,l,y,(seg&1)?OC_GRASS_B:OC_GRASS_A);
        ol_v(l,y-2,rgt,y,(seg&1)?OC_ROAD_B:OC_ROAD);
        ol_v(rgt,y-2,OL_W,y,(seg&1)?OC_GRASS_B:OC_GRASS_A);
        if (seg&1) ol_v(center-2,y-2,center+2,y,OC_STRIPE);
        if (y>=OL_H-4){ gOlRoadL=l; gOlRoadR=rgt; }
        { short t; for (t=0;t<8;t++){ long rel=kOlTreeZ[t]-(gOlZ%OL_TRACKLEN); if(rel<0)rel+=OL_TRACKLEN;
            if (rel<30||rel>400) continue;
            if (rel>=z*4-8 && rel<z*4+8){ short th=(short)(1600/(rel?rel:1)), tw=(short)(800/(rel?rel:1)), tx;
                if(th<2)th=2; if(th>40)th=40; if(tw<2)tw=2; if(tw>24)tw=24;
                tx=(t&1)?(short)(rgt+4):(short)(l-4-tw);
                ol_v((short)(tx+tw/2-tw/8),(short)(y-th/2),(short)(tx+tw/2+tw/8),y,OC_TRUNK);
                ol_v(tx,(short)(y-th),(short)(tx+tw),(short)(y-th/2),OC_CANOPY); } } }
    }
    { short t; for (t=0;t<4;t++){ long rel=gOlTraffic[t]-(gOlZ%OL_TRACKLEN); if(rel<0)rel+=OL_TRACKLEN;
        if (rel>=10&&rel<=400){ short ch2=(short)(1500/rel), cw=(short)(2100/rel), cy, cx;
            if(ch2<2)ch2=2; if(ch2>40)ch2=40; if(cw<3)cw=3; if(cw>50)cw=50;
            cy=(short)(OL_HORIZON+4800/(rel/4+25)); if(cy>OL_H-6)cy=OL_H-6;
            cx=(short)(160+(kOlTrafLane[t]?30:-30)*(200-(short)(rel/2))/200);
            ol_v((short)(cx-cw/2),(short)(cy-ch2),(short)(cx+cw/2),cy, kOlTrafDir[t]?OC_TRAF_SAME:OC_TRAF_ON); } } }
    if (!(gOlCrash&4)) {
        ol_v(gOlX-14,168,gOlX+14,186,OC_CAR); ol_v(gOlX-10,171,gOlX+10,177,OC_CARWIN);
        ol_v(gOlX-16,182,gOlX-10,190,OC_WHEEL); ol_v(gOlX+10,182,gOlX+16,190,OC_WHEEL);
    }
    ol_v(0,0,OL_W,12,OC_HUD);
    { char nb[16], *p=hud; const char *s="Speed "; while(*s)*p++=*s++; num(gOlSpeed,nb); { char *q=nb; while(*q)*p++=*q++; }
      s="  Score "; while(*s)*p++=*s++; num(gOlScore,nb); { char *q=nb; while(*q)*p++=*q++; }
      s="  Time "; while(*s)*p++=*s++; num(gOlTime,nb); { char *q=nb; while(*q)*p++=*q++; } *p=0; }
    fb_text(g_olR.x+6, ol_ty(4), hud, FB_RGB(255,255,255), -1);
    if (gOlState==2) {
        ol_v(80,70,240,130,OC_HUD);
        fb_text(ol_tx(120), ol_ty(90), "GAME OVER", FB_RGB(255,255,255), -1);
        fb_text(ol_tx(108), ol_ty(112), "N: new game", FB_RGB(0,200,200), -1);
    }
}
static int ol_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e=(const unoui_event*)ev; (void)w;(void)ctx;
    if (e->kind==UI_EV_CHAR) { if (e->ch=='n'||e->ch=='N'){ ol_new(); return 1; } return 0; }
    if (e->kind!=UI_EV_KEY || gOlState!=1 || gOlCrash) return 0;
    switch (e->key) {
    case UI_KEY_LEFT:  gOlX-=9; if(gOlX<40)gOlX=40;    return 1;
    case UI_KEY_RIGHT: gOlX+=9; if(gOlX>280)gOlX=280;  return 1;
    case UI_KEY_UP:    gOlSpeed+=4; if(gOlSpeed>60)gOlSpeed=60; return 1;
    case UI_KEY_DOWN:  gOlSpeed-=8; if(gOlSpeed<0)gOlSpeed=0;   return 1;
    default: return 0;
    }
}
static void ol_tick(void)
{
    olFrame++;
    if (gOlState!=1) return;
    if (olFrame-(unsigned long)gOlLastStep < 2) return;   /* ~30 Hz */
    gOlLastStep=olFrame;
    if (gOlCrash) { gOlCrash--; if (!gOlCrash){ gOlX=160; gOlSpeed=5; } }
    else {
        short seg=(short)((gOlZ/OL_SEGLEN)&(OL_NSEG-1)); short t;
        if (gOlSpeed<60) gOlSpeed++;
        if (gOlX<gOlRoadL||gOlX>gOlRoadR){ gOlSpeed-=2; if(gOlSpeed<5)gOlSpeed=5; }
        gOlX -= kOlCurve[seg]/8; if(gOlX<40)gOlX=40; if(gOlX>280)gOlX=280;
        gOlZ += gOlSpeed; gOlScore += gOlSpeed>>2;
        for (t=0;t<4;t++){ if(kOlTrafDir[t])gOlTraffic[t]+=5; else gOlTraffic[t]-=5;
            if(gOlTraffic[t]<0)gOlTraffic[t]+=OL_TRACKLEN; if(gOlTraffic[t]>=OL_TRACKLEN)gOlTraffic[t]-=OL_TRACKLEN;
            { long rel=gOlTraffic[t]-(gOlZ%OL_TRACKLEN); if(rel<0)rel+=OL_TRACKLEN;
              if(rel<15){ short cx=(short)(160+(kOlTrafLane[t]?30:-30)); if(gOlX>cx-25&&gOlX<cx+25)gOlCrash=30; } } }
        for (t=0;t<8;t++){ long rel=kOlTreeZ[t]-(gOlZ%OL_TRACKLEN); if(rel<0)rel+=OL_TRACKLEN;
            if(rel<12 && ((t&1)?(gOlX>gOlRoadR-5):(gOlX<gOlRoadL+5))) gOlCrash=30; }
        if (gOlCrash) gOlSpeed=0;
    }
    if (olFrame-(unsigned long)gOlLastSec >= 60) { gOlLastSec=olFrame; if(--gOlTime<=0){ gOlTime=0; gOlState=2; } }
}

/* ------------------------------------------------------ registry ----------- */
static unoui_canvas g_dostris = { dt_draw, dt_event, 0 };
static unoui_canvas g_pacman  = { pm_draw, pm_event, 0 };
static unoui_canvas g_outlast = { ol_draw, ol_event, 0 };

unoui_canvas *pc64_game_canvas(int game)
{
    switch (game) {
    case GAME_DOSTRIS: return &g_dostris;
    case GAME_PACMAN:  return &g_pacman;
    case GAME_OUTLAST: return &g_outlast;
    default:           return 0;
    }
}
void pc64_game_open(int game)
{
    if (game == GAME_DOSTRIS)      { dtState = 0; dt_new(); }
    else if (game == GAME_PACMAN)  { gPmState = PM_TITLE; }
    else if (game == GAME_OUTLAST) { gOlState = 0; }
}
void pc64_game_tick(int game)
{
    if (game == GAME_DOSTRIS)      dt_tick();
    else if (game == GAME_PACMAN)  pm_tick();
    else if (game == GAME_OUTLAST) ol_tick();
}
const char *pc64_game_name(int game)
{
    static const char *n[PC64_NGAMES] = { "Dostris", "Pac-Man", "OutLast" };
    return (game >= 0 && game < PC64_NGAMES) ? n[game] : "Game";
}
