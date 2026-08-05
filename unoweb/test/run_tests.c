/* ===========================================================================
 * unoweb host tests. Plain gcc, no OS, and - the point of the exercise - NO
 * JavaScript engine linked. If this ever needs unojs, the split has broken.
 *
 *   make && ./run_tests          run everything
 *   ./run_tests <substring>      run only matching cases
 * ======================================================================== */
#include "../unoweb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0, run = 0;
static const char *g_filter;

static int want(const char *name)
{ return !g_filter || strstr(name, g_filter) != NULL; }

static void show_diff(const char *want_s, const char *got)
{
    const char *a = want_s, *b = got;
    int line = 1;
    while (*a && *b) {
        const char *ae = strchr(a, '\n'), *be = strchr(b, '\n');
        int al = ae ? (int)(ae - a) : (int)strlen(a);
        int bl = be ? (int)(be - b) : (int)strlen(b);
        if (al != bl || memcmp(a, b, (size_t)al)) {
            printf("      line %d\n        want: |%.*s|\n        got:  |%.*s|\n",
                   line, al, a, bl, b);
            return;
        }
        if (!ae || !be) break;
        a = ae + 1; b = be + 1; line++;
    }
    printf("      want:\n%s      got:\n%s", want_s, got);
}

/* ---- parse-to-dump cases ------------------------------------------------- */
static void tdump(const char *name, const char *html, const char *expect)
{
    char buf[8192];
    uw_doc *d;
    int n;
    if (!want(name)) return;
    run++;
    d = uw_parse_string(html, -1, NULL);
    if (!d) { printf("  FAIL %-22s (no doc)\n", name); fails++; return; }
    n = uw_dump(d, NULL, buf, sizeof buf);
    if (n >= (int)sizeof buf) { printf("  FAIL %-22s (dump overflow: %d)\n", name, n); fails++; }
    else if (strcmp(buf, expect)) { printf("  FAIL %-22s\n", name); show_diff(expect, buf); fails++; }
    uw_doc_free(d);
}

/* ---- layout goldens ------------------------------------------------------ */
static int fake_width(void *u, const uw_style *s, const char *t, int len)
{ (void)u; (void)s; (void)t; return len * 10; }
static int fake_lineh(void *u, const uw_style *s)
{ (void)u; return s->font_size; }
static int fake_image(void *u, const char *src, int *w, int *h, void **handle)
{ (void)u; (void)src; *w = 50; *h = 40; *handle = (void *)0x1234; return 1; }
static int big_image(void *u, const char *src, int *w, int *h, void **handle)
{ (void)u; (void)src; *w = 512; *h = 430; *handle = (void *)0x1; return 1; }


static void tlayout_w(const char *name, const char *html, int vw,
                      const char *want_boxes, const char *want_paint)
{
    char buf[8192];
    uw_doc *d;
    uw_metrics m;
    if (!want(name)) return;
    run++;
    d = uw_parse_string(html, -1, NULL);
    if (!d) { printf("  FAIL %-22s (no doc)\n", name); fails++; return; }
    uw_add_inline_sheets(d);
    uw_style_document(d, vw, 600);
    memset(&m, 0, sizeof m);
    m.text_width = fake_width;
    m.line_height = fake_lineh;
    uw_layout(d, vw, 600, &m);
    if (want_boxes) {
        uw_layout_dump(d, buf, sizeof buf);
        if (strcmp(buf, want_boxes)) {
            printf("  FAIL %-22s (boxes)\n", name); show_diff(want_boxes, buf); fails++; }
    }
    if (want_paint) {
        uw_paint(d);
        uw_paint_dump(d, buf, sizeof buf);
        if (strcmp(buf, want_paint)) {
            printf("  FAIL %-22s (paint)\n", name); show_diff(want_paint, buf); fails++; }
    }
    uw_doc_free(d);
}

static void tlayout(const char *name, const char *html,
                    const char *want_boxes, const char *want_paint)
{ tlayout_w(name, html, 800, want_boxes, want_paint); }


/* ---- computed-style goldens ---------------------------------------------- */
static void tstyle(const char *name, const char *html, const char *expect)
{
    char buf[8192];
    uw_doc *d;
    if (!want(name)) return;
    run++;
    d = uw_parse_string(html, -1, NULL);
    if (!d) { printf("  FAIL %-22s (no doc)\n", name); fails++; return; }
    uw_add_inline_sheets(d);
    uw_style_document(d, 800, 600);
    {   uw_node *c;
        int off = 0;
        buf[0] = 0;
        for (c = uw_first_child(uw_body(d)); c; c = uw_next_sibling(c)) {
            int k = uw_style_dump(d, c, buf + off, (int)sizeof buf - off);
            off += k;
            if (off >= (int)sizeof buf) break;
        } }
    if (strcmp(buf, expect)) { printf("  FAIL %-22s\n", name); show_diff(expect, buf); fails++; }
    uw_doc_free(d);
}


