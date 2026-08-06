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
#include "unoui_theme.h"      /* the chrome reads the shell's palette */
#include "fb.h"
#include "pc64_font.h"
#include "pc64_fs.h"
#include "js.h"
#include "../csslib/uwx.h"
#include "../unoweb/unoweb.h"
#include "../unomedia/unomedia.h"
#include <stdlib.h>
#include "pc64_http.h"
#include "pc64_fetch.h"
#include "pc64_native.h"
#include "webjs.h"
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

/* Location buffer size. Hoisted above the rest of the browser's constants
 * because the subresource fetch code (page base, link sheets, network
 * images) sits with the DOM helpers, far above the chrome section where the
 * other sizes are declared. */
#define LOCMAX    256

/* ---- content faces (fixed: page content doesn't follow the UI font) ------ */
#define BR_BODY_SLOT 1                 /* Sans - body + headings */
#define BR_MONO_SLOT 2                 /* Mono - code spans/blocks */
#define BR_BODY_PX   14                /* base body size; scale N -> 14+(N-1)*7 */

typedef struct { int scale, bold, ital, under, mono; fb_px color; } bstyle;

/* ---- the layout cursor (one active flow at a time) ----------------------- */
static int fx, fy, fleft, fright, fscroll, flh;
static unoui_rect fclip;

/* ---- the link map, built as the page paints ------------------------------
 * The flow painter drew links but kept no record of WHERE, so outside the
 * unoweb engine path (the default build) a link could be neither clicked nor
 * keyboard-selected. Every painted word of a link now contributes a rect in
 * DOCUMENT space (the unscrolled y), so one hit test works at any scroll. */
#define BR_MAXLINK 192
#define BR_MAXHREF 64
#define BR_HREFMAX 200
typedef struct { short x, y, w, h, href; } blinkrect;
static blinkrect g_link[BR_MAXLINK];
static int  g_nlink;
static char g_href[BR_MAXHREF][BR_HREFMAX];
static int  g_nhref;
static int  g_link_cur = -1;      /* href index being painted (-1 = plain text) */
static int  g_link_sel = -1;      /* keyboard-selected link                     */

static int link_add(const char *s, int len)
{
    int i;
    if (len <= 0 || len >= BR_HREFMAX) return -1;
    for (i = 0; i < g_nhref; i++)                    /* one entry per target */
        if (!strncmp(g_href[i], s, (size_t)len) && !g_href[i][len]) return i;
    if (g_nhref >= BR_MAXHREF) return -1;
    memcpy(g_href[g_nhref], s, (size_t)len);
    g_href[g_nhref][len] = 0;
    return g_nhref++;
}

/* ---- find in page ---------------------------------------------------------
 * Matches are collected the same way links are: as the page PAINTS, because
 * the painter is the only thing that knows where a word ends up. That keeps
 * one geometry source for finding, highlighting and scrolling-to, and it
 * means find works on whatever the renderer drew rather than on a second,
 * possibly disagreeing, pass over the source text.
 *
 * The engine path collects from the display list instead (same rects, same
 * document coordinates), so both renderers answer Ctrl-F. */
#define FIND_MAX  128
#define FIND_TEXT 64
static char g_find[FIND_TEXT];      /* the needle; empty = find is off      */
static int  g_find_on;              /* the find bar has the keyboard        */
static blinkrect g_findhit[FIND_MAX];
static int  g_nfind, g_find_sel;
static int  g_find_collect;         /* 1 while a paint pass is collecting   */

/* ASCII-caseless substring: a find that only matched the user's exact case
 * would be useless on prose. */
static int find_in(const char *hay, const char *needle)
{
    int nl = (int)strlen(needle), i, k;
    if (!nl) return 0;
    for (i = 0; hay[i]; i++) {
        for (k = 0; k < nl; k++) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (!a || a != b) break;
        }
        if (k == nl) return 1;
    }
    return 0;
}

static void find_add(int x, int y, int w, int h)
{
    if (g_nfind >= FIND_MAX) return;
    g_findhit[g_nfind].x = (short)x; g_findhit[g_nfind].y = (short)y;
    g_findhit[g_nfind].w = (short)w; g_findhit[g_nfind].h = (short)h;
    g_findhit[g_nfind].href = -1;
    g_nfind++;
}

