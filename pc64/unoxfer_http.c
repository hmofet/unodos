/* ===========================================================================
 * UnoDOS/pc64 - unoxfer's HTTP family: plain HTTP(S) fetch, and WebDAV.
 *
 * WHY THIS DOES NOT USE pc64_http.  That client is the BROWSER's, it speaks
 * GET and POST, and it is a lane this subsystem consumes rather than widens
 * (AGENTS.md §2, and the standing note that Studio shipping its own client was
 * the right call rather than a wider pc64_http).  WebDAV needs PROPFIND,
 * MKCOL and DELETE, and a resumable download needs a Range header - none of
 * which pc64_http exposes, and all of which would be a structural change to
 * somebody else's file.
 *
 * So this is a small HTTP/1.1 client of its own, ~300 lines, sitting directly
 * on netsock (plaintext) and tls.h's HANDLE api (TLS).  It deliberately does
 * NOT reimplement: cookies, caching, keep-alive pooling, redirect chains
 * across origins, or chunked-encoding-with-trailers.  A transfer client wants
 * one request, one response, one file - and every one of those features is a
 * place for a transfer to silently return the wrong bytes.
 *
 * Redirects ARE followed (up to 4, same request method for 307/308, GET
 * otherwise) because a plain download URL is redirected roughly always.
 * ======================================================================== */
#include "unoxfer_int.h"
#include "net.h"
#include "netsock.h"
#include "tls.h"
#include "pc64_http.h"          /* pc64_net_up only */

void *malloc(unsigned long);
void  free(void *);
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
char *strstr(const char *, const char *);
int   snprintf(char *, unsigned long, const char *, ...);
long  TickCount(void);
int   uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
long  uno_fs_size(int vol, const char *name);

#define HX_HDRCAP 2048
#define HX_URLCAP 512

/* ===========================================================================
 * One transport, two flavours.
 *
 * A plain socket and a TLS handle answer the same three questions - are you
 * up, take these bytes, give me those bytes - so everything above this struct
 * is written once.  Without it the file would carry two copies of the HTTP
 * state machine and they would drift; that is the single most common way an
 * http/https client ends up behaving differently on one of them.
 * ======================================================================== */
typedef struct {
    int       sock;                 /* plaintext netsock, -1 when TLS         */
    tls_conn *tls;                  /* TLS handle, 0 when plaintext           */
    int       dead;
} hxio;

static int hx_up(hxio *io)
{
    if (io->tls) {
        int r = tls_poll(io->tls);
        if (r < 0) { io->dead = 1; return -1; }
        return r == TLS_READY ? 1 : 0;
    }
    if (io->sock < 0) return -1;
    {
        int st = net_sock_state(io->sock);
        if (st == TCP_ESTABLISHED) return 1;
        if (st == TCP_DONE || st == TCP_CLOSED) { io->dead = 1; return -1; }
        return 0;
    }
}

static int hx_send(hxio *io, const void *d, int n)
{ return io->tls ? tls_send(io->tls, d, n) : net_send(io->sock, d, n); }

static int hx_recv(hxio *io, void *b, int cap)
{ return io->tls ? tls_recv(io->tls, b, cap) : net_recv(io->sock, b, cap); }

static void hx_shut(hxio *io)
{
    if (io->tls) { tls_free(io->tls); io->tls = 0; }
    if (io->sock >= 0) { net_sock_close(io->sock); io->sock = -1; }
}

/* Send every byte, pumping while the window is shut.  A partial send that the
 * caller does not notice is a request the server never sees the end of, which
 * presents as a hang and not as an error. */
/* A NEGATIVE net_send IS NOT A FAILURE.  netsock keeps one segment in flight
 * at a time and returns -1 while that segment is outstanding, so -1 is ordinary
 * back-pressure and only means "closed" if the socket actually is.  Treating it
 * as fatal made large sends fail intermittently - which reads as a flaky
 * server, and is not.  pc64_http makes exactly this distinction; so does this.
 * A TLS handle is different: tls_send returns 0 for "not now" and negative
 * only for a real failure. */
