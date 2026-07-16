/* pc64_http - a tiny HTTP/1.0 GET client for the browser, over the pc64 net
 * stack (e1000 + net.c). Brings the link up on demand (DHCP), resolves the
 * host by DNS (or accepts an IP literal), does one GET, and returns the body.
 * HTTPS is not supported yet (needs CA trust); an https:// URL reports that. */
#ifndef PC64_HTTP_H
#define PC64_HTTP_H

/* Bring the NIC + net stack up if they aren't already. Idempotent.
 * Returns 1 if a link is (or came) up, 0 if there is no e1000 NIC. */
int pc64_net_up(void);

/* GET `url` (http://host[:port]/path, or bare host/path). On success returns
 * the body length (>=0) copied into `body` (NUL-terminated, capped at
 * bodymax-1); `status` gets a short human-readable status/result line. On
 * failure returns a negative code and `status` explains why. */
int pc64_http_get(const char *url, char *body, int bodymax,
                  char *status, int statusmax);

#endif