static void fl_reset(unoui_rect r, int scroll, int pad)
{
    fleft = r.x + pad; fright = r.x + r.w - pad;
    fx = fleft; fy = r.y + pad; fscroll = scroll; fclip = r; flh = 12;
    g_nlink = 0; g_nhref = 0; g_link_cur = -1;
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
    if (g_link_cur >= 0 && g_nlink < BR_MAXLINK) {       /* clickable ink */
        blinkrect *L = &g_link[g_nlink++];
        L->x = (short)fx; L->y = (short)fy;
        L->w = (short)ww; L->h = (short)lh; L->href = (short)g_link_cur;
    }
    if (g_find_collect && g_find[0] && find_in(buf, g_find))
        find_add(fx, fy, ww, lh);
    dy = fy - fscroll;
    if (dy > fclip.y - lh && dy < fclip.y + fclip.h) {          /* visible row */
        if (g_link_cur >= 0 && g_link_cur == g_link_sel)         /* selected */
            fb_fill_rect(fx - 1, dy - 1, ww + 2, ch + 2, FB_RGB(215, 228, 250));
        /* a match is painted UNDER the word, and the current one is the
         * loud colour - a page with forty hits is unreadable if they all
         * shout equally */
        if (g_find[0] && find_in(buf, g_find)) {
            int hit = -1, i;
            for (i = 0; i < g_nfind; i++)
                if (g_findhit[i].x == (short)fx && g_findhit[i].y == (short)fy) { hit = i; break; }
            fb_fill_rect(fx - 1, dy - 1, ww + 2, ch + 2,
                         hit == g_find_sel ? FB_RGB(255, 190, 60)
                                           : FB_RGB(255, 240, 150));
        }
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
                int te = i + 1, us = 0, ue = 0;
                while (te < len && s[te] != ']') te++;
                if (te + 1 < len && s[te + 1] == '(') {          /* the target */
                    us = te + 2; ue = us;
                    while (ue < len && s[ue] != ')') ue++;
                }
                g_link_cur = (ue > us) ? link_add(s + us, ue - us) : -1;
                { bstyle lk = base; lk.color = PG_LINK; lk.under = 1;
                  fl_text(s + i + 1, te - (i + 1), indent, &lk); }
                g_link_cur = -1;
                i = (ue > te) ? ue : te;
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

/* ---- HTML: parsed by unoweb, painted by the flow above --------------------
 * This used to be an inline tag scanner over the source text. It now walks a
 * real DOM: unoweb parses the document once per load (uw_parse_string), and
 * the walk below turns element nesting into the same style stack the flow
 * painter always used. The gain is everything a scanner cannot do - correct
 * nesting through unclosed tags, character references, quoted attributes with
 * '>' inside them, comments, and RAWTEXT so a '<' in a <style> block is not
 * mistaken for markup.
 *
 * The DOM is cached: br_draw runs every frame, and re-parsing per frame would
 * be absurd, so dom_sync() re-parses only when the source actually changes. */
#ifdef UW_ENGINE
static void img_cache_reset(void);   /* defined with the image cache below */
#endif
static uw_doc  *g_dom;
static unsigned g_dom_sig;
/* Which TREE this is, as opposed to which SOURCE it came from. The two are not
 * the same thing and the difference rendered pages blank: the layout cache
 * keyed off the source fingerprint, so re-parsing the SAME text - which is
 * exactly what a progressive paint followed by the completed load does -
 * produced a brand-new tree that compared equal to the one already laid out,
 * and was therefore never laid out or painted at all. Bumped once per rebuild,
 * so a new tree can never be mistaken for the one on screen. */
static unsigned g_dom_gen;

/* Fingerprint the source rather than plumb an "invalidate" call through every
 * loader: g_doc is refilled from several places (a built-in demo, a local
 * file, a fetch, and the not-found page), and one missed call would leave a
 * stale tree on screen. Hashing 32 KB once per frame is nothing next to
 * painting it. */
static unsigned doc_sig(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h ? h : 1;
}

/* The location the current document came from: subresource URLs resolve
 * against it. Set by load_loc; "" for the built-in pages, which reference
 * nothing external. */
static char g_page_base[LOCMAX];
static void resolve(const char *href, const char *base, char *out, int cap);
static int  loc_is_net(const char *loc);

/* RE-ENTRANCY GUARD - set in load_progress, read by fetch_link_sheets.
 * A progressive repaint must not start a NESTED fetch on the single TCP slot
 * the outer response is still using. Declared outside the engine guard
 * because progressive rendering happens in BOTH builds; only the thing it
 * protects is engine-only. */
static int g_in_progress_paint;

#ifdef UW_ENGINE
static void fetch_link_sheets(uw_doc *d);        /* defined with the URL helpers */
static void prefetch_subresources(uw_doc *d);    /* likewise */

/* ---- page scripts on the live DOM (M5) ------------------------------------
 * One VM per page, built here and torn down by the next navigation. Every
 * <script> in the document runs in it, in document order, and whatever it
 * leaves behind - handlers, timers, closures - stays alive for the page.
 * The console output of all of them shares one buffer, surfaced by the
 * status line rather than appended to the document: the document is now a
 * real tree, and stapling a console panel onto it would mutate the very
 * thing the script is manipulating. */
static char g_js_log[2048];

#ifdef UW_ENGINE
/* ---- forms ----------------------------------------------------------------
 * The focused control, and submission. A control's VALUE lives in its own
 * `value` attribute rather than in a side table: the DOM already has to hold
 * it (script reads and writes it), submission just walks the form reading
 * attributes, and a re-render cannot lose what the user typed. */
static uw_node *g_form_focus;

/* percent-encode into a form body */
static int form_enc(char *out, int cap, int at, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s && at < cap - 4; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out[at++] = (char)ch;
        else if (ch == ' ') out[at++] = '+';
        else { out[at++] = '%'; out[at++] = hex[ch >> 4]; out[at++] = hex[ch & 15]; }
    }
    out[at] = 0;
    return at;
}

/* the <form> enclosing `n`, or NULL */
static uw_node *form_of(uw_node *n)
{
    for (; n; n = uw_parent(n))
        if (uw_type(n) == UW_NODE_ELEMENT && !strcmp(uw_tag_name(g_dom, n), "form"))
            return n;
    return NULL;
}

/* Build "a=1&b=2" from every named control in `form`. */
static int form_body(uw_node *form, char *out, int cap)
{
    uw_node *n;
    int at = 0;
    for (n = uw_next_in_order(form, form); n; n = uw_next_in_order(n, form)) {
        const char *nm, *val, *type, *tg;
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        tg = uw_tag_name(g_dom, n);
        if (strcmp(tg, "input") && strcmp(tg, "textarea") && strcmp(tg, "select")) continue;
        nm = uw_attr(g_dom, n, "name");
        if (!nm || !*nm) continue;                  /* unnamed: not submitted */
        type = uw_attr(g_dom, n, "type");
        if (type && (!strcmp(type, "submit") || !strcmp(type, "button") ||
                     !strcmp(type, "reset"))) continue;
        val = uw_attr(g_dom, n, "value");
        if (at) { if (at < cap - 1) out[at++] = '&'; }
        at = form_enc(out, cap, at, nm);
        if (at < cap - 1) out[at++] = '=';
        at = form_enc(out, cap, at, val ? val : "");
    }
    out[at] = 0;
    return at;
}
#endif /* UW_ENGINE */


/* Milliseconds since this page's VM was built. The TSC is the only clock a
 * PRODUCTION build has (uno_dbg_uptime_ms is debug-only), and it is the same
 * source the shell's animation clock uses. 0 when uncalibrated, which simply
 * means timers all come due at once - correct, if not smooth. */
static unsigned page_clock_ms(void)
{
    static unsigned long long t0;
    unsigned long long per_ms = uno_native_tsc_per_us() * 1000ull, now;
    if (!per_ms) return 0;
    now = uno_native_rdtsc();
    if (!t0) t0 = now;
    return (unsigned)((now - t0) / per_ms);
}

static void run_page_scripts(uw_doc *d)
{
    uw_node *n;
    if (!d) return;
    g_js_log[0] = 0;
    if (webjs_page_begin(d) != 0) return;
    for (n = uw_next_in_order(uw_document(d), uw_document(d)); n;
         n = uw_next_in_order(n, uw_document(d))) {
        uw_node *t;
        const char *src;
        int len = 0;
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        if (strcmp(uw_tag_name(d, n), "script")) continue;
        if (uw_attr(d, n, "src")) continue;      /* external: M5b, with the
                                                  * fetch queue in front */
        t = uw_first_child(n);
        src = t ? uw_text(t, &len) : NULL;
        if (src && len > 0)
            webjs_run(src, len, g_js_log, sizeof g_js_log);
    }
    webjs_event(uw_body(d), "load", g_js_log, sizeof g_js_log);
    webjs_take_dirty();          /* the first layout has not happened yet */
}
#endif

static void dom_sync(const char *src)
{
    uw_config c;
    unsigned sig = doc_sig(src);
    if (g_dom && sig == g_dom_sig) return;
    if (g_dom) { uw_doc_free(g_dom); g_dom = NULL; }
#ifdef UW_ENGINE
    img_cache_reset();               /* decoded frames belong to the old page */
#endif
    memset(&c, 0, sizeof c);
    c.arena_max = 2u << 20;          /* a page's tree, bounded */
    c.max_depth = 96;
    g_dom = uw_parse_string(src, -1, &c);
    g_dom_sig = sig;
    g_dom_gen++;                     /* a new tree, whatever the text says */
#ifdef UW_ENGINE
    prefetch_subresources(g_dom);    /* ask for everything before needing any */
    fetch_link_sheets(g_dom);
    run_page_scripts(g_dom);
#endif
}

/* Emit a text node. Outside <pre>, every run of whitespace (including the
 * newlines the source is wrapped with) collapses to one space; inside <pre>
 * each source line is placed as its own row. */
static void emit_text(const char *s, int len, int pre, int indent, bstyle *st)
{
    char buf[256];
    int i = 0, n = 0;
    if (pre) {
        for (i = 0; i <= len; i++) {
            if (i == len || s[i] == '\n') {
                buf[n] = 0;
                fx = fleft;
                if (n) fl_word(buf, 12, st);
                fl_nl();
                n = 0;
                continue;
            }
            if (n < (int)sizeof buf - 1) buf[n++] = s[i];
        }
        return;
    }
    for (i = 0; i < len; i++) {
        char ch = s[i];
        if (ch == '\n' || ch == '\t' || ch == '\r' || ch == '\f') ch = ' ';
        if (ch == ' ' && (n == 0 || buf[n-1] == ' ')) continue;
        if (n < (int)sizeof buf - 1) buf[n++] = ch;
        if (n == (int)sizeof buf - 1) { buf[n] = 0; fl_text(buf, n, indent, st); n = 0; }
    }
    buf[n] = 0;
    if (n) fl_text(buf, n, indent, st);
}

static void walk_dom(uw_doc *d, uw_node *parent, bstyle st, int li, int pre)
{
    uw_node *n;
    for (n = uw_first_child(parent); n; n = uw_next_sibling(n)) {
        if (uw_type(n) == UW_NODE_TEXT) {
            int tl = 0;
            const char *t = uw_text(n, &tl);
            if (t && tl) emit_text(t, tl, pre, li, &st);
            continue;
        }
        if (uw_type(n) != UW_NODE_ELEMENT) continue;      /* comments, doctype */
        {   const char *g = uw_tag_name(d, n);
            bstyle s = st;
            if (!strcmp(g,"head") || !strcmp(g,"title") ||
                !strcmp(g,"style") || !strcmp(g,"script")) continue;
            if (g[0]=='h' && g[1]>='1' && g[1]<='6' && !g[2]) {
                int lvl = g[1]-'0';
                s.scale = lvl <= 1 ? 3 : 2; s.bold = 1; s.color = PG_HEAD;
                fl_gap(lvl <= 2 ? 8 : 4);
                walk_dom(d, n, s, li, pre);
                fl_gap(6);
                continue;
            }
            if (!strcmp(g,"b") || !strcmp(g,"strong")) s.bold = 1;
            else if (!strcmp(g,"i") || !strcmp(g,"em")) { s.ital = 1; s.color = FB_RGB(70,70,95); }
            else if (!strcmp(g,"code") || !strcmp(g,"tt")) { s.mono = 1; s.color = PG_CODE; }
            else if (!strcmp(g,"a")) {
                const char *href = uw_attr(d, n, "href");
                int save = g_link_cur;
                s.color = PG_LINK; s.under = 1;
                if (href && *href) g_link_cur = link_add(href, (int)strlen(href));
                walk_dom(d, n, s, li, pre);
                g_link_cur = save;
                continue;
            }
            else if (!strcmp(g,"br")) { fl_nl(); continue; }
            else if (!strcmp(g,"hr")) {
                int yy;
                fl_gap(6);
                yy = fy - fscroll;
                if (yy > fclip.y && yy < fclip.y + fclip.h)
                    fb_hline(fleft, yy, fright - fleft, PG_RULE);
                fl_gap(8);
                continue;
            }
            else if (!strcmp(g,"pre")) {
                s.mono = 1; s.color = PG_CODE;
                fl_gap(6);
                walk_dom(d, n, s, li, 1);
                fl_gap(6);
                continue;
            }
            else if (!strcmp(g,"p") || !strcmp(g,"div") || !strcmp(g,"section")) {
                walk_dom(d, n, s, li, pre);
                fl_gap(8);
                continue;
            }
            else if (!strcmp(g,"ul") || !strcmp(g,"ol")) {
                fl_gap(2);
                walk_dom(d, n, s, li, pre);
                fl_gap(6);
                continue;
            }
            else if (!strcmp(g,"li")) {
                int yy = fy - fscroll + 4;
                fx = fleft;
                if (yy > fclip.y && yy < fclip.y + fclip.h)
                    fb_fill_rect(fleft + 6, yy, 3, 3, PG_TEXT);
                walk_dom(d, n, s, 18, pre);
                fl_nl();
                continue;
            }
            walk_dom(d, n, s, li, pre);      /* unknown tag: text still shows */
        }
    }
}

/* ---- the unoweb engine path (BROWSER_ENGINE=uw) ---------------------------
 * Instead of walking the DOM and painting inline, run the real pipeline -
 * cascade, block layout, display list - and replay the commands. The flow
 * painter above stays the default until this has been through real pages;
 * both paths are compiled so neither can rot.
 *
 * unoweb knows nothing about fonts, so the metrics hook is where pc64 tells it
 * how wide text actually is. Getting this wrong does not crash anything - it
 * just lays the page out for the wrong font, which is exactly why the hook
 * exists rather than a hard-coded guess inside the engine. */
#ifdef UW_ENGINE
static int uw_slot(const uw_style *s)
{ return s->font_family == UW_FF_MONO ? BR_MONO_SLOT : BR_BODY_SLOT; }

static int uw_fstyle(const uw_style *s)
{ return (s->font_weight >= 600 ? UNO_FS_BOLD : 0) | (s->font_style ? UNO_FS_ITALIC : 0); }

static int uwm_width(void *u, const uw_style *s, const char *t, int len)
{
    char buf[256];
    int n = len < (int)sizeof buf - 1 ? len : (int)sizeof buf - 1;
    (void)u;
    memcpy(buf, t, (size_t)n);
    buf[n] = 0;
    return uno_font_text_w_styled(uw_slot(s), s->font_size, uw_fstyle(s), buf);
}

static int uwm_lineh(void *u, const uw_style *s)
{
    int h = uno_font_height_px(uw_slot(s), s->font_size) + 3;
    (void)u;
    return s->line_height > h ? s->line_height : h;
}

/* The real ascent, so mixed font sizes and images share a baseline rather
 * than a common top. unoweb falls back to ~80% of the line box without this;
 * pc64 has the font, so it answers properly. */
static int uwm_baseline(void *u, const uw_style *s)
{
    (void)u;
    return uno_font_baseline_px(uw_slot(s), s->font_size);
}

static int  g_uw_w;                 /* width the current layout was built for */
static int  g_uw_h;                 /* its resulting document height           */
                                    /* (g_link_sel is shared with the flow
                                     * painter's link map, declared above)     */

/* ---- images: unomedia behind the uw_images hook ---------------------------
 * unoweb reserves the box; the decoding happens HERE, which is the whole
 * point of the hook. Decoded frames are cached per document because layout
 * asks for a size on every reflow and re-decoding a PNG per keystroke would
 * be absurd.
 *
 * Both LOCAL files and NETWORK URLs resolve: a src that resolves against the
 * page base to http/https goes through pc64_fetch (fetch-once-per-page,
 * failures remembered), and the bytes are decoded straight out of memory
 * through the same um_src indirection a file uses. An image that cannot be
 * had still lays out as an empty replaced box, which is the documented
 * behaviour for one that has not arrived. */
#define IMG_CACHE   8
#define IMG_MAX_PX  (4u * 1024u * 1024u)     /* 4 MP: a fuzzed header cannot
                                              * talk us into a huge malloc   */
typedef struct { char name[64]; um_px *px; int w, h; } imgent;
static imgent g_imgs[IMG_CACHE];
static int    g_nimgs;
static int    g_img_vol;                     /* volume of the file being read */
static char   g_img_file[64];

static long img_src_read(void *ctx, long off, unsigned char *dst, long n)
{ (void)ctx; return uno_fs_read_at(g_img_vol, g_img_file, off, dst, n); }

/* the same indirection over already-fetched bytes (network images) */
typedef struct { const unsigned char *p; long len; } membuf;

static long img_mem_read(void *ctx, long off, unsigned char *dst, long n)
{
    membuf *m = (membuf *)ctx;
    if (!m || off < 0 || off >= m->len) return 0;
    if (n > m->len - off) n = m->len - off;
    memcpy(dst, m->p + off, (size_t)n);
    return n;
}

static void img_cache_reset(void)
{
    int i;
    for (i = 0; i < g_nimgs; i++) { free(g_imgs[i].px); g_imgs[i].px = 0; }
    g_nimgs = 0;
}

static int uwi_resolve(void *user, const char *src, int *w, int *h, void **handle)
{
    um_src s;
    um_image_info info;
    int i, v, nv, delay = 0;
    long sz = -1;
    (void)user;
    *w = *h = 0; *handle = 0;
    if (!src || !*src) return 0;
    for (i = 0; i < g_nimgs; i++)
        if (!strcmp(g_imgs[i].name, src)) {
            if (!g_imgs[i].px) return 0;
            *w = g_imgs[i].w; *h = g_imgs[i].h; *handle = &g_imgs[i];
            return 1;
        }
    if (g_nimgs >= IMG_CACHE) return 0;

    /* NETWORK first: a src that resolves to http/https against the page base
     * is fetched (once - failures are remembered by pc64_fetch) and decoded
     * from memory. `mem` must outlive um_image_open's reads, hence the
     * function-scope lifetime rather than a block-local. */
    {   static membuf mem;
        char abs[LOCMAX];
        const unsigned char *bytes;
        int len;
        resolve(src, g_page_base, abs, sizeof abs);
        if (g_page_base[0] && loc_is_net(abs) &&
            (len = pc64_fetch_get(abs, &bytes)) > 0) {
            mem.p = bytes; mem.len = len;
            um_set_alloc(malloc, free);
            s.read = img_mem_read; s.size = len; s.ctx = &mem;
            /* the NAME matters to the decoder only as an extension hint;
             * pass the URL so .png/.jpg still steer the sniffer */
            if (!um_image_open(&s, abs, &info)) return 0;
            goto decoded;
        }
    }

    { int k = 0; while (src[k] && k < 63) { g_img_file[k] = src[k]; k++; } g_img_file[k] = 0; }
    nv = uno_fs_volumes();
    for (v = 0; v < nv; v++) { sz = uno_fs_size(v, g_img_file); if (sz > 0) { g_img_vol = v; break; } }
    if (sz <= 0) return 0;
    um_set_alloc(malloc, free);
    s.read = img_src_read; s.size = sz; s.ctx = 0;
    if (!um_image_open(&s, g_img_file, &info)) return 0;
decoded:
    if (info.w < 1 || info.h < 1 ||
        (unsigned)info.w * (unsigned)info.h > IMG_MAX_PX) { um_image_close(); return 0; }
    {   imgent *e = &g_imgs[g_nimgs];
        unsigned long bytes = (unsigned long)info.w * (unsigned long)info.h * 4ul;
        int k = 0;
        while (src[k] && k < 63) { e->name[k] = src[k]; k++; }
        e->name[k] = 0;
        e->px = (um_px *)malloc(bytes);
        if (!e->px) { um_image_close(); return 0; }
        memset(e->px, 0, bytes);
        if (um_image_frame(e->px, &delay) != 1) { free(e->px); e->px = 0;
                                                  um_image_close(); return 0; }
        um_image_close();
        e->w = info.w; e->h = info.h;
        g_nimgs++;
        *w = e->w; *h = e->h; *handle = e;
        return 1;
    }
}
static unsigned g_uw_sig;

static void render_uw(const char *src, unoui_rect r, int scroll)
{
    uw_metrics m;
    int i, n, avail = r.w - 12;
    unsigned sig;
    dom_sync(src);
    if (!g_dom) return;
    /* The GENERATION, not the source fingerprint: a re-parse of identical text
     * still yields a tree that has never been laid out, and comparing text
     * fingerprints told this cache it was already on screen. See g_dom_gen. */
    sig = g_dom_gen;
    /* Script that changed the tree since the last frame invalidates the
     * layout exactly like a resize does - without this a DOM change is
     * computed and then never drawn. The timer pump runs first so a
     * setTimeout that mutates is picked up in the SAME frame. */
    if (webjs_page_active()) {
        webjs_pump(page_clock_ms(), g_js_log, sizeof g_js_log);
        if (webjs_take_dirty()) g_uw_sig = 0;
    }
    if (sig != g_uw_sig || avail != g_uw_w) {
        memset(&m, 0, sizeof m);
        m.text_width = uwm_width;
        m.line_height = uwm_lineh;
        m.baseline = uwm_baseline;
        {   uw_images im;
            memset(&im, 0, sizeof im);
            im.resolve = uwi_resolve;
            uw_set_images(g_dom, &im); }
        uw_add_inline_sheets(g_dom);
        uw_style_document(g_dom, avail, r.h);
        g_uw_h = uw_layout(g_dom, avail, r.h, &m);
        uw_paint(g_dom);
        g_uw_sig = sig;
        g_uw_w = avail;
    }
    n = uw_paint_count(g_dom);
    for (i = 0; i < n; i++) {
        const uw_paint_cmd *c = uw_paint_at(g_dom, i);
        int x = r.x + c->x, y = r.y + c->y - scroll;
        fb_px col = FB_RGB(c->color.r, c->color.g, c->color.b);
        if (y > r.y + r.h || y + c->h < r.y) continue;     /* off-screen */
        switch (c->cmd) {
        case UW_CMD_RECT:
        case UW_CMD_BORDER:
        case UW_CMD_BULLET:
            fb_fill_rect(x, y, c->w, c->h, col);
            break;
        case UW_CMD_CONTROL: {
            /* unoweb says where the control is; the LOOK is ours, because a
             * text field looks like whatever this OS's widgets look like. */
            const char *type = uw_attr(g_dom, c->node, "type");
            const char *val = uw_attr(g_dom, c->node, "value");
            const char *tg = uw_tag_name(g_dom, c->node);
            int btn = (tg && !strcmp(tg, "button")) ||
                      (type && (!strcmp(type, "submit") || !strcmp(type, "button") ||
                                !strcmp(type, "reset")));
            int focused = (c->node == g_form_focus);
            fb_fill_rect(x, y, c->w, c->h, btn ? FB_RGB(226, 226, 222)
                                               : FB_RGB(255, 255, 255));
            fb_frame_rect(x, y, c->w, c->h,
                          focused ? PG_LINK : FB_RGB(150, 150, 150));
            {   char buf[128];
                int n = 0;
                const char *sv = val ? val : "";
                if (btn && !val) sv = "Submit";
                while (sv[n] && n < (int)sizeof buf - 2) { buf[n] = sv[n]; n++; }
                if (focused && n < (int)sizeof buf - 2) buf[n++] = '_';
                buf[n] = 0;
                if (n) fb_text(x + 4, y + 3, buf, PG_TEXT, -1); }
            break; }
        case UW_CMD_IMAGE:
            /* No decoder is wired in yet, so an image paints as its reserved
             * box rather than silently occupying nothing - the layout is
             * already correct, only the pixels are missing. unomedia lands
             * behind the same uw_images hook. */
            if (c->image) {
                const imgent *e = (const imgent *)c->image;
                if (c->w == e->w && c->h == e->h) {
                    fb_blit(x, y, e->w, e->h, (const fb_px *)e->px, e->w);
                } else if (c->w > 0 && c->h > 0) {
                    /* Layout scaled the box to fit the column, so the pixels
                     * follow. Nearest neighbour, ONE ROW AT A TIME: it needs no
                     * second full-size buffer, which matters when the decoded
                     * frame is already megabytes. */
                    static fb_px row[2048];
                    int ry, rx;
                    int w = c->w > 2048 ? 2048 : c->w;
                    for (ry = 0; ry < c->h; ry++) {
                        const um_px *srow = e->px + (long)((long)ry * e->h / c->h) * e->w;
                        for (rx = 0; rx < w; rx++)
                            row[rx] = (fb_px)srow[(long)rx * e->w / c->w];
                        fb_blit(x, y + ry, w, 1, row, w);
                    }
                }
            } else fb_frame_rect(x, y, c->w, c->h, PG_RULE);
            break;
        case UW_CMD_TEXT: {
            char buf[256];
            int k = c->len < (int)sizeof buf - 1 ? c->len : (int)sizeof buf - 1;
            int slot = uw_slot(c->style), st = uw_fstyle(c->style);
            memcpy(buf, c->text, (size_t)k);
            buf[k] = 0;
            /* find, from the display list: the same rects the flow painter
             * records, in the same document coordinates, so Ctrl-F behaves
             * identically whichever renderer drew the page */
            if (g_find[0] && find_in(buf, g_find)) {
                int hit = g_nfind;
                if (g_find_collect) find_add(c->x, c->y, c->w, c->h);
                fb_fill_rect(x - 1, y - 1, c->w + 2, c->h + 2,
                             hit == g_find_sel ? FB_RGB(255, 190, 60)
                                               : FB_RGB(255, 240, 150));
            }
            uno_font_draw_styled(slot, c->style->font_size, st, x, y, buf, col, -1);
            if (c->style->underline) {
                int bl = uno_font_baseline_px(slot, c->style->font_size);
                fb_hline(x, y + bl + 2, c->w, col);
            }
            break; }
        default: break;
        }
    }
    /* br_draw clamps the scroll from fy/flh, so hand it the laid-out height.
     * This must be the height the REAL layout produced - calling uw_layout
     * again here (with no metrics) would re-flow the whole page every frame
     * against the wrong font. */
    fy = r.y + g_uw_h;
    flh = 0;
}
#endif /* UW_ENGINE */

static void render_html(const char *src, unoui_rect r, int scroll)
{
    bstyle base = { 1, 0, 0, 0, 0, PG_TEXT };
#ifdef UW_ENGINE
    render_uw(src, r, scroll);
    return;
#endif
    dom_sync(src);
    fl_reset(r, scroll, 10);
    if (!g_dom) return;
    walk_dom(g_dom, uw_body(g_dom) ? uw_body(g_dom) : uw_document(g_dom), base, 0, 0);
}

/* =================== the browser app (canvas) =============================
 *
 * The shell hands this app ONE unoui canvas that fills its window, so the
 * browser draws its own chrome inside it - and it is real chrome, not a title
 * strip: a tab strip, a toolbar (back / forward / reload / home / bookmark),
 * an editable address bar, drop-down Bookmarks and History panels, and a
 * status line.
 *
 *   +---------------------------------------------------------+
 *   | Welcome  x | example.com  x | +                         |  tabs
 *   | < > @ [ https://...                    ] * Marks  Hist  |  toolbar
 *   |                                                         |
 *   |   the page, the start page, or a panel over them        |  content
 *   |                                                         |
 *   | status / hovered link                                   |  status
 *   +---------------------------------------------------------+
 *
 * Everything the user can reach is ONE LOCATION STRING, which is what the
 * tabs, the history stacks, the bookmarks file and the address bar all store:
 *
 *   uno:start            the start page (built-ins + what the disks hold)
 *   uno:welcome/sample/script   the built-in demo documents
 *   file:<vol>:<name>    a file on a local volume
 *   path:<path>          a document opened by path (the Help deep-links)
 *   http:// https://     the network (pc64_http, CA-validated TLS)
 *
 * so navigation, history and bookmarks are one code path rather than four.
 * ========================================================================= */
#include "pc64_icons.h"                 /* pc64_shell_theme() for the panels */

/* A tab's document buffer GROWS to the document. It was a flat 32768, a second
 * and tighter cut behind pc64_http's old 48 KB one: google.com's <body> opens
 * 62,883 bytes into its body, so neither layer could ever have delivered a page
 * that size and the tab rendered <head> and nothing else.
 *
 * DOC_MAX matches the transport's RAW_MAX and pc64_fetch's FETCH_ONE_MAX - one
 * number for "the biggest single resource this browser handles", rather than
 * three that disagree. Six tabs at the maximum would be 6 MB of a 32 MB heap,
 * which is why the buffer is sized per document and not per tab: a tab showing
 * the start page holds DOC_MIN. */
#define DOC_MIN   32768
#define DOC_MAX   (1024 * 1024)
#define TITMAX    48
#define MAXTABS   6
#define HISTN     16                    /* back / forward depth per tab       */
#define MAXBM     32                    /* bookmarks                          */
#define MAXHIST   48                    /* global history                     */
#define BM_FILE   "BOOKMARK.TXT"

typedef struct {
    int   used;
    char  loc[LOCMAX];
    char  title[TITMAX];
    char *doc;                          /* doccap bytes, grown on demand      */
    int   doccap;
    int   is_html, scroll;
    int   start;                        /* 1 = showing the start page         */
    int   sel, top;                     /* start page: selection + first row  */
    char  back[HISTN][LOCMAX]; int nback;
    char  fwd [HISTN][LOCMAX]; int nfwd;
} btab;

static btab g_tab[MAXTABS];
static int  g_ntab, g_cur;

static char g_status[128];
static char g_hint[128];                /* hover text, beats g_status         */
static unoui_rect g_rect;               /* last-drawn canvas rect             */

/* the address bar (its own tiny editor: caret, insert, delete) */
static char g_addr[LOCMAX];
static int  g_addr_focus, g_addr_caret;

/* drop-down panels */
enum { PANEL_NONE = 0, PANEL_MARKS, PANEL_HIST };
static int g_panel, g_panel_sel, g_panel_top;

/* bookmarks (persisted) + history (this session) */
static char g_bm_loc[MAXBM][LOCMAX], g_bm_lbl[MAXBM][TITMAX + LOCMAX];
static const char *g_bm_ptr[MAXBM];
static int  g_nbm;
static char g_hs_loc[MAXHIST][LOCMAX], g_hs_lbl[MAXHIST][TITMAX + LOCMAX];
static const char *g_hs_ptr[MAXHIST];
static int  g_nhs;

/* the start page's file list */
#define MAXFILES 64
static char g_names[MAXFILES][32];
static char g_rows[MAXFILES][40];
static const char *g_row_ptr[MAXFILES];
static int  g_vol[MAXFILES];                  /* -1 = built-in demo */
static int  g_nfiles;

static const char kWelcome[] =
"# UnoDOS Browser\n\n"
"A tiny **HTML / Markdown / CSS** renderer for pc64. It lays out a document and "
"paints it straight into the framebuffer, *runs JavaScript*, and *loads pages "
"over the network*.\n\n"
"## Features\n\n"
"- Headings, **bold**, *italic*, `inline code`\n"
"- Bullet lists and paragraphs with word-wrap\n"
"- Runs `<script>` blocks (see the **Script.html** demo)\n"
"- Loads files from the local disks (see the start page)\n"
"- **Network**: type a `http://` or `https://` URL in the address bar and press "
"Enter. HTTPS is CA-validated (TLS 1.2) against a bundled root store, using the "
"system clock.\n\n"
"---\n\n"
"## Try it\n\n"
"- [The HTML sample](uno:sample) - tags, lists, a rule and a code block\n"
"- [The JavaScript demo](uno:script) - a `<script>` block that writes the page\n"
"- [The engines](uno:engine) - switch the script engine and the CSS cascade\n"
"- [The start page](uno:start) - every document on the local disks\n\n"
"## Getting around\n\n"
"- Click a **link** to follow it; `Left` / `Right` step through a page's links "
"and `Enter` follows the selected one.\n"
"- `Backspace` goes **back**, the toolbar arrows go back and forward.\n"
"- `Ctrl-T` opens a **tab**, `Ctrl-L` jumps to the address bar, `Ctrl-D` "
"**bookmarks** the page, `F5` reloads.\n"
"- Scroll with the wheel or `Up` / `Down` / `PgUp` / `PgDn`.\n\n"
"> UnoDOS runs bare-metal on x86-64 UEFI - this page is drawn by the same\n"
"> software framebuffer that draws the whole desktop.\n";

static const char kSample[] =
"<h1>HTML sample</h1>"
"<p>This page is <b>HTML</b>, rendered by the same engine. It supports "
"<i>emphasis</i>, <code>code spans</code>, and <a href='uno:welcome'>links</a> "
"(that one goes back to the welcome page).</p>"
"<h2>A list</h2><ul><li>first item</li><li>second item</li><li>third item</li></ul>"
"<hr><pre>  pre-formatted text\n  keeps its   spacing</pre>"
"<p>Unknown tags are ignored; their text still shows.</p>";

static const char kScript[] =
"<h1>JavaScript</h1>"
"<p>The <code>&lt;script&gt;</code> block below runs on the selected script "
"engine (<a href='uno:engine'>switch engines here</a>); its "
"<code>document.write</code> output is spliced into the page "
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

#ifdef UW_ENGINE
/* rel="stylesheet" is ASCII-caseless and may carry other tokens
 * (rel="stylesheet alternate"); match the token, not the whole string. */
static int rel_is_stylesheet(const char *rel)
{
    static const char kw[] = "stylesheet";
    const char *p = rel;
    while (*p) {
        const char *b;
        int i;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        b = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (p - b == (int)sizeof kw - 1) {
            for (i = 0; i < (int)sizeof kw - 1; i++) {
                int c = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i];
                if (c != kw[i]) break;
            }
            if (i == (int)sizeof kw - 1) return 1;
        }
    }
    return 0;
}

