/* Dostris app module (APP_DOSTRIS). Separate artifact -> app05.so.
   Faithful port of the dostris_* block from unodos.c.

   THE LOOK IS THE 16-BIT ORIGINAL'S, RESTORED.  apps/tetris.asm - the CGA
   Dostris that shipped before pc64 existed - drew a 3D block (base fill,
   highlight along the top AND left edge), a framed Next-piece preview, three
   readouts, New Game / Pause buttons and two lines of help.  The C port kept
   only the readouts and a 2 px white strip along the top of each cell, which
   is why the game reads as a flat blue slab with a few coloured squares in
   it: the panel is four fifths empty and the pieces have no relief at all.
   Everything below restores that layout, and derives the bevel shades from
   each piece's own colour instead of painting them all white - which is what
   the VGA build (apps/tetrisv.asm) reached for with its per-piece
   highlight/base/shadow palette triplets. */
#include "uno_mod.h"

#define DT_COLS 10
#define DT_ROWS 20

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
static const GameRGB kDtWell  = {  0,  0, 60, C_BLUE };  /* well: a shade under the panel */
static const GameRGB kDtBand  = { 70,  0,110, C_MAG  };  /* the title band               */
static const long kDtLineScore[5] = { 0, 40, 100, 300, 1200 };

static unsigned char gDtBoard[DT_ROWS][DT_COLS];
static short gDtState = 0;                 /* 0 idle  1 playing  2 paused  3 over */
static short gDtPiece, gDtRot, gDtCol, gDtRow, gDtNext;
static long  gDtScore, gDtLines;
static short gDtLevel;
static long  gDtLastDrop;
static unsigned long gDtSeed = 1;

/* the layout, recomputed from the window on every draw, so the click handler
   and the painter can never disagree about where a button is */
static short gCell = 16, gBX = 8, gBY = 8, gPX = 190, gPW = 140;
static UiBtn gBtnNew, gBtnPause;

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
        gDtLevel=(short)(gDtLines/10)+1; if(gDtLevel>15)gDtLevel=15;
        /* The line-clear blip.  The native build (pc64_games.c) fired one and
           the module port dropped it, so the only sound Dostris made was the
           looping theme - a cleared line, the one event worth hearing, was
           silent.  music_note_on is the KernelApi's one-shot note: it borrows
           the single voice for 5 ticks and the theme resumes underneath. */
        music_note_on(88, 5); }
}
static void dt_lock(void){
    short i; const signed char *sh=kDtShape[gDtPiece][gDtRot];
    for(i=0;i<4;i++){ short c=gDtCol+sh[i*2], r=gDtRow+sh[i*2+1];
        if(r>=0&&r<DT_ROWS&&c>=0&&c<DT_COLS) gDtBoard[r][c]=(unsigned char)(gDtPiece+1); }
    dt_clear_lines(); dt_spawn();
}

/* ---- the 3D block --------------------------------------------------------
 * tetris.asm painted the highlight pure white on every piece because CGA had
 * four colours to spend.  With a real framebuffer the shades come off the
 * piece's own colour instead, so a cyan I-piece and a red S-piece are lit by
 * the same light. */
