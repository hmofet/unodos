/* ===========================================================================
 * UnoDOS runtime-app-loading DEMONSTRATOR kernel (host shim).
 * ===========================================================================
 * Proves the new architecture end to end on the host path (WSL gcc), the only
 * emulator-equivalent route on this dev box: the kernel contains NO app code;
 * the apps live in a SEPARATE module loaded from STORAGE at runtime (a .so in
 * apps_store/, dlopen'd by uno_load_module); the window manager dispatches
 * purely through the AppInterface function pointers obtained from that module.
 *
 * It reuses the real platform layer (fb.* + mac_compat.* + the shared 8x8
 * font) and the real UnoDOS widget helpers (text_at / uno_fill / fill_rgb,
 * copied verbatim from unodos.c) so what renders is byte-for-byte what the
 * full desktop renders.  Built + run by build_modular.sh; output is a PPM.
 * ===========================================================================
 */
#include "mac_compat.h"
#include "uno_app.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* `qd` is provided by mac_compat.c (the shim owns QuickDraw state). */
extern QDGlobals qd;

enum { C_BLUE = 0, C_CYAN = 1, C_MAG = 2, C_WHITE = 3 };
static RGBColor kPalette[4] = {
    { 0x0000, 0x0000, 0xAAAA }, { 0x0000, 0xAAAA, 0xAAAA },
    { 0xAAAA, 0x0000, 0xAAAA }, { 0xFFFF, 0xFFFF, 0xFFFF }
};
static RGBColor kBlack = { 0, 0, 0 };

#define TBAR_H 18
#define MENUBAR_H 20
#define MAXWIN 6
#define ICON_PITCH 92
#define ICON0_X 36
#define ICON0_Y 44
#define ICONS_ROW 6
#define ICON_ROW_H 72

/* ---- the real UnoDOS widget helpers (verbatim from unodos.c) ------------ */
static void desktop_bg(Rect *r){ RGBForeColor(&kPalette[C_BLUE]); PaintRect(r); RGBForeColor(&kBlack); }
static void uno_fill(Rect *r, short c){ RGBForeColor(&kPalette[c]); PaintRect(r); RGBForeColor(&kBlack); }
static void uno_box(Rect *r, short c){ RGBForeColor(&kPalette[c]); FrameRect(r); RGBForeColor(&kBlack); }
static void uno_invert(Rect *r){ InvertRect(r); }
static void text_at(short x, short y, const char *s, short fg, short bg, Boolean opaque){
    short len=(short)strlen(s); MoveTo(x,y);
    RGBForeColor(&kPalette[fg]);
    if(opaque){ RGBBackColor(&kPalette[bg]); TextMode(srcCopy);} else TextMode(srcOr);
    DrawText((Ptr)s,0,len);
    RGBForeColor(&kBlack); RGBBackColor(&kPalette[C_WHITE]); TextMode(srcOr);
}
static void text_at_max(short x, short y, const char *s, short fg, short maxw){
    short len=(short)strlen(s);
    while(len>0 && TextWidth((Ptr)s,0,len)>maxw) len--;
    if(len<=0) return; MoveTo(x,y); RGBForeColor(&kPalette[fg]); TextMode(srcOr);
    DrawText((Ptr)s,0,len); RGBForeColor(&kBlack);
}
static void fill_rgb(Rect *q, const GameRGB *c){
    RGBColor rc; rc.red=(unsigned short)(c->r<<8); rc.green=(unsigned short)(c->g<<8);
    rc.blue=(unsigned short)(c->b<<8); RGBForeColor(&rc); PaintRect(q); RGBForeColor(&kBlack);
}
static void fmt_u(long v, char *out){ char t[12]; int n=0,i=0; if(v<=0)t[n++]='0';
    while(v>0){t[n++]=(char)('0'+v%10); v/=10;} while(n)out[i++]=t[--n]; out[i]=0; }
static void put2(long v, char *out){ out[0]=(char)('0'+(v/10)%10); out[1]=(char)('0'+v%10); out[2]=0; }
static long gBootTicks; static long now_secs(void){ return (TickCount()-gBootTicks)/60; }

/* ---- window manager (the real z-order/WM contract) --------------------- */
static Rect   gScreen;
static UnoWin gWins[MAXWIN];
static short  gZ[MAXWIN];
static short  gZCount = 0;

static UnoWin *zwin(short z){ return &gWins[gZ[z]]; }

/* minimal stubs for the KernelApi members the apps may reference (FAT/sound) -
   the demonstrator focuses on draw/key/click/tick dispatch; storage + audio
   are exercised by the full ports.  These keep the ABI complete. */
static short gFatCount = 0;
static unsigned char gFatNames[16][13];
static long gFatSizes[16];
static Boolean fat12_mount(void){ return false; }
static void fat12_list(void){}
static long fat12_read(const char*n,unsigned char*b,long m){(void)n;(void)b;(void)m;return 0;}
static Boolean fat12_write(const char*n,const unsigned char*b,long l){(void)n;(void)b;(void)l;return false;}
static void music_open_chan(void){} static void music_note_on(short a,short b){(void)a;(void)b;}
static void music_quiet(void){} static void music_start(void){} static void music_stop(void){}
static void gm_start(const Note*n,short c,short o){(void)n;(void)c;(void)o;} static void gm_stop(void){}

static UnoWin *find_app_window(short proc);
static void draw_window(UnoWin *w);
static void repaint_all(void);
static void launch_app(short proc);
static void draw_desktop(void);

