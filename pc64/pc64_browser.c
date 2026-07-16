/* ===========================================================================
 * UnoDOS/pc64 - a minimal web browser (HTML + Markdown + basic CSS).
 *
 * A native unoui canvas app: it lays out a document and paints it with fb
 * primitives, wrapping text to the canvas width and scaling headings. No JS
 * and no network yet - it renders a built-in welcome page and opens files from
 * the local file system (uno_fs_*: the RAM disk today, FAT32/local disks next).
 *
 * Rendering model: a single immediate-mode flow. A cursor walks left-to-right
 * wrapping words; block elements start new lines and add vertical gaps; a small
 * style state (scale / bold / underline / colour) is set from Markdown syntax
 * or HTML tags + their default (and inline) CSS. Good enough for docs, help
 * pages and READMEs; not a spec-complete engine.
 * ======================================================================== */
#include "unoui.h"
#include "fb.h"
#include "pc64_fs.h"
#include <string.h>

/* ---- page palette (a light "document" look) ------------------------------ */
#define PG_BG    FB_RGB(250, 250, 248)
#define PG_TEXT  FB_RGB(30, 32, 40)
#define PG_HEAD  FB_RGB(20, 40, 90)
#define PG_LINK  FB_RGB(40, 90, 210)
#define PG_CODE  FB_RGB(150, 40, 40)
#define PG_CODEBG FB_RGB(235, 235, 230)
#define PG_QUOTE FB_RGB(90, 100, 120)
#define PG_RULE  FB_RGB(200, 200, 195)

typedef struct { int scale, bold, under, mono; fb_px color; } bstyle;

/* ---- the layout cursor (one active flow at a time) ----------------------- */
static int fx, fy, fleft, fright, fscroll, flh;
static unoui_rect fclip;

static void fl_reset(unoui_rect r, int scroll, int pad)
{
    fleft = r.x + pad; fright = r.x + r.w - pad;
    fx = fleft; fy = r.y + pad; fscroll = scroll; fclip = r; flh = 12;
}
static void fl_nl(void) { fx = fleft; fy += flh; flh = 12; }
static void fl_gap(int h) { if (fx > fleft) fl_nl(); fy += h; }

/* place one word (already NUL-terminated in `buf`); wraps at the right edge */
static void fl_word(const char *buf, int indent, bstyle *s)
{
    int len = (int)strlen(buf), sc = s->scale ? s->scale : 1;
    int ww = len * 8 * sc, lh = 8 * sc + 5, dy;
    if (lh > flh) flh = lh;
    if (fx + ww > fright && fx > fleft + indent) fl_nl();
    if (fx == fleft) fx = fleft + indent;
    dy = fy - fscroll;
    if (dy > fclip.y - lh && dy < fclip.y + fclip.h) {          /* visible row */
        if (s->mono) fb_fill_rect(fx - 1, dy - 1, ww + 2, lh - 2, PG_CODEBG);
        if (sc > 1) fb_big_text(fx, dy, buf, s->color, -1, sc);
        else { fb_text(fx, dy, buf, s->color, -1);
               if (s->bold) fb_text(fx + 1, dy, buf, s->color, -1); }
        if (s->under) fb_hline(fx, dy + lh - 3, ww, s->color);
    }
    fx += ww + 8 * sc;                                          /* + space */
}

/* emit a run of text, splitting on spaces into wrappable words */
static void fl_text(const char *t, int len, int indent, bstyle *s)
{
    char buf[128]; int i = 0, n;
    while (i < len) {
        while (i < len && (t[i] == ' ' || t[i] == '\t')) i++;
        n = 0;
        while (i < len && t[i] != ' ' && t[i] != '\t' && n < 127) buf[n++] = t[i++];
        if (n) { buf[n] = 0; fl_word(buf, indent, s); }
    }
}

