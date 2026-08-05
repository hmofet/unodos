/* pc64_cookie - the browser's cookie jar.
 *
 * A cookie is the difference between a browser that can read the web and one
 * that can be logged into it. The jar lives here rather than inside
 * pc64_http.c because it is state with its own rules - matching, scoping and
 * expiry - and because the browser has to be able to show and clear it
 * (uno:cookies), which a header-building helper buried in the transport
 * could not offer.
 *
 * Scope rules implemented: domain match (exact, or suffix for a Domain=
 * attribute), path prefix, Secure (https only), HttpOnly (stored, and
 * meaningless until script can read cookies at all), Max-Age expiry against
 * the CMOS RTC. NOT implemented: the Expires= date form (HTTP-date parsing
 * for a value almost always paired with Max-Age), and __Host-/__Secure-
 * prefixes. Both are noted in DOCS rather than silently missing.
 */
#ifndef PC64_COOKIE_H
#define PC64_COOKIE_H

/* Absorb one Set-Cookie value (the text AFTER "Set-Cookie:"). `host` and
 * `path` are the request's, for defaulting scope and for rejecting a cookie
 * that tries to set itself on someone else's domain. */
void pc64_cookie_set(const char *host, const char *path, const char *value);

/* Build the Cookie: header value for a request - "a=1; b=2", or "" when
 * nothing matches. Returns the length written. */
int  pc64_cookie_header(const char *host, const char *path, int secure,
                        char *out, int outmax);

/* Enumeration, for the uno:cookies page. Returns the count; each getter
 * fills what it is given and tolerates NULL. */
int  pc64_cookie_count(void);
int  pc64_cookie_at(int i, const char **domain, const char **path,
                    const char **name, const char **value, int *secure);
void pc64_cookie_clear(void);

#endif
