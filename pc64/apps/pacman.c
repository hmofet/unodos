/* Pac-Man app module (APP_PACMAN). Separate artifact -> app07.so.
   Faithful port of the pacman_* block from unodos.c. */
#include "uno_mod.h"

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

enum { PM_TITLE=0, PM_READY, PM_PLAY, PM_DEAD, PM_OVER, PM_LEVELUP };
enum { GH_HOUSE=0, GH_SCATTER, GH_CHASE, GH_FRIGHT, GH_EATEN };
enum { D_UP=0, D_LEFT, D_DOWN, D_RIGHT, D_NONE };
typedef struct { short x,y,dir,state,timer; } PmGhost;

static unsigned char gPmMaze[PM_ROWS][PM_COLS];
static short gPmState=PM_TITLE;
static short gPmX,gPmY,gPmDir,gPmNextDir;
static PmGhost gPmGh[3];
static long gPmScore,gPmHi;
static short gPmLives,gPmLevel,gPmDots;
static short gPmMode,gPmFright,gPmKills;
static long gPmModeT,gPmLastStep,gPmStateT;
static unsigned long gPmSeed=7;
static const short kPmModeDur[8]={127,364,127,364,91,364,91,0x7FFF};
static const short kPmDX[4]={0,-1,0,1};
static const short kPmDY[4]={-1,0,1,0};
static const short kPmCornX[3]={26,1,1};
static const short kPmCornY[3]={1,1,23};

