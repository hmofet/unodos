/* ===========================================================================
 * UnoDOS/pc64 - a minimal web browser (HTML + Markdown + basic CSS).
 *
 * A native unoui canvas app: it lays out a document and paints it with fb
 * primitives, wrapping text to the canvas width and scaling headings. It runs
 * <script> blocks through a tiny interpreter (js.c) and loads pages over the
 * network (address bar -> pc64_http GET over e1000/TCP; HTTP + CA-validated HTTPS).
 * It also renders a built-in welcome page and opens files from the local file
 * system (uno_fs_*: the RAM disk and FAT32/local disks).
 *
 * Rendering model: a single immediate-mode flow. A cursor walks left-to-right
 * wrapping words; block elements start new lines and add vertical gaps; a small
 * style state (scale / bold / underline / colour) is set from Markdown syntax
 * or HTML tags + their default (and inline) CSS. Good enough for docs, help
 * pages and READMEs; not a spec-complete engine.
 * ======================================================================== */
#include "unoui.h"
#include "fb.h"
#include "pc64_font.h"
#include "pc64_fs.h"
#include "js.h"
#include "pc64_http.h"
#include <string.h>

void uno_pc64_present(void);       /* fb -> GOP (for a Loading frame mid-fetch) */

/* ---- page palette (a light "document" look) ------------------------------ */
#define PG_BG    FB_RGB(250, 250, 248)
#define PG_TEXT  FB_RGB(30, 32, 40)
#define PG_HEAD  FB_RGB(20, 40, 90)
#define PG_LINK  FB_RGB(40, 90, 210)
#define PG_CODE  FB_RGB(150, 40, 40)
#define PG_CODEBG FB_RGB(235, 235, 230)
#define PG_QUOTE FB_RGB(90, 100, 120)
#define PG_RULE  FB_RGB(200, 200, 195)

/* ---- content faces (fixed: page content doesn't follow the UI font) ------ */
#define BR_BODY_SLOT 1                 /* Sans - body + headings */
#define BR_MONO_SLOT 2                 /* Mono - code spans/blocks */
#define BR_BODY_PX   14                /* base body size; scale N -> 14+(N-1)*7 */

typedef struct { int scale, bold, ital, under, mono; fb_px color; } bstyle;

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

/* run parameters for a style: content face, pixel size, style bits */
static int br_slot(const bstyle *s)  { return s->mono ? BR_MONO_SLOT : BR_BODY_SLOT; }
static int br_px(const bstyle *s)    { int sc = s->scale ? s->scale : 1;
                                       return BR_BODY_PX + (sc - 1) * (BR_BODY_PX / 2); }
static int br_style(const bstyle *s) { return (s->bold ? UNO_FS_BOLD   : 0)
                                            | (s->ital ? UNO_FS_ITALIC : 0); }

