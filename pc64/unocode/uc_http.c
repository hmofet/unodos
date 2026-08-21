/*
 * VENDORED FILE - DO NOT EDIT HERE.
 *
 * UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
 * An edit made here is lost at the next sync, and until then it silently
 * forks the editor away from the tree the desktop builds are cut from.
 *
 * Change it there; bring it back with pc64/tools/sync_unocode.py.
 * See pc64/UNOCODE-UPSTREAM.md.
 */
/* ===========================================================================
 * uc_http.c - the HTTP/1.1 half of the assistant's transport.
 *
 * A state machine over uc_net.h.  Nothing here waits: every state does what
 * the socket allows this instant and returns, so a request costs the caller
 * one poll per frame and no frames.
 *
 * Two things in here are subtler than they look, and both are the reason this
 * file exists rather than a copy of studio_ai.c's do_request():
 *
 * 1. CHUNKED DECODING HAPPENS AS BYTES ARRIVE, not once at the end.  Studio
 *    reads the whole response, then de-chunks it in place.  That is fine for a
 *    reply you are going to show all at once and useless for a stream, because
 *    the whole point of a stream is that there is no "end" to wait for.  The
 *    decoder below is a state machine over the chunk framing, fed whatever the
 *    socket had.
 *
 * 2. AN SSE EVENT CAN ARRIVE IN PIECES.  `data: {"type":...` may be split
 *    across TCP segments, across chunk boundaries, or both, in any
 *    combination.  So events are assembled in a line buffer and only dispatched
 *    on a blank line, which is what the SSE framing actually says.  A decoder
 *    that assumed one read equals one event would work perfectly on loopback
 *    and fail intermittently over the internet, which is the worst way to
 *    fail.
 * ======================================================================== */
/* unocode.h declares malloc/realloc/free/memcpy/memset/strlen/strncmp for the
 * freestanding build, and they are the real ones on a hosted one.  No
 * <stdlib.h> here on purpose: this file compiles into a .UNO module where
 * there is no libc to include. */
#include "unocode.h"
#include "uc_http.h"

#define DEF_MAX_BODY (1024 * 1024)
#define LINE_CAP     4096
#define READ_CHUNK   2048

enum {
    S_RESOLVE, S_CONNECT, S_SEND, S_STATUS, S_HEADERS, S_BODY, S_CHUNK_SIZE,
    S_CHUNK_DATA, S_CHUNK_CRLF, S_DONE, S_FAILED
};

struct uc_http {
    uc_conn *conn;
    int   state;
    int   err;                    /* a negative UC_* code once S_FAILED     */

    char *req;                    /* the whole request, headers and body    */
    int   reqlen, reqsent;

    char  host[256];              /* kept: the SNI, and the name being looked up */
    unsigned short port;
    int   resolving;              /* a lookup of OURS is in flight          */

    char *body;                   /* the response body, grown as it arrives */
    int   blen, bcap, maxbody;

    int   status;
    int   chunked;
    long  content_len;            /* -1 when the server did not say         */
    long  chunk_left;

    char  line[LINE_CAP];         /* one header, or one SSE line            */
    int   linelen;

    /* SSE assembly */
    uc_sse_fn sse;
    void     *sse_user;
    char      ev[128];            /* the current event: name ...            */
    int       evlen;
    char     *dat;                /* ... and its accumulated data           */
    int       datlen, datcap;

    const char *msg;
};

/* ---- the little appender -------------------------------------------------- */

void uc_buf_n(char *buf, int *pos, int cap, const char *s, int n)
{
    int i;
    for (i = 0; i < n && *pos < cap - 1; i++) buf[(*pos)++] = s[i];
    if (*pos < cap) buf[*pos] = 0;
}

void uc_buf_raw(char *buf, int *pos, int cap, const char *s)
{
    if (s) uc_buf_n(buf, pos, cap, s, (int)strlen(s));
}

void uc_buf_int(char *buf, int *pos, int cap, long v)
{
    char t[24];
    int n = 0;
    unsigned long u;
    if (v < 0) { uc_buf_n(buf, pos, cap, "-", 1); u = (unsigned long)(-v); }
    else u = (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (u % 10)); u /= 10; }
    while (n) uc_buf_n(buf, pos, cap, &t[--n], 1);
}