static int hx_send_all(hxio *io, const void *d, int n, volatile int *cancel)
{
    const unsigned char *p = (const unsigned char *)d;
    long t0 = TickCount();
    int sent = 0;
    while (sent < n) {
        int w;
        if (cancel && *cancel) return -2;
        ux_pump(0);
        if (io->tls) tls_poll(io->tls);
        w = hx_send(io, p + sent, n - sent);
        if (w > 0) { sent += w; t0 = TickCount(); continue; }
        if (w < 0) {
            if (io->tls) return -1;
            if (net_sock_state(io->sock) != TCP_ESTABLISHED) return -1;
            /* else: a segment is in flight - wait for it */
        }
        ux_pump(1);                       /* nothing moved: yield             */
        if (TickCount() - t0 > 60 * 20) return -1;      /* 20 s of nothing */
    }
    return sent;
}

/* ===========================================================================
 * URLs.
 *
 * Split "https://host:443/a/b?c" into its parts.  Kept separate from
 * unoxfer_parse_url (which builds a SITE and refuses credentials) because this
 * one is asked about a URL a site has already been built from, and has to
 * accept the path and query verbatim.
 * ======================================================================== */
typedef struct {
    char host[UNOXFER_HOSTLEN];
    char path[HX_URLCAP];
    int  port, tls;
} hxurl;

static int hx_url(const char *url, hxurl *u)
{
    const char *s = url, *p;
    int n;
    memset(u, 0, sizeof *u);
    if (!url || !*url) return 0;
    /* scheme.  Only the LENGTH and a trailing 's' are read: http/https and
     * webdav/webdavs are the same wire, and which of the four it is was
     * already decided when the site was made. */
    p = strstr(url, "://");
    if (p) {
        n = (int)(p - url);
        if (n == 5 && (url[4] == 's' || url[4] == 'S')) u->tls = 1;   /* https */
        else if (n == 7 && (url[6] == 's' || url[6] == 'S')) u->tls = 1; /* webdavs */
        s = p + 3;
    }
    /* host[:port] up to the first '/' */
    for (p = s; *p && *p != '/'; p++) { }
    {
        const char *colon = 0, *q;
        for (q = s; q < p; q++) if (*q == ':') colon = q;
        n = (int)((colon ? colon : p) - s);
        if (n <= 0 || n >= (int)sizeof u->host) return 0;
        memcpy(u->host, s, (unsigned long)n);
        u->host[n] = 0;
        u->port = colon ? (int)ux_u64(colon + 1) : (u->tls ? 443 : 80);
    }
    ux_cpy(u->path, (int)sizeof u->path, *p ? p : "/");
    return 1;
}

/* ===========================================================================
 * A request.
 *
 * One method, one URL, an optional body, and a SINK for the response body -
 * because a download must not be required to fit in a return value.  The sink
 * is what lets the same code path serve "read a 400-byte PROPFIND into a
 * buffer" and "stream 900 MB onto a disk".
 * ======================================================================== */
typedef int (*hx_sink)(void *ctx, const unsigned char *d, int n);

typedef struct {
    const char *method;
    const char *body;               /* NUL-terminated, or 0                   */
    int         bodylen;
    const char *extra;              /* extra headers, each CRLF-terminated    */
    long long   range_from;         /* >0 = a Range request (resume)          */
    hx_sink     sink;
    void       *sink_ctx;
    unoxfer_prog *prog;
    /* filled by the call */
    int         status;
    long long   length;             /* Content-Length, or -1                  */
    char        location[HX_URLCAP];
} hxreq;

