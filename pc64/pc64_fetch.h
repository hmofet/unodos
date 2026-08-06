/* pc64_fetch - the browser's subresource fetch queue + cache.
 *
 * A page is not one document: it references images and stylesheets by URL,
 * and every one of them is another GET. This owns that second tier - fetch
 * once per URL per page, keep the bytes for as long as the page lives, and
 * hand them to whoever asked (unomedia for an image, the cascade for a
 * sheet). pc64_http does the transport; this decides what is worth asking
 * for and remembers the answer.
 *
 * SEVERAL AT ONCE. The queue used to be synchronous by design, because the
 * transport could only hold one connection and a queue with nowhere to put a
 * second request is just a list. It can now hold a few, so this runs up to
 * FETCH_PAR of them together and a page's stylesheets cost one round trip
 * rather than one each. The shape callers see is unchanged: ask for a URL and
 * you get the bytes. What changed is that asking EARLY (pc64_fetch_start,
 * during the parse, before layout needs anything) is now worth something.
 *
 * Everything is bounded - a hostile page cannot make the browser fetch
 * forever or eat the heap (see the caps in the .c). */
#ifndef PC64_FETCH_H
#define PC64_FETCH_H

/* Drop every cached subresource, and cancel anything still in flight. Call on
 * navigation: the bytes belong to the page that referenced them. */
void pc64_fetch_reset(void);

/* Ask for `url` WITHOUT waiting. Cheap and idempotent, so the parser can call
 * it for every subresource it sees; the ones over the concurrency limit queue
 * up and start as slots free. This is what turns a page's fetches from
 * sequential into parallel - pc64_fetch_get alone can only ever run one,
 * because by the time layout needs an image it needs it now. */
void pc64_fetch_start(const char *url);

/* Advance everything in flight. Non-blocking. Call from anywhere that is
 * already looping (the frame loop, a parse pass) to overlap fetches with work
 * that is not waiting on them. */
void pc64_fetch_pump(void);

/* Fetch `url` (absolute http/https) or return the already-fetched copy.
 * On success returns the byte length and points *data at the bytes (owned
 * by the cache, valid until pc64_fetch_reset). Returns <0 on failure, and
 * a failure is REMEMBERED so a broken image is not re-requested on every
 * reflow. BLOCKS until this URL specifically is done - but everything else
 * in flight keeps moving while it waits. */
int pc64_fetch_get(const char *url, const unsigned char **data);

/* How many resources are cached / how many bytes they hold (status line). */
int pc64_fetch_count(void);
int pc64_fetch_bytes(void);
/* How many are in flight right now (the concurrency gate reads this). */
int pc64_fetch_inflight(void);

#endif