/* ---- prefetch -------------------------------------------------------------
 * Everything the page references, asked for AT ONCE, the moment the DOM
 * exists - before the cascade wants the first stylesheet and long before
 * layout wants the first image.
 *
 * Discovering a subresource is not the same as needing it, and the two used to
 * be welded together: the only way to learn about a stylesheet was to arrive
 * at it in fetch_link_sheets, which fetched it on the spot and blocked. Four
 * sheets therefore cost four round trips end to end no matter how fast the
 * server was. This walk finds all of them in one pass and hands them to the
 * queue, which runs several at a time; the fetch each consumer then performs
 * is usually a table lookup on bytes that already arrived.
 *
 * Runs BEFORE fetch_link_sheets on purpose - that one splices <style> elements
 * over the <link>s it consumes, so the links have to be read first. */
static void prefetch_subresources(uw_doc *d)
{
    uw_node *n;
    if (!d || !g_page_base[0]) return;
    /* never from inside a progressive paint - see the guard's declaration */
    if (g_in_progress_paint) return;
    for (n = uw_next_in_order(uw_document(d), uw_document(d)); n;
         n = uw_next_in_order(n, uw_document(d))) {
        const char *tag, *url = 0;
        char abs[LOCMAX];
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        tag = uw_tag_name(d, n);
        if (!strcmp(tag, "link")) {
            const char *rel = uw_attr(d, n, "rel");
            if (!rel || !rel_is_stylesheet(rel)) continue;
            url = uw_attr(d, n, "href");
        } else if (!strcmp(tag, "img")) {
            url = uw_attr(d, n, "src");
        }
        if (!url || !*url) continue;
        resolve(url, g_page_base, abs, sizeof abs);
        if (!loc_is_net(abs)) continue;          /* only network resources */
        pc64_fetch_start(abs);
    }
}

/* ---- linked stylesheets ---------------------------------------------------
 * <link rel=stylesheet href=...> is fetched here and spliced into the DOM as
 * a <style> element holding the CSS text, in the link's own position.
 *
 * That splice is the whole trick: BOTH cascades already collect <style>
 * elements in document order (the built-in one through uw_add_inline_sheets,
 * the libcss bridge through its own scan), so linked sheets land in the
 * right cascade position for both engines with no new API and no way for the
 * two to disagree about them. */
static void fetch_link_sheets(uw_doc *d)
{
    uw_node *n, *next;
    if (!d || !g_page_base[0]) return;
    /* never from inside a progressive paint - see the guard's declaration */
    if (g_in_progress_paint) return;
    for (n = uw_next_in_order(uw_document(d), uw_document(d)); n; n = next) {
        const char *rel, *href;
        char abs[LOCMAX];
        const unsigned char *css;
        int len;
        next = uw_next_in_order(n, uw_document(d));
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        if (strcmp(uw_tag_name(d, n), "link")) continue;
        rel = uw_attr(d, n, "rel");
        href = uw_attr(d, n, "href");
        if (!rel || !href || !*href) continue;
        if (!rel_is_stylesheet(rel)) continue;
        resolve(href, g_page_base, abs, sizeof abs);
        if (!loc_is_net(abs)) continue;          /* only network sheets */
        len = pc64_fetch_get(abs, &css);
        if (len <= 0) continue;
        {   uw_node *st = uw_create_element(d, "style");
            uw_node *tx = st ? uw_create_text(d, (const char *)css, len) : 0;
            if (st && tx) {
                uw_append(d, st, tx);
                uw_insert_before(d, uw_parent(n), st, n);
            }
        }
    }
}
#endif


static void navigate(const char *loc, int push);