static int hx_connect(unoxfer_client *c, const hxurl *u, hxio *io)
{
    unsigned char ip[4];
    long t0;

    memset(io, 0, sizeof *io);
    io->sock = -1;
    if (!pc64_net_up()) return ux_fail(c, UNOXFER_EIO, "no network");
    if (!ux_resolve(u->host, ip))
        return ux_failf(c, UNOXFER_EIO, "cannot resolve %s", u->host);

    if (u->tls) {
        io->tls = tls_open(ip, (unsigned short)u->port, u->host, TLS_TRUST_CA);
        if (!io->tls) return ux_failf(c, UNOXFER_EIO, "TLS refused (%d)", tls_open_error());
    } else {
        io->sock = net_socket(SOCK_TCP);
        if (io->sock < 0) return ux_fail(c, UNOXFER_EIO, "no socket");
        if (net_connect(io->sock, ip, (unsigned short)u->port) != 0) {
            hx_shut(io);
            return ux_failf(c, UNOXFER_EIO, "connect to %s:%d refused", u->host, u->port);
        }
    }
    t0 = TickCount();
    for (;;) {
        int r;
        ux_pump(1);
        r = hx_up(io);
        if (r == 1) return UNOXFER_OK;
        if (r < 0) { hx_shut(io); return ux_failf(c, UNOXFER_EIO,
                       "connection to %s failed", u->host); }
        if (TickCount() - t0 > 60 * 30) {
            /* Name the SOCKET STATE, not just "timed out".  SYN_SENT means the
             * SYN went out and nothing came back (a firewall, or nothing
             * listening); CLOSED means the stack never got as far as sending
             * one.  Those are different problems and a bare timeout hides
             * which - it cost a debugging round here exactly once. */
            int st = io->tls ? -1 : net_sock_state(io->sock);
            hx_shut(io);
            return ux_failf(c, UNOXFER_EIO,
                            "timed out connecting to %s:%d (sock state %d)",
                            u->host, u->port, st);
        }
    }
}

/* Read the status line and headers, stopping exactly at the blank line.  What
 * follows in the same read is BODY and is handed straight to the sink - losing
 * it is the classic first bug of a hand-rolled HTTP client. */
static int hx_headers(unoxfer_client *c, hxio *io, hxreq *r,
                      unsigned char *spill, int *spilln, int spillcap)
{
    char hdr[HX_HDRCAP];
    int n = 0, end = -1, i;
    long t0 = TickCount();

    r->status = 0; r->length = -1; r->location[0] = 0;
    while (n < (int)sizeof hdr - 1) {
        int got;
        ux_pump(1);
        if (io->tls && tls_poll(io->tls) < 0) break;
        got = hx_recv(io, hdr + n, (int)sizeof hdr - 1 - n);
        if (got < 0) break;
        if (got == 0) {
            if (TickCount() - t0 > 60 * 30) break;
            continue;
        }
        t0 = TickCount();
        n += got;
        hdr[n] = 0;
        for (i = 3; i < n; i++)
            if (hdr[i - 3] == '\r' && hdr[i - 2] == '\n' &&
                hdr[i - 1] == '\r' && hdr[i] == '\n') { end = i + 1; break; }
        if (end >= 0) break;
    }
    if (end < 0) return ux_fail(c, UNOXFER_EIO, "no HTTP response");

    /* status */
    for (i = 0; i < end && hdr[i] != ' '; i++) { }
    r->status = (int)ux_u64(hdr + i + 1);

    /* the two headers this client acts on.  Case-insensitive by scanning for
     * both spellings rather than by lower-casing the buffer, which would
     * corrupt a Location that matters. */
    {
        char *p = hdr;
        hdr[end - 1] = 0;
        for (p = hdr; p < hdr + end; ) {
            char *line = p, *nl = strstr(p, "\r\n");
            if (!nl) break;
            *nl = 0;
            {
                static const char *kLen = "content-length:";
                static const char *kLoc = "location:";
                int j;
                char low[24];
                for (j = 0; j < 23 && line[j] && line[j] != ':'; j++)
                    low[j] = (char)(line[j] >= 'A' && line[j] <= 'Z' ? line[j] + 32 : line[j]);
                low[j] = line[j] == ':' ? ':' : 0;
                if (line[j] == ':') low[j + 1] = 0;
                if (ux_eq(low, kLen)) r->length = (long long)ux_u64(line + j + 1);
                else if (ux_eq(low, kLoc)) {
                    char *v = line + j + 1;
                    while (*v == ' ') v++;
                    ux_cpy(r->location, (int)sizeof r->location, v);
                }
            }
            p = nl + 2;
        }
    }

    /* whatever arrived after the blank line is body */
    *spilln = n - end;
    if (*spilln > spillcap) *spilln = spillcap;
    if (*spilln > 0) memcpy(spill, hdr + end, (unsigned long)*spilln);
    return UNOXFER_OK;
}

