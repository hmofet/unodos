/* ===========================================================================
 * pc64_fetch.c - the browser's subresource fetch queue + cache.
 *
 * See pc64_fetch.h for the contract. The caps below are the whole security
 * story of this file: a page can reference a thousand images, and every one
 * of them is a GET this box would otherwise make.
 *
 * The queue is a table walked in document order, exactly as before. What is
 * new is that up to FETCH_PAR entries are IN FLIGHT at once, on their own
 * connections, advanced from one pump. FETCH_PAR is deliberately small: the
 * socket table is NSOCK (16) slots shared with the URC link, discovery and the
 * document's own fetch, and a browser that could exhaust it would take the
 * machine's remote channel down with it. pc64_http caps the browser's total
 * share (HTTP_MAX_CONNS) on top of this; FETCH_PAR is what keeps the in-flight
 * half of that budget from being spent entirely on images.
 * ======================================================================== */
#include "pc64_fetch.h"
#include "pc64_http.h"
#include <stdlib.h>
#include <string.h>

/* Declared rather than included: the two calls this file makes into the
 * kernel are the whole dependency, and net.h drags the NIC seam in behind it -
 * which is what would stop quickjs/test/fetch_test.c linking the scheduler on
 * its own. */
void net_poll(void);                      /* pump the NIC (net.c)      */
void uno_pc64_delay_ms(int ms);           /* firmware Stall (uefi_main) */

/* Caps. Deliberately small: this is a browser on a hobby OS, and the cost
 * of a too-low cap is a missing image, while the cost of a too-high one is
 * a hung boot on a hostile page. */
#define FETCH_MAX      24                 /* resources per page              */
#define FETCH_URL_MAX  256
#define FETCH_ONE_MAX  (1u << 20)         /* 1 MB per resource               */
#define FETCH_ALL_MAX  (4u << 20)         /* 4 MB per page                   */
#define FETCH_PAR      4                  /* requests in flight at once      */

enum { E_QUEUED = 0, E_INFLIGHT, E_DONE, E_FAILED };

typedef struct {
    char           url[FETCH_URL_MAX];
    unsigned char *data;                  /* NULL = the fetch failed         */
    int            len;
    int            state;
    http_req      *req;                   /* only while E_INFLIGHT           */
} entry;

static entry   g_ent[FETCH_MAX];
static int     g_n;
static unsigned g_bytes;

static entry *find(const char *url)
{
    int i;
    for (i = 0; i < g_n; i++) if (!strcmp(g_ent[i].url, url)) return &g_ent[i];
    return 0;
}

void pc64_fetch_reset(void)
{
    int i;
    for (i = 0; i < g_n; i++) {
        if (g_ent[i].req) { pc64_http_free(g_ent[i].req); g_ent[i].req = 0; }
        free(g_ent[i].data); g_ent[i].data = 0;
    }
    g_n = 0;
    g_bytes = 0;
}

int pc64_fetch_count(void) { return g_n; }
int pc64_fetch_bytes(void) { return (int)g_bytes; }

int pc64_fetch_inflight(void)
{
    int i, n = 0;
    for (i = 0; i < g_n; i++) if (g_ent[i].state == E_INFLIGHT) n++;
    return n;
}

/* A finished request becomes bytes we own. Sized from the request rather than
 * taken into a worst-case buffer: a 1 MB allocation per 300-byte icon would
 * blow the page cap after three images. */
static void reap(entry *e)
{
    char status[128];
    int n = pc64_http_len(e->req);

    if (n < 0 || (unsigned)n > FETCH_ONE_MAX) {
        e->state = E_FAILED;                 /* entry stays, data NULL */
    } else {
        unsigned char *buf = (unsigned char *)malloc((size_t)n + 1);
        if (!buf) {
            e->state = E_FAILED;
        } else {
            /* +1: the take NUL-terminates, and a caller that treats the bytes
             * as text (a stylesheet) then needs no copy of its own. */
            pc64_http_take(e->req, (char *)buf, n + 1, status, sizeof status);
            e->data = buf;
            e->len = n;
            g_bytes += (unsigned)n;
            e->state = E_DONE;
        }
    }
    pc64_http_free(e->req);
    e->req = 0;
}

void pc64_fetch_pump(void)
{
    int i, live = 0;

    for (i = 0; i < g_n; i++) {
        entry *e = &g_ent[i];
        if (e->state != E_INFLIGHT) continue;
        if (pc64_http_poll(e->req)) reap(e);
        else live++;
    }
    /* fill the freed slots, in document order - the first thing the page
     * referenced is the first thing it will want to draw */
    for (i = 0; i < g_n && live < FETCH_PAR; i++) {
        entry *e = &g_ent[i];
        if (e->state != E_QUEUED) continue;
        if (g_bytes >= FETCH_ALL_MAX) { e->state = E_FAILED; continue; }
        e->req = pc64_http_begin(e->url, 0);
        if (!e->req) { e->state = E_FAILED; continue; }
        e->state = E_INFLIGHT;
        live++;
    }
}

void pc64_fetch_start(const char *url)
{
    entry *e;

    if (!url || !*url) return;
    if (strlen(url) >= FETCH_URL_MAX) return;
    if (find(url)) return;                        /* already asked */
    if (g_n >= FETCH_MAX) return;
    if (g_bytes >= FETCH_ALL_MAX) return;

    e = &g_ent[g_n++];
    memcpy(e->url, url, strlen(url) + 1);
    e->data = 0; e->len = 0; e->req = 0;
    e->state = E_QUEUED;
    pc64_fetch_pump();                            /* start it if a slot is free */
}

int pc64_fetch_get(const char *url, const unsigned char **data)
{
    entry *e;

    if (data) *data = 0;
    if (!url || !*url) return -1;
    if (strlen(url) >= FETCH_URL_MAX) return -1;

    e = find(url);
    if (!e) {
        pc64_fetch_start(url);
        e = find(url);
        if (!e) return -1;                        /* over a cap */
    }

    /* Wait for THIS one, while everything else in flight keeps moving. A
     * queued entry can only start once a slot frees, which pump does. */
    while (e->state == E_QUEUED || e->state == E_INFLIGHT) {
        net_poll();
        pc64_fetch_pump();
        uno_pc64_delay_ms(1);       /* pace it: the wire moves one segment per RTT */
    }

    if (e->state != E_DONE || !e->data) return -1;   /* a REMEMBERED failure */
    if (data) *data = e->data;
    return e->len;
}
