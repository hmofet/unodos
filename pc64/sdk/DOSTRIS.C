/* DOSTRIS.C - Dostris in UnoC.
   The shipped game, ported to UnoC as the Studio SDK's big example. */
#include "UNO.H"

#define DT_COLS 10
#define DT_ROWS 20
#define DT_CELL 16
#define DT_BX 10
#define DT_BY 8

static const signed char kDtShape[7][4][8] = {
  { {0,1,1,1,2,1,3,1}, {2,0,2,1,2,2,2,3}, {0,2,1,2,2,2,3,2}, {1,0,1,1,1,2,1,3} },
  { {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1} },
  { {1,0,0,1,1,1,2,1}, {0,0,0,1,1,1,0,2}, {0,0,1,0,2,0,1,1}, {1,0,0,1,1,1,1,2} },
  { {1,0,2,0,0,1,1,1}, {0,0,0,1,1,1,1,2}, {1,0,2,0,0,1,1,1}, {0,0,0,1,1,1,1,2} },
  { {0,0,1,0,1,1,2,1}, {1,0,0,1,1,1,0,2}, {0,0,1,0,1,1,2,1}, {1,0,0,1,1,1,0,2} },
  { {0,0,0,1,1,1,2,1}, {0,0,1,0,0,1,0,2}, {0,0,1,0,2,0,2,1}, {1,0,1,1,0,2,1,2} },
  { {2,0,0,1,1,1,2,1}, {0,0,0,1,0,2,1,2}, {0,0,1,0,2,0,0,1}, {0,0,1,0,1,1,1,2} },
};
static const GameRGB kDtRGB[7] = {
    {  0,220,220,C_CYAN}, {235,215,0,C_WHITE}, {160,60,220,C_MAG}, {40,200,60,C_CYAN},
    {230,50,50,C_MAG}, {60,100,240,C_WHITE}, {240,150,40,C_CYAN},
};
static const long kDtLineScore[5] = { 0, 40, 100, 300, 1200 };

static unsigned char gDtBoard[DT_ROWS][DT_COLS];
static short gDtState = 0;
static short gDtPiece, gDtRot, gDtCol, gDtRow, gDtNext;
static long  gDtScore, gDtLines;
static short gDtLevel;
static long  gDtLastDrop;
static unsigned long gDtSeed = 1;

static const Note kKoro[] = { {76,16},{71,10},{72,10},{74,16},{72,10},{71,10},{69,16},{69,10},{72,10},{76,16},{74,10},{72,10},{71,26},{72,10},{74,16},{76,16},{72,16},{69,16},{69,33},{0,10} };
#define N_KKORO (short)(sizeof(kKoro)/sizeof(kKoro[0]))

static short dt_rand7(void){ gDtSeed=gDtSeed*1103515245UL+12345UL; return (short)((gDtSeed>>16)%7); }