/* ---- Markdown inline: **bold** *italic* `code` [text](url) --------------- */
static void md_inline(const char *s, int len, int indent, bstyle base)
{
    int i = 0, start = 0; bstyle cur = base;
    for (i = 0; i <= len; i++) {
        int flush = (i == len);
        if (!flush && s[i] == '*' && i + 1 < len && s[i + 1] == '*') flush = 1;
        else if (!flush && s[i] == '*') flush = 1;
        else if (!flush && s[i] == '`') flush = 1;
        else if (!flush && s[i] == '[') flush = 1;
        if (flush) {
            if (i > start) fl_text(s + start, i - start, indent, &cur);
            if (i == len) break;
            if (s[i] == '*' && i + 1 < len && s[i + 1] == '*') { cur.bold ^= 1; i++; }
            else if (s[i] == '*') { cur.scale = cur.scale; cur.under ^= 0; /* italic~underline hint */ cur.bold ^= 0; cur.color = (cur.color==base.color)?FB_RGB(70,70,90):base.color; }
            else if (s[i] == '`') {                              /* toggle code */
                cur.mono ^= 1; cur.color = cur.mono ? PG_CODE : base.color;
            }
            else if (s[i] == '[') {                              /* [text](url) */
                int te = i + 1; while (te < len && s[te] != ']') te++;
                { bstyle lk = base; lk.color = PG_LINK; lk.under = 1;
                  fl_text(s + i + 1, te - (i + 1), indent, &lk); }
                i = te; if (i + 1 < len && s[i + 1] == '(') { while (i < len && s[i] != ')') i++; }
            }
            start = i + 1;
        }
    }
}

/* ---- Markdown document --------------------------------------------------- */
static void render_md(const char *src, unoui_rect r, int scroll)
{
    const char *p = src; int code = 0;
    bstyle base = { 1, 0, 0, 0, PG_TEXT };
    fl_reset(r, scroll, 10);
    while (*p) {
        const char *ls = p; int llen;
        while (*p && *p != '\n') p++;
        llen = (int)(p - ls); if (*p == '\n') p++;
        if (llen >= 3 && ls[0] == '`' && ls[1] == '`' && ls[2] == '`') { code = !code; fl_gap(4); continue; }
        if (code) {                                              /* code block */
            bstyle cs = { 1, 0, 0, 1, PG_CODE };
            if (llen == 0) { fl_gap(10); }
            else { fx = fleft; fl_word("", 12, &cs);   /* indent */
                   { char b[256]; int n = llen<255?llen:255; memcpy(b,ls,n); b[n]=0; fl_word(b,12,&cs); } fl_nl(); }
            continue;
        }
        { int h = 0; while (h < 6 && ls[h] == '#') h++;
          if (h > 0 && h <= 6 && ls[h] == ' ') {                 /* heading */
              bstyle hs = base; hs.scale = h <= 1 ? 3 : h == 2 ? 2 : 2; hs.bold = 1; hs.color = PG_HEAD;
              fl_gap(h <= 2 ? 8 : 4); md_inline(ls + h + 1, llen - h - 1, 0, hs); fl_gap(6); continue;
          } }
        if (llen >= 3 && (ls[0] == '-' || ls[0] == '*' || ls[0] == '_')) {  /* hr? */
            int all = 1, k; for (k = 0; k < llen; k++) if (ls[k] != ls[0] && ls[k] != ' ') { all = 0; break; }
            if (all) { fl_gap(6); { int yy = fy - fscroll; if (yy > fclip.y && yy < fclip.y+fclip.h) fb_hline(fleft, yy, fright-fleft, PG_RULE); } fl_gap(8); continue; }
        }
        if ((ls[0] == '-' || ls[0] == '*' || ls[0] == '+') && ls[1] == ' ') {   /* bullet */
            int yy = fy - fscroll + 4; fx = fleft;
            if (yy > fclip.y && yy < fclip.y+fclip.h) fb_fill_rect(fleft + 6, yy, 3, 3, PG_TEXT);
            md_inline(ls + 2, llen - 2, 18, base); fl_nl(); continue;
        }
        if (ls[0] == '>' ) {                                     /* blockquote */
            bstyle qs = base; qs.color = PG_QUOTE;
            { int y0 = fy - fscroll; if (y0 > fclip.y-flh && y0 < fclip.y+fclip.h) fb_fill_rect(fleft, y0, 2, flh, PG_RULE); }
            md_inline(ls + (ls[1]==' '?2:1), llen - (ls[1]==' '?2:1), 12, qs); fl_nl(); continue;
        }
        if (llen == 0) { fl_gap(9); continue; }                  /* blank line */
        md_inline(ls, llen, 0, base); fl_nl();                    /* paragraph */
    }
}