static GameRGB dt_lit(const GameRGB *c)
{
    GameRGB o; short v;
    v=(short)(c->r+95); o.r=(unsigned char)(v>255?255:v);
    v=(short)(c->g+95); o.g=(unsigned char)(v>255?255:v);
    v=(short)(c->b+95); o.b=(unsigned char)(v>255?255:v);
    o.mono=C_WHITE; return o;
}
static GameRGB dt_dim(const GameRGB *c)
{
    GameRGB o;
    o.r=(unsigned char)(c->r*45/100); o.g=(unsigned char)(c->g*45/100);
    o.b=(unsigned char)(c->b*45/100); o.mono=c->mono; return o;
}
static void dt_block(short x, short y, short cell, short piece)
{
    Rect q,h; short bw=(short)(cell>=12?2:1);
    GameRGB lit=dt_lit(&kDtRGB[piece]), dim=dt_dim(&kDtRGB[piece]);
    SetRect(&q,x,y,(short)(x+cell-1),(short)(y+cell-1)); fill_rgb(&q,&kDtRGB[piece]);
    /* Shadow first, highlight second: the top-left corner then belongs to the
       highlight and the bottom-right to the shadow, which is the corner rule
       the asm original spelled out with its skip-one-pixel edge draws. */
    SetRect(&h,x,(short)(y+cell-1-bw),(short)(x+cell-1),(short)(y+cell-1)); fill_rgb(&h,&dim);
    SetRect(&h,(short)(x+cell-1-bw),y,(short)(x+cell-1),(short)(y+cell-1)); fill_rgb(&h,&dim);
    SetRect(&h,x,y,(short)(x+cell-1),(short)(y+bw)); fill_rgb(&h,&lit);
    SetRect(&h,x,y,(short)(x+bw),(short)(y+cell-1)); fill_rgb(&h,&lit);
}
static void dt_cell(UnoWin *w, short c, short r, short piece)
{
    dt_block((short)(w->bounds.left+gBX+c*gCell),
             (short)(w->bounds.top+TBAR_H+gBY+r*gCell), gCell, piece);
}

/* ---- layout --------------------------------------------------------------
 * Derived from the window rather than frozen at 16 px, so the well fills the
 * height it is given and the side panel keeps a usable width at any desktop
 * resolution instead of being whatever happens to be left over. */
static void dt_layout(UnoWin *w)
{
    short cw=(short)(w->bounds.right-w->bounds.left);
    short cy=(short)(w->bounds.top+TBAR_H);
    short chh=(short)(w->bounds.bottom-cy);
    short cell=(short)((chh-16)/DT_ROWS), byw=(short)((cw-150)/DT_COLS);
    if(byw<cell) cell=byw;
    if(cell>20) cell=20;
    if(cell<6)  cell=6;
    gCell=cell; gBX=8; gBY=8;
    gPX=(short)(gBX+DT_COLS*cell+12);
    gPW=(short)(cw-gPX-8); if(gPW<60) gPW=60;
    /* The buttons sit UNDER the Next box, and the help under them, rather
       than either being pinned to the bottom edge: pinned, a tall window
       opened a hand's width of empty blue in the middle of the panel and it
       read as unfinished again.  The clamp is for the other end - a short
       window, where a fixed offset would push the help off the bottom. */
    { short by=(short)(cy+206), lim=(short)(cy+chh-124);
      if(by>lim) by=lim;
      /* 22 px, not the 16 the other apps use: ui_button puts the label's
         BASELINE 5 px off the bottom edge, which centres an 8x8 glyph (the
         font every other port in this tree draws with) and hangs a pc64 TTF's
         descenders over the border.  A module cannot measure a glyph, so the
         box is sized for the taller font instead of the label being moved. */
      gBtnNew.x=(short)(w->bounds.left+gPX); gBtnNew.w=gPW; gBtnNew.h=22;
      gBtnNew.y=by;
      gBtnPause=gBtnNew; gBtnPause.y=(short)(by+28); }
}

/* the Next box: framed, with the piece centred on its own bounding box so a
   flat I-piece and a square O-piece both sit in the middle of it */
static void dt_preview(UnoWin *w, short bx, short by, short box)
{
    Rect r; short i,nc,ox,oy,minc=3,maxc=0,minr=3,maxr=0;
    const signed char *sh;
    (void)w;
    SetRect(&r,bx,by,(short)(bx+box),(short)(by+box));
    uno_fill(&r,C_BLUE); uno_box(&r,C_WHITE);
    if(gDtState==0) return;
    sh=kDtShape[gDtNext][0];
    for(i=0;i<4;i++){
        short c=sh[i*2], rr=sh[i*2+1];
        if(c<minc)  minc=c;
        if(c>maxc)  maxc=c;
        if(rr<minr) minr=rr;
        if(rr>maxr) maxr=rr;
    }
    nc=(short)((box-8)/4); if(nc>gCell) nc=gCell; if(nc<4) nc=4;
    ox=(short)(bx+(box-(maxc-minc+1)*nc)/2);
    oy=(short)(by+(box-(maxr-minr+1)*nc)/2);
    for(i=0;i<4;i++)
        dt_block((short)(ox+(sh[i*2]-minc)*nc),(short)(oy+(sh[i*2+1]-minr)*nc),nc,gDtNext);
}