static short pm_rand(short n){ gPmSeed=gPmSeed*1103515245UL+12345UL; return (short)((gPmSeed>>16)%n); }
static Boolean pm_walkable(short tx,short ty,short forGhost,short eaten){
    unsigned char t; if(ty<0||ty>=PM_ROWS) return false;
    if(tx<0)tx+=PM_COLS; if(tx>=PM_COLS)tx-=PM_COLS; t=gPmMaze[ty][tx];
    if(t==1) return false; if(t==4) return forGhost&&eaten; if(t==5) return forGhost&&eaten; return true;
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
    pm_load_maze(); pm_reset_actors(); gPmState=PM_READY; gPmStateT=TickCount()+66;
    gPmLastStep=TickCount(); gPmSeed=(unsigned long)TickCount()|1; }
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
    if(gPmLives<=0){ gPmState=PM_OVER; if(gPmScore>gPmHi)gPmHi=gPmScore; gm_stop(); }
    else { pm_reset_actors(); gPmState=PM_READY; gPmStateT=TickCount()+66; } }
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
            if(!gPmDots){ gPmLevel++; pm_load_maze(); pm_reset_actors(); gPmState=PM_READY; gPmStateT=TickCount()+66; return; }
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
static void pm_tile_rect(UnoWin *w, short tx, short ty, Rect *r){
    SetRect(r,w->bounds.left+4+tx*PM_TILE,w->bounds.top+TBAR_H+2+ty*PM_TILE,
            w->bounds.left+4+tx*PM_TILE+PM_TILE,w->bounds.top+TBAR_H+2+ty*PM_TILE+PM_TILE);
}
static void pacman_draw(UnoWin *w){
    short r,c,i,px; Rect q; char num[16];
    SetRect(&q,w->bounds.left+4,w->bounds.top+TBAR_H+2,w->bounds.left+4+PM_COLS*PM_TILE,w->bounds.top+TBAR_H+2+PM_ROWS*PM_TILE);
    { RGBColor blk={0,0,0}; RGBForeColor(&blk); PaintRect(&q); RGBForeColor(&kBlack); }
    if(gPmState==PM_TITLE){ text_at(w->bounds.left+4+84,w->bounds.top+TBAR_H+60,"P A C - M A N",C_WHITE,C_BLUE,false);
        text_at(w->bounds.left+4+76,w->bounds.top+TBAR_H+110,"N: new game",C_CYAN,C_BLUE,false); return; }
    for(r=0;r<PM_ROWS;r++) for(c=0;c<PM_COLS;c++){ unsigned char t=gPmMaze[r][c];
        if(t==1){pm_tile_rect(w,c,r,&q);InsetRect(&q,1,1);uno_fill(&q,C_CYAN);}
        else if(t==2){pm_tile_rect(w,c,r,&q);InsetRect(&q,3,3);uno_fill(&q,C_WHITE);}
        else if(t==3){pm_tile_rect(w,c,r,&q);InsetRect(&q,2,2);uno_fill(&q,C_WHITE);}
        else if(t==5){pm_tile_rect(w,c,r,&q);q.top+=3;q.bottom-=3;uno_fill(&q,C_MAG);} }
    if(gPmState!=PM_DEAD){ SetRect(&q,w->bounds.left+4+gPmX,w->bounds.top+TBAR_H+2+gPmY,w->bounds.left+4+gPmX+7,w->bounds.top+TBAR_H+2+gPmY+7);
        RGBForeColor(&kPalette[C_WHITE]); PaintOval(&q); RGBForeColor(&kBlack); }
    for(i=0;i<3;i++){ static const GameRGB kGhRGB[3]={{230,40,30,C_MAG},{250,150,200,C_MAG},{245,160,50,C_MAG}};
        static const GameRGB kGhFr={40,40,200,C_CYAN}, kGhFl={240,240,240,C_WHITE};
        PmGhost *g=&gPmGh[i]; const GameRGB *grgb=&kGhRGB[i];
        if(g->state==GH_FRIGHT) grgb=(gPmFright<70&&(gPmFright&8))?&kGhFl:&kGhFr;
        SetRect(&q,w->bounds.left+4+g->x,w->bounds.top+TBAR_H+2+g->y,w->bounds.left+4+g->x+7,w->bounds.top+TBAR_H+2+g->y+7);
        if(g->state==GH_EATEN){ InsetRect(&q,2,2); uno_fill(&q,C_WHITE); }
        else { fill_rgb(&q,grgb); { Rect e=q; e.right=e.left+2; e.bottom=e.top+2; OffsetRect(&e,1,1); uno_fill(&e,C_WHITE); OffsetRect(&e,3,0); uno_fill(&e,C_WHITE);} } }
    px=w->bounds.left+4+PM_COLS*PM_TILE+8;
    text_at(px,w->bounds.top+TBAR_H+14,"SCORE",C_CYAN,C_BLUE,false); fmt_u(gPmScore,num); text_at(px,w->bounds.top+TBAR_H+28,num,C_WHITE,C_BLUE,false);
    text_at(px,w->bounds.top+TBAR_H+48,"HI",C_CYAN,C_BLUE,false); fmt_u(gPmHi,num); text_at(px,w->bounds.top+TBAR_H+62,num,C_WHITE,C_BLUE,false);
    text_at(px,w->bounds.top+TBAR_H+82,"LIVES",C_CYAN,C_BLUE,false); fmt_u(gPmLives,num); text_at(px,w->bounds.top+TBAR_H+96,num,C_WHITE,C_BLUE,false);
    text_at(px,w->bounds.top+TBAR_H+116,"LEVEL",C_CYAN,C_BLUE,false); fmt_u(gPmLevel,num); text_at(px,w->bounds.top+TBAR_H+130,num,C_WHITE,C_BLUE,false);
    if(gPmState==PM_READY) text_at(w->bounds.left+4+88,w->bounds.top+TBAR_H+108,"READY!",C_WHITE,C_BLUE,false);
    else if(gPmState==PM_OVER) text_at(w->bounds.left+4+76,w->bounds.top+TBAR_H+108,"GAME  OVER",C_WHITE,C_BLUE,false);
}
static Boolean pacman_key(char ch, short code, Boolean cmd){
    if(cmd) return false;
    if(ch=='n'||ch=='N'){ pm_new_game(); return true; }
    if(gPmState!=PM_PLAY&&gPmState!=PM_READY) return false;
    if(code==0x7E||ch==0x1E){gPmNextDir=D_UP;return true;}
    if(code==0x7D||ch==0x1F){gPmNextDir=D_DOWN;return true;}
    if(code==0x7B||ch==0x1C){gPmNextDir=D_LEFT;return true;}
    if(code==0x7C||ch==0x1D){gPmNextDir=D_RIGHT;return true;}
    return false;
}
static void pacman_tick(void){
    UnoWin *w; long now=TickCount();
    if(gPmState==PM_TITLE||gPmState==PM_OVER) return;
    if(gPmState==PM_READY){ if(now>=gPmStateT) gPmState=PM_PLAY; else return; }
    if(now-gPmLastStep<2) return; gPmLastStep=now; pm_step();
    w=find_app_window(APP_PACMAN); if(w) draw_window(w);
}

static const AppInterface kIface = {
    pacman_draw, pacman_key, 0, pacman_tick, 0, 0,
    "Pac-Man", { 70, 30, 404, 262 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