/* ---- small string helpers ------------------------------------------------ */
static void sput(char *dst, int cap, const char *src)
{
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static char *sapp(char *p, const char *end, const char *s)
{ while (s && *s && p < end - 1) *p++ = *s++; *p = 0; return p; }

/* Extensions are matched case-INSENSITIVELY: FAT hands back 8.3 names in
 * upper case, so a case-sensitive compare recognised "page.html" but not the
 * "PAGE.HTM" the file system actually reports - every local HTML file on a
 * real disk rendered as plain text. */
static int ext_is(const char *name, const char *ext)
{
    int n = (int)strlen(name), e = (int)strlen(ext), i;
    if (n <= e) return 0;
    for (i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}
static int name_is_html(const char *name)
{ return ext_is(name, ".html") || ext_is(name, ".htm"); }

/* ---- <script> expansion --------------------------------------------------
 * Run <script> blocks in the tab's document: replace each with its
 * document.write output and collect console.log lines into a "console" panel
 * appended at the end. A tiny tree-walking interpreter (js.c) does the work;
 * this is the HTML<->JS glue. */
static int ci(char c){ return (c>='A'&&c<='Z') ? c+32 : c; }
static int tag_at(const char *p, const char *name)   /* case-insensitive "<name" */
{ while (*name){ if (ci(*p)!=*name) return 0; p++; name++; } return 1; }

/* The pre-DOM script path, for the FLOW-PAINTER build only. It rewrites the
 * source text, replacing each <script> with its document.write output, which
 * is all a renderer without a DOM can do.
 *
 * The ENGINE build has a real DOM, so it runs scripts against it instead
 * (run_page_scripts below) - that is M5, and it is why this whole function
 * is compiled out there rather than left to fight with the live bindings
 * over who owns <script>. */
#ifndef UW_ENGINE
/* `cap` is the document buffer's size, not a constant: the buffer grows to the
 * page now, so a static scratch of the maximum would be a megabyte of BSS
 * carried by every build to serve the rare page that needs it. One allocation
 * per navigation instead - and if it fails, the document is left exactly as it
 * arrived, which renders the page with its scripts unexpanded rather than not
 * at all. */
static void js_expand(char *doc, int cap)
{
    static char code[8192], wbuf[8192], logbuf[4096], lbuf[2048];
    char *out = (char *)malloc((size_t)cap);
    int oi = 0, haslog = 0;
    const char *p = doc;
    if (!out) return;
    logbuf[0] = 0;
    while (*p && oi < cap-1) {
        if (p[0]=='<' && tag_at(p+1,"script")) {
            const char *s = p + 7; while (*s && *s!='>') s++; if (*s=='>') s++;
            const char *e = s;
            while (*e) { if (e[0]=='<' && e[1]=='/' && tag_at(e+2,"script")) break; e++; }
            int inlen = (int)(e - s); if (inlen > (int)sizeof(code)-1) inlen = sizeof(code)-1;
            memcpy(code, s, inlen); code[inlen] = 0;
            wbuf[0] = 0; lbuf[0] = 0;
            int jrc = js_run(code, wbuf, sizeof wbuf, lbuf, sizeof lbuf);
            int wl = (int)strlen(wbuf);
            if (wl > cap-1-oi) wl = cap-1-oi;
            memcpy(out+oi, wbuf, wl); oi += wl;
            /* Only surface a console panel for scripts that RAN (rc==0): real web
             * pages carry minified JS this tiny interpreter can't parse, and on a
             * parse/oom error js_run leaves only a diagnostic in lbuf - dumping
             * that made a script-heavy page (e.g. google.com) render as a wall of
             * "JS parse error" instead of the page's own text. Skip those. */
            if (jrc == 0 && lbuf[0]) { int have=(int)strlen(logbuf), ll=(int)strlen(lbuf);
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
        if (oi+hl+ll+fl2 < cap-1) {
            memcpy(out+oi,h,hl); oi+=hl; memcpy(out+oi,logbuf,ll); oi+=ll;
            memcpy(out+oi,f,fl2); oi+=fl2; out[oi]=0;
        }
    }
    memcpy(doc, out, (size_t)oi+1);
    free(out);
}
#endif /* !UW_ENGINE */

/* ---- the start page's list ----------------------------------------------- */
static void refresh_files(void)
{
    int v, nv, i;
    g_nfiles = 0;
    sput(g_names[g_nfiles], 32, "Welcome.md");  g_vol[g_nfiles++] = -1;
    sput(g_names[g_nfiles], 32, "Sample.html"); g_vol[g_nfiles++] = -1;
    sput(g_names[g_nfiles], 32, "Script.html"); g_vol[g_nfiles++] = -1;
    nv = uno_fs_volumes();
    for (v = 0; v < nv && g_nfiles < MAXFILES; v++) {
        char nm[32]; int cnt = uno_fs_list_begin(v);
        for (i = 0; i < cnt && g_nfiles < MAXFILES; i++)
            if (uno_fs_list_get(v, i, nm, sizeof nm)) {
                sput(g_names[g_nfiles], 32, nm);
                g_vol[g_nfiles++] = v;
            }
    }
    for (i = 0; i < g_nfiles; i++) {                  /* one row label each */
        char *p = g_rows[i], *end = g_rows[i] + sizeof g_rows[i];
        p = sapp(p, end, g_vol[i] < 0 ? "*  " : "   ");
        sapp(p, end, g_names[i]);
        g_row_ptr[i] = g_rows[i];
    }
}

/* ---- bookmarks + history ------------------------------------------------- *
 * Bookmarks live in ONE line-per-entry file (loc|title) on the first writable
 * volume, so they survive a reboot; history is this session's. Both are shown
 * through the same scrolling list the toolkit gives every app. */
static void relabel(char *dst, int cap, const char *title, const char *loc)
{
    char *p = dst, *end = dst + cap;
    p = sapp(p, end, (title && *title) ? title : loc);
    if (title && *title && loc && *loc) { p = sapp(p, end, "   -  "); sapp(p, end, loc); }
}

static void bm_relabel(void)
{ int i; for (i = 0; i < g_nbm; i++) g_bm_ptr[i] = g_bm_lbl[i]; }

static int bm_volume(void)
{
    int v, nv = uno_fs_volumes();
    for (v = 0; v < nv; v++) if (uno_fs_writable(v)) return v;
    return -1;
}

static void bm_save(void)
{
    static char buf[MAXBM * (LOCMAX + TITMAX + 4)];
    char *p = buf, *end = buf + sizeof buf;
    int i, v = bm_volume();
    if (v < 0) return;                       /* read-only system: RAM only */
    for (i = 0; i < g_nbm; i++) {
        char title[TITMAX]; int k = 0;
        const char *l = g_bm_lbl[i];
        while (l[k] && k < TITMAX - 1 && !(l[k] == ' ' && l[k+1] == ' ')) { title[k] = l[k]; k++; }
        title[k] = 0;
        p = sapp(p, end, g_bm_loc[i]);
        p = sapp(p, end, "|");
        p = sapp(p, end, title);
        p = sapp(p, end, "\n");
    }
    uno_fs_write(v, BM_FILE, (const unsigned char *)buf, (long)(p - buf));
}

static void bm_load(void)
{
    static char buf[MAXBM * (LOCMAX + TITMAX + 4)];
    int v, nv = uno_fs_volumes();
    long n = -1;
    char *p;
    g_nbm = 0;
    for (v = 0; v < nv && n < 0; v++)
        n = uno_fs_read(v, BM_FILE, (unsigned char *)buf, (long)sizeof buf - 1);
    if (n <= 0) { bm_relabel(); return; }
    buf[n] = 0;
    p = buf;
    while (*p && g_nbm < MAXBM) {
        char *loc = p, *title = 0, *nl;
        for (nl = p; *nl && *nl != '\n'; nl++) if (*nl == '|' && !title) { *nl = 0; title = nl + 1; }
        if (*nl) { *nl = 0; p = nl + 1; } else p = nl;
        if (*loc == '\r' || !*loc) continue;
        { char *cr = loc; while (*cr) { if (*cr == '\r') *cr = 0; else cr++; } }
        if (title) { char *cr = title; while (*cr) { if (*cr == '\r') *cr = 0; else cr++; } }
        sput(g_bm_loc[g_nbm], LOCMAX, loc);
        relabel(g_bm_lbl[g_nbm], sizeof g_bm_lbl[0], title, loc);
        g_nbm++;
    }
    bm_relabel();
}

static int bm_index(const char *loc)
{ int i; for (i = 0; i < g_nbm; i++) if (!strcmp(g_bm_loc[i], loc)) return i; return -1; }

/* Ctrl-D / the star: bookmark the page, or un-bookmark it if it already is */
static void bm_toggle(void)
{
    btab *t = &g_tab[g_cur];
    int at = bm_index(t->loc);
    if (at >= 0) {
        int i;
        for (i = at; i < g_nbm - 1; i++) {
            sput(g_bm_loc[i], LOCMAX, g_bm_loc[i+1]);
            sput(g_bm_lbl[i], sizeof g_bm_lbl[0], g_bm_lbl[i+1]);
        }
        g_nbm--;
        sput(g_status, sizeof g_status, "Bookmark removed.");
    } else if (g_nbm < MAXBM) {
        sput(g_bm_loc[g_nbm], LOCMAX, t->loc);
        relabel(g_bm_lbl[g_nbm], sizeof g_bm_lbl[0], t->title, t->loc);
        g_nbm++;
        sput(g_status, sizeof g_status, "Bookmarked.");
    } else {
        sput(g_status, sizeof g_status, "Bookmark list is full.");
    }
    bm_relabel();
    bm_save();
}

static void hist_add(const char *loc, const char *title)
{
    int i;
    if (!loc || !*loc || !strcmp(loc, "uno:start")) return;
    for (i = 0; i < g_nhs; i++)                       /* move an old visit up */
        if (!strcmp(g_hs_loc[i], loc)) {
            for (; i > 0; i--) {
                sput(g_hs_loc[i], LOCMAX, g_hs_loc[i-1]);
                sput(g_hs_lbl[i], sizeof g_hs_lbl[0], g_hs_lbl[i-1]);
            }
            sput(g_hs_loc[0], LOCMAX, loc);
            relabel(g_hs_lbl[0], sizeof g_hs_lbl[0], title, loc);
            return;
        }
    if (g_nhs < MAXHIST) g_nhs++;
    for (i = g_nhs - 1; i > 0; i--) {                 /* newest first */
        sput(g_hs_loc[i], LOCMAX, g_hs_loc[i-1]);
        sput(g_hs_lbl[i], sizeof g_hs_lbl[0], g_hs_lbl[i-1]);
    }
    sput(g_hs_loc[0], LOCMAX, loc);
    relabel(g_hs_lbl[0], sizeof g_hs_lbl[0], title, loc);
    for (i = 0; i < g_nhs; i++) g_hs_ptr[i] = g_hs_lbl[i];
}

/* ---- tabs ---------------------------------------------------------------- */
/* The tab's document buffer, with room for at least `need` bytes (NUL
 * included). Never shrinks - a tab that has held a big page will hold another -
 * and never grows past DOC_MAX. NULL only if the heap refused, in which case a
 * previous buffer is still valid and still returned. */
static char *tab_doc_cap(btab *t, int need)
{
    char *bigger;
    if (need < DOC_MIN) need = DOC_MIN;
    if (need > DOC_MAX) need = DOC_MAX;
    if (t->doc && t->doccap >= need) return t->doc;
    bigger = (char *)realloc(t->doc, (size_t)need);
    if (!bigger) return t->doc;                 /* keep whatever we had */
    if (!t->doc) bigger[0] = 0;
    t->doc = bigger;
    t->doccap = need;
    return t->doc;
}

static char *tab_doc(btab *t) { return tab_doc_cap(t, DOC_MIN); }

static int tab_new(const char *loc)
{
    int i;
    for (i = 0; i < MAXTABS; i++) if (!g_tab[i].used) break;
    if (i == MAXTABS) { sput(g_status, sizeof g_status, "Six tabs is the limit."); return -1; }
    g_tab[i].used = 1; g_tab[i].nback = g_tab[i].nfwd = 0;
    g_tab[i].scroll = 0; g_tab[i].sel = g_tab[i].top = 0;
    g_tab[i].loc[0] = 0;
    sput(g_tab[i].title, TITMAX, "New tab");
    if (i >= g_ntab) g_ntab = i + 1;
    g_cur = i;
    g_link_sel = -1;
    navigate(loc ? loc : "uno:start", 0);
    return i;
}

static void tab_close(int i)
{
    int k, alive = 0;
    if (i < 0 || i >= MAXTABS || !g_tab[i].used) return;
    for (k = 0; k < MAXTABS; k++) if (g_tab[k].used) alive++;
    if (alive <= 1) { navigate("uno:start", 1); return; }   /* never zero tabs */
    if (g_tab[i].doc) { free(g_tab[i].doc); g_tab[i].doc = 0; g_tab[i].doccap = 0; }
    g_tab[i].used = 0;
    while (g_ntab > 0 && !g_tab[g_ntab-1].used) g_ntab--;
    if (g_cur == i) {
        for (k = i; k >= 0; k--) if (g_tab[k].used) { g_cur = k; break; }
        if (!g_tab[g_cur].used)
            for (k = 0; k < MAXTABS; k++) if (g_tab[k].used) { g_cur = k; break; }
    }
    g_link_sel = -1;
}

/* ---- loading a location -------------------------------------------------- */
static void loading_frame(const char *what);

static int loc_is_net(const char *loc)
{ return !strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8); }

/* "file:<vol>:<name>" -> vol + name */
static int loc_file(const char *loc, const char **name)
{
    int v = 0; const char *p = loc + 5;
    if (strncmp(loc, "file:", 5)) return -1;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    if (*p != ':') return -1;
    *name = p + 1;
    return v;
}

static void title_from_loc(btab *t, const char *loc)
{
    const char *base = loc, *p;
    if (!strncmp(loc, "file:", 5)) { const char *nm; if (loc_file(loc, &nm) >= 0) base = nm; }
    else if (!strncmp(loc, "path:", 5)) base = loc + 5;
    for (p = base; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    sput(t->title, TITMAX, *base ? base : loc);
}

static void doc_set(btab *t, const char *src, int html)
{
    char *d = tab_doc_cap(t, (int)strlen(src) + 1);
    if (!d) return;
    sput(d, t->doccap, src);
    t->is_html = html;
#ifndef UW_ENGINE
    if (html) js_expand(d, t->doccap);
#else
    (void)html;                  /* the engine build runs scripts on the DOM */
#endif
}

static void doc_error(btab *t, const char *what, const char *why)
{
    char *d = tab_doc(t), *p, *end;
    if (!d) return;
    p = d; end = d + t->doccap;
    p = sapp(p, end, "<h1>Couldn't load the page</h1><p><b>Where:</b> ");
    p = sapp(p, end, what);
    p = sapp(p, end, "</p><p><b>Reason:</b> ");
    p = sapp(p, end, why && *why ? why : "unknown");
    sapp(p, end, "</p><hr><p>The address bar takes <code>http://host/path</code> or "
                 "<code>https://</code>, and the start page lists what the local "
                 "disks hold. <a href='uno:start'>Back to the start page</a>.</p>");
    t->is_html = 1;
}

/* the one loader: every scheme lands here, so history, bookmarks and the
 * address bar all navigate through the same door */
/* ---- progressive render ----------------------------------------------------
 * The transport hands over the body every few KB; this paints it. The cost of
 * re-rendering a partial document is bounded by the transport's throttle, which
 * grows with the body (~6 KB, then a quarter of what has arrived) precisely
 * because this callback re-parses and re-renders the whole thing each time -
 * see the note on it in pc64_http.c. A page is drawn a couple of dozen times on
 * the way in whatever its size. Streaming into a live parser
 * would avoid even that, but the browser keys its DOM cache on the document
 * TEXT, and threading a parser through that is a bigger change than the
 * saving justifies at this size. */
static btab *g_partial_tab;
static void br_draw(struct unoui_widget *w, unoui_rect r, void *ctx);

/* g_in_progress_paint is declared with fetch_link_sheets above, which is the
 * function it protects. The progressive callback repaints from INSIDE
 * pc64_http's receive loop; that repaint re-parses the partial document, and
 * parsing walked straight into fetch_link_sheets - starting a nested HTTP
 * request on the single TCP slot the outer response was still using.
 * Against a real server the browser fetched the page, then fetched its first
 * stylesheet from inside the page's own receive loop, and hung with the other
 * two never requested. Subresources are not urgent mid-response: the document
 * is re-rendered when it completes, and the sheets are fetched then. */

static void load_progress(const char *body, int len, long total)
{
    btab *t = g_partial_tab;
    char *d;
    (void)total;
    if (!t || !(d = tab_doc_cap(t, len + 1))) return;
    if (len > t->doccap - 1) len = t->doccap - 1;
    memcpy(d, body, (size_t)len);
    d[len] = 0;
    t->is_html = 1;
    g_dom_sig = 0;                    /* the tree must be rebuilt from this */
    if (g_rect.w > 0) {
        g_in_progress_paint = 1;
        br_draw(0, g_rect, 0);
        g_in_progress_paint = 0;
        uno_pc64_present();           /* straight to the screen, mid-fetch */
    }
}

static void load_loc(btab *t, const char *loc)
{
    t->scroll = 0;
    t->start = 0;
    g_link_sel = -1;
    sput(t->loc, LOCMAX, loc);
    /* subresources belong to the page that referenced them: the base moves
     * and the cache empties on EVERY navigation, including one that fails */
    sput(g_page_base, LOCMAX, loc);
    pc64_fetch_reset();
    title_from_loc(t, loc);
    sput(g_status, sizeof g_status,
         "Left / Right: links   Enter: follow   Backspace: back");

    if (!strcmp(loc, "uno:start")) {
        refresh_files();
        t->start = 1;
        if (t->sel >= g_nfiles) t->sel = g_nfiles ? g_nfiles - 1 : 0;
        sput(t->title, TITMAX, "Start");
        sput(g_status, sizeof g_status,
             "Pick a document, or type an address and press Enter.");
        return;
    }
    if (!strcmp(loc, "uno:welcome")) { doc_set(t, kWelcome, 0); sput(t->title, TITMAX, "Welcome"); return; }
    if (!strcmp(loc, "uno:sample"))  { doc_set(t, kSample, 1);  sput(t->title, TITMAX, "HTML sample"); return; }
    if (!strcmp(loc, "uno:script"))  { doc_set(t, kScript, 1);  sput(t->title, TITMAX, "JavaScript"); return; }

    /* uno:engine - the engine switches (script + layout cascade). Plain
     * links carry the actions so the page works with the keyboard
     * link-walker like every other internal page; both switches apply from
     * the next page load on. */
    if (!strcmp(loc, "uno:engine") ||
        !strcmp(loc, "uno:engine/unojs") || !strcmp(loc, "uno:engine/quickjs") ||
        !strcmp(loc, "uno:engine/cascade/builtin") ||
        !strcmp(loc, "uno:engine/cascade/libcss")) {
        char pg[1280], *p = pg, *end = pg + sizeof pg;
        if (!strcmp(loc, "uno:engine/unojs"))   js_engine_set(JS_ENGINE_UNOJS);
        if (!strcmp(loc, "uno:engine/quickjs")) js_engine_set(JS_ENGINE_QUICKJS);
        if (!strcmp(loc, "uno:engine/cascade/builtin")) uwx_libcss_unregister();
        if (!strcmp(loc, "uno:engine/cascade/libcss"))  uwx_libcss_register();
#ifdef UW_ENGINE
        g_uw_w = 0;                    /* force restyle+relayout on next paint */
#endif
        p = sapp(p, end, "<h1>Engines</h1>"
                         "<h2>Script engine</h2><p>Pages' <code>&lt;script&gt;</code> "
                         "blocks are running on <b>");
        p = sapp(p, end, js_engine_name(js_engine_get()));
        p = sapp(p, end, "</b>.</p><ul><li><a href='uno:engine/unojs'>unojs</a> - "
                         "the UnoDOS engine (the default)</li>"
                         "<li><a href='uno:engine/quickjs'>quickjs</a> - the vendored "
                         "quickjs-ng, full modern JavaScript</li></ul>"
                         "<h2>Layout cascade</h2><p>Styles are computed by the <b>");
        p = sapp(p, end, uw_cascade_active() ? "libcss" : "built-in");
        p = sapp(p, end, "</b> cascade.</p>"
                         "<ul><li><a href='uno:engine/cascade/builtin'>built-in</a> - "
                         "unoweb's own matcher and cascade (the default)</li>"
                         "<li><a href='uno:engine/cascade/libcss'>libcss</a> - the "
                         "vendored NetSurf CSS engine (full selectors)</li></ul>"
#ifndef UW_ENGINE
                         "<p><i>This build paints pages with the flow renderer; the "
                         "cascade choice takes effect in unoweb-engine builds "
                         "(BROWSER_ENGINE=uw).</i></p>"
#endif
                         "<hr><p>Try the script engines on <a href='uno:script'>the "
                         "JavaScript demo</a>, the cascades on <a href='uno:sample'>"
                         "the HTML sample</a>. Choices last until the browser closes.</p>");
        (void)p;
        doc_set(t, pg, 1);
        sput(t->title, TITMAX, "Engines");
        return;
    }

    if (!strncmp(loc, "file:", 5) || !strncmp(loc, "path:", 5)) {
        char *d;
        const char *name = loc + 5;
        long n = -1, sz;
        int v = -1;
        /* Ask the volume how big the file is and size the buffer to it - a
         * local document was capped at 32 KB by the same constant that cut
         * google.com, and a long README truncated just as silently. */
        if (!strncmp(loc, "file:", 5)) {
            v = loc_file(loc, &name);
            sz = (v >= 0) ? uno_fs_size(v, name) : -1;
            d = tab_doc_cap(t, (sz > 0 ? (int)sz : 0) + 1);
            if (!d) return;
            if (v >= 0) n = uno_fs_read(v, name, (unsigned char *)d, t->doccap - 1);
        } else {
            int k, nv = uno_fs_volumes();                    /* search every volume */
            sz = -1;
            for (k = 0; k < nv && sz <= 0; k++) sz = uno_fs_size(k, name);
            d = tab_doc_cap(t, (sz > 0 ? (int)sz : 0) + 1);
            if (!d) return;
            for (k = 0; k < nv && n < 0; k++)
                n = uno_fs_read(k, name, (unsigned char *)d, t->doccap - 1);
        }
        if (n < 0) { doc_error(t, name, "no volume carries that file"); return; }
        d[n] = 0;
        t->is_html = name_is_html(name);
#ifndef UW_ENGINE
        if (t->is_html) js_expand(d, t->doccap);
#endif
        return;
    }

    if (loc_is_net(loc)) {
        http_req *rq;
        char *d;
        int n, q, i;
        loading_frame(loc);                     /* show progress before we block */
        /* Draw the page AS IT ARRIVES rather than only when the last byte
         * lands. The partial body is rendered by the ordinary path - a
         * document is a document whether or not more of it is coming - and
         * an unclosed tag at the cut simply renders as the parser's usual
         * recovery, which is what a real browser shows mid-load too. */
        g_partial_tab = t;
        pc64_http_on_progress(load_progress);
        /* The handle flow, not pc64_http_get, for ONE reason: the blocking call
         * needs its buffer before the answer exists, so the only size it can be
         * given is the worst case - which is exactly the 32 KB that cut
         * google.com's <body> off. Here the request finishes first and the
         * buffer is sized from what actually arrived. */
        rq = pc64_http_begin(loc, 0);
        if (!rq) { g_partial_tab = 0; pc64_http_on_progress(0);
                   doc_error(t, loc, "Out of memory"); return; }
        pc64_http_req_progress(rq, 1);
        pc64_http_wait(rq);
        pc64_http_on_progress(0);
        g_partial_tab = 0;
        n = pc64_http_len(rq);
        d = tab_doc_cap(t, (n > 0 ? n : 0) + 1);
        if (!d) { pc64_http_free(rq); return; }
        n = pc64_http_take(rq, d, t->doccap, g_status, sizeof g_status);
        pc64_http_free(rq);
        /* Force one more parse now that the document is COMPLETE and the
         * re-entrancy guard is clear. Without this the last progressive
         * paint's cached tree is reused - the text is identical, so the
         * document-signature cache short-circuits - and the page's
         * stylesheets are never fetched at all. */
        g_dom_sig = 0;
        if (n < 0) { doc_error(t, loc, g_status); return; }
        /* pick MD vs HTML from the suffix; HTML is the default (it also
         * renders plain text sensibly) */
        q = (int)strlen(loc);
        for (i = 0; i < q; i++) if (loc[i]=='?' || loc[i]=='#') { q = i; break; }
        t->is_html = 1;
        if (q >= 3 && !strncmp(loc+q-3, ".md", 3)) t->is_html = 0;
        else if (q >= 4 && !strncmp(loc+q-4, ".txt", 4)) t->is_html = 0;
#ifndef UW_ENGINE
        if (t->is_html) js_expand(d, t->doccap);
#endif
        return;
    }

    /* bare host or path typed into the address bar: try it as http:// */
    {
        char url[LOCMAX];
        char *p = url, *end = url + sizeof url;
        p = sapp(p, end, "http://");
        sapp(p, end, loc);
        load_loc(t, url);
    }
}

/* resolve a link href against the page it came from */
static void resolve(const char *href, const char *base, char *out, int cap)
{
    if (!href || !*href) { sput(out, cap, base); return; }
    if (loc_is_net(href) || !strncmp(href, "uno:", 4) ||
        !strncmp(href, "file:", 5) || !strncmp(href, "path:", 5)) {
        sput(out, cap, href); return;
    }
    if (loc_is_net(base)) {                       /* relative to a web page */
        char *p = out, *end = out + cap;
        if (href[0] == '/') {                     /* site root */
            const char *s = base + (base[4] == 's' ? 8 : 7);
            const char *slash = s; while (*slash && *slash != '/') slash++;
            { int hostlen = (int)(slash - base);
              int k; for (k = 0; k < hostlen && p < end - 1; k++) *p++ = base[k];
              *p = 0; }
            sapp(p, end, href);
        } else {                                  /* alongside the current page */
            int cut = (int)strlen(base), k;
            while (cut > 0 && base[cut-1] != '/') cut--;
            for (k = 0; k < cut && p < end - 1; k++) *p++ = base[k];
            *p = 0;
            sapp(p, end, href);
        }
        return;
    }
    if (!strncmp(base, "file:", 5)) {             /* a sibling file */
        const char *nm; int v = loc_file(base, &nm);
        char *p = out, *end = out + cap;
        if (v >= 0) {
            char vs[8]; int k = 0, vv = v;
            if (!vv) vs[k++] = '0';
            while (vv) { vs[k++] = (char)('0' + vv % 10); vv /= 10; }
            p = sapp(p, end, "file:");
            while (k) { if (p < end - 1) *p++ = vs[--k]; else k = 0; }
            *p = 0;
            p = sapp(p, end, ":");
            sapp(p, end, href);
            return;
        }
    }
    sput(out, cap, href);
}

#ifdef UW_ENGINE
/* Submit the form containing `ctl`. GET puts the fields in the query and is
 * an ordinary navigation; POST sends them as a body (pc64_http_request). */
static void form_submit(btab *t, uw_node *ctl)
{
    uw_node *form = form_of(ctl);
    const char *action, *method;
    char body[1024], url[LOCMAX];
    int n;
    if (!form) return;
    action = uw_attr(g_dom, form, "action");
    method = uw_attr(g_dom, form, "method");
    n = form_body(form, body, sizeof body);
    resolve(action && *action ? action : t->loc, g_page_base[0] ? g_page_base : t->loc,
            url, sizeof url);
    if (method && (method[0] == 'p' || method[0] == 'P')) {
        sput(g_status, sizeof g_status, "Submitting...");
        pc64_shell_dirty();
        /* Same allocate-from-len flow as a GET navigation: a form's reply is
         * as likely to be a full page as any other document, and taking it
         * into a fixed buffer would truncate it the same way. */
        {   http_req *rq = pc64_http_begin(url, body);
            char *d;
            int r;
            if (!rq) { doc_error(t, url, "Out of memory"); return; }
            pc64_http_wait(rq);
            r = pc64_http_len(rq);
            d = tab_doc_cap(t, (r > 0 ? r : 0) + 1);
            if (!d) { pc64_http_free(rq); return; }
            r = pc64_http_take(rq, d, t->doccap, g_status, sizeof g_status);
            pc64_http_free(rq);
            if (r < 0) doc_error(t, url, g_status);
            else t->is_html = 1;
            sput(t->loc, LOCMAX, url);
            sput(g_page_base, LOCMAX, url);
            t->scroll = 0;
            g_dom_sig = 0;                 /* force a re-parse of the reply */
        }
        return;
    }
    /* GET: the fields become the query string and it is a normal navigation */
    {   int k = (int)strlen(url), i;
        for (i = 0; i < k; i++) if (url[i] == '?') { url[i] = 0; k = i; break; }
        if (n && k < LOCMAX - 2) {
            url[k++] = '?';
            for (i = 0; i < n && k < LOCMAX - 1; i++) url[k++] = body[i];
            url[k] = 0;
        }
    }
    navigate(url, 1);
}
#endif /* UW_ENGINE */

static void navigate(const char *loc, int push)
{
    btab *t = &g_tab[g_cur];
    if (push && t->loc[0]) {
        if (t->nback == HISTN) {                  /* drop the oldest */
            int i; for (i = 0; i < HISTN - 1; i++) sput(t->back[i], LOCMAX, t->back[i+1]);
            t->nback--;
        }
        sput(t->back[t->nback++], LOCMAX, t->loc);
        t->nfwd = 0;
    }
    load_loc(t, loc);
    g_hint[0] = 0;                     /* the old link target is gone */
    hist_add(t->loc, t->title);
    if (!g_addr_focus) { sput(g_addr, LOCMAX, t->loc); g_addr_caret = (int)strlen(g_addr); }
}

static void go_back(void)
{
    btab *t = &g_tab[g_cur];
    if (!t->nback) { sput(g_status, sizeof g_status, "No page to go back to."); return; }
    if (t->nfwd < HISTN) sput(t->fwd[t->nfwd++], LOCMAX, t->loc);
    { char to[LOCMAX]; sput(to, LOCMAX, t->back[--t->nback]); load_loc(t, to); }
    if (!g_addr_focus) { sput(g_addr, LOCMAX, t->loc); g_addr_caret = (int)strlen(g_addr); }
}

static void go_fwd(void)
{
    btab *t = &g_tab[g_cur];
    if (!t->nfwd) { sput(g_status, sizeof g_status, "No page to go forward to."); return; }
    if (t->nback < HISTN) sput(t->back[t->nback++], LOCMAX, t->loc);
    { char to[LOCMAX]; sput(to, LOCMAX, t->fwd[--t->nfwd]); load_loc(t, to); }
    if (!g_addr_focus) { sput(g_addr, LOCMAX, t->loc); g_addr_caret = (int)strlen(g_addr); }
}

/* ---- chrome geometry -----------------------------------------------------
 * One function per band, all derived from the font, so the chrome scales with
 * the UI font and the draw code and the hit test can never disagree. */
static const unoui_theme *TH(void) { return pc64_shell_theme(); }

/* the tab strip is a unoui control now, so the band is exactly its height */
static int ch_tabh(void)  { return unoui_tabs_h(TH()); }
static int ch_barh(void)  { return fb_text_h() + 12; }
static int ch_stath(void) { return fb_text_h() + 5; }
static int ch_btnw(void)  { return fb_text_h() + 12; }   /* square icon button */

static unoui_rect band_tabs(unoui_rect r)
{ unoui_rect b = { r.x, r.y, r.w, ch_tabh() }; return b; }
static unoui_rect band_bar(unoui_rect r)
{ unoui_rect b = { r.x, r.y + ch_tabh(), r.w, ch_barh() }; return b; }
static unoui_rect band_body(unoui_rect r)
{
    unoui_rect b = { r.x, r.y + ch_tabh() + ch_barh(), r.w,
                     r.h - ch_tabh() - ch_barh() - ch_stath() };
    if (b.h < 20) b.h = 20;
    return b;
}
static unoui_rect band_stat(unoui_rect r)
{ unoui_rect b = { r.x, r.y + r.h - ch_stath(), r.w, ch_stath() }; return b; }

/* toolbar slots: 4 nav buttons, the address field, then 3 on the right */
enum { TB_BACK = 0, TB_FWD, TB_RELOAD, TB_HOME, TB_STAR, TB_MARKS, TB_HIST, TB_N };

static int tb_rightw(void)
{ return ch_btnw() + 8 + fb_text_w("Marks") + 12 + fb_text_w("History") + 12; }

static unoui_rect tb_rect(unoui_rect r, int which)
{
    unoui_rect b = band_bar(r), o;
    int bw = ch_btnw(), pad = 3, y = b.y + 3, h = b.h - 6;
    int rx = b.x + b.w - 4;
    o.y = y; o.h = h;
    switch (which) {
    case TB_BACK:   o.x = b.x + 4;                    o.w = bw; break;
    case TB_FWD:    o.x = b.x + 4 + (bw + pad);       o.w = bw; break;
    case TB_RELOAD: o.x = b.x + 4 + 2 * (bw + pad);   o.w = bw; break;
    case TB_HOME:   o.x = b.x + 4 + 3 * (bw + pad);   o.w = bw; break;
    case TB_HIST:   o.w = fb_text_w("History") + 12;  o.x = rx - o.w; break;
    case TB_MARKS:  o.w = fb_text_w("Marks") + 12;
                    o.x = rx - (fb_text_w("History") + 12) - 4 - o.w; break;
    case TB_STAR:   o.w = bw;
                    o.x = rx - (fb_text_w("History") + 12) - 4
                             - (fb_text_w("Marks") + 12) - 4 - o.w; break;
    default: {      /* the address field: everything in between */
        int lx = b.x + 4 + 4 * (bw + pad) + 4;
        o.x = lx; o.w = (rx - tb_rightw() - 8) - lx;
        if (o.w < 40) o.w = 40;
        break; }
    }
    return o;
}
#define TB_ADDR TB_N          /* tb_rect's default case = the address field */

/* the drop-down panel (bookmarks / history), anchored under its button */
static unoui_rect panel_rect(unoui_rect r)
{
    unoui_rect b = band_bar(r), o;
    int w = r.w - 24, rows = 8;
    if (w > 380) w = 380;
    o.w = w;
    o.x = r.x + r.w - w - 6;
    o.y = b.y + b.h + 1;
    o.h = rows * (fb_text_h() + 3) + 8 + fb_text_h() + 6;
    if (o.h > r.h - (o.y - r.y) - ch_stath() - 4) o.h = r.h - (o.y - r.y) - ch_stath() - 4;
    return o;
}
static unoui_rect panel_list_rect(unoui_rect r)
{
    unoui_rect p = panel_rect(r), o;
    int head = fb_text_h() + 6;
    o.x = p.x + 4; o.y = p.y + head; o.w = p.w - 8; o.h = p.h - head - 4;
    return o;
}

/* ---- chrome painting -----------------------------------------------------
 * The chrome reads the SHELL'S palette instead of fixed light-grey values, so
 * the browser is themed like every other window. That became a requirement
 * rather than a nicety once the tab strip turned into a unoui control: the
 * control paints from the palette, and leaving the toolbar on hard-coded light
 * grey would have put a themed strip on top of chrome that ignored the theme -
 * unmissable on Aurora Dark, which is the shell's own default. The role
 * mapping is one for one with the constants it replaces.
 *
 * The PAGE colours (PG_*) are deliberately NOT themed: a document renders as
 * the document, the way every browser paints a page regardless of the desktop. */
#define CH_FACE   (TH()->pal.face)
#define CH_EDGE   (TH()->pal.shadow)
#define CH_TEXT   (TH()->pal.text)
#define CH_DIM    (TH()->pal.text_dim)
#define CH_ACTIVE (TH()->pal.win_bg)
#define CH_HOT    (TH()->pal.light)

static int g_hot = -1;                 /* toolbar slot under the pointer, -1 */
static int g_hot_tab = -1, g_hot_close = 0, g_hot_plus = 0;
static int g_tab_first;                /* first visible tab, if they overflow */

/* ---- the tab strip ------------------------------------------------------- *
 * The browser keeps tabs in a SPARSE array (g_tab[i].used); unoui_tabs_model
 * is dense. tabs_model() builds the dense view each time it is needed - a
 * label array pointing straight into each tab's own mutable title, plus a
 * slot -> tab map - so nothing about btab's storage had to change and unoui
 * never holds a pointer it could outlive. */
static const char *g_tab_lbl[MAXTABS];
static int         g_tab_map[MAXTABS];

static void tabs_model(unoui_tabs_model *m)
{
    int i, n = 0;
    m->sel = 0; m->hot = -1; m->hot_part = UI_TAB_NONE;
    for (i = 0; i < MAXTABS; i++) {
        if (!g_tab[i].used) continue;
        g_tab_lbl[n] = g_tab[i].title;
        g_tab_map[n] = i;
        if (i == g_cur)     m->sel = n;
        if (i == g_hot_tab) m->hot = n;
        n++;
    }
    m->labels = g_tab_lbl;
    m->n      = n;
    m->first  = g_tab_first;
    m->flags  = UI_TF_CLOSE | UI_TF_PLUS | UI_TF_ELASTIC | UI_TF_OVERFLOW;
    if (m->hot >= 0)     m->hot_part = g_hot_close ? UI_TAB_CLOSE : UI_TAB_SEL;
    else if (g_hot_plus) m->hot_part = UI_TAB_PLUS;
}

static void btn_box(unoui_rect r, int hot, int on)
{
    fb_fill_rect(r.x, r.y, r.w, r.h, hot ? CH_HOT : (on ? CH_ACTIVE : CH_FACE));
    fb_frame_rect(r.x, r.y, r.w, r.h, CH_EDGE);
}

static void glyph_arrow(unoui_rect r, int left, fb_px col)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, i;
    /* column i is 2i+1 tall, so the TIP is the shortest column: put it on the
     * side the arrow points at */
    for (i = 0; i < 5; i++)
        fb_vline(cx + (left ? i - 2 : 2 - i), cy - i, 2 * i + 1, col);
}
static void glyph_reload(unoui_rect r, fb_px col)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, i;
    for (i = 0; i < 12; i++) {                      /* a broken ring */
        static const signed char ox[12] = { 0, 2, 4, 5, 4, 2, 0,-2,-4,-5,-4,-2 };
        static const signed char oy[12] = {-5,-4,-2, 0, 2, 4, 5, 4, 2, 0,-2,-4 };
        if (i == 2) continue;
        fb_fill_rect(cx + ox[i], cy + oy[i], 2, 2, col);
    }
    fb_fill_rect(cx + 2, cy - 6, 4, 2, col);        /* the arrow head */
    fb_fill_rect(cx + 4, cy - 6, 2, 4, col);
}
static void glyph_home(unoui_rect r, fb_px col)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, i;
    for (i = 0; i < 5; i++) fb_hline(cx - i, cy - 5 + i, 2 * i + 1, col);
    fb_frame_rect(cx - 3, cy, 7, 6, col);
}
/* a bookmark RIBBON, not a star: at ten pixels a five-point star is mush,
 * while a ribbon (a tag with a notched foot) still reads as itself */
static void glyph_star(unoui_rect r, fb_px col, int filled)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, i;
    int w = 9, h = 12, x0 = cx - w / 2, y0 = cy - h / 2;
    if (filled) fb_fill_rect(x0, y0, w, h - 3, col);
    else {
        fb_frame_rect(x0, y0, w, h - 3, col);
        fb_vline(x0, y0, h - 3, col); fb_vline(x0 + w - 1, y0, h - 3, col);
    }
    for (i = 0; i < 4; i++) {                       /* the notched foot */
        int y = y0 + h - 4 + i, len = w / 2 - i;
        if (len <= 0) break;
        if (filled) {
            fb_hline(x0, y, len, col);
            fb_hline(x0 + w - len, y, len, col);
        } else {
            fb_pixel(x0 + len - 1, y, col);
            fb_pixel(x0 + w - len, y, col);
        }
    }
}