static void dostris_draw(UnoWin *w)
{
    Rect r=w->bounds,b,p; short c,rr,i,px,cy,chh,y,box; char num[16];
    dt_layout(w);
    cy=(short)(r.top+TBAR_H);
    chh=(short)(r.bottom-cy);
    px=(short)(r.left+gPX);

    /* ---- the well.  Filled before it is framed, for the reason in the panel
       comment below: a C_WHITE frame drawn straight onto a light theme's
       window fill is white on white, so the playfield had no visible edge at
       all - the pieces just floated in the window.  The fill is a shade
       darker than the panel so the two read as separate surfaces. */
    SetRect(&b,(short)(r.left+gBX-3),(short)(cy+gBY-3),
              (short)(r.left+gBX+DT_COLS*gCell+2),(short)(cy+gBY+DT_ROWS*gCell+2));
    fill_rgb(&b,&kDtWell);
    uno_box(&b,C_WHITE);
    for(rr=0;rr<DT_ROWS;rr++) for(c=0;c<DT_COLS;c++)
        if(gDtBoard[rr][c]) dt_cell(w,c,rr,(short)(gDtBoard[rr][c]-1));
    if(gDtState==1||gDtState==2){ const signed char *sh=kDtShape[gDtPiece][gDtRot];
        for(i=0;i<4;i++){ short cc=(short)(gDtCol+sh[i*2]),cr=(short)(gDtRow+sh[i*2+1]);
            if(cr>=0) dt_cell(w,cc,cr,gDtPiece);} }

    /* ---- the side panel gets its OWN background.  Every text_at below names
       C_BLUE as its background but draws transparently, so the readouts
       actually landed on the shell's themed window fill - and the values are
       C_WHITE, which is invisible on a light theme (the C_CYAN labels beside
       them stayed readable, which is why this looked like missing numbers
       rather than a colour bug).  Painting the strip the colour the app has
       always claimed makes it true on all ten themes. */
    SetRect(&p,(short)(px-8),cy,r.right,r.bottom);
    uno_fill(&p,C_BLUE);
    SetRect(&b,(short)(px-6),(short)(cy+4),(short)(r.right-4),(short)(r.bottom-4));
    uno_box(&b,C_WHITE);

    /* Title, on a band, so it reads as a heading rather than one more label.
       The asm original coloured DOSTRIS a letter at a time; the module ABI
       has no way to measure a glyph (text_at draws whole strings, and no
       width call is exported to modules), so a proportional font cannot be
       stepped through by hand.  A band under the word carries the same
       weight without guessing at advances. */
    SetRect(&b,(short)(px-2),(short)(cy+8),(short)(r.right-8),(short)(cy+26));
    fill_rgb(&b,&kDtBand);
    text_at((short)(px+6),(short)(cy+21),"DOSTRIS",C_WHITE,C_BLUE,false);

    y=(short)(cy+46);
    text_at(px,y,"Score",C_CYAN,C_BLUE,false);
    fmt_u(gDtScore,num); text_at_max((short)(px+56),y,num,C_WHITE,(short)(gPW-56)); y+=16;
    text_at(px,y,"Lines",C_CYAN,C_BLUE,false);
    fmt_u(gDtLines,num); text_at_max((short)(px+56),y,num,C_WHITE,(short)(gPW-56)); y+=16;
    text_at(px,y,"Level",C_CYAN,C_BLUE,false);
    fmt_u(gDtLevel,num); text_at_max((short)(px+56),y,num,C_WHITE,(short)(gPW-56));

    text_at(px,(short)(cy+98),"Next",C_CYAN,C_BLUE,false);
    box=(short)(gPW>56?56:gPW);
    dt_preview(w,px,(short)(cy+108),box);

    /* The state line.  The game says what it is doing instead of leaving the
       player to infer it from a board that has stopped moving. */
    y=(short)(cy+178);
    if(gDtState==0)      text_at(px,y,"Ready",C_CYAN,C_BLUE,false);
    else if(gDtState==2) text_at(px,y,"PAUSED",C_WHITE,C_BLUE,false);
    else if(gDtState==3) text_at(px,y,"GAME OVER",C_MAG,C_BLUE,false);

    ui_button(&gBtnNew,"New Game",false);
    ui_button(&gBtnPause,gDtState==2?"Resume":"Pause",false);

    /* the help sits just under the buttons rather than pinned to the bottom
       edge: pinned, a tall window opened a hand's width of empty blue between
       the two and the panel read as unfinished again */
    y=(short)(gBtnPause.y+gBtnPause.h+20);
    if(y>(short)(cy+chh-30)) y=(short)(cy+chh-30);
    text_at_max(px,y,"Arrows  move",C_CYAN,gPW); y+=12;
    text_at_max(px,y,"Up  rotate",C_CYAN,gPW);   y+=12;
    text_at_max(px,y,"Space drop  P pause",C_CYAN,gPW);
}
static void dt_redraw(void){ UnoWin *w=find_app_window(APP_DOSTRIS); if(w) draw_window(w); }
static void dt_pause_toggle(void){
    if(gDtState==1){ gDtState=2; gm_stop(); }
    else if(gDtState==2){ gDtState=1; gDtLastDrop=TickCount(); gm_start(kKoro,N_KKORO,APP_DOSTRIS); }
}
static Boolean dostris_key(char ch, short code, Boolean cmd){
    if(cmd) return false;
    if(ch=='n'||ch=='N'){ dt_new_game(); dt_redraw(); return true; }
    if(ch=='p'||ch=='P'){ dt_pause_toggle(); dt_redraw(); return true; }
    if(gDtState!=1){ return (Boolean)(ch==' '); }
    if(code==0x7B||ch==0x1C){ if(dt_fits(gDtPiece,gDtRot,gDtCol-1,gDtRow)) gDtCol--; dt_redraw(); return true; }
    if(code==0x7C||ch==0x1D){ if(dt_fits(gDtPiece,gDtRot,gDtCol+1,gDtRow)) gDtCol++; dt_redraw(); return true; }
    if(code==0x7E||ch==0x1E){ short nr=(short)((gDtRot+1)&3); if(dt_fits(gDtPiece,nr,gDtCol,gDtRow)) gDtRot=nr; dt_redraw(); return true; }
    if(code==0x7D||ch==0x1F){ if(dt_fits(gDtPiece,gDtRot,gDtCol,gDtRow+1)){gDtRow++;gDtScore++;gDtLastDrop=TickCount();} else dt_lock(); dt_redraw(); return true; }
    if(ch==' '){ while(dt_fits(gDtPiece,gDtRot,gDtCol,gDtRow+1)){gDtRow++;gDtScore+=2;} dt_lock(); dt_redraw(); return true; }
    return false;
}
/* The buttons the asm original had.  A game whose only way to start is an
   undocumented keypress is a game most people close again. */
static void dostris_click(UnoWin *w, Point p){
    if(ui_hit(&gBtnNew,p))   { dt_new_game();     draw_window(w); return; }
    if(ui_hit(&gBtnPause,p)) { dt_pause_toggle(); draw_window(w); return; }
}
static void dostris_tick(void){
    if(gDtState!=1) return;
    if(TickCount()-gDtLastDrop<dt_drop_interval()) return;
    gDtLastDrop=TickCount();
    if(dt_fits(gDtPiece,gDtRot,gDtCol,gDtRow+1)) gDtRow++; else dt_lock();
    dt_redraw();
}

/* gm_start plays on the ONE global sequencer, so a Dostris that goes away
 * without stopping it leaves Korobeiniki looping over whatever opens next -
 * and over an empty desktop.  unoapp_close() calls this slot; it was null.
 * apps/outlast.c has had the same handler since it was written. */
static void dostris_closed(void){ gm_stop(); }

static const AppInterface kIface = {
    dostris_draw, dostris_key, dostris_click, dostris_tick, 0, dostris_closed,
    "Dostris", { 14, 8, 354, 370 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