/* ---- HTML (a pragmatic subset) ------------------------------------------- */
static int tag_is(const char *t, int len, const char *name)
{ int n=(int)strlen(name); return len>=n && !strncmp(t,name,n) && (len==n || t[n]==' ' || t[n]=='>' || t[n]=='/'); }

static void render_html(const char *src, unoui_rect r, int scroll)
{
    const char *p = src; bstyle st[24]; int sp = 0, li = 0, pre = 0;
    bstyle base = { 1, 0, 0, 0, PG_TEXT };
    st[0] = base;
    fl_reset(r, scroll, 10);
    while (*p) {
        if (*p == '<') {                                         /* a tag */
            const char *ts = ++p; int close = 0;
            if (*p == '/') { close = 1; p++; ts = p; }
            while (*p && *p != '>') p++;
            { int tl = (int)(p - ts); if (*p == '>') p++;
              if (tl && ts[0] == '!') continue;                  /* comment/doctype */
              #define PUSH(mod) do { if (sp<23){ st[sp+1]=st[sp]; sp++; mod; } } while(0)
              #define POP()     do { if (sp>0) sp--; } while(0)
              if (tag_is(ts,tl,"h1")||tag_is(ts,tl,"h2")||tag_is(ts,tl,"h3")||
                  tag_is(ts,tl,"h4")||tag_is(ts,tl,"h5")||tag_is(ts,tl,"h6")) {
                  if (close) { POP(); fl_gap(6); }
                  else { int lvl = ts[1]-'0'; fl_gap(lvl<=2?8:4); PUSH(st[sp].scale=lvl<=1?3:2; st[sp].bold=1; st[sp].color=PG_HEAD); }
              }
              else if (tag_is(ts,tl,"b")||tag_is(ts,tl,"strong")) { if(close)POP(); else PUSH(st[sp].bold=1); }
              else if (tag_is(ts,tl,"i")||tag_is(ts,tl,"em")) { if(close)POP(); else PUSH(st[sp].color=FB_RGB(70,70,95)); }
              else if (tag_is(ts,tl,"code")||tag_is(ts,tl,"tt")) { if(close)POP(); else PUSH(st[sp].mono=1; st[sp].color=PG_CODE); }
              else if (tag_is(ts,tl,"a")) { if(close){POP();} else PUSH(st[sp].color=PG_LINK; st[sp].under=1); }
              else if (tag_is(ts,tl,"pre")) { pre = !close; fl_gap(6); if(!close) PUSH(st[sp].mono=1; st[sp].color=PG_CODE); else POP(); }
              else if (tag_is(ts,tl,"p")||tag_is(ts,tl,"div")||tag_is(ts,tl,"section")) { fl_gap(close?8:0); }
              else if (tag_is(ts,tl,"br")) { fl_nl(); }
              else if (tag_is(ts,tl,"hr")) { fl_gap(6); { int yy=fy-fscroll; if(yy>fclip.y&&yy<fclip.y+fclip.h) fb_hline(fleft,yy,fright-fleft,PG_RULE);} fl_gap(8); }
              else if (tag_is(ts,tl,"ul")||tag_is(ts,tl,"ol")) { fl_gap(close?6:2); }
              else if (tag_is(ts,tl,"li")) { if(!close){ int yy=fy-fscroll+4; fx=fleft; if(yy>fclip.y&&yy<fclip.y+fclip.h) fb_fill_rect(fleft+6,yy,3,3,PG_TEXT); li=18; } else { fl_nl(); li=0; } }
              else if (tag_is(ts,tl,"title")||tag_is(ts,tl,"head")||tag_is(ts,tl,"style")||tag_is(ts,tl,"script")) {
                  if (!close) { const char *e=p; while(*e){ if(*e=='<'&&e[1]=='/') break; e++; } p=e; }  /* skip contents */
              }
              #undef PUSH
              #undef POP
            }
            continue;
        }
        /* run of text until the next tag */
        { const char *ts = p; while (*p && *p != '<') p++;
          { int tl = (int)(p - ts), i, n = 0; char buf[256];
            for (i = 0; i < tl; i++) {                           /* collapse whitespace + entities */
                char c = ts[i];
                if (!pre && (c=='\n'||c=='\t'||c=='\r')) c=' ';
                if (!pre && c==' ' && (n==0 || buf[n-1]==' ')) continue;
                if (c=='&') { if(!strncmp(ts+i,"&lt;",4)){c='<';i+=3;} else if(!strncmp(ts+i,"&gt;",4)){c='>';i+=3;}
                              else if(!strncmp(ts+i,"&amp;",5)){c='&';i+=4;} else if(!strncmp(ts+i,"&nbsp;",6)){c=' ';i+=5;} }
                if (n<255) buf[n++]=c;
            }
            buf[n]=0;
            if (n) { if (pre) { fx=fleft; fl_word(buf,12,&st[sp]); fl_nl(); } else fl_text(buf,n,li,&st[sp]); } }
        }
    }
}