void uc_buf_json(char *buf, int *pos, int cap, const char *s)
{
    int n;
    if (*pos >= cap - 1) return;
    n = uc_json_esc(buf + *pos, cap - *pos, s ? s : "");
    if (n > 0) *pos += n;
    if (*pos < cap) buf[*pos] = 0;
}

/* ---- growth --------------------------------------------------------------- */

static int grow(char **p, int *cap, int need)
{
    int c = *cap ? *cap : 1024;
    char *n;
    while (c < need) c *= 2;
    if (c == *cap) return 1;
    n = (char *)realloc(*p, (unsigned long)c);
    if (!n) return 0;
    *p = n; *cap = c;
    return 1;
}

static void fail(uc_http *h, int code, const char *why)
{
    h->state = S_FAILED;
    h->err = code;
    h->msg = why;
}

static int open_conn(uc_http *h, const unsigned char ip[4]);

/* Release the name lookup, if this handle is the one holding it.  Guarded by
 * `resolving` because the resolver is a single global slot: a handle that
 * never started a lookup, or has already finished one, must not end somebody
 * else's. */
static void drop_resolve(uc_http *h)
{
    if (h->resolving) { uc_net_resolve_end(); h->resolving = 0; }
}

/* ---- building the request ------------------------------------------------- */

static int build_request(uc_http *h, const uc_http_req *r, int blen)
{
    int cap = blen + 1024, pos = 0, i;
    for (i = 0; i < r->nheaders; i++)
        cap += (int)strlen(r->headers[i].name) +
               (int)strlen(r->headers[i].value) + 4;

    h->req = (char *)malloc((unsigned long)cap);
    if (!h->req) return 0;

    uc_buf_raw(h->req, &pos, cap, r->method ? r->method : "GET");
    uc_buf_raw(h->req, &pos, cap, " ");
    uc_buf_raw(h->req, &pos, cap, r->path ? r->path : "/");
    uc_buf_raw(h->req, &pos, cap, " HTTP/1.1\r\nHost: ");
    uc_buf_raw(h->req, &pos, cap, r->host);
    /* Connection: close, deliberately.  Keep-alive would mean owning a
     * connection pool and deciding when a socket has gone stale, and this
     * client's traffic is a handful of long requests rather than the twenty
     * small ones a web page makes. */
    uc_buf_raw(h->req, &pos, cap, "\r\nConnection: close\r\n");

    for (i = 0; i < r->nheaders; i++) {
        uc_buf_raw(h->req, &pos, cap, r->headers[i].name);
        uc_buf_raw(h->req, &pos, cap, ": ");
        uc_buf_raw(h->req, &pos, cap, r->headers[i].value);
        uc_buf_raw(h->req, &pos, cap, "\r\n");
    }
    if (blen > 0) {
        uc_buf_raw(h->req, &pos, cap, "Content-Length: ");
        uc_buf_int(h->req, &pos, cap, blen);
        uc_buf_raw(h->req, &pos, cap, "\r\n");
    }
    uc_buf_raw(h->req, &pos, cap, "\r\n");

    /* The body is copied raw rather than appended: it is not a C string, it
     * may contain anything, and uc_buf_raw would stop at a zero byte. */
    if (blen > 0 && pos + blen < cap) {
        memcpy(h->req + pos, r->body, (unsigned long)blen);
        pos += blen;
    }
    h->reqlen = pos;
    return 1;
}