#include "app_loader.c"   /* the generic loader: KernelApi build + dispatch */

static void draw_window(UnoWin *w)
{
    Rect r=w->bounds, tb, ct;
    Boolean active=(gZCount>0 && zwin(gZCount-1)==w);
    ct=r; ct.top+=TBAR_H; InsetRect(&ct,1,1); uno_fill(&ct,C_BLUE);
    tb=r; tb.bottom=tb.top+TBAR_H; uno_fill(&tb,C_WHITE);
    uno_box(&r,C_WHITE);
    if(active){ short yy; RGBForeColor(&kPalette[C_BLUE]);
        for(yy=tb.top+3; yy<=tb.bottom-4; yy+=3){ MoveTo(tb.left+4,yy); LineTo(tb.right-5,yy);} RGBForeColor(&kBlack);}
    { Rect cb; SetRect(&cb,r.right-22,tb.top+1,r.right-2,tb.bottom-2); uno_fill(&cb,C_WHITE);
      SetRect(&cb,r.right-18,r.top+4,r.right-7,r.top+15); RGBForeColor(&kPalette[C_BLUE]); FrameRect(&cb); RGBForeColor(&kBlack);}
    { short len=0; const char*p=w->title; while(*p++)len++;
      { short tw=TextWidth((Ptr)w->title,0,len); short tx=(short)((r.left+r.right-tw)/2); Rect tp;
        SetRect(&tp,tx-6,tb.top+1,tx+tw+6,tb.bottom-2); uno_fill(&tp,C_WHITE);
        text_at(tx,r.top+13,w->title,C_BLUE,C_WHITE,true);} }
    draw_app_content(w->proc, w);
}

static void repaint_all(void){ short z; draw_desktop(); for(z=0;z<gZCount;z++) draw_window(zwin(z)); }

static UnoWin *find_app_window(short proc){ short i;
    for(i=0;i<gZCount;i++) if(gWins[gZ[i]].proc==proc) return &gWins[gZ[i]]; return NULL; }

static void launch_app(short proc){ short i,slot=-1;
    for(i=0;i<gZCount;i++) if(gWins[gZ[i]].proc==proc) return;
    for(i=0;i<MAXWIN;i++) if(!gWins[i].used){ slot=i; break; }
    if(slot<0) return;
    gWins[slot].used=true; gWins[slot].proc=proc; gWins[slot].title=app_title(proc);
    app_default_rect(proc,&gWins[slot].bounds);
    gZ[gZCount++]=slot;
    app_opened(proc);
}

/* desktop icons (names come from the modules' titles) */
static const char *kIconNames[APP_NAPPS]={"Sys Info","Clock","Files","Notepad","Music",
    "Dostris","OutLast","Pac-Man","Tracker","Paint","Theme"};
static void draw_desktop(void){ short i; desktop_bg(&gScreen);
    text_at(gScreen.left+6,gScreen.top+14,"UnoDOS Mac (modular apps)",C_WHITE,C_BLUE,true);
    for(i=0;i<APP_NAPPS;i++){ short x=ICON0_X+(i%ICONS_ROW)*ICON_PITCH, y=ICON0_Y+(i/ICONS_ROW)*ICON_ROW_H;
        Rect g; SetRect(&g,x,y,x+18,y+16); uno_box(&g,C_CYAN);
        text_at(x-8,y+28,kIconNames[i],C_WHITE,C_BLUE,true);} }

/* present + the autotest driver */
void uno_host_present(void);

int main(void)
{
    int i;
    InitGraf(&qd.thePort);
    gScreen = qd.screenBits.bounds;
    gBootTicks = TickCount();
    app_loader_init();

    draw_desktop();

    /* launch the named apps as MODULES loaded from storage, then drive each
       through the function-pointer dispatch so the screenshot proves render */
    { short order[]={APP_SYSINFO, APP_FILES, APP_DOSTRIS, APP_PACMAN, APP_THEME};
      for(i=0;i<(int)(sizeof(order)/sizeof(order[0]));i++) launch_app(order[i]); }

    /* tile the windows so several are visible at once */
    { short n=gZCount, k; for(k=0;k<n;k++){ UnoWin *w=&gWins[gZ[k]];
        short col=k%3, row=k/3; short ww=w->bounds.right-w->bounds.left, wh=w->bounds.bottom-w->bounds.top;
        short nx=10+col*210, ny=MENUBAR_H+10+row*210;
        SetRect(&w->bounds,nx,ny,nx+ww,ny+wh);} }

    /* play a few Dostris/Pac-Man frames through the real key+tick pointers */
    { const AppInterface *dt=app_iface(APP_DOSTRIS);
      if(dt && dt->key){ dt->key('n',0,false); for(i=0;i<6;i++){ dt->key(0,0x7B,false); dt->key(' ',0,false);} } }
    { const AppInterface *pm=app_iface(APP_PACMAN);
      if(pm && pm->key){ pm->key('n',0,false); for(i=0;i<120;i++) if(pm->tick) pm->tick(); } }
    if(!getenv("UNO_NO_THEME")){ const AppInterface *th=app_iface(APP_THEME);
      if(th && th->key){ th->key(0,0x7D,false); th->key(0,0x7D,false); th->key(0,0x7D,false); th->key('\r',0,false); } }

    repaint_all();
    uno_host_present();
    fprintf(stderr,"modular kernel: %d app modules loaded + dispatched\n", gZCount);
    return 0;
}