static int hx_do(unoxfer_client *c, const char *url, hxreq *r, int depth)
{
    hxurl u;
    hxio  io;
    char  req[HX_HDRCAP];
    unsigned char spill[HX_HDRCAP];
    int   spilln = 0, rc, n;
    long long got = 0;
    volatile int *cancel = r->prog ? &r->prog->cancel : 0;
    long t0;

    if (depth > 4) return ux_fail(c, UNOXFER_EIO, "too many redirects");
    if (!hx_url(url, &u)) return ux_failf(c, UNOXFER_EARG, "cannot parse URL: %s", url);
    rc = hx_connect(c, &u, &io);
    if (rc != UNOXFER_OK) return rc;

    n = snprintf(req, sizeof req,
                 "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: UnoDOS-unoxfer/1\r\n"
                 "Accept: */*\r\nConnection: close\r\n",
                 r->method, u.path, u.host);
    if (r->range_from > 0)
        n += snprintf(req + n, sizeof req - (unsigned long)n,
                      "Range: bytes=%lld-\r\n", r->range_from);
    if (r->extra)
        n += snprintf(req + n, sizeof req - (unsigned long)n, "%s", r->extra);
    if (r->body)
        n += snprintf(req + n, sizeof req - (unsigned long)n,
                      "Content-Length: %d\r\n", r->bodylen);
    n += snprintf(req + n, sizeof req - (unsigned long)n, "\r\n");

    if (hx_send_all(&io, req, n, cancel) < 0) {
        hx_shut(&io); return ux_fail(c, UNOXFER_EIO, "sending the request failed");
    }
    if (r->body && hx_send_all(&io, r->body, r->bodylen, cancel) < 0) {
        hx_shut(&io); return ux_fail(c, UNOXFER_EIO, "sending the body failed");
    }

    rc = hx_headers(c, &io, r, spill, &spilln, (int)sizeof spill);
    if (rc != UNOXFER_OK) { hx_shut(&io); return rc; }

    if ((r->status == 301 || r->status == 302 || r->status == 303 ||
         r->status == 307 || r->status == 308) && r->location[0]) {
        char next[HX_URLCAP];
        ux_cpy(next, (int)sizeof next, r->location);
        hx_shut(&io);
        /* A Location without a scheme is relative to this origin.  Building it
         * here rather than in the caller keeps every redirect on one code
         * path, including the ones a server sends for a directory. */
        if (!strstr(next, "://")) {
            char abs[HX_URLCAP];
            snprintf(abs, sizeof abs, "%s://%s:%d%s%s",
                     u.tls ? "https" : "http", u.host, u.port,
                     next[0] == '/' ? "" : "/", next);
            ux_cpy(next, (int)sizeof next, abs);
        }
        if (r->status != 307 && r->status != 308) { r->method = "GET"; r->body = 0; }
        return hx_do(c, next, r, depth + 1);
    }

    if (r->status >= 400) {
        hx_shut(&io);
        return ux_failf(c, r->status == 401 || r->status == 403 ? UNOXFER_EAUTH :
                           r->status == 404 ? UNOXFER_ENOENT : UNOXFER_EPERM,
                        "HTTP %d for %s", r->status, url);
    }
    if (r->prog) { r->prog->done = 0;
                   r->prog->total = r->length > 0 ? (unsigned long long)r->length : 0ull; }

    /* A HEAD HAS NO BODY, so the read loop below must not run: it would wait
     * for Content-Length bytes that will never come, watch the server close,
     * and then report the answer it was given as a TRUNCATED transfer.  That
     * turned every plan's byte total into 0 and every progress line into
     * "23/0", which reads as a server that would not say how big the file is
     * rather than as a bug on this side. */
    if (r->method[0] == 'H' && r->method[1] == 'E' && r->method[2] == 'A') {
        hx_shut(&io);
        return UNOXFER_OK;
    }

    if (spilln > 0) {
        if (r->sink && r->sink(r->sink_ctx, spill, spilln) < 0) {
            hx_shut(&io); return ux_fail(c, UNOXFER_ENOSPC, "the sink refused the body");
        }
        got = spilln;
        if (r->prog) r->prog->done = (unsigned long long)got;
    }

    t0 = TickCount();
    for (;;) {
        unsigned char buf[4096];
        int up;
        if (cancel && *cancel) { hx_shut(&io); return ux_fail(c, UNOXFER_ECANCEL, "cancelled"); }
        ux_pump(0);
        up = hx_up(&io);
        n = hx_recv(&io, buf, (int)sizeof buf);
        if (n > 0) {
            t0 = TickCount();
            if (r->sink && r->sink(r->sink_ctx, buf, n) < 0) {
                hx_shut(&io); return ux_fail(c, UNOXFER_ENOSPC, "the sink refused the body");
            }
            got += n;
            if (r->prog) r->prog->done = (unsigned long long)got;
            continue;
        }
        if (n < 0 || up < 0) break;                     /* the peer closed    */
        if (r->length >= 0 && got >= r->length) break;   /* we have it all    */
        ux_pump(1);                                     /* nothing arrived    */
        if (TickCount() - t0 > 60 * 30) {
            hx_shut(&io);
            return ux_failf(c, UNOXFER_EIO, "stalled after %lld bytes", got);
        }
    }
    hx_shut(&io);

    /* A short read is a FAILURE, loudly.  "Connection: close" plus a truncated
     * body is exactly what a proxy timing out looks like, and a transfer
     * client that commits those bytes as a complete file is worse than one
     * that cannot download at all. */
    if (r->length >= 0 && got < r->length)
        return ux_failf(c, UNOXFER_EIO, "truncated: got %lld of %lld bytes",
                        got, r->length);
    r->length = got;
    return UNOXFER_OK;
}