uc_http *uc_http_begin(const uc_http_req *r)
{
    unsigned char ip[4];
    uc_http *h;
    int blen;

    if (!r || !r->host || !*r->host) return 0;

    h = (uc_http *)malloc(sizeof *h);
    if (!h) return 0;
    memset(h, 0, sizeof *h);

    blen = r->body ? (r->body_len < 0 ? (int)strlen(r->body) : r->body_len) : 0;
    h->maxbody = r->max_body > 0 ? r->max_body : DEF_MAX_BODY;
    h->content_len = -1;
    h->msg = "no error";

    if (!build_request(h, r, blen)) { free(h); return 0; }

    {   unsigned n = 0;
        while (r->host[n] && n < sizeof h->host - 1) { h->host[n] = r->host[n]; n++; }
        h->host[n] = 0;
    }
    h->port = r->port ? r->port : 443;

    /* NOTHING BLOCKS FROM HERE.  This used to call uc_net_resolve(), which
     * blocks - up to five seconds of retries on a cold cache, which is five
     * seconds the editor cannot scroll, type or repaint.  Every other step was
     * already pumped; this was the last one that was not. */
    if (r->ip) {
        memcpy(ip, r->ip, 4);
    } else {
        int rc = uc_net_resolve_begin(h->host, ip);
        if (rc == 0) { h->resolving = 1; h->state = S_RESOLVE; return h; }
        if (rc < 0) {
            fail(h, rc,
                 rc == UC_NET_EBUSY
                    ? "another name is being looked up - try again in a moment"
                    : rc == UC_NET_ENOLINK ? "there is no network link"
                                           : "that host name did not resolve");
            return h;
        }
    }

    if (!open_conn(h, ip)) return h;
    return h;
}

/* Shared by the immediate path above and the resolved-later path in poll(). */
static int open_conn(uc_http *h, const unsigned char ip[4])
{
    h->conn = uc_tls_open(ip, h->port, h->host);
    if (!h->conn) {
        int e = uc_net_open_error();
        fail(h, e ? e : UC_NET_ERR,
             e == UC_NET_ENOENTROPY
                ? "this machine has no usable random source, so TLS is refused"
                : e == UC_NET_ENOLINK ? "there is no network link"
                                      : "the connection could not be opened");
        return 0;
    }
    h->state = S_CONNECT;
    return 1;
}

void uc_http_on_event(uc_http *h, uc_sse_fn fn, void *user)
{
    if (!h) return;
    h->sse = fn;
    h->sse_user = user;
}

/* ---- SSE ------------------------------------------------------------------ */

static void sse_dispatch(uc_http *h)
{
    if (h->datlen > 0 && h->sse) {
        h->ev[h->evlen] = 0;
        h->dat[h->datlen] = 0;
        h->sse(h->sse_user, h->ev, h->dat, h->datlen);
    }
    h->evlen = 0;
    h->datlen = 0;
}

/* One complete line of an SSE body.  A blank line ends an event; everything
 * else is a `field: value`.  Only `event` and `data` are acted on - `id` and
 * `retry` exist and neither matters to a request that is not resumed. */
static void sse_line(uc_http *h, const char *s, int n)
{
    int off;
    if (n == 0) { sse_dispatch(h); return; }
    if (s[0] == ':') return;                         /* a comment / keepalive */

    if (n >= 5 && strncmp(s, "data:", 5) == 0) {
        off = 5;
        if (off < n && s[off] == ' ') off++;         /* one optional space    */
        if (h->datlen && h->datlen + 1 < h->datcap) h->dat[h->datlen++] = '\n';
        if (!grow(&h->dat, &h->datcap, h->datlen + (n - off) + 2)) return;
        memcpy(h->dat + h->datlen, s + off, (unsigned long)(n - off));
        h->datlen += n - off;
    } else if (n >= 6 && strncmp(s, "event:", 6) == 0) {
        off = 6;
        if (off < n && s[off] == ' ') off++;
        h->evlen = n - off;
        if (h->evlen > (int)sizeof h->ev - 1) h->evlen = (int)sizeof h->ev - 1;
        memcpy(h->ev, s + off, (unsigned long)h->evlen);
    }
}

/* ---- the body ------------------------------------------------------------- */

/* Body bytes, however they were framed.  With an SSE handler installed they go
 * through the line assembler and are NOT kept; without one they accumulate.
 *
 * EXCEPT when the status is not 2xx.  An error reply is a BODY, not a stream:
 * a 401's JSON is one line matching no SSE field, so feeding it to the line
 * assembler silently deletes the server's whole explanation - the caller sees
 * "401" and nothing else, which turns a fixable key problem into a mystery.
 * The status always precedes the body, so the choice can be made per byte. */