static Boolean dt_fits(short p, short rot, short col, short row){
    short i; const signed char *sh=kDtShape[p][rot];
    for(i=0;i<4;i++){ short c=col+sh[i*2], r=row+sh[i*2+1];
        if(c<0||c>=DT_COLS||r>=DT_ROWS) return false;
        if(r>=0 && gDtBoard[r][c]) return false; }
    return true;
}
static long dt_drop_interval(void){ short t=18-gDtLevel; if(t<2)t=2; return (long)t*10/3; }
static void dt_spawn(void){
    gDtPiece=gDtNext; gDtNext=dt_rand7(); gDtRot=0; gDtCol=3; gDtRow=-1; gDtLastDrop=TickCount();
    if(!dt_fits(gDtPiece,gDtRot,gDtCol,gDtRow+1)){ gDtState=3; gm_stop(); }
}
static void dt_new_game(void){
    memset(gDtBoard,0,sizeof(gDtBoard)); gDtScore=0; gDtLines=0; gDtLevel=1;
    gDtSeed=(unsigned long)TickCount()|1; gDtNext=dt_rand7(); gDtState=1; dt_spawn();
    gm_start(kKoro,N_KKORO,APP_DOSTRIS);
}
static void dt_clear_lines(void){
    short r,c,n=0;
    for(r=0;r<DT_ROWS;r++){ Boolean full=true;
        for(c=0;c<DT_COLS;c++) if(!gDtBoard[r][c]){full=false;break;}
        if(full){ short rr; n++;
            for(rr=r;rr>0;rr--) memcpy(gDtBoard[rr],gDtBoard[rr-1],DT_COLS);
            memset(gDtBoard[0],0,DT_COLS);} }
    if(n){ gDtScore+=kDtLineScore[n]*(gDtLevel+1); gDtLines+=n;
        gDtLevel=(short)(gDtLines/10)+1; if(gDtLevel>15)gDtLevel=15; }
}
static void dt_lock(void){
    short i; const signed char *sh=kDtShape[gDtPiece][gDtRot];
    for(i=0;i<4;i++){ short c=gDtCol+sh[i*2], r=gDtRow+sh[i*2+1];
        if(r>=0&&r<DT_ROWS&&c>=0&&c<DT_COLS) gDtBoard[r][c]=(unsigned char)(gDtPiece+1); }
    dt_clear_lines(); dt_spawn();
}
static void dt_cell(UnoWin *w, short c, short r, short piece){
    Rect q; short x=w->bounds.left+DT_BX+c*DT_CELL, y=w->bounds.top+TBAR_H+DT_BY+r*DT_CELL;
    SetRect(&q,x,y,x+DT_CELL-1,y+DT_CELL-1); fill_rgb(&q,&kDtRGB[piece]);
    { Rect h=q; h.bottom=h.top+2; RGBForeColor(&kPalette[C_WHITE]); PaintRect(&h); RGBForeColor(&kBlack); }
}
static void dostris_draw(UnoWin *w){
    Rect r=w->bounds,b; short c,rr,i,px; char num[16];
    SetRect(&b,r.left+DT_BX-2,r.top+TBAR_H+DT_BY-2,r.left+DT_BX+DT_COLS*DT_CELL+1,r.top+TBAR_H+DT_BY+DT_ROWS*DT_CELL+1);
    uno_box(&b,C_WHITE);
    for(rr=0;rr<DT_ROWS;rr++) for(c=0;c<DT_COLS;c++) if(gDtBoard[rr][c]) dt_cell(w,c,rr,gDtBoard[rr][c]-1);
    if(gDtState==1||gDtState==2){ const signed char *sh=kDtShape[gDtPiece][gDtRot];
        for(i=0;i<4;i++){ short cc=gDtCol+sh[i*2],cr=gDtRow+sh[i*2+1]; if(cr>=0) dt_cell(w,cc,cr,gDtPiece);} }
    px=r.left+DT_BX+DT_COLS*DT_CELL+14;
    text_at(px,r.top+TBAR_H+20,"DOSTRIS",C_MAG,C_BLUE,false);
    text_at(px,r.top+TBAR_H+44,"Score",C_CYAN,C_BLUE,false); fmt_u(gDtScore,num); text_at(px+56,r.top+TBAR_H+44,num,C_WHITE,C_BLUE,false);
    text_at(px,r.top+TBAR_H+60,"Lines",C_CYAN,C_BLUE,false); fmt_u(gDtLines,num); text_at(px+56,r.top+TBAR_H+60,num,C_WHITE,C_BLUE,false);
    text_at(px,r.top+TBAR_H+76,"Level",C_CYAN,C_BLUE,false); fmt_u(gDtLevel,num); text_at(px+56,r.top+TBAR_H+76,num,C_WHITE,C_BLUE,false);
    if(gDtState==0) text_at(px,r.bottom-12,"N: new game",C_WHITE,C_BLUE,false);
    else if(gDtState==3) text_at(px,r.bottom-12,"GAME OVER",C_MAG,C_BLUE,false);
}
static void dt_redraw(void){ UnoWin *w=find_app_window(APP_DOSTRIS); if(w) draw_window(w); }
static Boolean dostris_key(char ch, short code, Boolean cmd){
    if(cmd) return false;
    if(ch=='n'||ch=='N'){ dt_new_game(); dt_redraw(); return true; }
    if(gDtState!=1){ return (ch==' '); }
    if(code==0x7B||ch==0x1C){ if(dt_fits(gDtPiece,gDtRot,gDtCol-1,gDtRow)) gDtCol--; dt_redraw(); return true; }
    if(code==0x7C||ch==0x1D){ if(dt_fits(gDtPiece,gDtRot,gDtCol+1,gDtRow)) gDtCol++; dt_redraw(); return true; }
    if(code==0x7E||ch==0x1E){ short nr=(short)((gDtRot+1)&3); if(dt_fits(gDtPiece,nr,gDtCol,gDtRow)) gDtRot=nr; dt_redraw(); return true; }
    if(code==0x7D||ch==0x1F){ if(dt_fits(gDtPiece,gDtRot,gDtCol,gDtRow+1)){gDtRow++;gDtScore++;gDtLastDrop=TickCount();} else dt_lock(); dt_redraw(); return true; }
    if(ch==' '){ while(dt_fits(gDtPiece,gDtRot,gDtCol,gDtRow+1)){gDtRow++;gDtScore+=2;} dt_lock(); dt_redraw(); return true; }
    return false;
}
static void dostris_tick(void){
    if(gDtState!=1) return;
    if(TickCount()-gDtLastDrop<dt_drop_interval()) return;
    gDtLastDrop=TickCount();
    if(dt_fits(gDtPiece,gDtRot,gDtCol,gDtRow+1)) gDtRow++; else dt_lock();
    dt_redraw();
}

static const AppInterface kIface = {
    dostris_draw, dostris_key, 0, dostris_tick, 0, 0,
    "Dostris", { 20, 10, 330, 388 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