/* =================== the browser app (canvas) ============================= */
#define DOC_MAX 32768
static char g_doc[DOC_MAX];
static int  g_is_html, g_scroll, g_view;      /* g_view: 0 list, 1 document */
static char g_title[48] = "UnoDOS Browser";

/* the file list: built-in demos + whatever the file system offers */
#define MAXFILES 40
static char g_names[MAXFILES][32];
static int  g_vol[MAXFILES];                  /* -1 = built-in demo */
static int  g_nfiles, g_sel;

static const char kWelcome[] =
"# UnoDOS Browser\n\n"
"A tiny **HTML / Markdown / CSS** renderer for pc64. It lays out a document and "
"paints it straight into the framebuffer - *no JavaScript, no network* yet.\n\n"
"## Features\n\n"
"- Headings, **bold**, *italic*, `inline code`\n"
"- Bullet lists and paragraphs with word-wrap\n"
"- [Links](none) and horizontal rules\n"
"- Loads files from the local disks (see the file list)\n\n"
"---\n\n"
"## Try it\n\n"
"Press **Backspace** to return to the file list, pick a `.md` / `.html` / `.txt`\n"
"file and press Enter. Scroll with the Up / Down / PgUp / PgDn keys.\n\n"
"> UnoDOS runs bare-metal on x86-64 UEFI - this page is drawn by the same\n"
"> software framebuffer that draws the whole desktop.\n";

static const char kSample[] =
"<h1>HTML sample</h1>"
"<p>This page is <b>HTML</b>, rendered by the same engine. It supports "
"<i>emphasis</i>, <code>code spans</code>, and <a href='none'>links</a>.</p>"
"<h2>A list</h2><ul><li>first item</li><li>second item</li><li>third item</li></ul>"
"<hr><pre>  pre-formatted text\n  keeps its   spacing</pre>"
"<p>Unknown tags are ignored; their text still shows.</p>";

static long fs_load(int vol, const char *name, char *buf, long max);   /* fwd */

static void detect_kind(const char *name) { int n=(int)strlen(name);
    g_is_html = (n>5 && !strcmp(name+n-5,".html")) || (n>4 && !strcmp(name+n-4,".htm")); }

static void open_entry(int idx)
{
    if (idx < 0 || idx >= g_nfiles) return;
    g_scroll = 0;
    if (g_vol[idx] < 0) {                          /* built-in demo */
        const char *d = (idx == 0) ? kWelcome : kSample;
        strncpy(g_doc, d, DOC_MAX-1); g_doc[DOC_MAX-1]=0;
        g_is_html = (idx == 1);
    } else {
        long n = fs_load(g_vol[idx], g_names[idx], g_doc, DOC_MAX-1);
        if (n < 0) n = 0; g_doc[n] = 0;
        detect_kind(g_names[idx]);
    }
    strncpy(g_title, g_names[idx], 47); g_title[47]=0;
    g_view = 1;
}

