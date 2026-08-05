/* webjs_test.c - the M5 host test: live DOM bindings, on BOTH engines.
 *
 * Parses a document with unoweb, opens a page VM, and runs scripts that read
 * and MUTATE the tree - then checks the tree itself, not the script's own
 * report. A binding that returns the right string while changing nothing is
 * exactly the failure this has to catch.
 *
 * Written to run per-engine so "both engines get the same DOM" can be held
 * honest; today only the unojs pass runs, because that is the only adapter
 * webjs_engine_current() will hand out (js.c explains why).
 */
#include <stdio.h>
#include <string.h>
#include "../../webjs.h"
#include "../../js.h"

/* uno_native_* stubs (qjs_port's clocks) */
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{ *y = 2026; *mo = 8; *d = 6; *h = 12; *mi = 0; *s = 0; return 0; }
unsigned long long uno_native_rdtsc(void) { static unsigned long long t; return t += 1000; }
unsigned long long uno_native_tsc_per_us(void) { return 0; }

static int g_pass, g_fail;
static const char *g_eng;

static void note(int ok, const char *name)
{
    printf("%s %-10s %s\n", ok ? "pass" : "FAIL", g_eng, name);
    if (ok) g_pass++; else g_fail++;
}

/* run `js` against `html`, then hand the document back for inspection */
static uw_doc *run(const char *html, const char *js, char *log, int logmax)
{
    uw_doc *d = uw_parse_string(html, -1, NULL);
    if (!d) return NULL;
    log[0] = 0;
    if (webjs_page_begin(d) != 0) { uw_doc_free(d); return NULL; }
    webjs_run(js, (int)strlen(js), log, logmax);
    return d;
}

static const char *text_of(uw_doc *d, const char *id)
{
    static char buf[512];
    uw_node *n = uw_get_element_by_id(d, id), *c;
    int at = 0;
    buf[0] = 0;
    if (!n) return buf;
    for (c = uw_first_child(n); c; c = uw_next_sibling(c)) {
        int tl = 0;
        const char *t = uw_type(c) == UW_NODE_TEXT ? uw_text(c, &tl) : NULL;
        if (t && tl > 0 && at + tl < (int)sizeof buf - 1)
        { memcpy(buf + at, t, (size_t)tl); at += tl; }
    }
    buf[at] = 0;
    return buf;
}

