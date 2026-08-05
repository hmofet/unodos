/* cache_test.c - the response cache's freshness rules.
 *
 * A cache that serves something it should not is worse than no cache, so
 * most of these are refusals too: no-store, max-age=0, an expired entry,
 * and a URL that merely looks similar.
 */
#include <stdio.h>
#include <string.h>
#include "../../pc64_cache.h"

/* A movable clock: the cache reads the RTC, so the test drives time by
 * changing what the RTC says rather than by waiting. */
static int g_min;
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{ *y = 2026; *mo = 8; *d = 6; *h = 12; *mi = g_min; *s = 0; return 0; }

static int g_pass, g_fail;
static void note(int ok, const char *name)
{ printf("%s %s\n", ok ? "pass" : "FAIL", name); if (ok) g_pass++; else g_fail++; }

static pc64_cache_ctl ctl_of(int no_store, long max_age)
{ pc64_cache_ctl c; c.no_store = no_store; c.max_age = max_age; return c; }

static int hit(const char *url, const char *want)
{
    char body[256], status[64];
    int n = pc64_cache_get(url, body, sizeof body, status, sizeof status);
    if (n < 0) return 0;
    return want ? strcmp(body, want) == 0 : 1;
}

int main(void)
{
    pc64_cache_ctl c;

    /* a stored response comes back, body and status both */
    pc64_cache_clear(); g_min = 0;
    c = ctl_of(0, 300);
    pc64_cache_put("http://a.com/", "hello", 5, "200 OK", &c);
    note(hit("http://a.com/", "hello"), "a fresh entry is served");
    {   char body[256], status[64];
        pc64_cache_get("http://a.com/", body, sizeof body, status, sizeof status);
        note(strcmp(status, "200 OK") == 0, "  (with its status line)"); }
    note(!hit("http://a.com/other", NULL), "a different URL is not a hit");
    note(!hit("http://a.com", NULL), "  (nor a prefix of it)");

    /* no-store is never kept */
    pc64_cache_clear();
    c = ctl_of(1, 300);
    pc64_cache_put("http://a.com/", "secret", 6, "200 OK", &c);
    note(!hit("http://a.com/", NULL), "no-store is not cached");
    note(pc64_cache_count() == 0, "  (nothing is stored at all)");

    /* max-age=0 means already stale */
    pc64_cache_clear();
    c = ctl_of(0, 0);
    pc64_cache_put("http://a.com/", "x", 1, "200 OK", &c);
    note(!hit("http://a.com/", NULL), "max-age=0 is not cached");

    /* max-age expires: 60s TTL, then the clock moves two minutes */
    pc64_cache_clear(); g_min = 0;
    c = ctl_of(0, 60);
    pc64_cache_put("http://a.com/", "old", 3, "200 OK", &c);
    note(hit("http://a.com/", "old"), "within max-age: hit");
    g_min = 2;                                        /* +120s */
    note(!hit("http://a.com/", NULL), "past max-age: miss");
    note(pc64_cache_count() == 0, "  (and the stale entry is dropped)");

    /* no directives at all still gets a short heuristic life */
    pc64_cache_clear(); g_min = 0;
    c = ctl_of(0, -1);
    pc64_cache_put("http://a.com/", "heur", 4, "200 OK", &c);
    note(hit("http://a.com/", "heur"), "no Cache-Control: heuristic hit");
    g_min = 5;                                        /* +300s */
    note(!hit("http://a.com/", NULL), "  (but not for long)");

    /* a re-put replaces rather than duplicating */
    pc64_cache_clear(); g_min = 0;
    c = ctl_of(0, 300);
    pc64_cache_put("http://a.com/", "v1", 2, "200 OK", &c);
    pc64_cache_put("http://a.com/", "v2", 2, "200 OK", &c);
    note(hit("http://a.com/", "v2"), "re-put replaces the body");
    note(pc64_cache_count() == 1, "  (one entry, not two)");

    /* several URLs coexist */
    pc64_cache_clear();
    pc64_cache_put("http://a.com/", "A", 1, "200 OK", &c);
    pc64_cache_put("http://b.com/", "B", 1, "200 OK", &c);
    note(hit("http://a.com/", "A") && hit("http://b.com/", "B"),
         "distinct URLs coexist");

    /* clear really clears */
    pc64_cache_clear();
    note(pc64_cache_count() == 0 && pc64_cache_bytes() == 0, "clear empties it");

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail != 0;
}
