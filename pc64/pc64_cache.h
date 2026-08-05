/* pc64_cache - the browser's response cache.
 *
 * Back/forward and a re-visit are the common case in browsing, and each one
 * currently costs a full DNS + TCP + TLS + transfer round on a box where
 * that is seconds, not milliseconds. The cache turns those into a memcpy.
 *
 * In memory only, and deliberately: a disk tier would need eviction,
 * corruption handling and a format, all to speed up a case (across reboots)
 * that a machine booting from USB hits rarely. The memory tier is where the
 * win is.
 *
 * Freshness: an entry carries an absolute expiry taken from the response's
 * Cache-Control max-age; without one it gets a short heuristic lifetime, and
 * with no-store or no-cache it is never stored at all. A reload (F5) bypasses
 * the cache entirely, which is what a reload is FOR.
 */
#ifndef PC64_CACHE_H
#define PC64_CACHE_H

/* Freshness directives parsed out of a response, handed to _put. */
typedef struct {
    int no_store;         /* never keep it                                  */
    long max_age;         /* seconds; <0 = absent                           */
} pc64_cache_ctl;

/* A fresh copy, or <0. Fills body/status exactly as pc64_http_get would. */
int  pc64_cache_get(const char *url, char *body, int bodymax,
                    char *status, int statusmax);

/* Offer a response to the cache. Ignores anything it should not keep. */
void pc64_cache_put(const char *url, const char *body, int len,
                    const char *status, const pc64_cache_ctl *ctl);

/* Drop everything (the uno:cache page, and any "clear browsing data"). */
void pc64_cache_clear(void);
int  pc64_cache_count(void);
int  pc64_cache_bytes(void);

#endif