/* Thirty lines of hand-rolled strip became this. Everything it used to do -
 * elastic widths, truncation, the close boxes, the "+", the hover states - is
 * the toolkit's now, and the geometry it draws is the same geometry the hit
 * test below reads, which the two hand-written copies could not guarantee. */
static void draw_tabs(unoui_rect r)
{
    unoui_tabs_model m;
    unoui_rect b = band_tabs(r);
    tabs_model(&m);
    g_tab_first = unoui_tabs_reveal(TH(), b, &m, m.sel);   /* keep it in view */
    m.first = g_tab_first;
    unoui_tabs_draw(TH(), b, &m);
}

static void draw_toolbar(unoui_rect r)
{
    unoui_rect b = band_bar(r), o;
    btab *t = &g_tab[g_cur];
    fb_fill_rect(b.x, b.y, b.w, b.h, CH_FACE);
    fb_hline(b.x, b.y + b.h - 1, b.w, CH_EDGE);

    o = tb_rect(r, TB_BACK);   btn_box(o, g_hot == TB_BACK, 0);
    glyph_arrow(o, 1, t->nback ? CH_TEXT : CH_DIM);
    o = tb_rect(r, TB_FWD);    btn_box(o, g_hot == TB_FWD, 0);
    glyph_arrow(o, 0, t->nfwd ? CH_TEXT : CH_DIM);
    o = tb_rect(r, TB_RELOAD); btn_box(o, g_hot == TB_RELOAD, 0);
    glyph_reload(o, CH_TEXT);
    o = tb_rect(r, TB_HOME);   btn_box(o, g_hot == TB_HOME, 0);
    glyph_home(o, CH_TEXT);

    o = tb_rect(r, TB_STAR);   btn_box(o, g_hot == TB_STAR, 0);
    glyph_star(o, bm_index(t->loc) >= 0 ? FB_RGB(226, 170, 30) : CH_DIM,
               bm_index(t->loc) >= 0);
    o = tb_rect(r, TB_MARKS);  btn_box(o, g_hot == TB_MARKS, g_panel == PANEL_MARKS);
    fb_text(o.x + 6, o.y + (o.h - fb_text_h()) / 2, "Marks", CH_TEXT, -1);
    o = tb_rect(r, TB_HIST);   btn_box(o, g_hot == TB_HIST, g_panel == PANEL_HIST);
    fb_text(o.x + 6, o.y + (o.h - fb_text_h()) / 2, "History", CH_TEXT, -1);

    /* the address field */
    o = tb_rect(r, TB_ADDR);
    fb_fill_rect(o.x, o.y, o.w, o.h, FB_RGB(255,255,255));
    fb_frame_rect(o.x, o.y, o.w, o.h, g_addr_focus ? PG_LINK : CH_EDGE);
    {   int ty = o.y + (o.h - fb_text_h()) / 2;
        const char *s = g_addr;
        int off = 0, cw;
        /* keep the caret in view: scroll the text left until it fits */
        while (s[off] && (cw = fb_text_w(s + off)) > o.w - 10) {
            if (fb_text_w(s + off) - fb_text_w(s + off + 1) <= 0) break;
            off++;
        }
        fb_text(o.x + 5, ty, s + off, CH_TEXT, -1);
        if (g_addr_focus) {
            char pre[LOCMAX];
            int k = g_addr_caret - off; if (k < 0) k = 0;
            sput(pre, LOCMAX, s + off);
            if (k < (int)sizeof pre) pre[k] = 0;
            fb_vline(o.x + 5 + fb_text_w(pre), o.y + 3, o.h - 6, CH_TEXT);
        }
    }
}