/* ---- serialize round-trip ------------------------------------------------ */
static void tserial(const char *name, const char *html, const char *expect)
{
    char buf[4096];
    uw_doc *d;
    if (!want(name)) return;
    run++;
    d = uw_parse_string(html, -1, NULL);
    if (!d) { printf("  FAIL %-22s (no doc)\n", name); fails++; return; }
    uw_serialize(d, uw_body(d), buf, sizeof buf);
    if (strcmp(buf, expect)) {
        printf("  FAIL %-22s\n      want: |%s|\n      got:  |%s|\n", name, expect, buf);
        fails++;
    }
    uw_doc_free(d);
}

static void ttext(const char *name, const char *html, const char *expect)
{
    char buf[4096];
    uw_doc *d;
    if (!want(name)) return;
    run++;
    d = uw_parse_string(html, -1, NULL);
    if (!d) { printf("  FAIL %-22s (no doc)\n", name); fails++; return; }
    uw_text_content(d, uw_body(d), buf, sizeof buf);
    if (strcmp(buf, expect)) {
        printf("  FAIL %-22s\n      want: |%s|\n      got:  |%s|\n", name, expect, buf);
        fails++;
    }
    uw_doc_free(d);
}

#define CHECK(name, cond) do { if (want(name)) { run++; \
    if (!(cond)) { printf("  FAIL %s\n", name); fails++; } } } while (0)

/* ---- script hook: the ONLY channel to an embedder ------------------------ */
static int g_nscripts;
static char g_lastscript[512];
static uw_parser *g_writeback;
static const char *g_writetext;

static void on_script(void *user, uw_parser *p, uw_node *el,
                      const char *src, int len)
{
    (void)user; (void)el;
    g_nscripts++;
    if (len > (int)sizeof g_lastscript - 1) len = (int)sizeof g_lastscript - 1;
    memcpy(g_lastscript, src, (size_t)len);
    g_lastscript[len] = 0;
    if (g_writeback && g_writetext) {         /* this is document.write */
        uw_parse_insert(p, g_writetext, -1);
        g_writetext = NULL;
    }
}