static int take_body(uc_http *h, const char *p, int n)
{
    int i;

    if (h->sse && h->status / 100 == 2) {
        for (i = 0; i < n; i++) {
            char c = p[i];
            if (c == '\r') continue;                 /* CRLF or LF, both      */
            if (c == '\n') { sse_line(h, h->line, h->linelen); h->linelen = 0; }
            else if (h->linelen < LINE_CAP - 1) h->line[h->linelen++] = c;
        }
        return 1;
    }

    if (h->blen + n > h->maxbody) {
        fail(h, UC_HTTP_ETOOBIG, "the response was larger than the caller's cap");
        return 0;
    }
    if (!grow(&h->body, &h->bcap, h->blen + n + 1)) {
        fail(h, UC_NET_ENOMEM, "out of memory holding the response");
        return 0;
    }
    memcpy(h->body + h->blen, p, (unsigned long)n);
    h->blen += n;
    h->body[h->blen] = 0;
    return 1;
}

/* ---- header parsing ------------------------------------------------------- */

static int ci_starts(const char *s, int n, const char *pre)
{
    int i;
    for (i = 0; pre[i]; i++) {
        int a = i < n ? s[i] : 0, b = pre[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void header_line(uc_http *h, const char *s, int n)
{
    int i;
    if (ci_starts(s, n, "transfer-encoding:")) {
        for (i = 0; i + 6 < n; i++)
            if (ci_starts(s + i, n - i, "chunked")) { h->chunked = 1; break; }
    } else if (ci_starts(s, n, "content-length:")) {
        long v = 0;
        for (i = 15; i < n; i++) {
            if (s[i] == ' ') continue;
            if (s[i] < '0' || s[i] > '9') break;
            v = v * 10 + (s[i] - '0');
        }
        h->content_len = v;
    }
}

/* ---- the machine ---------------------------------------------------------- */

/* Feed one byte of the response.  Returns 0 to stop (finished or failed). */
static int feed(uc_http *h, char c)
{
    switch (h->state) {
    case S_STATUS:
    case S_HEADERS:
        if (c == '\r') return 1;
        if (c != '\n') {
            if (h->linelen < LINE_CAP - 1) h->line[h->linelen++] = c;
            return 1;
        }
        if (h->state == S_STATUS) {
            /* "HTTP/1.1 200 OK" - the code is what matters. */
            int i = 0;
            while (i < h->linelen && h->line[i] != ' ') i++;
            while (i < h->linelen && h->line[i] == ' ') i++;
            h->status = 0;
            while (i < h->linelen && h->line[i] >= '0' && h->line[i] <= '9')
                h->status = h->status * 10 + (h->line[i++] - '0');
            if (h->status < 100) {
                fail(h, UC_HTTP_EPROTO, "the server did not answer with HTTP");
                return 0;
            }
            h->state = S_HEADERS;
        } else if (h->linelen == 0) {
            /* The blank line: headers are over. */
            h->state = h->chunked ? S_CHUNK_SIZE : S_BODY;
            h->chunk_left = 0;
        } else {
            header_line(h, h->line, h->linelen);
        }
        h->linelen = 0;
        return 1;

    case S_BODY:
        if (!take_body(h, &c, 1)) return 0;
        /* the accumulated cases: no handler, or an error reply being kept for
         * the caller (take_body above).  A streamed 2xx has no meaningful
         * Content-Length to complete against - the server ends it. */
        if (h->content_len >= 0 && h->blen >= h->content_len &&
            (!h->sse || h->status / 100 != 2)) {
            h->state = S_DONE;
            return 0;
        }
        return 1;

    case S_CHUNK_SIZE:
        if (c == '\r') return 1;
        if (c != '\n') {
            if (h->linelen < LINE_CAP - 1) h->line[h->linelen++] = c;
            return 1;
        }
        {   long sz = 0; int i;
            for (i = 0; i < h->linelen; i++) {
                int d = h->line[i];
                if (d >= '0' && d <= '9') d -= '0';
                else if (d >= 'a' && d <= 'f') d -= 'a' - 10;
                else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
                else break;                       /* ';' and chunk extensions */
                sz = sz * 16 + d;
            }
            h->linelen = 0;
            if (sz == 0) {                        /* the terminating chunk    */
                if (h->sse) { sse_line(h, "", 0); }
                h->state = S_DONE;
                return 0;
            }
            h->chunk_left = sz;
            h->state = S_CHUNK_DATA;
        }
        return 1;

    case S_CHUNK_DATA:
        if (!take_body(h, &c, 1)) return 0;
        if (--h->chunk_left <= 0) h->state = S_CHUNK_CRLF;
        return 1;

    case S_CHUNK_CRLF:
        if (c == '\n') h->state = S_CHUNK_SIZE;
        return 1;
    }
    return 1;
}

int uc_http_poll(uc_http *h)
{
    char buf[READ_CHUNK];
    int r, n, i;

    if (!h) return UC_NET_ERR;
    if (h->state == S_FAILED) return h->err;
    if (h->state == S_DONE)   return UC_HTTP_DONE;

    if (h->state == S_RESOLVE) {
        unsigned char ip[4];
        int rc = uc_net_resolve_poll(ip);
        if (rc == 0) return UC_HTTP_PENDING;
        drop_resolve(h);
        if (rc < 0) { fail(h, rc, "that host name did not resolve"); return h->err; }
        if (!open_conn(h, ip)) return h->err;
    }

    r = uc_tls_poll(h->conn);
    if (r < 0) { fail(h, r, uc_net_error(h->conn)); return h->err; }

    if (r == UC_NET_PENDING && h->state == S_CONNECT) return UC_HTTP_PENDING;
    if (h->state == S_CONNECT) h->state = S_SEND;

    if (h->state == S_SEND) {
        while (h->reqsent < h->reqlen) {
            n = uc_tls_send(h->conn, h->req + h->reqsent,
                            h->reqlen - h->reqsent);
            if (n < 0) { fail(h, n, "the request could not be sent"); return h->err; }
            if (n == 0) return UC_HTTP_PENDING;    /* the window is full      */
            h->reqsent += n;
        }
        h->state = S_STATUS;
        h->linelen = 0;
    }

    for (;;) {
        n = uc_tls_recv(h->conn, buf, sizeof buf);
        if (n < 0) { fail(h, n, uc_net_error(h->conn)); return h->err; }
        if (n == 0) break;
        for (i = 0; i < n; i++)
            if (!feed(h, buf[i])) break;
        if (h->state == S_DONE)   return UC_HTTP_DONE;
        if (h->state == S_FAILED) return h->err;
    }

    /* A server that closes without Content-Length and without chunking has
     * signalled the end of the body BY closing - HTTP/1.0's framing, and what
     * `Connection: close` invites.  So EOF completes the request rather than
     * failing it, as long as we had a status line. */
    if (r == UC_NET_EOF) {
        if (h->sse) sse_dispatch(h);
        if (h->state == S_BODY || h->state == S_DONE) h->state = S_DONE;
        else if (h->status) h->state = S_DONE;
        else { fail(h, UC_HTTP_EPROTO, "the server closed before answering");
               return h->err; }
        return UC_HTTP_DONE;
    }
    return UC_HTTP_PENDING;
}

int uc_http_status(uc_http *h) { return h ? h->status : 0; }

const char *uc_http_body(uc_http *h, int *len)
{
    if (len) *len = h ? h->blen : 0;
    return (h && h->body) ? h->body : "";
}

const char *uc_http_error(uc_http *h)
{
    return (h && h->msg) ? h->msg : "no error";
}

/* Freeing a handle IS cancelling it, at any point.  A request abandoned while
 * resolving orphans the lookup; one abandoned mid-transfer tears the TLS
 * session down rather than letting it drain into a buffer nobody owns.  There
 * is no separate cancel() because there is nothing for it to do differently -
 * and two ways to stop something is how one of them stops being tested. */
void uc_http_free(uc_http *h)
{
    if (!h) return;
    drop_resolve(h);
    if (h->conn) uc_tls_free(h->conn);
    if (h->req)  free(h->req);
    if (h->body) free(h->body);
    if (h->dat)  free(h->dat);
    free(h);
}
