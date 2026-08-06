/* ===========================================================================
 * pc64_cache.c - the response cache. See pc64_cache.h.
 * ======================================================================== */
#include "pc64_cache.h"
#include "pc64_native.h"
#include <stdlib.h>
#include <string.h>

#define CACHE_MAX      12
#define CACHE_URL_MAX  256
#define CACHE_ONE_MAX  (256u * 1024u)     /* one page                        */
#define CACHE_ALL_MAX  (1u << 20)         /* the whole cache                 */
#define HEURISTIC_TTL  60                 /* seconds, when nothing says      */

typedef struct {
    char   url[CACHE_URL_MAX];
    char   status[128];
    char  *body;
    int    len;
    long long expires;                    /* absolute seconds; 0 = unusable  */
} entry;

static entry    g_ent[CACHE_MAX];
static int      g_n;
static unsigned g_bytes;

/* the same civil-time arithmetic the cookie jar uses; a machine with a dead
 * RTC returns 0 and every entry then reads as expired, so a broken clock
 * degrades to "no cache" rather than to "stale forever" */
static long long civil_days(int y, int m, int d)
{
    long long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static long long now_s(void)
{
    int y, mo, d, h, mi, s;
    if (uno_native_rtc_read(&y, &mo, &d, &h, &mi, &s) != 0) return 0;
    return civil_days(y, mo, d) * 86400ll + h * 3600ll + mi * 60ll + s;
}

static void drop(int i)
{
    if (!g_ent[i].body) return;
    g_bytes -= (unsigned)g_ent[i].len;
    free(g_ent[i].body);
    g_ent[i].body = 0;
    g_ent[i].len = 0;
    g_ent[i].url[0] = 0;
}

int pc64_cache_len(const char *url)
{
    long long t = now_s();
    int i;
    if (!url) return -1;
    for (i = 0; i < CACHE_MAX; i++) {
        if (!g_ent[i].body || strcmp(g_ent[i].url, url)) continue;
        if (!t || g_ent[i].expires <= t) return -1;      /* stale = absent */
        return g_ent[i].len;
    }
    return -1;
}

int pc64_cache_get(const char *url, char *body, int bodymax,
                   char *status, int statusmax)
{
    long long t = now_s();
    int i;
    if (!url || !body || bodymax <= 0) return -1;
    for (i = 0; i < CACHE_MAX; i++) {
        int n;
        if (!g_ent[i].body || strcmp(g_ent[i].url, url)) continue;
        if (!t || g_ent[i].expires <= t) { drop(i); return -1; }   /* stale */
        n = g_ent[i].len;
        if (n > bodymax - 1) n = bodymax - 1;
        memcpy(body, g_ent[i].body, (size_t)n);
        body[n] = 0;
        if (status && statusmax > 0) {
            strncpy(status, g_ent[i].status, (size_t)statusmax - 1);
            status[statusmax - 1] = 0;
        }
        return n;
    }
    return -1;
}

void pc64_cache_put(const char *url, const char *body, int len,
                    const char *status, const pc64_cache_ctl *ctl)
{
    long long t = now_s();
    long ttl;
    int i, slot = -1;

    if (!url || !body || len <= 0) return;
    if (!t) return;                                  /* no clock, no cache   */
    if ((unsigned)len > CACHE_ONE_MAX) return;
    if (strlen(url) >= CACHE_URL_MAX) return;
    if (ctl && ctl->no_store) return;

    ttl = (ctl && ctl->max_age >= 0) ? ctl->max_age : HEURISTIC_TTL;
    if (ttl <= 0) return;                            /* max-age=0: not fresh */

    for (i = 0; i < CACHE_MAX; i++) {                /* replace, or take free */
        if (g_ent[i].body && !strcmp(g_ent[i].url, url)) { drop(i); slot = i; break; }
        if (!g_ent[i].body && slot < 0) slot = i;
    }
    if (slot < 0) {                                  /* full: evict the soonest
                                                      * to expire, which is the
                                                      * least useful to keep  */
        long long soonest = 0;
        for (i = 0; i < CACHE_MAX; i++)
            if (g_ent[i].body && (slot < 0 || g_ent[i].expires < soonest))
                { slot = i; soonest = g_ent[i].expires; }
        if (slot < 0) return;
        drop(slot);
    }
    while (g_bytes + (unsigned)len > CACHE_ALL_MAX) {  /* make room */
        int victim = -1;
        long long soonest = 0;
        for (i = 0; i < CACHE_MAX; i++)
            if (g_ent[i].body && (victim < 0 || g_ent[i].expires < soonest))
                { victim = i; soonest = g_ent[i].expires; }
        if (victim < 0) return;
        drop(victim);
    }

    g_ent[slot].body = (char *)malloc((size_t)len + 1);
    if (!g_ent[slot].body) return;
    memcpy(g_ent[slot].body, body, (size_t)len);
    g_ent[slot].body[len] = 0;
    g_ent[slot].len = len;
    g_ent[slot].expires = t + ttl;
    strncpy(g_ent[slot].url, url, CACHE_URL_MAX - 1);
    g_ent[slot].url[CACHE_URL_MAX - 1] = 0;
    if (status) {
        strncpy(g_ent[slot].status, status, sizeof g_ent[slot].status - 1);
        g_ent[slot].status[sizeof g_ent[slot].status - 1] = 0;
    } else g_ent[slot].status[0] = 0;
    g_bytes += (unsigned)len;
    if (slot >= g_n) g_n = slot + 1;
}

void pc64_cache_clear(void)
{
    int i;
    for (i = 0; i < CACHE_MAX; i++) drop(i);
    g_n = 0;
    g_bytes = 0;
}

int pc64_cache_count(void)
{
    int i, n = 0;
    for (i = 0; i < CACHE_MAX; i++) if (g_ent[i].body) n++;
    return n;
}

int pc64_cache_bytes(void) { return (int)g_bytes; }
