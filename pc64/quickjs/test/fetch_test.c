/* fetch_test.c - does the subresource queue actually run things in PARALLEL?
 *
 * pc64_fetch is where a page's images and stylesheets are turned into GETs.
 * It used to fetch them one at a time because the transport could only hold
 * one connection; now it holds several, and the thing worth asserting is the
 * SCHEDULING: does it start up to the cap and no more, does a slow resource
 * overlap with its neighbours instead of delaying them, does a queued one
 * start the moment a slot frees, and is a failure still remembered.
 *
 * None of that needs a network. pc64_http is stubbed with a request that
 * "takes" a fixed number of polls, so the test can count rounds and compare
 * what happened against what serial would have cost - which is the only way
 * to tell parallel from merely fast.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pc64_fetch.h"
#include "pc64_http.h"

/* ---- the transport, stubbed ---------------------------------------------- */
struct http_req {
    int  left;                  /* polls remaining before it finishes */
    int  len;                   /* body length it will hand back      */
    char url[256];
};

static int g_live, g_peak, g_begun, g_polls;
static int g_cost = 10;         /* polls each request takes           */
static int g_fail_all;

http_req *pc64_http_begin(const char *url, const char *post)
{
    http_req *r;
    (void)post;
    r = (http_req *)calloc(1, sizeof *r);
    if (!r) return 0;
    r->left = g_cost;
    r->len  = g_fail_all ? -1 : (int)strlen(url);   /* body = the url, so a mix-up shows */
    strncpy(r->url, url, sizeof r->url - 1);
    g_begun++;
    if (++g_live > g_peak) g_peak = g_live;
    return r;
}

int pc64_http_poll(http_req *r)
{
    if (!r) return 1;
    g_polls++;
    if (r->left > 0) { r->left--; return 0; }
    return 1;
}

int pc64_http_len(http_req *r) { return r ? r->len : -2; }

int pc64_http_take(http_req *r, char *body, int bodymax, char *status, int statusmax)
{
    int n;
    if (statusmax > 0) status[0] = 0;
    if (!r || r->len < 0) return -1;
    n = r->len;
    if (n > bodymax - 1) n = bodymax - 1;
    memcpy(body, r->url, (size_t)n);
    body[n] = 0;
    return n;
}

void pc64_http_free(http_req *r) { if (r) { g_live--; free(r); } }
void pc64_http_req_progress(http_req *r, int on) { (void)r; (void)on; }

/* the kernel calls pc64_fetch makes */
void net_poll(void) { }
void uno_pc64_delay_ms(int ms) { (void)ms; }

/* ---- the gate ------------------------------------------------------------ */
static int fails;
static void ck(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails = 1;
}

static void zero(void)
{ pc64_fetch_reset(); g_live = g_peak = g_begun = g_polls = 0; g_fail_all = 0; g_cost = 10; }

int main(void)
{
    const unsigned char *p;
    int i, n;

    printf("pc64_fetch scheduling gate\n");

    /* 1. the cap. Six resources asked for; four in flight, two waiting - the
     *    socket table is shared with the URC link and discovery, so a browser
     *    that could open one per image would take the machine's remote
     *    channel down with it. */
    zero();
    for (i = 0; i < 6; i++) {
        char u[64];
        sprintf(u, "http://h/%d.css", i);
        pc64_fetch_start(u);
    }
    ck(pc64_fetch_inflight() == 4, "four in flight, not one and not six");
    ck(g_begun == 4, "only four requests were actually begun");

    /* 2. and the other two start as slots free, without anyone asking again */
    n = pc64_fetch_get("http://h/5.css", &p);
    ck(n > 0 && p && !strcmp((const char *)p, "http://h/5.css"),
       "the queued sixth resource arrives, with ITS bytes");
    ck(g_begun == 6, "every queued resource was started exactly once");
    ck(g_peak == 4, "concurrency never exceeded the cap");

    /* 3. PARALLEL, measured. Six requests at ten polls each is sixty polls of
     *    work; four at a time costs about twenty rounds of wall clock. Serial
     *    would be sixty. The margin is wide because the point is the shape,
     *    not the constant. */
    ck(g_polls >= 50, "the stub really was polled for all six (no shortcut)");
    {   int rounds_if_serial = 6 * g_cost;
        /* every entry is done, so the loop above ran roughly
         * ceil(6/4) * cost = 20 rounds, each polling <= 4 requests */
        ck(g_polls < rounds_if_serial + 10 && g_peak > 1,
           "overlapped rather than serialised");
    }

    /* 4. asking twice costs nothing */
    {   int before = g_begun;
        n = pc64_fetch_get("http://h/0.css", &p);
        ck(n > 0 && g_begun == before, "a second ask is served from the table");
    }

    /* 5. a failure is REMEMBERED, or a broken image is re-requested on every
     *    reflow - which on a page with one is a request per frame. */
    zero();
    g_fail_all = 1;
    ck(pc64_fetch_get("http://h/gone.png", &p) < 0, "a failed fetch reports failure");
    {   int before = g_begun;
        ck(pc64_fetch_get("http://h/gone.png", &p) < 0 && g_begun == before,
           "and is not asked for a second time");
    }

    /* 6. navigation cancels what is in flight - the bytes belong to the page
     *    that referenced them, and so do the sockets. */
    zero();
    for (i = 0; i < 4; i++) {
        char u[64];
        sprintf(u, "http://h/x%d.png", i);
        pc64_fetch_start(u);
    }
    ck(g_live == 4, "four transports held");
    pc64_fetch_reset();
    ck(g_live == 0, "reset released every one of them");
    ck(pc64_fetch_count() == 0 && pc64_fetch_bytes() == 0, "and emptied the table");

    /* 7. the caps that keep a hostile page bounded */
    zero();
    for (i = 0; i < 40; i++) {
        char u[64];
        sprintf(u, "http://h/%d.png", i);
        pc64_fetch_start(u);
    }
    ck(pc64_fetch_count() == 24, "no more than FETCH_MAX resources per page");
    {   char longurl[400];
        memset(longurl, 'a', sizeof longurl - 1);
        longurl[sizeof longurl - 1] = 0;
        ck(pc64_fetch_get(longurl, &p) < 0, "an over-long URL is refused, not truncated");
    }

    printf("%s\n", fails ? ">> fetch gate FAILED" : ">> fetch gate OK");
    return fails;
}
