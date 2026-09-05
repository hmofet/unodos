/* pc64_http - a tiny HTTP/1.0 GET client for the browser, over the pc64 net
 * stack (e1000 + net.c). Brings the link up on demand (DHCP), resolves the
 * host by DNS (or accepts an IP literal), does a GET, and returns the body.
 * HTTPS is CA-validated (TLS 1.2, bundled root store). 3xx redirects are
 * followed (up to a few hops), including http<->https and apex->www upgrades. */
#ifndef PC64_HTTP_H
#define PC64_HTTP_H

/* Bring the NIC + net stack up if they aren't already. Idempotent.
 * Returns 1 if a link is (or came) up, 0 if there is no e1000 NIC. */
int pc64_net_up(void);

/* Proactive boot-time bring-up: try EVERY network device in turn, each with a
 * bounded timeout, and skip any that fails to get a DHCP lease - so a dead or
 * cableless NIC can neither hang boot nor strand the stack on a leaseless link.
 * Settles on the first device that actually leases. Idempotent (a no-op once
 * the net is up). Returns 1 if some device leased, else 0. Called once at boot;
 * the network is otherwise brought up lazily by pc64_net_up() on first use. */
int pc64_net_boot(void);

/* BOTH OF THE ABOVE MAY COME FROM THE PLATFORM. A build that defines
 * UNO_NET_BRINGUP_EXTERNAL compiles pc64_http.c without either definition and
 * links its own pair; the declarations and every caller are unchanged. See the
 * note at the top of pc64_http.c. */

/* GET `url` (http://host[:port]/path, or bare host/path). On success returns
 * the body length (>=0) copied into `body` (NUL-terminated, capped at
 * bodymax-1); `status` gets a short human-readable status/result line. On
 * failure returns a negative code and `status` explains why. */
int pc64_http_get(const char *url, char *body, int bodymax,
                  char *status, int statusmax);

/* The same, with a method: `post` NULL is a GET, otherwise the
 * form-encoded body to send as a POST. A POST is never served from nor
 * written to the cache - it asks the server to CHANGE something, and
 * replaying one is how a browser double-submits an order. A redirect after
 * a POST is followed as a GET, which is what browsers do. */
int pc64_http_request(const char *url, const char *post,
                      char *body, int bodymax, char *status, int statusmax);

/* ---- requests in flight ---------------------------------------------------
 * The two calls above BLOCK, which is fine for the one document the user asked
 * for and wrong for the twenty things that document references: a page and its
 * images and stylesheets used to cost four round trips end to end because
 * there was nowhere to put a second request. A request HANDLE is that
 * somewhere. Several exist at once, each on its own connection, and none of
 * them waits - you advance them all from one loop, so the slowest server on
 * the page costs its own latency and not everybody else's.
 *
 *     http_req *r = pc64_http_begin(url, 0);
 *     while (!pc64_http_poll(r)) { net_poll(); }
 *     n = pc64_http_take(r, body, sizeof body, status, sizeof status);
 *     pc64_http_free(r);
 *
 * Redirects, cookies, the cache and keep-alive all still happen; they are
 * inside the handle. pc64_http_get/request are now these calls plus a wait,
 * so there is exactly one implementation of what an HTTP request is. */
typedef struct http_req http_req;

/* Start a request. Returns a handle immediately - nothing has been sent yet -
 * or NULL if the URL is unusable or there is no memory. `post` (may be NULL)
 * is copied, so the caller's buffer need not outlive the call. */
http_req *pc64_http_begin(const char *url, const char *post);

/* Advance this request as far as the transport allows RIGHT NOW. Returns 0
 * while it is still going and 1 once it has finished, successfully or not.
 * Never blocks; it does not pump the NIC either, so one loop can drive many. */
int pc64_http_poll(http_req *r);

/* The result, once poll() has returned 1: the body length (>=0, copied into
 * `body` NUL-terminated and capped at bodymax-1) or a negative error code.
 * `status` is filled either way. Safe to call more than once. */
int pc64_http_take(http_req *r, char *body, int bodymax,
                   char *status, int statusmax);

/* The body length a finished request is holding, so a caller can allocate
 * exactly once instead of taking into a worst-case buffer. <0 mirrors the
 * error code from take(). */
int pc64_http_len(http_req *r);

/* Drive one request to completion, pumping the NIC. This is the poll loop out
 * of pc64_http_get, exposed so a caller that wants the ALLOCATE-FROM-len flow
 * does not have to choose between a worst-case buffer and reaching into the net
 * stack for a pump of its own. Returns 1 (finished), or 0 for a NULL handle. */
int pc64_http_wait(http_req *r);

/* Release the handle. Its connection returns to the keep-alive pool if it is
 * still good, and is closed if it is not. Safe on NULL, and safe on a request
 * that has not finished (which cancels it). */
void pc64_http_free(http_req *r);

/* ---- progressive delivery -------------------------------------------------
 * Called with the body SO FAR while a response is still arriving, throttled
 * to roughly every 6 KB. `total` is the Content-Length when the server gave
 * one, else -1. The buffer is the transport's and is valid only for the
 * duration of the call. Install NULL to turn it off.
 *
 * Only the BLOCKING calls report progress. A handle is silent unless
 * pc64_http_req_progress() opts it in: the point of progressive delivery is to
 * paint the document the reader is waiting for, and a page's images all
 * shouting through the same callback would repaint on somebody else's bytes.
 *
 * Not offered for chunked responses: those are still encoded mid-transfer,
 * and handing an embedder chunk-size lines to render is worse than making it
 * wait for the decode. */
typedef void (*pc64_http_progress_fn)(const char *body, int len, long total);
void pc64_http_on_progress(pc64_http_progress_fn fn);
void pc64_http_req_progress(http_req *r, int on);

/* Drop any kept-alive connection, and the resolver's memory of which host is
 * which address. Call when the browser goes idle or the network is
 * reconfigured; ordinary navigation does not need it, since a connection is
 * reused only for the origin it was opened to. */
void pc64_http_disconnect(void);

/* How many connections are open right now (pooled + in flight), against the
 * browser's budget of 8. Diagnostic: socket exhaustion presents as a fetch
 * that fails for no visible reason, and this is the number that explains it. */
int pc64_http_conns(void);

#endif
