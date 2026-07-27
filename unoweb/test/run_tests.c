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