/* ===========================================================================
 * Sinks: one into memory, one into the staging buffer for a file.
 * ======================================================================== */
typedef struct { unsigned char *p; int cap, len; } memsink;

static int sink_mem(void *ctx, const unsigned char *d, int n)
{
    memsink *m = (memsink *)ctx;
    int room = m->cap - m->len - 1;
    if (n > room) n = room;
    if (n <= 0) return 0;                 /* full: keep draining, discard tail */
    memcpy(m->p + m->len, d, (unsigned long)n);
    m->len += n;
    m->p[m->len] = 0;
    return n;
}

typedef struct { unsigned char *p; long long cap, len; } stagesink;

static int sink_stage(void *ctx, const unsigned char *d, int n)
{
    stagesink *s = (stagesink *)ctx;
    if (s->len + n > s->cap) return -1;   /* over the cap: fail, do not trim  */
    memcpy(s->p + s->len, d, (unsigned long)n);
    s->len += n;
    return n;
}

/* ===========================================================================
 * The plain HTTP(S) backend: fetch a URL that is already a file.
 *
 * No listing (a web server's index page is HTML, not a directory, and
 * pretending otherwise is how a "recursive download" becomes a crawler - and
 * crawling is exactly the half of Portage that was left out on purpose).
 * ======================================================================== */
typedef struct { char base[HX_URLCAP]; } hb;

static int hb_open(unoxfer_client *c, const unoxfer_site *s)
{
    hb *b = (hb *)malloc(sizeof *b);
    if (!b) return ux_fail(c, UNOXFER_EIO, "out of memory");
    memset(b, 0, sizeof *b);
    /* THE PORT HAS TO SURVIVE.  A site built from "webdav://host:8497/" holds
     * the port in site->port, not in site->host, so a base rebuilt from the
     * host alone silently dials 80 - and the only symptom is a connect
     * timeout to an address that is plainly reachable.  Cost an afternoon
     * exactly once; the explicit ":port" is why it will not again. */
    if (strstr(s->host, "://")) {
        ux_cpy(b->base, (int)sizeof b->base, s->host);
    } else {
        const char *scheme = (s->proto == UNOXFER_HTTPS ||
                              s->proto == UNOXFER_WEBDAVS) ? "https" : "http";
        int dflt = unoxfer_proto_port(s->proto);
        if (s->port && s->port != dflt)
            snprintf(b->base, sizeof b->base, "%s://%s:%d", scheme, s->host, s->port);
        else
            snprintf(b->base, sizeof b->base, "%s://%s", scheme, s->host);
    }
    c->impl = b;
    /* Range requests are plumbed and correct, and RESUME is still a lie until
     * unofs lands its append: without it a resumed body is staged on its own
     * and committed as the WHOLE file, i.e. the tail replaces the file.  So the
     * capability is advertised only when streaming is live.  A capability that
     * is true in a future build and false in this one has to be computed, not
     * declared in the table. */
    if (!unoxfer_streaming()) c->caps &= ~(unsigned)UNOXFER_CAP_RESUME;
    return UNOXFER_OK;
}