/* place one word (already NUL-terminated in `buf`); wraps at the right edge */
static void fl_word(const char *buf, int indent, bstyle *s)
{
    int slot = br_slot(s), px = br_px(s), st = br_style(s);
    int ww = uno_font_text_w_styled(slot, px, st, buf);
    int ch = uno_font_height_px(slot, px);           /* cell height */
    int lh = ch + 3, dy;                             /* + leading */
    int sp = uno_font_text_w_styled(slot, px, st, " ");
    if (sp <= 0) sp = px / 2;
    if (lh > flh) flh = lh;
    if (fx + ww > fright && fx > fleft + indent) fl_nl();
    if (fx == fleft) fx = fleft + indent;
    dy = fy - fscroll;
    if (dy > fclip.y - lh && dy < fclip.y + fclip.h) {          /* visible row */
        if (s->mono) fb_fill_rect(fx - 1, dy - 1, ww + 2, ch + 2, PG_CODEBG);
        uno_font_draw_styled(slot, px, st, fx, dy, buf, s->color, -1);
        if (s->under) { int bl = uno_font_baseline_px(slot, px);
                        fb_hline(fx, dy + bl + 2, ww, s->color); }
    }
    fx += ww + sp;                                              /* + space */
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
            else if (s[i] == '*') {                              /* toggle italic */
                cur.ital ^= 1; cur.color = cur.ital ? FB_RGB(70,70,90) : base.color;
            }
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
    bstyle base = { 1, 0, 0, 0, 0, PG_TEXT };
    fl_reset(r, scroll, 10);
    while (*p) {
        const char *ls = p; int llen;
        while (*p && *p != '\n') p++;
        llen = (int)(p - ls); if (*p == '\n') p++;
        if (llen >= 3 && ls[0] == '`' && ls[1] == '`' && ls[2] == '`') { code = !code; fl_gap(4); continue; }
        if (code) {                                              /* code block */
            bstyle cs = { 1, 0, 0, 0, 1, PG_CODE };
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
    bstyle base = { 1, 0, 0, 0, 0, PG_TEXT };
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
              else if (tag_is(ts,tl,"i")||tag_is(ts,tl,"em")) { if(close)POP(); else PUSH(st[sp].ital=1; st[sp].color=FB_RGB(70,70,95)); }
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
static char g_url[256] = "https://";           /* address bar buffer */
static char g_status[128];                     /* last fetch status / hint */
static int  g_addr;                            /* 1 = editing the address bar */
static unoui_rect g_rect;                      /* last-drawn rect (Loading present) */

/* the file list: built-in demos + whatever the file system offers */
#define MAXFILES 40
static char g_names[MAXFILES][32];
static int  g_vol[MAXFILES];                  /* -1 = built-in demo */
static int  g_nfiles, g_sel;

static const char kWelcome[] =
"# UnoDOS Browser\n\n"
"A tiny **HTML / Markdown / CSS** renderer for pc64. It lays out a document and "
"paints it straight into the framebuffer, *runs JavaScript*, and *loads pages "
"over the network*.\n\n"
"## Features\n\n"
"- Headings, **bold**, *italic*, `inline code`\n"
"- Bullet lists and paragraphs with word-wrap\n"
"- Runs `<script>` blocks (see the **Script.html** demo)\n"
"- Loads files from the local disks (see the file list)\n"
"- **Network**: type a `http://` or `https://` URL in the address bar and press "
"Enter (Up from the file list focuses the bar). HTTPS is CA-validated (TLS 1.2)"
" against a bundled root store, using the system clock.\n\n"
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

static const char kScript[] =
"<h1>JavaScript</h1>"
"<p>The <code>&lt;script&gt;</code> block below runs in a tiny tree-walking "
"interpreter; its <code>document.write</code> output is spliced into the page "
"and <code>console.log</code> appears in the console panel.</p>"
"<script>"
"document.write('<h2>Generated from JS</h2>');"
"function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); }"
"document.write('<p>A Fibonacci table:</p><ul>');"
"for (var i = 0; i < 8; i++)"
"  document.write('<li>fib(' + i + ') = ' + fib(i) + '</li>');"
"document.write('</ul>');"
"var sum = 0; for (var k = 1; k <= 10; k++) sum += k;"
"document.write('<p>Sum 1..10 = <b>' + sum + '</b>, sqrt(2) = ' + Math.sqrt(2) + '</p>');"
"console.log('script ran; fib(10) =', fib(10));"
"console.log('Math.floor(3.7) =', Math.floor(3.7));"
"</script>"
"<hr><p>Everything above the rule was produced at open time by the script.</p>";

static long fs_load(int vol, const char *name, char *buf, long max);   /* fwd */

static void detect_kind(const char *name) { int n=(int)strlen(name);
    g_is_html = (n>5 && !strcmp(name+n-5,".html")) || (n>4 && !strcmp(name+n-4,".htm")); }

/* Run <script> blocks in g_doc: replace each with its document.write output and
 * collect console.log lines into a "console" panel appended at the end. A tiny
 * tree-walking interpreter (js.c) does the work; this is the HTML<->JS glue. */
static int ci(char c){ return (c>='A'&&c<='Z') ? c+32 : c; }
static int tag_at(const char *p, const char *name)   /* case-insensitive "<name" / "</name" */
{ while (*name){ if (ci(*p)!=*name) return 0; p++; name++; } return 1; }

static void js_expand(void)
{
    static char out[DOC_MAX];
    static char code[8192], wbuf[8192], logbuf[4096], lbuf[2048];
    int oi = 0, haslog = 0;
    const char *p = g_doc;
    logbuf[0] = 0;
    while (*p && oi < DOC_MAX-1) {
        if (p[0]=='<' && tag_at(p+1,"script")) {
            const char *s = p + 7; while (*s && *s!='>') s++; if (*s=='>') s++;
            const char *e = s;
            while (*e) { if (e[0]=='<' && e[1]=='/' && tag_at(e+2,"script")) break; e++; }
            int inlen = (int)(e - s); if (inlen > (int)sizeof(code)-1) inlen = sizeof(code)-1;
            memcpy(code, s, inlen); code[inlen] = 0;
            wbuf[0] = 0; lbuf[0] = 0;
            js_run(code, wbuf, sizeof wbuf, lbuf, sizeof lbuf);
            int wl = (int)strlen(wbuf);
            if (wl > DOC_MAX-1-oi) wl = DOC_MAX-1-oi;
            memcpy(out+oi, wbuf, wl); oi += wl;
            if (lbuf[0]) { int have=(int)strlen(logbuf), ll=(int)strlen(lbuf);
                if (have+ll < (int)sizeof(logbuf)-1) memcpy(logbuf+have, lbuf, ll+1); haslog = 1; }
            p = e; while (*p && *p!='>') p++; if (*p=='>') p++;
            continue;
        }
        out[oi++] = *p++;
    }
    out[oi] = 0;
    if (haslog) {                                    /* a small console panel */
        const char *h = "<hr><h3>console</h3><pre>", *f = "</pre>";
        int hl=(int)strlen(h), ll=(int)strlen(logbuf), fl2=(int)strlen(f);
        if (oi+hl+ll+fl2 < DOC_MAX-1) {
            memcpy(out+oi,h,hl); oi+=hl; memcpy(out+oi,logbuf,ll); oi+=ll;
            memcpy(out+oi,f,fl2); oi+=fl2; out[oi]=0;
        }
    }
    memcpy(g_doc, out, oi+1);
}

static void open_entry(int idx)
{
    if (idx < 0 || idx >= g_nfiles) return;
    g_scroll = 0;
    if (g_vol[idx] < 0) {                          /* built-in demo */
        const char *d = (idx == 0) ? kWelcome : (idx == 1) ? kSample : kScript;
        strncpy(g_doc, d, DOC_MAX-1); g_doc[DOC_MAX-1]=0;
        g_is_html = (idx >= 1);
    } else {
        long n = fs_load(g_vol[idx], g_names[idx], g_doc, DOC_MAX-1);
        if (n < 0) n = 0; g_doc[n] = 0;
        detect_kind(g_names[idx]);
    }
    if (g_is_html) js_expand();                    /* run any <script> blocks */
    strncpy(g_title, g_names[idx], 47); g_title[47]=0;
    g_view = 1;
}

static void refresh_list(void)
{
    int v, nv;
    g_nfiles = 0;
    strcpy(g_names[g_nfiles], "Welcome.md");  g_vol[g_nfiles++] = -1;
    strcpy(g_names[g_nfiles], "Sample.html"); g_vol[g_nfiles++] = -1;
    strcpy(g_names[g_nfiles], "Script.html"); g_vol[g_nfiles++] = -1;
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

/* ---- network fetch ------------------------------------------------------- */
static void url_kind(const char *url)          /* pick MD vs HTML from suffix */
{
    int n = (int)strlen(url), q = n;
    int i; for (i = 0; i < n; i++) if (url[i]=='?' || url[i]=='#') { q = i; break; }
    g_is_html = 1;                              /* default: HTML (handles plain text) */
    if (q >= 3 && !strncmp(url+q-3, ".md", 3)) g_is_html = 0;
    else if (q >= 4 && !strncmp(url+q-4, ".txt", 4)) g_is_html = 0;
}

static void loading_frame(void)                 /* one presented "Loading" frame */
{
    unoui_rect r = g_rect;
    fb_fill_rect(r.x, r.y, r.w, r.h, PG_BG);
    fb_fill_rect(r.x, r.y, r.w, 20, FB_RGB(40, 60, 110));
    fb_text(r.x + 8, r.y + 6, "Loading...", FB_RGB(255,255,255), -1);
    fb_text(r.x + 12, r.y + 44, g_url, PG_LINK, -1);
    uno_pc64_present();
}

static void fetch_url(void)
{
    int n;
    loading_frame();                            /* show progress before we block */
    n = pc64_http_get(g_url, g_doc, DOC_MAX-1, g_status, sizeof g_status);
    g_scroll = 0;
    if (n < 0) {                                /* build an error page */
        char *d = g_doc; int o = 0;
        const char *a = "<h1>Couldn't load the page</h1><p><b>URL:</b> ";
        const char *b = "</p><p><b>Reason:</b> ";
        const char *c = "</p><hr><p>The address bar takes <code>http://host/path</code> "
                        "or <code>https://</code>. HTTPS and DNS need a working link (QEMU "
                        "SLIRP provides both; the X1 has no wired NIC).</p>";
        #define APP(s) do { int l=(int)strlen(s); if(o+l<DOC_MAX-1){memcpy(d+o,s,l);o+=l;} } while(0)
        APP(a); APP(g_url); APP(b); APP(g_status[0]?g_status:"unknown"); APP(c);
        #undef APP
        d[o] = 0; g_is_html = 1;
    } else {
        url_kind(g_url);
        if (g_is_html) js_expand();
    }
    strncpy(g_title, g_url, 47); g_title[47] = 0;
    g_view = 1;
}

/* ---- rendering ----------------------------------------------------------- */
static void br_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    (void)w; (void)ctx;
    g_rect = r;
    fb_fill_rect(r.x, r.y, r.w, r.h, PG_BG);
    if (g_view == 0) {                              /* file list + address bar */
        int i, ay = r.y + 24, top;
        fb_fill_rect(r.x, r.y, r.w, 20, FB_RGB(40, 60, 110));
        fb_text(r.x + 8, r.y + 6, "UnoDOS Browser", FB_RGB(255,255,255), -1);
        /* address bar */
        fb_fill_rect(r.x + 6, ay, r.w - 12, 18, FB_RGB(255,255,255));
        fb_frame_rect(r.x + 6, ay, r.w - 12, 18, g_addr ? PG_LINK : PG_RULE);
        fb_text(r.x + 12, ay + 5, g_url, PG_TEXT, -1);
        if (g_addr) { int cx = r.x + 12 + fb_text_w(g_url);
                      fb_vline(cx, ay + 3, 12, PG_TEXT); }
        fb_text(r.x + 8, ay + 22,
                g_status[0] ? g_status
                            : "Type a URL + Enter  -  or Down to the file list",
                g_status[0] ? PG_QUOTE : FB_RGB(120,120,130), -1);
        top = ay + 40;
        for (i = 0; i < g_nfiles; i++) {
            int y = top + i * 16;
            if (y > r.y + r.h - 12) break;
            if (!g_addr && i == g_sel) fb_fill_rect(r.x + 2, y - 2, r.w - 4, 15, FB_RGB(210, 225, 250));
            fb_text(r.x + 24, y, g_names[i], PG_TEXT, -1);
            fb_text(r.x + 8, y, g_vol[i] < 0 ? "*" : ">", g_vol[i]<0?PG_LINK:PG_QUOTE, -1);
        }
        return;
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
    if (g_view == 0) {                              /* file list + address bar */
        if (e->kind == UI_EV_CHAR && e->ch >= 32 && e->ch < 127) {
            if (!g_addr) { g_addr = 1; g_url[0] = 0; }   /* typing focuses the bar */
            { int l = (int)strlen(g_url); if (l < (int)sizeof(g_url)-1) { g_url[l] = (char)e->ch; g_url[l+1] = 0; } }
            return 1;
        }
        if (e->kind == UI_EV_KEY) {
            if (g_addr) {
                if (e->key == UI_KEY_BACKSPACE) { int l=(int)strlen(g_url); if (l>0) g_url[l-1]=0; return 1; }
                if (e->key == UI_KEY_ENTER)     { if (g_url[0]) fetch_url(); return 1; }
                if (e->key == UI_KEY_ESC || e->key == UI_KEY_DOWN) { g_addr = 0; return 1; }
                return 1;                       /* swallow other keys while editing */
            }
            if (e->key == UI_KEY_UP)   { if (g_sel > 0) g_sel--; else g_addr = 1; return 1; }
            if (e->key == UI_KEY_DOWN && g_sel < g_nfiles-1) { g_sel++; return 1; }
            if (e->key == UI_KEY_ENTER) { open_entry(g_sel); return 1; }
        }
        if (e->kind == UI_EV_MOUSE_DOWN) {      /* click bar or a file row (metal) */
            unoui_rect r = g_rect; int ay = r.y + 24, top = ay + 40;
            if (e->y >= ay && e->y < ay + 18) { g_addr = 1; return 1; }
            { int row = (e->y - top) / 16;
              if (row >= 0 && row < g_nfiles && e->y >= top) { g_addr = 0; g_sel = row; open_entry(row); return 1; } }
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
