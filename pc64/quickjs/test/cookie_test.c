/* cookie_test.c - the jar's scoping rules.
 *
 * Every check here is about a cookie going somewhere it should NOT, because
 * that is the failure with consequences: sending a session token to the
 * wrong host, or letting a page set one for a domain it has no business in.
 * The happy path is one check; the rest are refusals.
 */
#include <stdio.h>
#include <string.h>
#include "../../pc64_cookie.h"

/* the jar reads the RTC for Max-Age; a fixed clock keeps the test exact */
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{ *y = 2026; *mo = 8; *d = 6; *h = 12; *mi = 0; *s = 0; return 0; }

static int g_pass, g_fail;
static void note(int ok, const char *name)
{ printf("%s %s\n", ok ? "pass" : "FAIL", name); if (ok) g_pass++; else g_fail++; }

static int sends(const char *host, const char *path, int secure, const char *want)
{
    char buf[512];
    pc64_cookie_header(host, path, secure, buf, sizeof buf);
    return strstr(buf, want) != NULL;
}

int main(void)
{
    char buf[512];

    /* the happy path */
    pc64_cookie_clear();
    pc64_cookie_set("example.com", "/", "sid=abc123");
    note(sends("example.com", "/", 0, "sid=abc123"), "stored cookie comes back");
    note(sends("example.com", "/deep/page", 0, "sid=abc123"), "default path is the origin");

    /* a cookie must not leak to another host */
    note(!sends("evil.com", "/", 0, "sid"), "not sent to an unrelated host");
    note(!sends("notexample.com", "/", 0, "sid"), "suffix-without-dot is NOT a match");

    /* host-only by default: no Domain= means the exact host only */
    note(!sends("www.example.com", "/", 0, "sid"), "host-only: no subdomain");

    /* Domain= may WIDEN to a parent, and then subdomains match */
    pc64_cookie_clear();
    pc64_cookie_set("www.example.com", "/", "wide=1; Domain=example.com");
    note(sends("www.example.com", "/", 0, "wide=1"), "Domain= parent: origin matches");
    note(sends("api.example.com", "/", 0, "wide=1"), "Domain= parent: sibling matches");
    note(!sends("example.com.evil.com", "/", 0, "wide"), "Domain= cannot be spoofed by suffix");

    /* a page must NOT set a cookie for an unrelated domain */
    pc64_cookie_clear();
    pc64_cookie_set("good.com", "/", "bad=1; Domain=bank.com");
    note(!sends("bank.com", "/", 0, "bad"), "cannot set a cookie on another domain");
    note(sends("good.com", "/", 0, "bad=1"), "  (it stays on its own host)");

    /* Path= scoping */
    pc64_cookie_clear();
    pc64_cookie_set("example.com", "/", "p=1; Path=/admin");
    note(sends("example.com", "/admin", 0, "p=1"), "Path= matches its own path");
    note(sends("example.com", "/admin/x", 0, "p=1"), "Path= matches below it");
    note(!sends("example.com", "/public", 0, "p"), "Path= does not match a sibling");
    note(!sends("example.com", "/adminium", 0, "p"), "Path= is not a bare prefix");

    /* Secure must never travel over plain HTTP */
    pc64_cookie_clear();
    pc64_cookie_set("example.com", "/", "tok=xyz; Secure");
    note(sends("example.com", "/", 1, "tok=xyz"), "Secure cookie sent over https");
    note(!sends("example.com", "/", 0, "tok"), "Secure cookie NOT sent over http");

    /* Max-Age=0 deletes */
    pc64_cookie_clear();
    pc64_cookie_set("example.com", "/", "gone=1");
    pc64_cookie_set("example.com", "/", "gone=1; Max-Age=0");
    note(!sends("example.com", "/", 0, "gone"), "Max-Age=0 deletes the cookie");

    /* a re-set replaces rather than duplicates */
    pc64_cookie_clear();
    pc64_cookie_set("example.com", "/", "k=old");
    pc64_cookie_set("example.com", "/", "k=new");
    pc64_cookie_header("example.com", "/", 0, buf, sizeof buf);
    note(strstr(buf, "k=new") && !strstr(buf, "k=old"), "re-set replaces the value");
    note(pc64_cookie_count() == 1, "  (and does not duplicate the entry)");

    /* several cookies come back in one header */
    pc64_cookie_clear();
    pc64_cookie_set("example.com", "/", "a=1");
    pc64_cookie_set("example.com", "/", "b=2");
    pc64_cookie_header("example.com", "/", 0, buf, sizeof buf);
    note(strstr(buf, "a=1") && strstr(buf, "b=2") && strstr(buf, "; "),
         "multiple cookies join with '; '");

    /* junk must not crash or store anything */
    pc64_cookie_clear();
    pc64_cookie_set("example.com", "/", "novalue");
    pc64_cookie_set("example.com", "/", "");
    note(pc64_cookie_count() == 0, "malformed Set-Cookie stores nothing");

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail != 0;
}