static void hb_close(unoxfer_client *c)
{ if (c->impl) { free(c->impl); c->impl = 0; } }

static int hb_url(unoxfer_client *c, char *out, int cap, const char *path)
{
    hb *b = (hb *)c->impl;
    if (strstr(path, "://")) return ux_cpy(out, cap, path);
    return snprintf(out, (unsigned long)cap, "%s%s%s", b->base,
                    path[0] == '/' ? "" : "/", path) > 0;
}

static long long hb_size(unoxfer_client *c, const char *rpath)
{
    char url[HX_URLCAP];
    hxreq r;
    memset(&r, 0, sizeof r);
    r.method = "HEAD";
    if (!hb_url(c, url, (int)sizeof url, rpath)) return UNOXFER_EARG;
    if (hx_do(c, url, &r, 0) != UNOXFER_OK) return UNOXFER_ENOENT;
    return r.length;
}

static int hb_get(unoxfer_client *c, const char *rpath, long long off,
                  int vol, const char *lpath, unoxfer_prog *p)
{
    char url[HX_URLCAP];
    hxreq r;
    stagesink st;
    long long capn = 0;
    int rc;

    if (!hb_url(c, url, (int)sizeof url, rpath))
        return ux_fail(c, UNOXFER_EARG, "URL too long");

    st.p = ux_stage_get(0, &capn);
    if (!st.p) return ux_fail(c, UNOXFER_EIO,
                              "no staging buffer (another transfer holds it, or the heap is full)");
    st.cap = capn; st.len = 0;

    memset(&r, 0, sizeof r);
    r.method = "GET";
    r.range_from = off;
    r.sink = sink_stage;
    r.sink_ctx = &st;
    r.prog = p;
    if (p) ux_cpy(p->file, (int)sizeof p->file, unoxfer_basename(rpath));

    rc = hx_do(c, url, &r, 0);
    if (rc != UNOXFER_OK) {
        ux_stage_put();
        /* sink_stage's only failure is "over the cap", and saying so beats
         * "the sink refused the body", which describes the mechanism instead
         * of the problem. */
        if (rc == UNOXFER_ENOSPC)
            return ux_failf(c, UNOXFER_ETOOBIG,
                            "%s is larger than the %lld byte staging cap",
                            unoxfer_basename(rpath), capn);
        return rc;
    }
    rc = ux_commit_file(vol, lpath, st.p, (long)st.len);
    ux_stage_put();
    if (rc != UNOXFER_OK) return ux_failf(c, rc, "write failed: %s", lpath);
    return UNOXFER_OK;
}

const unoxfer_backend unoxfer_be_http = {
    "http",
    UNOXFER_CAP_GET | UNOXFER_CAP_SIZE | UNOXFER_CAP_RESUME,
    hb_open, hb_close, 0, hb_size, hb_get, 0, 0, 0
};

/* ===========================================================================
 * WebDAV.
 *
 * Listing is PROPFIND Depth:1, whose response is XML.  This does NOT parse
 * XML: it scans for <D:href> (in whatever namespace prefix the server used)
 * and the two properties that matter, which is what every small WebDAV client
 * does and is honest about its limits - a server that sends the href in an
 * unusual entity encoding will produce a name this cannot read, and that entry
 * is SKIPPED rather than guessed at.
 * ======================================================================== */
/* Is `tag` present anywhere inside this one <response> element?  A blunt
 * substring scan, bounded to the element, which is all "does it carry
 * <D:collection/>" needs and is not pretending to be an XML query. */
