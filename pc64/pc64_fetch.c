/* ===========================================================================
 * pc64_fetch.c - the browser's subresource fetch queue + cache.
 *
 * See pc64_fetch.h for the contract. The caps below are the whole security
 * story of this file: a page can reference a thousand images, and every one
 * of them is a GET this box would otherwise make.
 * ======================================================================== */
#include "pc64_fetch.h"
#include "pc64_http.h"
#include <stdlib.h>
#include <string.h>

/* Caps. Deliberately small: this is a browser on a hobby OS, and the cost
 * of a too-low cap is a missing image, while the cost of a too-high one is
 * a hung boot on a hostile page. */
#define FETCH_MAX      24                 /* resources per page              */
#define FETCH_URL_MAX  256
#define FETCH_ONE_MAX  (1u << 20)         /* 1 MB per resource               */
#define FETCH_ALL_MAX  (4u << 20)         /* 4 MB per page                   */

typedef struct {
    char           url[FETCH_URL_MAX];
    unsigned char *data;                  /* NULL = the fetch failed         */
    int            len;
} entry;

static entry   g_ent[FETCH_MAX];
static int     g_n;
static unsigned g_bytes;

void pc64_fetch_reset(void)
{
    int i;
    for (i = 0; i < g_n; i++) { free(g_ent[i].data); g_ent[i].data = 0; }
    g_n = 0;
    g_bytes = 0;
}

int pc64_fetch_count(void) { return g_n; }
int pc64_fetch_bytes(void) { return (int)g_bytes; }

int pc64_fetch_get(const char *url, const unsigned char **data)
{
    int i, n;
    char status[128];
    unsigned char *buf;
    entry *e;

    if (data) *data = 0;
    if (!url || !*url) return -1;
    if (strlen(url) >= FETCH_URL_MAX) return -1;

    for (i = 0; i < g_n; i++)                  /* already asked? */
        if (!strcmp(g_ent[i].url, url)) {
            if (!g_ent[i].data) return -1;     /* a REMEMBERED failure */
            if (data) *data = g_ent[i].data;
            return g_ent[i].len;
        }

    if (g_n >= FETCH_MAX) return -1;
    if (g_bytes >= FETCH_ALL_MAX) return -1;

    e = &g_ent[g_n++];
    memcpy(e->url, url, strlen(url) + 1);
    e->data = 0;
    e->len = 0;

    /* +1: pc64_http_get NUL-terminates, and a caller that treats the bytes
     * as text (a stylesheet) then needs no copy of its own. */
    buf = (unsigned char *)malloc(FETCH_ONE_MAX + 1);
    if (!buf) return -1;

    n = pc64_http_get(url, (char *)buf, FETCH_ONE_MAX, status, sizeof status);
    if (n < 0) { free(buf); return -1; }       /* entry stays, data NULL     */

    /* shrink to what arrived: a 1 MB buffer per 300-byte icon would blow the
     * page cap after three images */
    {   unsigned char *fit = (unsigned char *)malloc((size_t)n + 1);
        if (fit) { memcpy(fit, buf, (size_t)n + 1); free(buf); buf = fit; }
    }

    e->data = buf;
    e->len = n;
    g_bytes += (unsigned)n;
    if (data) *data = buf;
    return n;
}