static void draw_status(unoui_rect r)
{
    unoui_rect b = band_stat(r);
    const char *s = g_hint[0] ? g_hint : g_status;
    fb_fill_rect(b.x, b.y, b.w, b.h, CH_FACE);
    fb_hline(b.x, b.y, b.w, CH_EDGE);
    /* Find takes over the status band rather than opening a floating bar:
     * the band is already there, already the width of the window, and a
     * find bar that covers page text is a find bar that hides the thing you
     * were looking for. */
    if (g_find_on || g_find[0]) {
        char line[160];
        int n = 0;
        const char *lbl = "Find: ";
        while (*lbl && n < (int)sizeof line - 1) line[n++] = *lbl++;
        {   const char *q = g_find;
            while (*q && n < (int)sizeof line - 1) line[n++] = *q++; }
        if (g_find_on && n < (int)sizeof line - 1) line[n++] = '_';
        line[n] = 0;
        fb_text(b.x + 6, b.y + 2, line, CH_TEXT, -1);
        {   char cnt[48];
            int k = 0, i, tot = g_nfind, cur = g_nfind ? g_find_sel + 1 : 0;
            const char *w;
            if (!g_find[0]) w = "type to search   Esc closes";
            else if (!tot)  w = "no matches   Esc closes";
            else {
                char num[12];
                w = 0;
                for (i = 0; i < 2; i++) {
                    int v = i ? tot : cur, j = 0;
                    if (!v) num[j++] = '0';
                    while (v) { num[j++] = (char)('0' + v % 10); v /= 10; }
                    while (j) cnt[k++] = num[--j];
                    if (!i) { cnt[k++] = ' '; cnt[k++] = 'o'; cnt[k++] = 'f'; cnt[k++] = ' '; }
                }
                {   const char *t = "   Enter next   Esc closes";
                    while (*t && k < (int)sizeof cnt - 1) cnt[k++] = *t++; }
                cnt[k] = 0;
            }
            fb_text(b.x + b.w - 260, b.y + 2, w ? w : cnt, CH_DIM, -1); }
        return;
    }
    fb_text(b.x + 6, b.y + 2, s, g_hint[0] ? PG_LINK : CH_DIM, -1);
}

/* ---- download (Ctrl-S) ----------------------------------------------------
 * Save the page you are looking at to the first writable volume. The bytes
 * are the ones already in hand - the document the tab holds - so this costs
 * no second fetch and cannot save something different from what is on
 * screen, which is the failure mode of a save that re-requests.
 *
 * The name comes from the URL's last path segment, sanitised to something
 * FAT will accept; a URL that ends in a slash gets INDEX.HTM. */
static void save_name_from(const char *loc, char *out, int cap)
{
    const char *base = loc, *p;
    int n = 0, dot = 0;
    for (p = loc; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
    for (p = base; *p && n < cap - 1; p++) {
        char c = *p;
        if (c == '?' || c == '#') break;            /* query is not a name */
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            if (c == '.') dot = 1;
            out[n++] = c;
        }
    }
    out[n] = 0;
    if (!n) { const char *d = "INDEX.HTM"; n = 0; while (d[n] && n < cap - 1) { out[n] = d[n]; n++; } out[n] = 0; return; }
    if (!dot && n < cap - 5) { out[n++] = '.'; out[n++] = 'T'; out[n++] = 'X'; out[n++] = 'T'; out[n] = 0; }
}

static void save_page(btab *t)
{
    char name[64];
    const char *doc = t->doc;
    int v, nv = uno_fs_volumes(), len;
    if (!doc || !*doc) { sput(g_status, sizeof g_status, "Nothing to save."); return; }
    len = (int)strlen(doc);
    save_name_from(t->loc, name, sizeof name);
    for (v = 0; v < nv; v++) {
        if (!uno_fs_writable(v)) continue;
        if (uno_fs_write(v, name, (const unsigned char *)doc, len) == 0) {
            char msg[160], *p = msg, *end = msg + sizeof msg;
            p = sapp(p, end, "Saved ");
            p = sapp(p, end, name);
            p = sapp(p, end, " to volume ");
            {   char d[8]; int k = 0, x = v;
                if (!x) d[k++] = '0';
                while (x) { d[k++] = (char)('0' + x % 10); x /= 10; }
                while (k && p < end - 1) *p++ = d[--k];
                *p = 0; }
            sapp(p, end, ".");
            sput(g_status, sizeof g_status, msg);
            return;
        }
    }
    sput(g_status, sizeof g_status, "Could not save: no writable volume.");
}

/* Put the selected match on screen. Document coordinates are what both
 * painters record, so this is the same arithmetic for either. */
static void find_scroll_to(btab *t, unoui_rect r)
{
    int top, h = band_body(r).h;
    if (g_find_sel < 0 || g_find_sel >= g_nfind) return;
    top = g_findhit[g_find_sel].y - h / 3;      /* a third down, not flush */
    if (top < 0) top = 0;
    t->scroll = top;
}