static int dav_has(const char *entry, const char *end, const char *tag)
{
    const char *q = strstr(entry, tag);
    return q && q < end;
}

/* find "<...:name" or "<name" between p and end; returns the byte after '>' */
static const char *dav_open(const char *p, const char *end, const char *name)
{
    while (p && p < end) {
        const char *lt = 0, *q;
        for (q = p; q < end; q++) if (*q == '<') { lt = q; break; }
        if (!lt) return 0;
        {
            const char *n = lt + 1, *colon = 0, *gt = 0, *r;
            if (*n == '/') { p = lt + 1; continue; }
            for (r = n; r < end && *r != '>' && *r != ' '; r++)
                if (*r == ':') colon = r;
            gt = r;
            while (gt < end && *gt != '>') gt++;
            if (gt >= end) return 0;
            {
                const char *tag = colon ? colon + 1 : n;
                unsigned long tl = (unsigned long)(r - tag);
                if (tl == strlen(name)) {
                    unsigned long k;
                    int same = 1;
                    for (k = 0; k < tl; k++) {
                        char a = tag[k], b = name[k];
                        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                        if (a != b) { same = 0; break; }
                    }
                    if (same) return gt + 1;
                }
            }
            p = gt + 1;
        }
    }
    return 0;
}

/* percent-decode into dst; the href a server returns is URL-encoded */
static void dav_unescape(char *dst, int cap, const char *src, int n)
{
    int i = 0, o = 0;
    for (; i < n && o < cap - 1; i++) {
        if (src[i] == '%' && i + 2 < n) {
            int h = 0, k;
            for (k = 1; k <= 2; k++) {
                char ch = src[i + k];
                h <<= 4;
                if (ch >= '0' && ch <= '9') h |= ch - '0';
                else if (ch >= 'a' && ch <= 'f') h |= ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') h |= ch - 'A' + 10;
                else { h = -1; break; }
            }
            if (h >= 0) { dst[o++] = (char)h; i += 2; continue; }
        }
        dst[o++] = src[i];
    }
    dst[o] = 0;
}

static int dav_list(unoxfer_client *c, const char *path,
                    unoxfer_ent *out, int max, int *total)
{
    static const char kBody[] =
        "<?xml version=\"1.0\"?><D:propfind xmlns:D=\"DAV:\"><D:prop>"
        "<D:resourcetype/><D:getcontentlength/></D:prop></D:propfind>";
    char url[HX_URLCAP];
    hxreq r;
    memsink m;
    unsigned char *body;
    const char *p, *end;
    int n = 0, wrote = 0, rc;
    int baselen;

    if (!hb_url(c, url, (int)sizeof url, path))
        return ux_fail(c, UNOXFER_EARG, "URL too long");

    body = (unsigned char *)malloc(64 * 1024);
    if (!body) return ux_fail(c, UNOXFER_EIO, "out of memory");
    m.p = body; m.cap = 64 * 1024; m.len = 0; body[0] = 0;

    memset(&r, 0, sizeof r);
    r.method = "PROPFIND";
    r.extra  = "Depth: 1\r\nContent-Type: application/xml\r\n";
    r.body   = kBody;
    r.bodylen = (int)sizeof kBody - 1;
    r.sink = sink_mem;
    r.sink_ctx = &m;
    rc = hx_do(c, url, &r, 0);
    if (rc != UNOXFER_OK) { free(body); return rc; }

    /* The FIRST response element is the collection itself; skipping it is what
     * stops a listing from containing its own directory and a recursive walk
     * from never terminating. */
    baselen = (int)strlen(path);
    p = (const char *)body;
    end = (const char *)body + m.len;
    for (;;) {
        const char *resp = dav_open(p, end, "response"), *href, *rend;
        char raw[512], dec[512];
        int hl;
        if (!resp) break;
        rend = dav_open(resp, end, "response");
        if (!rend) rend = end;
        href = dav_open(resp, rend, "href");
        if (!href) { p = resp; continue; }
        for (hl = 0; href + hl < rend && href[hl] != '<' && hl < 511; hl++) raw[hl] = href[hl];
        raw[hl] = 0;
        dav_unescape(dec, (int)sizeof dec, raw, hl);
        {
            const char *leaf = dec;
            int dl = (int)strlen(dec), isdir;
            while (dl > 1 && dec[dl - 1] == '/') { dec[--dl] = 0; }
            leaf = unoxfer_basename(dec);
            isdir = dav_has(resp, rend, "collection") ||
                    dav_has(resp, rend, "Collection");
            if (!*leaf || (dl <= baselen && isdir)) { p = resp; continue; }
            n++;
            if (wrote < max) {
                memset(&out[wrote], 0, sizeof out[wrote]);
                ux_cpy(out[wrote].name, (int)sizeof out[wrote].name, leaf);
                out[wrote].is_dir = (unsigned char)(isdir != 0);
                if (!isdir) {
                    const char *len = dav_open(resp, rend, "getcontentlength");
                    if (len) out[wrote].size = ux_u64(len);
                }
                wrote++;
            }
        }
        p = resp;
    }
    free(body);
    if (total) *total = n;
    return wrote;
}