int main(int argc, char **argv)
{
    g_filter = argc > 1 ? argv[1] : NULL;

    /* ---- implied structure ------------------------------------------- */
    tdump("implied-structure", "<p>hi",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      p\n"
          "        \"hi\"\n");

    tdump("full-skeleton", "<!DOCTYPE html><html><head><title>T</title></head>"
                           "<body><h1>H</h1></body></html>",
          "#document\n"
          "  <!DOCTYPE html>\n"
          "  html\n"
          "    head\n"
          "      title\n"
          "        \"T\"\n"
          "    body\n"
          "      h1\n"
          "        \"H\"\n");

    tdump("head-then-body", "<meta charset=utf-8><title>x</title><p>body text",
          "#document\n"
          "  html\n"
          "    head\n"
          "      meta charset=\"utf-8\"\n"
          "      title\n"
          "        \"x\"\n"
          "    body\n"
          "      p\n"
          "        \"body text\"\n");

    /* ---- attributes --------------------------------------------------- */
    tdump("attrs-quoting", "<div id=a class=\"b c\" data-x='y' hidden>t</div>",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      div id=\"a\" class=\"b c\" data-x=\"y\" hidden=\"\"\n"
          "        \"t\"\n");

    tdump("attrs-case", "<DIV CLASS=X>t</DIV>",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      div class=\"X\"\n"
          "        \"t\"\n");

    tdump("attrs-dup-first-wins", "<p a=1 a=2>x",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      p a=\"1\"\n"
          "        \"x\"\n");

    /* ---- error recovery ----------------------------------------------- */
    tdump("unclosed-p", "<p>one<p>two",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      p\n"
          "        \"one\"\n"
          "      p\n"
          "        \"two\"\n");

    tdump("unclosed-li", "<ul><li>a<li>b</ul>",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      ul\n"
          "        li\n"
          "          \"a\"\n"
          "        li\n"
          "          \"b\"\n");

    tdump("stray-end-tag", "<p>a</div>b",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      p\n"
          "        \"ab\"\n");

    tdump("void-elements", "<p>a<br>b<img src=x>c",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      p\n"
          "        \"a\"\n"
          "        br\n"
          "        \"b\"\n"
          "        img src=\"x\"\n"
          "        \"c\"\n");

    tdump("nested-unclosed", "<div><span>x<div>y",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      div\n"
          "        span\n"
          "          \"x\"\n"
          "          div\n"
          "            \"y\"\n");

    /* ---- comments, doctype, bogus markup ------------------------------ */
    tdump("comment", "<p>a<!-- note -->b",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      p\n"
          "        \"a\"\n"
          "        <!--  note  -->\n"
          "        \"b\"\n");

    tdump("lone-lt", "<p>a < b",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      p\n"
          "        \"a < b\"\n");

    /* ---- entities ------------------------------------------------------ */
    ttext("entities-named", "<p>a &amp; b &lt;tag&gt; &quot;q&quot;", "a & b <tag> \"q\"");
    ttext("entities-numeric", "<p>&#65;&#x42;&#67;", "ABC");
    ttext("entities-unknown", "<p>&nosuchentity; ok", "&nosuchentity; ok");
    ttext("entities-utf8", "<p>caf&eacute; 50&deg;", "caf\xC3\xA9 50\xC2\xB0");
    ttext("entities-in-attr-not-text", "<p title='a&amp;b'>x", "x");

    /* ---- raw text ------------------------------------------------------ */
    tdump("style-rawtext", "<style>p { color: red } /* <b> */</style><p>x",
          "#document\n"
          "  html\n"
          "    head\n"
          "      style\n"
          "        \"p { color: red } /* <b> */\"\n"
          "    body\n"
          "      p\n"
          "        \"x\"\n");

    tdump("title-rcdata", "<title>a &amp; b</title>",
          "#document\n"
          "  html\n"
          "    head\n"
          "      title\n"
          "        \"a & b\"\n"
          "    body\n");

    /* ---- tables -------------------------------------------------------- */
    tdump("table-rows", "<table><tr><td>a<td>b<tr><td>c</table>",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      table\n"
          "        tr\n"
          "          td\n"
          "            \"a\"\n"
          "          td\n"
          "            \"b\"\n"
          "        tr\n"
          "          td\n"
          "            \"c\"\n");

    tdump("table-foster-text", "<table>stray<tr><td>in</table>",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      \"stray\"\n"
          "      table\n"
          "        tr\n"
          "          td\n"
          "            \"in\"\n");

    /* ---- serialization round-trip -------------------------------------- */
    tserial("serialize-basic", "<p class=x>a<b>bold</b></p>",
            "<p class=\"x\">a<b>bold</b></p>");
    tserial("serialize-void", "<p>a<br>b</p>", "<p>a<br>b</p>");
    tserial("serialize-escape", "<p>a &amp; b &lt; c</p>", "<p>a &amp; b &lt; c</p>");

    /* ---- streaming: identical result however the bytes are split ------- */
    if (want("streaming-split")) {
        const char *html = "<div id=q class=r><p>hello <b>world</b></p><!--c--></div>";
        char whole[4096], piece[4096];
        uw_doc *a = uw_parse_string(html, -1, NULL);
        int chunk, ok = 1;
        run++;
        uw_dump(a, NULL, whole, sizeof whole);
        for (chunk = 1; chunk <= 7 && ok; chunk++) {
            uw_doc *b = uw_doc_new(NULL);
            uw_parser *p = uw_parse_begin(b, NULL);
            int i, len = (int)strlen(html);
            for (i = 0; i < len; i += chunk)
                uw_parse_feed(p, html + i, (i + chunk <= len) ? chunk : len - i);
            uw_parse_end(p);
            uw_dump(b, NULL, piece, sizeof piece);
            if (strcmp(whole, piece)) {
                printf("  FAIL streaming-split (chunk=%d)\n", chunk);
                show_diff(whole, piece);
                fails++; ok = 0;
            }
            uw_doc_free(b);
        }
        uw_doc_free(a);
    }

    /* ---- the script hook ----------------------------------------------- */
    if (want("script-hook")) {
        uw_hooks h;
        uw_doc *d = uw_doc_new(NULL);
        uw_parser *p;
        run++;
        memset(&h, 0, sizeof h);
        h.script = on_script;
        g_nscripts = 0; g_lastscript[0] = 0; g_writeback = NULL;
        p = uw_parse_begin(d, &h);
        uw_parse_feed(p, "<body><script>var x = 1 < 2;</script><p>after", -1);
        uw_parse_end(p);
        if (g_nscripts != 1 || strcmp(g_lastscript, "var x = 1 < 2;")) {
            printf("  FAIL script-hook n=%d text=|%s|\n", g_nscripts, g_lastscript);
            fails++;
        }
        uw_doc_free(d);
    }

    /* NoScript build: with no hook installed the page still parses, and the
     * script's text simply is not executed. This is the M2 rendering path. */
    tdump("script-noscript-build", "<body><script>ignored()</script><p>x",
          "#document\n"
          "  html\n"
          "    head\n"
          "    body\n"
          "      script\n"
          "        \"ignored()\"\n"
          "      p\n"
          "        \"x\"\n");

    /* document.write: markup spliced at the insertion point must be parsed
     * BEFORE what follows the script. */
    if (want("script-document-write")) {
        uw_hooks h;
        uw_doc *d = uw_doc_new(NULL);
        uw_parser *p;
        char buf[2048];
        const char *expect =
            "#document\n"
            "  html\n"
            "    head\n"
            "    body\n"
            "      script\n"
            "        \"w()\"\n"
            "      h2\n"
            "        \"gen\"\n"
            "      p\n"
            "        \"after\"\n";
        run++;
        memset(&h, 0, sizeof h);
        h.script = on_script;
        g_nscripts = 0;
        g_writeback = (uw_parser *)1;
        g_writetext = "<h2>gen</h2>";
        p = uw_parse_begin(d, &h);
        g_writeback = p;
        uw_parse_feed(p, "<body><script>w()</script><p>after", -1);
        uw_parse_end(p);
        uw_dump(d, NULL, buf, sizeof buf);
        if (strcmp(buf, expect)) { printf("  FAIL script-document-write\n");
                                   show_diff(expect, buf); fails++; }
        uw_doc_free(d);
        g_writeback = NULL;
    }

    /* ---- the DOM API --------------------------------------------------- */
    if (want("dom-mutation")) {
        uw_doc *d = uw_parse_string("<div id=host><p>one</p></div>", -1, NULL);
        uw_node *host = uw_get_element_by_id(d, "host");
        char buf[512];
        run++;
        CHECK("dom-mutation/find", host != NULL);
        if (host) {
            uw_node *e = uw_create_element(d, "span");
            uw_node *t = uw_create_text(d, "two", -1);
            uw_append(d, e, t);
            uw_append(d, host, e);
            uw_set_attr(d, e, "class", "added");
            uw_serialize(d, host, buf, sizeof buf);
            if (strcmp(buf, "<p>one</p><span class=\"added\">two</span>")) {
                printf("  FAIL dom-mutation/serialize |%s|\n", buf); fails++;
            }
            uw_remove(d, uw_first_child(host));
            uw_serialize(d, host, buf, sizeof buf);
            if (strcmp(buf, "<span class=\"added\">two</span>")) {
                printf("  FAIL dom-mutation/remove |%s|\n", buf); fails++;
            }
        }
        uw_doc_free(d);
    }

    if (want("dom-fragment")) {
        uw_doc *d = uw_parse_string("<div id=h>old</div>", -1, NULL);
        uw_node *h = uw_get_element_by_id(d, "h");
        char buf[512];
        run++;
        uw_parse_fragment(d, h, "<b>new</b> text", -1);
        uw_serialize(d, h, buf, sizeof buf);
        if (strcmp(buf, "<b>new</b> text")) {
            printf("  FAIL dom-fragment |%s|\n", buf); fails++;
        }
        uw_doc_free(d);
    }

    if (want("dom-query")) {
        uw_doc *d = uw_parse_string("<p>a</p><div><p>b</p><p>c</p></div>", -1, NULL);
        uw_node *out[8];
        int n;
        run++;
        n = uw_elements_by_tag(d, NULL, "p", out, 8);
        if (n != 3) { printf("  FAIL dom-query/tag n=%d\n", n); fails++; }
        n = uw_elements_by_tag(d, NULL, "*", NULL, 0);
        if (n < 5) { printf("  FAIL dom-query/all n=%d\n", n); fails++; }
        uw_doc_free(d);
    }

    if (want("dom-detached-id")) {
        /* the id index is a cache; a detached node must not be findable */
        uw_doc *d = uw_parse_string("<div id=gone>x</div>", -1, NULL);
        uw_node *n = uw_get_element_by_id(d, "gone");
        run++;
        if (!n) { printf("  FAIL dom-detached-id/before\n"); fails++; }
        else {
            uw_remove(d, n);
            if (uw_get_element_by_id(d, "gone")) {
                printf("  FAIL dom-detached-id/after\n"); fails++;
            }
        }
        uw_doc_free(d);
    }

    /* ---- CSS: the cascade ---------------------------------------------- */
    tstyle("css-ua-defaults", "<p>x</p>",
           "p display=block font=14/400 color=#1e2028 margin=9,0,9,0\n");

    tstyle("css-author-overrides",
           "<style>p{color:red;font-size:20px}</style><p>x</p>",
           "p display=block font=20/400 color=#ff0000 margin=9,0,9,0\n");

    /* specificity: #id beats .class beats tag, regardless of source order */
    tstyle("css-specificity",
           "<style>p{color:red} .c{color:green} #i{color:blue}</style>"
           "<p id=i class=c>x</p>",
           "p display=block font=14/400 color=#0000ff margin=9,0,9,0\n");

    /* equal specificity: the later rule wins */
    tstyle("css-source-order",
           "<style>.a{color:red} .b{color:lime}</style><p class='a b'>x</p>",
           "p display=block font=14/400 color=#00ff00 margin=9,0,9,0\n");

    /* !important outranks a more specific normal declaration */
    tstyle("css-important",
           "<style>#i{color:red} p{color:lime !important}</style><p id=i>x</p>",
           "p display=block font=14/400 color=#00ff00 margin=9,0,9,0\n");

    /* the style attribute outranks author rules */
    tstyle("css-inline-attr",
           "<style>p{color:red}</style><p style='color:blue'>x</p>",
           "p display=block font=14/400 color=#0000ff margin=9,0,9,0\n");

    /* inheritance: color and font descend, margin does not */
    tstyle("css-inheritance",
           "<style>div{color:teal;font-size:18px;margin:5px}</style><div><span>x</span></div>",
           "div display=block font=18/400 color=#008080 margin=5,5,5,5\n"
           "  span display=inline font=18/400 color=#008080\n");

    tstyle("css-combinators",
           "<style>div>p{color:red} div p{font-weight:700} b+i{color:lime}</style>"
           "<div><p>a</p></div><b>x</b><i>y</i>",
           "div display=block font=14/400 color=#1e2028\n"
           "  p display=block font=14/700 color=#ff0000 margin=9,0,9,0\n"
           "b display=inline font=14/700 color=#1e2028\n"
           "i display=inline font=14/400i color=#00ff00\n");

    tstyle("css-shorthands",
           "<style>p{margin:1px 2px 3px 4px;padding:5px 6px;border:2px solid #abc}</style><p>x</p>",
           "p display=block font=14/400 color=#1e2028 margin=1,2,3,4 padding=5,6,5,6"
           " border0=2px#aabbcc border1=2px#aabbcc border2=2px#aabbcc border3=2px#aabbcc\n");

    tstyle("css-units",
           "<style>p{font-size:20px} p span{font-size:1.5em;margin-left:2em}"
           "div{width:50%;height:30px}</style><p><span>x</span></p><div></div>",
           "p display=block font=20/400 color=#1e2028 margin=9,0,9,0\n"
           "  span display=inline font=30/400 color=#1e2028 margin=0,0,0,60\n"
           "div display=block font=14/400 color=#1e2028 width=50% height=30\n");

    tstyle("css-colors",
           "<style>.a{color:#f00}.b{color:#ff0000}.c{color:rgb(0,128,0)}"
           ".d{background:yellow}</style>"
           "<i class=a>1</i><i class=b>2</i><i class=c>3</i><i class=d>4</i>",
           "i display=inline font=14/400i color=#ff0000\n"
           "i display=inline font=14/400i color=#ff0000\n"
           "i display=inline font=14/400i color=#008000\n"
           "i display=inline font=14/400i color=#1e2028 bg=#ffff00\n");

    /* display:none on head content is what keeps <style> text off the page */
    tstyle("css-display-none",
           "<style>.hide{display:none}</style><p class=hide>x</p><p>y</p>",
           "p display=none font=14/400 color=#1e2028 margin=9,0,9,0\n"
           "p display=block font=14/400 color=#1e2028 margin=9,0,9,0\n");

    /* pseudo-classes that need an interaction model must NOT match yet -
     * styling :hover now would paint the page as if the pointer were
     * everywhere at once */
    tstyle("css-pseudo",
           "<style>li:first-child{color:red} a:hover{color:lime}</style>"
           "<ul><li>a</li><li>b</li></ul>",
           "ul display=block font=14/400 color=#1e2028 margin=9,0,9,0 padding=0,0,0,24\n"
           "  li display=list-item font=14/400 color=#ff0000 bullet=1\n"
           "  li display=list-item font=14/400 color=#1e2028 bullet=1\n");

    /* a malformed rule must not eat the ones after it */
    tstyle("css-error-recovery",
           "<style>p{color:@@@;;;} p{font-weight:700} @unknown{x:y} p{color:lime}</style><p>x</p>",
           "p display=block font=14/700 color=#00ff00 margin=9,0,9,0\n");

    if (want("css-matches")) {
        uw_doc *d = uw_parse_string("<div class='a b' id=q><p>x</p></div>", -1, NULL);
        uw_node *div = uw_get_element_by_id(d, "q");
        run++;
        if (!uw_matches(d, div, "div")     ||
            !uw_matches(d, div, ".a")      ||
            !uw_matches(d, div, "#q")      ||
            !uw_matches(d, div, "div.a.b") ||
            !uw_matches(d, div, "[class]") ||
             uw_matches(d, div, "span")    ||
             uw_matches(d, div, ".c")) {
            printf("  FAIL css-matches\n"); fails++;
        }
        uw_doc_free(d);
    }


    /* ---- layout + display list -------------------------------------------
     * Metrics come from a FAKE FIXED-WIDTH font: 10px per char, line height =
     * font-size. unoweb measures nothing itself, so supplying a deterministic
     * font is what makes this geometry exact and reproducible off the OS - the
     * same seam pc64 fills with its real font. Every number below was checked
     * by hand against the box model, not blessed from the output. */

    /* body: 8px margin all round in the UA sheet, so its border box is at
     * (8,8) and 800-16 wide. p adds 9px top margin -> y=17. "ab"+space+"cd"
     * = 20+10+20 = 50. */
    tlayout("layout-single-block", "<p>ab cd</p>",
            "block body (8,8 784x32)\n"
            "  block p (8,17 784x14)\n"
            "    line (8,17 50x14)\n"
            "      text (8,17 20x14) \"ab\"\n"
            "      text (38,17 20x14) \"cd\"\n",
            "text (8,17 20x14) #1e2028 14/400 \"ab\"\n"
            "text (38,17 20x14) #1e2028 14/400 \"cd\"\n");

    tlayout("layout-text-lines", "<div>aaa bbb</div>",
            "block body (8,8 784x14)\n"
            "  block div (8,8 784x14)\n"
            "    line (8,8 70x14)\n"
            "      text (8,8 30x14) \"aaa\"\n"
            "      text (48,8 30x14) \"bbb\"\n",
            "text (8,8 30x14) #1e2028 14/400 \"aaa\"\n"
            "text (48,8 30x14) #1e2028 14/400 \"bbb\"\n");

    /* Wrapping at exactly the boundary: "aaa bbb" is 30+10+30 = 70 and fits a
     * 70px box; adding " ccc" would need 110, so it breaks. The first line box
     * must be 70 wide, NOT 80 - a trailing space at a line end is not drawn. */
    tlayout_w("layout-wrap", "<div style='width:70px'>aaa bbb ccc</div>", 200,
            "block body (8,8 184x28)\n"
            "  block div (8,8 70x28)\n"
            "    line (8,8 70x14)\n"
            "      text (8,8 30x14) \"aaa\"\n"
            "      text (48,8 30x14) \"bbb\"\n"
            "    line (8,22 30x14)\n"
            "      text (8,22 30x14) \"ccc\"\n",
            NULL);

    /* Margin collapsing: two <p>s each have 9px top AND bottom margins. The
     * gap between them must be 9 (40 - 31), not 18. */
    tlayout("layout-margin-collapse", "<p>a</p><p>b</p>",
            "block body (8,8 784x55)\n"
            "  block p (8,17 784x14)\n"
            "    line (8,17 10x14)\n"
            "      text (8,17 10x14) \"a\"\n"
            "  block p (8,40 784x14)\n"
            "    line (8,40 10x14)\n"
            "      text (8,40 10x14) \"b\"\n",
            NULL);

    /* Border box 784 wide; content inset by border(2)+padding(10) to (20,20);
     * height 14 + 2 + 2 + 10 + 10 = 38. Borders paint in CSS side order. */
    tlayout("layout-padding-border",
            "<div style='padding:10px;border:2px solid #f00;background:#eee'>x</div>",
            "block body (8,8 784x38)\n"
            "  block div (8,8 784x38)\n"
            "    line (20,20 10x14)\n"
            "      text (20,20 10x14) \"x\"\n",
            "rect (8,8 784x38) #eeeeee\n"
            "border (8,8 784x2) #ff0000\n"
            "border (790,8 2x38) #ff0000\n"
            "border (8,44 784x2) #ff0000\n"
            "border (8,8 2x38) #ff0000\n"
            "text (20,20 10x14) #1e2028 14/400 \"x\"\n");

    /* A percentage width resolves against the PARENT's content width (200), so
     * 50% is 100 - not 50% of the viewport. */
    tlayout("layout-nested-width",
            "<div style='width:200px'><div style='width:50%'>x</div></div>",
            "block body (8,8 784x14)\n"
            "  block div (8,8 200x14)\n"
            "    block div (8,8 100x14)\n"
            "      line (8,8 10x14)\n"
            "        text (8,8 10x14) \"x\"\n",
            NULL);

    /* ---- line alignment (M4: text-align + vertical-align) -------------------
     * Each of these is a LINE-CLOSE decision: while words are being placed
     * neither the line's final width nor its tallest item is known, so the
     * emitter stacks at the pen and closing the line puts things right. */
    tlayout_w("layout-align-center", "<style>p{text-align:center}</style><p>ab cd</p>", 200,
            "block body (8,8 184x32)\n"
            "  block p (8,17 184x14)\n"
            "    line (8,17 50x14)\n"
            "      text (75,17 20x14) \"ab\"\n"
            "      text (105,17 20x14) \"cd\"\n",
            NULL);
    tlayout_w("layout-align-right", "<style>p{text-align:right}</style><p>ab cd</p>", 200,
            "block body (8,8 184x32)\n"
            "  block p (8,17 184x14)\n"
            "    line (8,17 50x14)\n"
            "      text (142,17 20x14) \"ab\"\n"
            "      text (172,17 20x14) \"cd\"\n",
            NULL);
    /* justify: the WRAPPED line is stretched so its right edge lands exactly
     * on the content edge (172+20 = 8+184); the LAST line stays ragged. */
    tlayout_w("layout-align-justify",
            "<style>p{text-align:justify}</style><p>aa bb cc dd ee ff gg hh ii jj</p>", 200,
            "block body (8,8 184x46)\n"
            "  block p (8,17 184x28)\n"
            "    line (8,17 184x14)\n"
            "      text (8,17 20x14) \"aa\"\n"
            "      text (41,17 20x14) \"bb\"\n"
            "      text (74,17 20x14) \"cc\"\n"
            "      text (107,17 20x14) \"dd\"\n"
            "      text (140,17 20x14) \"ee\"\n"
            "      text (172,17 20x14) \"ff\"\n"
            "    line (8,31 110x14)\n"
            "      text (8,31 20x14) \"gg\"\n"
            "      text (38,31 20x14) \"hh\"\n"
            "      text (68,31 20x14) \"ii\"\n"
            "      text (98,31 20x14) \"jj\"\n",
            NULL);

    /* ---- floats (M6) --------------------------------------------------------
     * The float is out of flow and the PARAGRAPH is a separate block, so this
     * only works because the float context is passed down the block tree
     * rather than rebuilt per block - a float in <body> has to shorten the
     * lines of every paragraph after it. Lines start past the float and wrap
     * at the narrowed width; the body grows to contain the float. */
    tlayout_w("layout-float-left",
            "<style>.f{float:left;width:100px;height:40px}</style>"
            "<div class=f></div><p>aa bb cc dd ee ff gg hh</p>", 300,
            "block body (8,8 284x46)\n"
            "  block div (8,8 100x40)\n"
            "  block p (8,17 284x28)\n"
            "    line (108,17 170x14)\n"
            "      text (108,17 20x14) \"aa\"\n"
            "      text (138,17 20x14) \"bb\"\n"
            "      text (168,17 20x14) \"cc\"\n"
            "      text (198,17 20x14) \"dd\"\n"
            "      text (228,17 20x14) \"ee\"\n"
            "      text (258,17 20x14) \"ff\"\n"
            "    line (108,31 50x14)\n"
            "      text (108,31 20x14) \"gg\"\n"
            "      text (138,31 20x14) \"hh\"\n",
            NULL);
    /* a right float pins to the right content edge and leaves the left free */
    tlayout_w("layout-float-right",
            "<style>.f{float:right;width:100px;height:40px}</style>"
            "<div class=f></div><p>aa bb</p>", 300,
            "block body (8,8 284x40)\n"
            "  block div (192,8 100x40)\n"
            "  block p (8,17 284x14)\n"
            "    line (8,17 50x14)\n"
            "      text (8,17 20x14) \"aa\"\n"
            "      text (38,17 20x14) \"bb\"\n",
            NULL);
    /* clear pushes a later block below the float instead of beside it */
    tlayout_w("layout-clear",
            "<style>.f{float:left;width:100px;height:20px}.c{clear:left}</style>"
            "<div class=f></div><p class=c>below</p>", 300,
            "block body (8,8 284x43)\n"
            "  block div (8,8 100x20)\n"
            "  block p (8,28 284x14)\n"
            "    line (8,28 50x14)\n"
            "      text (8,28 50x14) \"below\"\n",
            NULL);

    /* ---- tables (M6) --------------------------------------------------------
     * Columns are proportional to how much text each holds, which is what
     * makes the common shape - a narrow index column beside a wide
     * description - readable; equal columns would put 192px under "1".
     * Cells stretch to the row height, so a row reads as a grid rather than
     * as independently-sized boxes. */
    tlayout_w("layout-table",
            "<table><tr><td>1</td><td>description here</td></tr>"
            "<tr><td>22</td><td>x</td></tr></table>", 400,
            "block body (8,8 384x54)\n"
            "  block table (8,14 384x42)\n"
            "    block tr (8,16 384x18)\n"
            "      block td (10,16 42x18)\n"
            "        line (14,18 10x14)\n"
            "          text (14,18 10x14) \"1\"\n"
            "      block td (54,16 336x18)\n"
            "        line (58,18 160x14)\n"
            "          text (58,18 110x14) \"description\"\n"
            "          text (178,18 40x14) \"here\"\n"
            "    block tr (8,36 384x18)\n"
            "      block td (10,36 42x18)\n"
            "        line (14,38 20x14)\n"
            "          text (14,38 20x14) \"22\"\n"
            "      block td (54,36 336x18)\n"
            "        line (58,38 10x14)\n"
            "          text (58,38 10x14) \"x\"\n",
            NULL);

    /* ---- images + hit testing --------------------------------------------- */
    if (want("layout-image")) {
        char buf[4096];
        uw_doc *d = uw_parse_string("<p>a<img src=x.png>b</p>", -1, NULL);
        uw_metrics m;
        uw_images im;
        /* The image is 40 tall and sits ON the baseline (what CSS means by an
         * inline replaced box defaulting to vertical-align: baseline); the
         * text shares that baseline, so its top is at 46 rather than at the
         * line top. The line is ascent(40) + descent(3) = 43, NOT max-height
         * 40 - sizing by height alone would let the text's descender hang
         * into the next line. Before vertical-align landed everything was
         * top-aligned and this read 17/40. */
        const char *expect =
            "block body (8,8 784x61)\n"
            "  block p (8,17 784x43)\n"
            "    line (8,17 70x43)\n"
            "      text (8,46 10x14) \"a\"\n"
            "      image img (18,17 50x40)\n"
            "      text (68,46 10x14) \"b\"\n";
        run++;
        uw_style_document(d, 800, 600);
        memset(&m, 0, sizeof m); m.text_width = fake_width; m.line_height = fake_lineh;
        memset(&im, 0, sizeof im); im.resolve = fake_image;
        uw_set_images(d, &im);
        uw_layout(d, 800, 600, &m);
        uw_layout_dump(d, buf, sizeof buf);
        if (strcmp(buf, expect)) { printf("  FAIL layout-image\n"); show_diff(expect, buf); fails++; }
        uw_doc_free(d);
    }

    /* No resolve hook: the image occupies nothing and the text closes up.
     * A broken or still-loading image must not reserve phantom space. */
    tlayout("layout-image-unresolved", "<p>a<img src=x.png>b</p>",
            "block body (8,8 784x32)\n"
            "  block p (8,17 784x14)\n"
            "    line (8,17 20x14)\n"
            "      text (8,17 10x14) \"a\"\n"
            "      text (18,17 10x14) \"b\"\n",
            NULL);

    if (want("hit-test")) {
        uw_doc *d = uw_parse_string(
            "<p>word <a href='/go'>link</a> tail</p><p id=second>below</p>", -1, NULL);
        uw_metrics m;
        uw_node *n, *a;
        run++;
        uw_style_document(d, 800, 600);
        memset(&m, 0, sizeof m); m.text_width = fake_width; m.line_height = fake_lineh;
        uw_layout(d, 800, 600, &m);
        /* "word " = 50px from x=8, so the link occupies x 58..98 on line y=17 */
        n = uw_hit_test(d, 60, 20);
        a = uw_link_at(d, n);
        if (!a || strcmp(uw_attr(d, a, "href"), "/go")) {
            printf("  FAIL hit-test/link (%s)\n", a ? "wrong href" : "no link"); fails++;
        }
        /* a point on the plain text before it is NOT inside the link */
        n = uw_hit_test(d, 12, 20);
        if (uw_link_at(d, n)) { printf("  FAIL hit-test/not-link\n"); fails++; }
        /* the second paragraph, well below */
        n = uw_hit_test(d, 12, 45);
        if (!n || !uw_has_attr(d, n, "id")) { printf("  FAIL hit-test/second\n"); fails++; }
        /* far outside the document */
        if (uw_hit_test(d, 5000, 5000)) { printf("  FAIL hit-test/outside\n"); fails++; }
        uw_doc_free(d);
    }


    /* An image wider than its column scales down proportionally rather than
     * overflowing. 512x430 into a 284px content box -> 284x238. Both the box
     * AND the paint command are checked: uw_paint_dump had no case for
     * UW_CMD_IMAGE, so every earlier golden was blind to images entirely and
     * would have passed with the paint side completely broken. */
    if (want("layout-image-scale")) {
        char buf[4096];
        uw_doc *d = uw_parse_string("<p>before <img src=x.png> after</p>", -1, NULL);
        uw_metrics m;
        uw_images im;
        const char *expect_paint =
            "text (8,17 60x14) #1e2028 14/400 \"before\"\n"
            "image (8,31 284x238)\n"
            "text (8,269 50x14) #1e2028 14/400 \"after\"\n";
        run++;
        uw_style_document(d, 300, 600);
        memset(&m, 0, sizeof m); m.text_width = fake_width; m.line_height = fake_lineh;
        memset(&im, 0, sizeof im); im.resolve = big_image;
        uw_set_images(d, &im);
        uw_layout(d, 300, 600, &m);
        uw_paint(d);
        uw_paint_dump(d, buf, sizeof buf);
        if (strcmp(buf, expect_paint)) {
            printf("  FAIL layout-image-scale\n"); show_diff(expect_paint, buf); fails++; }
        uw_doc_free(d);
    }


    /* ---- limits: hostile input must degrade, never run away ------------- */
    if (want("limit-depth")) {
        uw_config cfg;
        uw_doc *d;
        uw_parser *p;
        int i;
        run++;
        memset(&cfg, 0, sizeof cfg);
        cfg.max_depth = 32;
        d = uw_doc_new(&cfg);
        p = uw_parse_begin(d, NULL);
        for (i = 0; i < 5000; i++) uw_parse_feed(p, "<div>", 5);
        uw_parse_end(p);
        if (!uw_doc_truncated(d)) { printf("  FAIL limit-depth (not flagged)\n"); fails++; }
        uw_doc_free(d);
    }

    if (want("limit-arena")) {
        uw_config cfg;
        uw_doc *d;
        uw_parser *p;
        int i;
        run++;
        memset(&cfg, 0, sizeof cfg);
        cfg.arena_max = 256 * 1024;
        d = uw_doc_new(&cfg);
        p = uw_parse_begin(d, NULL);
        for (i = 0; i < 20000; i++) uw_parse_feed(p, "<p>some text here</p>", -1);
        uw_parse_end(p);
        if (!uw_doc_truncated(d)) { printf("  FAIL limit-arena (not flagged)\n"); fails++; }
        if (uw_doc_used(d) > cfg.arena_max) {
            printf("  FAIL limit-arena used=%lu\n", (unsigned long)uw_doc_used(d)); fails++;
        }
        uw_doc_free(d);
    }

    /* ---- malformed input must not crash -------------------------------- */
    if (want("fuzz-malformed")) {
        static const char *const bad[] = {
            "<", "<<<", "<p", "<p ", "<p a", "<p a=", "<p a=\"", "</", "</>",
            "<!", "<!-", "<!--", "<!-- x", "<!doctype", "<?", "<p a=\"unclosed",
            "<div><div><div>", "</p></div></html>", "&", "&#", "&#x", "&;",
            "<script>", "<style>", "<title>", "<table><td>", "<p>\xff\xfe",
            "<a href=\"\">>>", "<p/>", "<p//>", "< p>", "<3>", "\0", NULL
        };
        int i;
        run++;
        for (i = 0; bad[i]; i++) {
            uw_doc *d = uw_parse_string(bad[i], -1, NULL);
            if (d) { char b[1024]; uw_dump(d, NULL, b, sizeof b); uw_doc_free(d); }
        }
        /* reaching here without a crash IS the assertion */
    }

    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", run, fails);
    return fails ? 1 : 0;
}