static void draw_panel(unoui_rect r)
{
    const struct unoui_theme *th = pc64_shell_theme();
    unoui_rect p = panel_rect(r), l = panel_list_rect(r);
    const char **items = (g_panel == PANEL_MARKS) ? g_bm_ptr : g_hs_ptr;
    int n = (g_panel == PANEL_MARKS) ? g_nbm : g_nhs;
    fb_fill_rect(p.x, p.y, p.w, p.h, CH_FACE);
    fb_frame_rect(p.x, p.y, p.w, p.h, CH_EDGE);
    fb_text(p.x + 6, p.y + 3,
            g_panel == PANEL_MARKS ? "Bookmarks  (Ctrl-D adds this page)"
                                   : "History  (this session)", CH_TEXT, -1);
    if (!n) {
        fb_text(l.x + 6, l.y + 4,
                g_panel == PANEL_MARKS ? "No bookmarks yet." : "Nothing visited yet.",
                CH_DIM, -1);
        return;
    }
    if (th) unoui_list_draw(th, l, items, n, g_panel_sel, g_panel_top);
}

static unoui_rect start_list_rect(unoui_rect r);

/* the start page: the built-in documents plus whatever the disks hold, in a
 * scrolling list (the toolkit's, so the wheel / bar / keys all work) */
static void draw_start(unoui_rect r)
{
    const struct unoui_theme *th = pc64_shell_theme();
    btab *t = &g_tab[g_cur];
    unoui_rect body = band_body(r), l;
    fb_fill_rect(body.x, body.y, body.w, body.h, PG_BG);
    fb_text(body.x + 10, body.y + 6, "Documents on this machine", PG_HEAD, -1);
    l = start_list_rect(r);
    t->top = unoui_list_reveal(l, g_nfiles, t->sel, t->top);
    if (th) unoui_list_draw(th, l, g_row_ptr, g_nfiles, t->sel, t->top);
    fb_text(body.x + 10, body.y + body.h - fb_text_h() - 4,
            "Enter opens the highlighted document.  * = built in.", CH_DIM, -1);
}

/* the list sits between the heading and the hint line at the foot */
static unoui_rect start_list_rect(unoui_rect r)
{
    unoui_rect body = band_body(r), l;
    int hh = fb_text_h() + 6, foot = fb_text_h() + 10;
    l.x = body.x + 8; l.y = body.y + hh + 6;
    l.w = body.w - 16; l.h = body.h - hh - 6 - foot;
    if (l.h < 24) l.h = 24;
    return l;
}

static void loading_frame(const char *what)          /* one presented frame */
{
    unoui_rect r = g_rect, body;
    if (r.w <= 0) return;
    body = band_body(r);
    draw_tabs(r); draw_toolbar(r);
    fb_fill_rect(body.x, body.y, body.w, body.h, PG_BG);
    fb_text(body.x + 12, body.y + 16, "Loading...", PG_HEAD, -1);
    fb_text(body.x + 12, body.y + 16 + fb_text_h() + 6, what, PG_LINK, -1);
    sput(g_status, sizeof g_status, "Loading...");
    draw_status(r);
    uno_pc64_present();
}

static void br_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    btab *t;
    (void)w; (void)ctx;
    g_rect = r;
    if (!g_ntab) tab_new("uno:start");
    t = &g_tab[g_cur];

    draw_tabs(r);
    draw_toolbar(r);

    if (t->start) draw_start(r);
    else {
        unoui_rect body = band_body(r);
        fb_fill_rect(body.x, body.y, body.w, body.h, PG_BG);
        if (t->doc) {
            /* clip to the body: a page line that starts above the top of the
             * view is still painted (only fully off-screen rows are skipped),
             * so without this the document draws over the toolbar */
            fb_set_clip(body.x, body.y, body.w, body.h);
            /* Match collection wraps BOTH painters. It used to sit inside
             * the HTML one, which meant a Markdown page (the welcome page,
             * every README) highlighted its hits but counted none of them -
             * the count is what Enter steps through, so find silently did
             * nothing there. Rebuilt every paint: the painter already walks
             * every word, and a page changed under script cannot leave a
             * stale rect pointing at text that moved. */
            g_nfind = 0;
            g_find_collect = g_find[0] != 0;
            if (t->is_html) render_html(t->doc, body, t->scroll);
            else            render_md(t->doc, body, t->scroll);
            g_find_collect = 0;
            if (g_find_sel >= g_nfind) g_find_sel = g_nfind ? g_nfind - 1 : 0;
            fb_set_clip(r.x, r.y, r.w, r.h);
            /* track content height for scroll clamping */
            { int total = (fy + flh) - body.y;
              if (t->scroll > total - body.h) t->scroll = total - body.h;
              if (t->scroll < 0) t->scroll = 0; }
        }
    }
    if (g_panel) draw_panel(r);
    draw_status(r);
}

/* ---- input --------------------------------------------------------------- */
static void addr_focus(int on)
{
    g_addr_focus = on;
    if (on) { sput(g_addr, LOCMAX, g_tab[g_cur].loc); g_addr_caret = (int)strlen(g_addr); }
}

static void addr_insert(int chv)
{
    int len = (int)strlen(g_addr), i;
    if (len >= LOCMAX - 1) return;
    for (i = len; i > g_addr_caret; i--) g_addr[i] = g_addr[i-1];
    g_addr[g_addr_caret++] = (char)chv;
    g_addr[len + 1] = 0;
}
static void addr_delete(int before)
{
    int len = (int)strlen(g_addr), i, at = before ? g_addr_caret - 1 : g_addr_caret;
    if (at < 0 || at >= len) return;
    for (i = at; i < len; i++) g_addr[i] = g_addr[i+1];
    if (before) g_addr_caret--;
}

static void panel_open(int which)
{
    g_panel = (g_panel == which) ? PANEL_NONE : which;
    g_panel_sel = 0; g_panel_top = 0;
}

static void panel_activate(void)
{
    const char *loc = 0;
    if (g_panel == PANEL_MARKS && g_panel_sel < g_nbm) loc = g_bm_loc[g_panel_sel];
    if (g_panel == PANEL_HIST  && g_panel_sel < g_nhs) loc = g_hs_loc[g_panel_sel];
    g_panel = PANEL_NONE;
    if (loc) navigate(loc, 1);
}

static void open_start_row(int idx)
{
    char loc[LOCMAX];
    if (idx < 0 || idx >= g_nfiles) return;
    if (g_vol[idx] < 0) {
        sput(loc, LOCMAX, idx == 0 ? "uno:welcome" : idx == 1 ? "uno:sample" : "uno:script");
    } else {
        char *p = loc, *end = loc + LOCMAX;
        char vs[8]; int k = 0, v = g_vol[idx];
        if (!v) vs[k++] = '0';
        while (v) { vs[k++] = (char)('0' + v % 10); v /= 10; }
        p = sapp(p, end, "file:");
        while (k) { if (p < end - 1) *p++ = vs[--k]; else k = 0; }
        *p = 0;
        p = sapp(p, end, ":");
        sapp(p, end, g_names[idx]);
    }
    navigate(loc, 1);
}

/* follow the link at document position (px, py); 1 if one was there */
static int follow_link_at(int px, int py)
{
    int i;
    for (i = 0; i < g_nlink; i++) {
        blinkrect *L = &g_link[i];
        if (px >= L->x && px < L->x + L->w && py >= L->y - 2 && py < L->y + L->h) {
            char to[LOCMAX];
            resolve(g_href[L->href], g_tab[g_cur].loc, to, LOCMAX);
            navigate(to, 1);
            return 1;
        }
    }
    return 0;
}

static int link_hint_at(int px, int py)
{
    int i;
    for (i = 0; i < g_nlink; i++) {
        blinkrect *L = &g_link[i];
        if (px >= L->x && px < L->x + L->w && py >= L->y - 2 && py < L->y + L->h) {
            sput(g_hint, sizeof g_hint, g_href[L->href]);
            return 1;
        }
    }
    return 0;
}

/* Left / Right step through a page's links, Enter follows the selected one.
 * Keyboard link navigation is the PRIMARY path here: it needs no pointing
 * device, which matters on a machine whose trackpad may not be up yet - and
 * QEMU delivers no pointer input to this guest at all, so it is also the only
 * path a harness can drive. */
/* The engine renderer (BROWSER_ENGINE=uw) paints from the display list, so the
 * flow painter's link map is empty there. Fall back to the DOM's own <a>
 * elements, which keeps the keyboard path alive under both renderers. */
static const char *dom_link_at(int idx)
{
    uw_node *links[64];
    int n;
    if (!g_dom) return 0;
    n = uw_elements_by_tag(g_dom, NULL, "a", links, 64);
    if (idx < 0 || idx >= n) return 0;
    return uw_attr(g_dom, links[idx], "href");
}
static const char *dom_link_cycle(int dir)
{
    uw_node *links[64];
    int n, guard = 0;
    if (!g_dom) return 0;
    n = uw_elements_by_tag(g_dom, NULL, "a", links, 64);
    if (n <= 0) return 0;
    do {
        g_link_sel += dir;
        if (g_link_sel >= n) g_link_sel = 0;
        if (g_link_sel < 0) g_link_sel = n - 1;
    } while (!uw_attr(g_dom, links[g_link_sel], "href") && ++guard < n);
    return uw_attr(g_dom, links[g_link_sel], "href");
}

static int link_step(int dir)
{
    btab *t = &g_tab[g_cur];
    unoui_rect body = band_body(g_rect);
    int i;
    if (!g_nhref) {
        const char *h = dom_link_cycle(dir);
        if (!h) return 0;
        sput(g_hint, sizeof g_hint, h);
        return 1;
    }
    g_link_sel += dir;
    if (g_link_sel >= g_nhref) g_link_sel = 0;
    if (g_link_sel < 0) g_link_sel = g_nhref - 1;
    sput(g_hint, sizeof g_hint, g_href[g_link_sel]);
    for (i = 0; i < g_nlink; i++) {              /* scroll it into view */
        blinkrect *L = &g_link[i];
        if (L->href != g_link_sel) continue;
        if (L->y - t->scroll < body.y)
            t->scroll = L->y - body.y - 8;
        else if (L->y + L->h - t->scroll > body.y + body.h)
            t->scroll = L->y + L->h - (body.y + body.h) + 8;
        if (t->scroll < 0) t->scroll = 0;
        break;
    }
    return 1;
}

static void scroll_by(int dy)
{
    btab *t = &g_tab[g_cur];
    t->scroll += dy;
    if (t->scroll < 0) t->scroll = 0;
}

static int br_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    btab *t = &g_tab[g_cur];
    unoui_rect r = g_rect;
    (void)w; (void)ctx;

#ifdef UW_ENGINE
    /* ---- a focused form control owns the keyboard ----
     * Ahead of everything else: while you are typing in a field, a plain
     * letter is text, not a browser shortcut. Enter submits, Tab and Esc
     * leave the field. The value lives on the element, so a re-render (or a
     * script reading it) sees exactly what was typed. */
    if (g_form_focus) {
        if (e->kind == UI_EV_CHAR && e->ch >= 32 && e->ch < 127) {
            const char *cur = uw_attr(g_dom, g_form_focus, "value");
            char buf[256];
            int n = 0;
            while (cur && cur[n] && n < (int)sizeof buf - 2) { buf[n] = cur[n]; n++; }
            buf[n++] = (char)e->ch;
            buf[n] = 0;
            uw_set_attr(g_dom, g_form_focus, "value", buf);
            g_uw_sig = 0;                      /* the field must repaint */
            pc64_shell_dirty();
            return 1;
        }
        if (e->kind == UI_EV_KEY) {
            switch (e->key) {
            case UI_KEY_BACKSPACE: {
                const char *cur = uw_attr(g_dom, g_form_focus, "value");
                char buf[256];
                int n = 0;
                while (cur && cur[n] && n < (int)sizeof buf - 1) { buf[n] = cur[n]; n++; }
                if (n) buf[--n] = 0; else buf[0] = 0;
                uw_set_attr(g_dom, g_form_focus, "value", buf);
                g_uw_sig = 0;
                pc64_shell_dirty();
                return 1; }
            case UI_KEY_ENTER:
                form_submit(t, g_form_focus);
                g_form_focus = 0;
                return 1;
            case UI_KEY_ESC:
            case UI_KEY_TAB:
                g_form_focus = 0;
                pc64_shell_dirty();
                return 1;
            default: return 1;
            }
        }
    }
