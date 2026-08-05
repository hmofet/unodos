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

/* Drop any kept-alive connection. Call when the browser goes idle or the
 * network is reconfigured; ordinary navigation does not need it, since the
 * connection is reused only for the origin it was opened to. */
void pc64_http_disconnect(void);

#endif
