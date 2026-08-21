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
 * uc_http.h - HTTPS requests, over uc_net.h, shared by both platforms.
 *
 * This is the layer pc64_http.c should have been and could not be: it takes
 * ANY method, ANY headers and a body of any size and content type, because the
 * caller knows what it is sending and this does not.  The browser's client
 * hard-codes a form-encoded POST with no header hook, which is exactly why the
 * one other thing in the OS that needed `x-api-key` went around it and wrote
 * its own transport (pc64/apps/studio_ai.c).  That transport is where most of
 * this came from; what is new is the widening.
 *
 * EVERY CALL IS NON-BLOCKING, with one stated exception below.  A request is a
 * handle you advance from the frame loop:
 *
 *     uc_http *h = uc_http_begin(&req);
 *     while (uc_http_poll(h) == UC_HTTP_PENDING) { uc_net_pump(); draw(); }
 *     if (uc_http_status(h) == 200) body = uc_http_body(h, &len);
 *     uc_http_free(h);
 *
 * THE EXCEPTION IS DNS.  uc_http_begin() resolves the host name through
 * uc_net_resolve(), and that call blocks - getaddrinfo does on a host, and
 * net_dns_query does on the device.  A warm cache makes it invisible and a
 * cold one can cost seconds.  It is the last blocking step in a request and it
 * is UCD-47's to remove; until then, a caller that cares should resolve once
 * and reuse the address, which is why uc_http_req takes an optional one.
 *
 * STREAMING IS THE POINT OF uc_http_on_event().  A response can be read whole
 * at the end, which is what a small JSON reply wants, or delivered event by
 * event as it arrives, which is what a token stream wants.  The second is not
 * a variation on the first: an SSE body is chunked AND still arriving, so the
 * decoder has to hand out events mid-transfer.  pc64_http.c's progress
 * callback explicitly refuses chunked responses and so could not do this.
 * ======================================================================== */
#ifndef UC_HTTP_H
#define UC_HTTP_H

#include "uc_net.h"

enum {
    UC_HTTP_PENDING = 0,   /* still going - pump and call again              */
    UC_HTTP_DONE    = 1    /* finished; ask uc_http_status() what happened   */
};

/* Failures reuse uc_net.h's negative codes where they apply (ENOLINK, ENODNS,
 * ENOENTROPY, ETRUST, ENOMEM, ERR) and add these. */
enum {
    UC_HTTP_EPROTO   = -20,   /* the response was not HTTP we can follow     */
    UC_HTTP_ETOOBIG  = -21    /* the body exceeded the caller's cap          */
};

typedef struct { const char *name, *value; } uc_header;

typedef struct {
    const char *host;          /* name for DNS *and* the TLS certificate     */
    const unsigned char *ip;   /* optional: 4 bytes, skips the blocking DNS  */
    unsigned short port;       /* 0 means 443                                */
    const char *method;        /* "GET", "POST", ... (0 means "GET")         */
    const char *path;          /* "/v1/messages"                             */
    const uc_header *headers;  /* Host, Content-Length and Connection are    */
    int nheaders;              /*   supplied for you - do not repeat them    */
    const char *body;          /* may be 0                                   */
    int body_len;              /* <0 means strlen(body)                      */
    int max_body;              /* response cap, 0 = a sane default           */
} uc_http_req;

typedef struct uc_http uc_http;

/* Start a request.  Copies everything it needs, so the caller's buffers need
 * not outlive the call - including the body, which is what lets a caller build
 * a request in a scratch buffer and let it go. */
uc_http *uc_http_begin(const uc_http_req *req);

/* Advance as far as the transport allows right now.  UC_HTTP_PENDING,
 * UC_HTTP_DONE, or negative on failure.  Does not pump the network: call
 * uc_net_pump() once per frame yourself, so one loop drives several requests. */
int uc_http_poll(uc_http *h);

/* The HTTP status once poll() has returned UC_HTTP_DONE, or 0 if the exchange
 * never got that far.  A 4xx IS a completed request: the transport worked and
 * the server said no, and those are different failures with different fixes. */
int uc_http_status(uc_http *h);

/* The body.  NUL-terminated for convenience and `*len` is the true length, so
 * a body containing a zero byte is still measurable.  Valid until free(). */
const char *uc_http_body(uc_http *h, int *len);

/* A short sentence for a human.  Never NULL. */
const char *uc_http_error(uc_http *h);

void uc_http_free(uc_http *h);

/* ---- server-sent events ---------------------------------------------------
 * Install before the first poll().  `event` is the SSE event name ("" when the
 * stream did not give one) and `data` is that event's data with the trailing
 * newline removed.  Both are valid only for the duration of the call.
 *
 * With a handler installed the body is NOT accumulated - a token stream can
 * outrun any cap worth setting, and a caller that wanted the whole thing would
 * not be streaming.  Without one, an SSE response is just a body like any
 * other. */
typedef void (*uc_sse_fn)(void *user, const char *event,
                          const char *data, int len);
void uc_http_on_event(uc_http *h, uc_sse_fn fn, void *user);

/* ---- building a request ---------------------------------------------------
 * A tiny appender, because every caller of this needs one and they were all
 * about to write it again.  `pos` is both the write cursor and the length; it
 * is never advanced past `cap`, so a truncated buffer is detectable with
 * (*pos >= cap) rather than by corrupting memory.
 *
 * uc_buf_json escapes through uc_json_esc and adds the quotes, which is the
 * only correct way to put arbitrary text - a file, an error message, anything
 * a user typed - into a request body. */
void uc_buf_raw (char *buf, int *pos, int cap, const char *s);
void uc_buf_n   (char *buf, int *pos, int cap, const char *s, int n);
void uc_buf_int (char *buf, int *pos, int cap, long v);
void uc_buf_json(char *buf, int *pos, int cap, const char *s);

#endif
