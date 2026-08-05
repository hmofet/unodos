/* pc64_fetch - the browser's subresource fetch queue + cache.
 *
 * A page is not one document: it references images and stylesheets by URL,
 * and every one of them is another GET. This owns that second tier - fetch
 * once per URL per page, keep the bytes for as long as the page lives, and
 * hand them to whoever asked (unomedia for an image, the cascade for a
 * sheet). pc64_http does the transport; this decides what is worth asking
 * for and remembers the answer.
 *
 * Synchronous by design: pc64 is a cooperative single-address-space shell
 * and the browser already blocks on the main document's GET. Progressive
 * fetching belongs with the M7 progressive-render work, and the queue shape
 * here (a table walked in document order) is what that will drive.
 *
 * Everything is bounded - a hostile page cannot make the browser fetch
 * forever or eat the heap (see the caps in the .c). */
#ifndef PC64_FETCH_H
#define PC64_FETCH_H

/* Drop every cached subresource. Call on navigation: the bytes belong to
 * the page that referenced them. */
void pc64_fetch_reset(void);

/* Fetch `url` (absolute http/https) or return the already-fetched copy.
 * On success returns the byte length and points *data at the bytes (owned
 * by the cache, valid until pc64_fetch_reset). Returns <0 on failure, and
 * a failure is REMEMBERED so a broken image is not re-requested on every
 * reflow. */
int pc64_fetch_get(const char *url, const unsigned char **data);

/* How many resources are cached / how many bytes they hold (status line). */
int pc64_fetch_count(void);
int pc64_fetch_bytes(void);

#endif