static void suite(int engine, const char *name)
{
    char log[1024];
    uw_doc *d;
    g_eng = name;
    js_engine_set(engine);

    /* the binding is even THERE */
    d = run("<p id=a>hi</p>", "console.log(typeof document.getElementById)", log, sizeof log);
    note(d && strstr(log, "function") != NULL, "document.getElementById exists");
    if (d) { webjs_page_end(); uw_doc_free(d); }

    /* read: textContent + getAttribute */
    d = run("<p id=a class=hot>hi there</p>",
            "console.log(document.getElementById('a').getText());"
            "console.log(document.getElementById('a').getAttribute('class'));",
            log, sizeof log);
    note(d && strstr(log, "hi there") && strstr(log, "hot"), "read text + attribute");
    if (d) { webjs_page_end(); uw_doc_free(d); }

    /* WRITE: textContent - checked on the TREE, not on the script's word */
    d = run("<p id=a>old</p>", "document.getElementById('a').setText('new');",
            log, sizeof log);
    note(d && !strcmp(text_of(d, "a"), "new"), "write textContent mutates the tree");
    if (d) { webjs_page_end(); uw_doc_free(d); }

    /* WRITE: setAttribute */
    d = run("<p id=a>x</p>", "document.getElementById('a').setAttribute('data-k','v');",
            log, sizeof log);
    note(d && uw_attr(d, uw_get_element_by_id(d, "a"), "data-k") &&
         !strcmp(uw_attr(d, uw_get_element_by_id(d, "a"), "data-k"), "v"),
         "setAttribute mutates the tree");
    if (d) { webjs_page_end(); uw_doc_free(d); }

    /* innerHTML: parses real markup into the tree */
    d = run("<div id=a></div>", "document.getElementById('a').setHtml('<b>bold</b>');",
            log, sizeof log);
    {   uw_node *n = d ? uw_get_element_by_id(d, "a") : NULL;
        uw_node *c = n ? uw_first_child(n) : NULL;
        note(c && uw_type(c) == UW_NODE_ELEMENT && !strcmp(uw_tag_name(d, c), "b"),
             "innerHTML parses into the tree"); }
    if (d) { webjs_page_end(); uw_doc_free(d); }

    /* createElement + appendChild */
    d = run("<div id=a></div>",
            "var e = document.createElement('span');"
            "e.setText('made');"
            "document.getElementById('a').appendChild(e);", log, sizeof log);
    {   uw_node *n = d ? uw_get_element_by_id(d, "a") : NULL;
        uw_node *c = n ? uw_first_child(n) : NULL;
        note(c && !strcmp(uw_tag_name(d, c), "span"), "createElement + appendChild"); }
    if (d) { webjs_page_end(); uw_doc_free(d); }

    /* querySelector / querySelectorAll over the real matcher */
    d = run("<ul><li class=x>1</li><li>2</li><li class=x>3</li></ul>",
            "console.log('n=' + document.querySelectorAll('.x').length);"
            "console.log('t=' + document.querySelector('.x').getText());",
            log, sizeof log);
    note(d && strstr(log, "n=2") && strstr(log, "t=1"), "querySelector + All");
    if (d) { webjs_page_end(); uw_doc_free(d); }

    /* the dirty flag: a mutation must be VISIBLE to the browser, or the
     * change is computed and never drawn */
    d = run("<p id=a>x</p>", "document.getElementById('a').setText('y');",
            log, sizeof log);
    note(d && webjs_take_dirty() == 1, "mutation raises the dirty flag");
    if (d) { webjs_page_end(); uw_doc_free(d); }

    d = run("<p id=a>x</p>", "var q = document.getElementById('a').getText();",
            log, sizeof log);
    note(d && webjs_take_dirty() == 0, "a pure read does NOT");
    if (d) { webjs_page_end(); uw_doc_free(d); }

    /* timers: nothing runs before it is due, and the pump runs it after */
    d = run("<p id=a>x</p>",
            "setTimeout(function(){ document.getElementById('a').setText('fired'); }, 50);",
            log, sizeof log);
    note(d && !strcmp(text_of(d, "a"), "x"), "timer has not fired yet");
    if (d) {
        webjs_pump(10, log, sizeof log);
        note(!strcmp(text_of(d, "a"), "x"), "pump before due: still not fired");
        webjs_pump(60, log, sizeof log);
        note(!strcmp(text_of(d, "a"), "fired"), "pump after due: fired");
        webjs_page_end(); uw_doc_free(d);
    }

    /* clearTimeout really cancels */
    d = run("<p id=a>x</p>",
            "var t = setTimeout(function(){ document.getElementById('a').setText('no'); }, 10);"
            "clearTimeout(t);", log, sizeof log);
    if (d) {
        webjs_pump(100, log, sizeof log);
        note(!strcmp(text_of(d, "a"), "x"), "clearTimeout cancels");
        webjs_page_end(); uw_doc_free(d);
    }

    /* events: a click handler, dispatched at the node */
    d = run("<div id=b><p id=a>x</p></div>",
            "document.getElementById('a').addEventListener('click',"
            " function(){ document.getElementById('a').setText('clicked'); });",
            log, sizeof log);
    if (d) {
        uw_node *target = uw_get_element_by_id(d, "a");
        note(webjs_event(target, "click", log, sizeof log) == 1, "click handler ran");
        note(!strcmp(text_of(d, "a"), "clicked"), "click handler mutated the tree");
        webjs_page_end(); uw_doc_free(d);
    }

    /* events BUBBLE: a handler on the ancestor sees a click on the child */
    d = run("<div id=b><p id=a>x</p></div>",
            "document.getElementById('b').addEventListener('click',"
            " function(){ document.getElementById('a').setText('bubbled'); });",
            log, sizeof log);
    if (d) {
        webjs_event(uw_get_element_by_id(d, "a"), "click", log, sizeof log);
        note(!strcmp(text_of(d, "a"), "bubbled"), "click bubbles to the ancestor");
        webjs_page_end(); uw_doc_free(d);
    }

    /* a script error must not take the page with it */
    d = run("<p id=a>x</p>", "this is not javascript at all(", log, sizeof log);
    note(d != NULL, "syntax error survives");
    if (d) { webjs_page_end(); uw_doc_free(d); }
}

int main(void)
{
    /* ONE engine, deliberately. webjs_engine_current() is pinned to unojs
     * while the quickjs DOM adapter's mingw startup crash is unresolved (the
     * note in js.c has the details), so a second pass here would run the
     * unojs binding a second time under the label "quickjs" and report a
     * passing suite for a path that is not selected - the worst kind of
     * green. Restore the second pass in the same commit that unpins it. */
    suite(JS_ENGINE_UNOJS, "unojs");
    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail != 0;
}