static void refresh_list(void)
{
    int v, nv;
    g_nfiles = 0;
    strcpy(g_names[g_nfiles], "Welcome.md");  g_vol[g_nfiles++] = -1;
    strcpy(g_names[g_nfiles], "Sample.html"); g_vol[g_nfiles++] = -1;
    nv = uno_fs_volumes();
    for (v = 0; v < nv && g_nfiles < MAXFILES; v++) {
        char nm[32]; int i, cnt = uno_fs_list_begin(v);
        for (i = 0; i < cnt && g_nfiles < MAXFILES; i++)
            if (uno_fs_list_get(v, i, nm, sizeof nm)) {
                strncpy(g_names[g_nfiles], nm, 31); g_names[g_nfiles][31]=0;
                g_vol[g_nfiles++] = v;
            }
    }
    if (g_sel >= g_nfiles) g_sel = g_nfiles - 1;
    if (g_sel < 0) g_sel = 0;
}

static long fs_load(int vol, const char *name, char *buf, long max)
{ return uno_fs_read(vol, name, (unsigned char *)buf, max); }

/* ---- rendering ----------------------------------------------------------- */
static void br_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    (void)w; (void)ctx;
    fb_fill_rect(r.x, r.y, r.w, r.h, PG_BG);
    if (g_view == 0) {                              /* file list */
        int i; unoui_rect lr = r;
        fb_fill_rect(r.x, r.y, r.w, 20, FB_RGB(40, 60, 110));
        fb_text(r.x + 8, r.y + 6, "Open a page  (Up/Down + Enter)", FB_RGB(255,255,255), -1);
        for (i = 0; i < g_nfiles; i++) {
            int y = r.y + 26 + i * 16;
            if (y > r.y + r.h - 12) break;
            if (i == g_sel) fb_fill_rect(r.x + 2, y - 2, r.w - 4, 15, FB_RGB(210, 225, 250));
            fb_text(r.x + 24, y, g_names[i], PG_TEXT, -1);
            fb_text(r.x + 8, y, g_vol[i] < 0 ? "*" : ">", g_vol[i]<0?PG_LINK:PG_QUOTE, -1);
        }
        (void)lr; return;
    }
    /* document view: title bar + body */
    fb_fill_rect(r.x, r.y, r.w, 18, FB_RGB(40, 60, 110));
    fb_text(r.x + 8, r.y + 5, g_title, FB_RGB(255,255,255), -1);
    fb_text(r.x + r.w - 130, r.y + 5, "Bksp: files", FB_RGB(180,200,235), -1);
    { unoui_rect body = { r.x, r.y + 20, r.w, r.h - 20 };
      if (g_is_html) render_html(g_doc, body, g_scroll);
      else           render_md(g_doc, body, g_scroll);
      /* track content height for scroll clamping */
      { int total = (fy + flh) - body.y; if (g_scroll > total - body.h) g_scroll = total - body.h;
        if (g_scroll < 0) g_scroll = 0; } }
}

static int br_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w; (void)ctx;
    if (g_view == 0) {                              /* file list */
        if (e->kind == UI_EV_KEY) {
            if (e->key == UI_KEY_DOWN && g_sel < g_nfiles-1) { g_sel++; return 1; }
            if (e->key == UI_KEY_UP   && g_sel > 0)          { g_sel--; return 1; }
            if (e->key == UI_KEY_ENTER) { open_entry(g_sel); return 1; }
        }
        return 0;
    }
    if (e->kind == UI_EV_KEY) {                      /* document */
        if (e->key == UI_KEY_DOWN)      { g_scroll += 24; return 1; }
        if (e->key == UI_KEY_UP)        { g_scroll -= 24; if (g_scroll<0) g_scroll=0; return 1; }
        if (e->key == UI_KEY_PGDN)      { g_scroll += 180; return 1; }
        if (e->key == UI_KEY_PGUP)      { g_scroll -= 180; if (g_scroll<0) g_scroll=0; return 1; }
        if (e->key == UI_KEY_BACKSPACE) { g_view = 0; refresh_list(); return 1; }
    }
    if (e->kind == UI_EV_WHEEL) { g_scroll += e->wheel * 24; if (g_scroll<0) g_scroll=0; return 1; }
    return 0;
}

static unoui_canvas g_browser = { br_draw, br_event, 0 };

unoui_canvas *pc64_browser_canvas(void) { return &g_browser; }
void pc64_browser_open(void) { g_sel = 0; g_view = 0; refresh_list(); }