#endif /* UW_ENGINE */

    /* ---- the find bar owns the keyboard while it is open ----
     * Ahead of the address bar because Ctrl-F is what put it there; a page
     * scroll or a link walk arriving mid-search would be the wrong answer to
     * every key. Each edit resets the selection to the first hit, so typing
     * more characters always lands you at the top of the new result set. */
    if (g_find_on) {
        if (e->kind == UI_EV_CHAR && e->ch >= 32 && e->ch < 127) {
            int n = (int)strlen(g_find);
            if (n < FIND_TEXT - 1) { g_find[n] = (char)e->ch; g_find[n + 1] = 0; }
            g_find_sel = 0;
            return 1;
        }
        if (e->kind == UI_EV_KEY) {
            switch (e->key) {
            case UI_KEY_BACKSPACE: {
                int n = (int)strlen(g_find);
                if (n) g_find[n - 1] = 0;
                g_find_sel = 0;
                return 1; }
            case UI_KEY_ESC:
                g_find_on = 0;
                g_find[0] = 0;              /* closing clears the highlights */
                g_nfind = 0;
                return 1;
            case UI_KEY_ENTER:
            case UI_KEY_DOWN:
                if (g_nfind) {
                    g_find_sel = (g_find_sel + 1) % g_nfind;
                    find_scroll_to(t, r);
                }
                return 1;
            case UI_KEY_UP:
                if (g_nfind) {
                    g_find_sel = (g_find_sel + g_nfind - 1) % g_nfind;
                    find_scroll_to(t, r);
                }
                return 1;
            default: return 1;              /* swallow the rest while open */
            }
        }
    }

    /* ---- the address bar owns the keyboard while it has focus ---- */
    if (g_addr_focus) {
        if (e->kind == UI_EV_CHAR && e->ch >= 32 && e->ch < 127) { addr_insert(e->ch); return 1; }
        if (e->kind == UI_EV_KEY) {
            switch (e->key) {
            case UI_KEY_BACKSPACE: addr_delete(1); return 1;
            case UI_KEY_DELETE:    addr_delete(0); return 1;
            case UI_KEY_LEFT:      if (g_addr_caret > 0) g_addr_caret--; return 1;
            case UI_KEY_RIGHT:     if (g_addr[g_addr_caret]) g_addr_caret++; return 1;
            case UI_KEY_HOME:      g_addr_caret = 0; return 1;
            case UI_KEY_END:       g_addr_caret = (int)strlen(g_addr); return 1;
            case UI_KEY_ESC:       addr_focus(0); return 1;
            case UI_KEY_ENTER:
                g_addr_focus = 0;
                if (g_addr[0]) navigate(g_addr, 1);
                return 1;
            default: return 1;                     /* swallow the rest */
            }
        }
    }

    /* ---- a panel owns the keyboard while it is open ---- */
    if (g_panel && e->kind == UI_EV_KEY) {
        int n = (g_panel == PANEL_MARKS) ? g_nbm : g_nhs;
        unoui_rect l = panel_list_rect(r);
        switch (e->key) {
        case UI_KEY_UP:    if (g_panel_sel > 0) g_panel_sel--; break;
        case UI_KEY_DOWN:  if (g_panel_sel < n - 1) g_panel_sel++; break;
        case UI_KEY_PGUP:  g_panel_sel -= unoui_list_rows(l); break;
        case UI_KEY_PGDN:  g_panel_sel += unoui_list_rows(l); break;
        case UI_KEY_HOME:  g_panel_sel = 0; break;
        case UI_KEY_END:   g_panel_sel = n - 1; break;
        case UI_KEY_ENTER: panel_activate(); return 1;
        case UI_KEY_ESC:   g_panel = PANEL_NONE; return 1;
        default: return 1;
        }
        if (g_panel_sel < 0) g_panel_sel = 0;
        if (g_panel_sel > n - 1) g_panel_sel = n - 1;
        g_panel_top = unoui_list_reveal(l, n, g_panel_sel, g_panel_top);
        return 1;
    }

    if (e->kind == UI_EV_CHAR && e->ch >= 32 && e->ch < 127) {
        addr_focus(1);                       /* typing goes to the address bar */
        sput(g_addr, LOCMAX, ""); g_addr_caret = 0;
        addr_insert(e->ch);
        return 1;
    }

    if (e->kind == UI_EV_KEY) {
        if (e->mods & UI_MOD_CTRL) {         /* Ctrl-Left / Ctrl-Right = history */
            if (e->key == UI_KEY_LEFT)  { go_back(); return 1; }
            if (e->key == UI_KEY_RIGHT) { go_fwd();  return 1; }
        }
        if (t->start) {                      /* the start page's list */
            unoui_rect l = start_list_rect(r);
            switch (e->key) {
            case UI_KEY_UP:    if (t->sel > 0) t->sel--; break;
            case UI_KEY_DOWN:  if (t->sel < g_nfiles - 1) t->sel++; break;
            case UI_KEY_PGUP:  t->sel -= unoui_list_rows(l); break;
            case UI_KEY_PGDN:  t->sel += unoui_list_rows(l); break;
            case UI_KEY_HOME:  t->sel = 0; break;
            case UI_KEY_END:   t->sel = g_nfiles - 1; break;
            case UI_KEY_ENTER: open_start_row(t->sel); return 1;
            case UI_KEY_BACKSPACE: go_back(); return 1;
            default: return 0;
            }
            if (t->sel < 0) t->sel = 0;
            if (t->sel > g_nfiles - 1) t->sel = g_nfiles - 1;
            t->top = unoui_list_reveal(l, g_nfiles, t->sel, t->top);
            return 1;
        }
        switch (e->key) {                    /* a document */
        case UI_KEY_DOWN:  scroll_by(24);  return 1;
        case UI_KEY_UP:    scroll_by(-24); return 1;
        case UI_KEY_PGDN:  scroll_by(180); return 1;
        case UI_KEY_PGUP:  scroll_by(-180);return 1;
        case UI_KEY_HOME:  t->scroll = 0;  return 1;
        case UI_KEY_BACKSPACE: go_back();  return 1;
        case UI_KEY_RIGHT: return link_step(1);
        case UI_KEY_LEFT:  return link_step(-1);
        case UI_KEY_ENTER: {
            const char *href = (g_link_sel >= 0 && g_link_sel < g_nhref)
                               ? g_href[g_link_sel] : dom_link_at(g_link_sel);
            if (href && *href && href[0] != '#') {
                char to[LOCMAX];
                resolve(href, t->loc, to, LOCMAX);
                navigate(to, 1);
                return 1;
            }
            return 0; }
        case UI_KEY_ESC:   g_link_sel = -1; return 1;
        default: return 0;
        }
    }

    if (e->kind == UI_EV_WHEEL) {
        unoui_rect body = band_body(r);
        if (g_panel) {
            unoui_rect l = panel_list_rect(r);
            int n = (g_panel == PANEL_MARKS) ? g_nbm : g_nhs;
            g_panel_top += e->wheel * 3;
            { int mt = unoui_list_maxtop(l, n);
              if (g_panel_top > mt) g_panel_top = mt;
              if (g_panel_top < 0) g_panel_top = 0; }
            return 1;
        }
        if (t->start) {
            unoui_rect l = start_list_rect(r);
            int mt = unoui_list_maxtop(l, g_nfiles);
            t->top += e->wheel * 3;
            if (t->top > mt) t->top = mt;
            if (t->top < 0) t->top = 0;
            return 1;
        }
        (void)body;
        scroll_by(e->wheel * 24);
        return 1;
    }

    if (e->kind == UI_EV_MOUSE_MOVE) {
        int i;
        g_hot = -1; g_hot_tab = -1; g_hot_close = 0; g_hot_plus = 0;
        g_hint[0] = 0;
        for (i = 0; i < TB_N; i++) {
            unoui_rect o = tb_rect(r, i);
            if (e->x >= o.x && e->x < o.x + o.w && e->y >= o.y && e->y < o.y + o.h) {
                static const char *kTip[TB_N] = {
                    "Back (Backspace)", "Forward", "Reload (F5)", "Start page",
                    "Bookmark this page (Ctrl-D)", "Bookmarks (Ctrl-B)",
                    "History (Ctrl-H)" };
                g_hot = i; sput(g_hint, sizeof g_hint, kTip[i]);
                return 1;
            }
        }
        {   /* the strip: one hit test, and the close zone is the box that was
             * actually drawn rather than "the last 18 px" */
            unoui_tabs_model m;
            int slot = -1, part;
            tabs_model(&m);
            part = unoui_tabs_hit(TH(), band_tabs(r), &m, e->x, e->y, &slot);
            if ((part == UI_TAB_SEL || part == UI_TAB_CLOSE) && slot >= 0) {
                g_hot_tab = g_tab_map[slot];
                g_hot_close = (part == UI_TAB_CLOSE);
                sput(g_hint, sizeof g_hint,
                     g_hot_close ? "Close this tab" : g_tab[g_hot_tab].loc);
                return 1;
            }
            if (part == UI_TAB_PLUS) {
                g_hot_plus = 1; sput(g_hint, sizeof g_hint, "New tab (Ctrl-T)");
                return 1;
            }
            if (part == UI_TAB_OVER) {
                sput(g_hint, sizeof g_hint, "More tabs"); return 1;
            } }
        if (!t->start && !g_panel) {                  /* a link under the pointer */
            unoui_rect body = band_body(r);
            if (e->y >= body.y && e->y < body.y + body.h)
                link_hint_at(e->x, e->y + t->scroll);
        }
        return 0;
    }

    if (e->kind == UI_EV_MOUSE_DOWN) {
        int i;
        unoui_rect body = band_body(r);

        if (g_panel) {                                /* the open panel first */
            unoui_rect p = panel_rect(r), l = panel_list_rect(r);
            int n = (g_panel == PANEL_MARKS) ? g_nbm : g_nhs;
            int inside = (e->x >= p.x && e->x < p.x + p.w &&
                          e->y >= p.y && e->y < p.y + p.h);
            if (inside) {
                if (n && e->y >= l.y && e->y < l.y + l.h) {
                    unoui_rect bar = unoui_list_bar(l, n);
                    if (bar.w && e->x >= bar.x) {     /* its scrollbar */
                        int mt = unoui_list_maxtop(l, n);
                        g_panel_top += (e->y < bar.y + bar.h / 2) ? -1 : 1;
                        if (g_panel_top > mt) g_panel_top = mt;
                        if (g_panel_top < 0) g_panel_top = 0;
                        return 1;
                    }
                    g_panel_sel = unoui_list_index_at(l, n, g_panel_top, e->y);
                    panel_activate();
                }
                return 1;
            }
            g_panel = PANEL_NONE;                     /* click-out dismisses */
            /* and fall through, so the click still does what it was aimed at */
        }

        for (i = 0; i < TB_N; i++) {                  /* toolbar */
            unoui_rect o = tb_rect(r, i);
            if (e->x < o.x || e->x >= o.x + o.w || e->y < o.y || e->y >= o.y + o.h) continue;
            switch (i) {
            case TB_BACK:   go_back(); break;
            case TB_FWD:    go_fwd();  break;
            case TB_RELOAD: load_loc(t, t->loc); break;
            case TB_HOME:   navigate("uno:start", 1); break;
            case TB_STAR:   bm_toggle(); break;
            case TB_MARKS:  panel_open(PANEL_MARKS); break;
            case TB_HIST:   panel_open(PANEL_HIST); break;
            }
            return 1;
        }
        {   unoui_rect o = tb_rect(r, TB_ADDR);       /* the address field */
            if (e->x >= o.x && e->x < o.x + o.w && e->y >= o.y && e->y < o.y + o.h) {
                addr_focus(1);
                return 1;
            } }
        {   /* the strip: select, close, new, or scroll - one hit test, and it
             * reads the same geometry the painter drew */
            unoui_tabs_model m;
            int slot = -1, part;
            tabs_model(&m);
            part = unoui_tabs_hit(TH(), band_tabs(r), &m, e->x, e->y, &slot);
            if (part == UI_TAB_CLOSE && slot >= 0) { tab_close(g_tab_map[slot]); return 1; }
            if (part == UI_TAB_SEL && slot >= 0) {
                g_cur = g_tab_map[slot]; g_link_sel = -1;
                if (!g_addr_focus) { sput(g_addr, LOCMAX, g_tab[g_cur].loc);
                                     g_addr_caret = (int)strlen(g_addr); }
                return 1;
            }
            if (part == UI_TAB_PLUS) { tab_new("uno:start"); return 1; }
            if (part == UI_TAB_OVER) {
                int mf = unoui_tabs_maxfirst(TH(), band_tabs(r), &m);
                if (g_tab_first < mf) g_tab_first++;
                return 1;
            } }

        if (e->y >= body.y && e->y < body.y + body.h) {
            if (g_addr_focus) addr_focus(0);
            if (t->start) {                           /* the start page's list */
                unoui_rect l = start_list_rect(r);
                unoui_rect bar = unoui_list_bar(l, g_nfiles);
                if (e->y < l.y || e->y >= l.y + l.h) return 1;
                if (bar.w && e->x >= bar.x) {
                    int mt = unoui_list_maxtop(l, g_nfiles);
                    t->top += (e->y < bar.y + bar.h / 2) ? -1 : 1;
                    if (t->top > mt) t->top = mt;
                    if (t->top < 0) t->top = 0;
                    return 1;
                }
                t->sel = unoui_list_index_at(l, g_nfiles, t->top, e->y);
                open_start_row(t->sel);
                return 1;
            }
#ifdef UW_ENGINE
            /* the engine owns page geometry when it is the renderer: ask the
             * display list what is under the pointer */
            if (g_dom) {
                uw_node *n = uw_hit_test(g_dom, e->x - r.x, e->y - r.y + t->scroll);
                uw_node *a;
                /* handlers see the click FIRST, as they do in a browser; a
                 * handler that rewrites the page marks the tree dirty and the
                 * next frame relays it out */
                if (n && webjs_page_active() &&
                    webjs_event(n, "click", g_js_log, sizeof g_js_log)) {
                    if (webjs_take_dirty()) { g_uw_sig = 0; pc64_shell_dirty(); }
                    return 1;
                }
                /* a form control takes focus (or submits) before the click
                 * is considered as a link - a submit button inside an <a>
                 * must submit, not navigate */
                {   uw_node *ctl = n;
                    for (; ctl; ctl = uw_parent(ctl)) {
                        const char *tg;
                        if (uw_type(ctl) != UW_NODE_ELEMENT) continue;
                        tg = uw_tag_name(g_dom, ctl);
                        if (!strcmp(tg, "input") || !strcmp(tg, "textarea") ||
                            !strcmp(tg, "select") || !strcmp(tg, "button")) break;
                    }
                    if (ctl) {
                        const char *ty = uw_attr(g_dom, ctl, "type");
                        const char *tg = uw_tag_name(g_dom, ctl);
                        if (!strcmp(tg, "button") ||
                            (ty && (!strcmp(ty, "submit") || !strcmp(ty, "button"))))
                            form_submit(t, ctl);
                        else { g_form_focus = ctl; pc64_shell_dirty(); }
                        return 1;
                    } }
                a = uw_link_at(g_dom, n);
                if (a) {
                    const char *href = uw_attr(g_dom, a, "href");
                    if (href && *href && href[0] != '#') {
                        char to[LOCMAX];
                        resolve(href, t->loc, to, LOCMAX);
                        navigate(to, 1);
                        return 1;
                    }
                }
            }
#endif
            return follow_link_at(e->x, e->y + t->scroll);
        }
        return 0;
    }
    return 0;
}

/* accelerators, routed by the shell while the browser window is in front (a
 * canvas never sees Ctrl-modified characters otherwise). 1 = consumed. */
int pc64_browser_key(int uni, int scan, int ctrl)
{
    btab *t = &g_tab[g_cur];
    if (scan == 0x0F) { load_loc(t, t->loc); return 1; }            /* F5 */
    if (!ctrl) return 0;
    switch (uni) {
    case 'f': case 'F':                       /* find in page */
        g_panel = PANEL_NONE;
        g_find_on = 1;
        return 1;
    case 'l': case 'L': g_panel = PANEL_NONE; addr_focus(1);
                        g_addr_caret = (int)strlen(g_addr); return 1;
    case 't': case 'T': g_panel = PANEL_NONE; tab_new("uno:start"); return 1;
    case 'd': case 'D': bm_toggle(); return 1;
    case 'b': case 'B': panel_open(PANEL_MARKS); return 1;
    case 'h': case 'H': panel_open(PANEL_HIST); return 1;
    case 'r': case 'R': load_loc(t, t->loc); return 1;
    case 's': case 'S': save_page(t); return 1;      /* download this page */
    default: break;
    }
    if (scan == 0x0E) { tab_close(g_cur); return 1; }               /* Ctrl-F4 */
    return 0;
}

static unoui_canvas g_browser = { br_draw, br_event, 0 };

unoui_canvas *pc64_browser_canvas(void) { return &g_browser; }

void pc64_browser_open(void)
{
    if (!g_nbm && !g_nhs) bm_load();          /* first open: the saved marks */
    refresh_files();
    if (!g_ntab) tab_new("uno:start");
    g_panel = PANEL_NONE;
    g_addr_focus = 0;
    sput(g_addr, LOCMAX, g_tab[g_cur].loc);
    g_addr_caret = (int)strlen(g_addr);
}

/* open a local document by path (subdirectories fine: "DOCS\\API.MD") - the
 * Help menu's entry point. It lands in the current tab like any other
 * navigation, so Back returns to whatever was on screen. */
void pc64_browser_open_path(const char *path)
{
    char loc[LOCMAX];
    char *p = loc, *end = loc + LOCMAX;
    if (!g_nbm && !g_nhs) bm_load();
    if (!g_ntab) tab_new("uno:start");
    p = sapp(p, end, "path:");
    sapp(p, end, path);
    navigate(loc, 1);
}