static int dav_put(unoxfer_client *c, int vol, const char *lpath,
                   const char *rpath, unoxfer_prog *p)
{
    char url[HX_URLCAP];
    hxreq r;
    unsigned char *buf;
    long long capn = 0;
    long sz = uno_fs_size(vol, lpath), n;
    int rc;

    if (sz < 0) return ux_failf(c, UNOXFER_ENOENT, "no such local file: %s", lpath);
    if (!hb_url(c, url, (int)sizeof url, rpath))
        return ux_fail(c, UNOXFER_EARG, "URL too long");
    buf = ux_stage_get(sz, &capn);
    if (!buf) return ux_fail(c, UNOXFER_EIO,
                             "no staging buffer (another transfer holds it, or the heap is full)");
    if (sz > capn) { ux_stage_put();
        return ux_failf(c, UNOXFER_ETOOBIG, "%s is over the %lld byte staging cap",
                        lpath, capn); }
    n = uno_fs_read(vol, lpath, buf, sz);
    if (n < 0) { ux_stage_put(); return ux_failf(c, UNOXFER_EIO, "read failed: %s", lpath); }

    memset(&r, 0, sizeof r);
    r.method = "PUT";
    r.body = (const char *)buf;
    r.bodylen = (int)n;
    r.extra = "Content-Type: application/octet-stream\r\n";
    r.prog = p;
    if (p) { ux_cpy(p->file, (int)sizeof p->file, unoxfer_basename(lpath));
             p->total = (unsigned long long)n; p->done = 0; }
    rc = hx_do(c, url, &r, 0);
    ux_stage_put();
    if (rc == UNOXFER_OK && p) p->done = (unsigned long long)n;
    return rc;
}

static int dav_mkdir(unoxfer_client *c, const char *path)
{
    char url[HX_URLCAP];
    hxreq r;
    int rc;
    if (!hb_url(c, url, (int)sizeof url, path))
        return ux_fail(c, UNOXFER_EARG, "URL too long");
    memset(&r, 0, sizeof r);
    r.method = "MKCOL";
    rc = hx_do(c, url, &r, 0);
    /* 405 is "it is already there", which for a mkdir is success.  Treating it
     * as an error is how a re-run of a working recipe fails. */
    if (rc != UNOXFER_OK && r.status == 405) return UNOXFER_OK;
    return rc;
}

static int dav_del(unoxfer_client *c, const char *path)
{
    char url[HX_URLCAP];
    hxreq r;
    if (!hb_url(c, url, (int)sizeof url, path))
        return ux_fail(c, UNOXFER_EARG, "URL too long");
    memset(&r, 0, sizeof r);
    r.method = "DELETE";
    return hx_do(c, url, &r, 0);
}

const unoxfer_backend unoxfer_be_webdav = {
    "webdav",
    UNOXFER_CAP_LIST | UNOXFER_CAP_GET | UNOXFER_CAP_PUT | UNOXFER_CAP_MKDIR |
    UNOXFER_CAP_DELETE | UNOXFER_CAP_SIZE | UNOXFER_CAP_RESUME,
    hb_open, hb_close, dav_list, hb_size, hb_get, dav_put, dav_mkdir, dav_del
};
