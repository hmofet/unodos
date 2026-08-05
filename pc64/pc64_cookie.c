/* ===========================================================================
 * pc64_cookie.c - the cookie jar. See pc64_cookie.h for what is and is not
 * implemented, and why.
 * ======================================================================== */
#include "pc64_cookie.h"
#include "pc64_native.h"
#include <string.h>

#define JAR_MAX     48
#define NAME_MAX    64
#define VALUE_MAX   512
#define DOMAIN_MAX  96
#define PATH_MAX    96

typedef struct {
    char domain[DOMAIN_MAX];      /* no leading dot; matching handles suffix */
    char path[PATH_MAX];
    char name[NAME_MAX];
    char value[VALUE_MAX];
    long long expires;            /* 0 = session cookie (never expires here) */
    unsigned char secure, httponly, hostonly;
    unsigned char used;
} cookie;

static cookie g_jar[JAR_MAX];

/* ---- time ----------------------------------------------------------------
 * Max-Age is relative, so the jar only needs a monotonically sane "now".
 * The CMOS RTC gives one; a machine with a dead clock returns 0 and every
 * Max-Age cookie then behaves as a session cookie, which errs toward
 * forgetting rather than toward keeping something too long. */
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

/* ---- small helpers -------------------------------------------------------- */
static int ci_eq(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static void put(char *dst, int cap, const char *s, int n)
{
    if (n > cap - 1) n = cap - 1;
    if (n < 0) n = 0;
    memcpy(dst, s, (size_t)n);
    dst[n] = 0;
}

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

/* "example.com" matches host "www.example.com" (suffix on a dot boundary) or
 * exactly. This is the check that stops a page setting a cookie for a domain
 * it has no business in. */
static int domain_matches(const char *host, const char *dom)
{
    int hl = (int)strlen(host), dl = (int)strlen(dom);
    if (dl == 0 || dl > hl) return 0;
    if (!ci_eq(host + hl - dl, dom, dl)) return 0;
    return hl == dl || host[hl - dl - 1] == '.';
}

static int path_matches(const char *reqpath, const char *cpath)
{
    int cl = (int)strlen(cpath);
    if (cl == 0) return 1;
    if (strncmp(reqpath, cpath, (size_t)cl)) return 0;
    return cpath[cl - 1] == '/' || reqpath[cl] == '/' || reqpath[cl] == 0;
}

/* the "directory" of a request path, which is a cookie's default scope */
static void default_path(const char *reqpath, char *out, int outmax)
{
    int n = (int)strlen(reqpath), cut = n;
    while (cut > 0 && reqpath[cut - 1] != '/') cut--;
    if (cut <= 1) { put(out, outmax, "/", 1); return; }
    put(out, outmax, reqpath, cut - 1);
}

static void expire_pass(void)
{
    long long t = now_s();
    int i;
    if (!t) return;
    for (i = 0; i < JAR_MAX; i++)
        if (g_jar[i].used && g_jar[i].expires && g_jar[i].expires <= t)
            g_jar[i].used = 0;
}

/* ---- Set-Cookie ----------------------------------------------------------- */
void pc64_cookie_set(const char *host, const char *path, const char *value)
{
    cookie c;
    const char *p = value, *eq, *semi;
    int i, free_slot = -1;

    if (!host || !path || !value) return;
    memset(&c, 0, sizeof c);
    put(c.domain, DOMAIN_MAX, host, (int)strlen(host));
    c.hostonly = 1;
    default_path(path, c.path, PATH_MAX);

    /* name=value, up to the first ';' */
    p = skip_ws(p);
    semi = strchr(p, ';');
    eq = strchr(p, '=');
    if (!eq || (semi && eq > semi)) return;            /* no name: not a cookie */
    put(c.name, NAME_MAX, p, (int)(eq - p));
    {   const char *ve = semi ? semi : p + strlen(p);
        put(c.value, VALUE_MAX, eq + 1, (int)(ve - eq - 1)); }
    if (!c.name[0]) return;

    /* attributes */
    while (semi) {
        const char *a = skip_ws(semi + 1), *ae, *av;
        semi = strchr(a, ';');
        ae = semi ? semi : a + strlen(a);
        av = a;
        while (av < ae && *av != '=') av++;
        {   int alen = (int)(av - a);
            if (alen == 6 && ci_eq(a, "secure", 6)) c.secure = 1;
            else if (alen == 8 && ci_eq(a, "httponly", 8)) c.httponly = 1;
            else if (alen == 7 && ci_eq(a, "max-age", 7) && av < ae) {
                long long v = 0, sign = 1;
                const char *q = skip_ws(av + 1);
                if (*q == '-') { sign = -1; q++; }
                while (q < ae && *q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
                v *= sign;
                if (v <= 0) {                       /* max-age<=0 deletes it */
                    for (i = 0; i < JAR_MAX; i++)
                        if (g_jar[i].used && !strcmp(g_jar[i].name, c.name) &&
                            domain_matches(host, g_jar[i].domain))
                            g_jar[i].used = 0;
                    return;
                }
                {   long long t = now_s();
                    c.expires = t ? t + v : 0; }    /* no clock: session-only */
            }
            else if (alen == 6 && ci_eq(a, "domain", 6) && av < ae) {
                const char *q = skip_ws(av + 1);
                int ql;
                if (*q == '.') q++;
                ql = (int)(ae - q);
                /* a page may WIDEN scope to a parent domain, never to an
                 * unrelated one - the whole point of the check */
                {   char cand[DOMAIN_MAX];
                    put(cand, DOMAIN_MAX, q, ql);
                    if (cand[0] && domain_matches(host, cand)) {
                        put(c.domain, DOMAIN_MAX, cand, (int)strlen(cand));
                        c.hostonly = 0;
                    } }
            }
            else if (alen == 4 && ci_eq(a, "path", 4) && av < ae) {
                const char *q = skip_ws(av + 1);
                put(c.path, PATH_MAX, q, (int)(ae - q));
            }
            /* Expires= is deliberately ignored - see the header. */
        }
    }

    expire_pass();
    for (i = 0; i < JAR_MAX; i++) {
        if (!g_jar[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (!strcmp(g_jar[i].name, c.name) &&
            !strcmp(g_jar[i].domain, c.domain) &&
            !strcmp(g_jar[i].path, c.path)) { g_jar[i] = c; g_jar[i].used = 1; return; }
    }
    if (free_slot < 0) free_slot = 0;               /* full: evict the oldest */
    g_jar[free_slot] = c;
    g_jar[free_slot].used = 1;
}

/* ---- Cookie: --------------------------------------------------------------- */
int pc64_cookie_header(const char *host, const char *path, int secure,
                       char *out, int outmax)
{
    int i, n = 0;
    if (!out || outmax <= 0) return 0;
    out[0] = 0;
    if (!host || !path) return 0;
    expire_pass();
    for (i = 0; i < JAR_MAX; i++) {
        cookie *c = &g_jar[i];
        int need;
        if (!c->used) continue;
        if (c->secure && !secure) continue;         /* https-only stays there */
        if (c->hostonly) { if (strcmp(host, c->domain)) continue; }
        else if (!domain_matches(host, c->domain)) continue;
        if (!path_matches(path, c->path)) continue;
        need = (int)strlen(c->name) + (int)strlen(c->value) + 3;
        if (n + need >= outmax) break;
        if (n) { out[n++] = ';'; out[n++] = ' '; }
        {   int l = (int)strlen(c->name); memcpy(out + n, c->name, (size_t)l); n += l; }
        out[n++] = '=';
        {   int l = (int)strlen(c->value); memcpy(out + n, c->value, (size_t)l); n += l; }
        out[n] = 0;
    }
    return n;
}

/* ---- enumeration ----------------------------------------------------------- */
int pc64_cookie_count(void)
{
    int i, n = 0;
    expire_pass();
    for (i = 0; i < JAR_MAX; i++) if (g_jar[i].used) n++;
    return n;
}

int pc64_cookie_at(int idx, const char **domain, const char **path,
                   const char **name, const char **value, int *secure)
{
    int i, n = 0;
    for (i = 0; i < JAR_MAX; i++) {
        if (!g_jar[i].used) continue;
        if (n++ != idx) continue;
        if (domain) *domain = g_jar[i].domain;
        if (path)   *path   = g_jar[i].path;
        if (name)   *name   = g_jar[i].name;
        if (value)  *value  = g_jar[i].value;
        if (secure) *secure = g_jar[i].secure;
        return 1;
    }
    return 0;
}

void pc64_cookie_clear(void) { memset(g_jar, 0, sizeof g_jar); }
