/* ===========================================================================
 * UnoDOS/pc64 - unoxfer's TFTP backend (RFC 1350, octet mode).
 *
 * The smallest protocol in the set and the one with the fewest promises: no
 * listing, no authentication, no delete, no mkdir, no resume.  Get and put by
 * exact filename, and that is the whole protocol.  It is here because it is
 * what network gear speaks - a switch, a PXE server, a BMC - and because those
 * are exactly the machines a box like this ends up talking to.
 *
 * RFC 1782's block-size option is NOT negotiated: 512-byte blocks, stop and
 * wait, which is slow and is universally supported.  A TFTP transfer is a
 * firmware image or a config file, and correctness on every server beats
 * throughput on some of them.
 *
 * THE ONE TRAP, and it is the classic one: the server answers the request from
 * a NEW ephemeral port, not from port 69.  Every subsequent packet goes to
 * that port, and a client that keeps talking to 69 gets one block and then
 * silence.  net_recvfrom() hands back the sender's port, and the first reply
 * latches it.
 * ======================================================================== */
#include "unoxfer_int.h"
#include "net.h"
#include "netsock.h"
#include "pc64_http.h"          /* pc64_net_up */

void *malloc(unsigned long);
void  free(void *);
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
long  TickCount(void);
int   uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
long  uno_fs_size(int vol, const char *name);

#define TF_BLK      512
#define TF_RETRIES  5
#define TF_TIMEOUT  3000        /* ms per block before a retransmit          */

enum { OP_RRQ = 1, OP_WRQ = 2, OP_DATA = 3, OP_ACK = 4, OP_ERR = 5 };

typedef struct {
    unsigned char ip[4];
    int  port;
} tf;

static int tf_open(unoxfer_client *c, const unoxfer_site *s)
{
    tf *x;
    unsigned char ip[4];
    if (!pc64_net_up()) return ux_fail(c, UNOXFER_EIO, "no network");
    if (!net_dns_query(s->host, ip))
        return ux_failf(c, UNOXFER_EIO, "cannot resolve %s", s->host);
    x = (tf *)malloc(sizeof *x);
    if (!x) return ux_fail(c, UNOXFER_EIO, "out of memory");
    memcpy(x->ip, ip, 4);
    x->port = s->port ? s->port : 69;
    c->impl = x;
    return UNOXFER_OK;
}

static void tf_close(unoxfer_client *c)
{ if (c->impl) { free(c->impl); c->impl = 0; } }

/* build "op | filename\0 | octet\0" */
static int tf_request(unsigned char *p, int cap, int op, const char *name)
{
    int n = 0;
    unsigned long fl = strlen(name);
    if ((int)fl + 10 > cap) return 0;
    p[n++] = 0; p[n++] = (unsigned char)op;
    memcpy(p + n, name, fl); n += (int)fl; p[n++] = 0;
    memcpy(p + n, "octet", 5); n += 5; p[n++] = 0;
    return n;
}

/* Wait for one datagram from the server, latching its ephemeral port the first
 * time.  Returns the payload length, or -1 on timeout. */
static int tf_wait(int sock, tf *x, unsigned char *buf, int cap, int ms, int *latched)
{
    long t0 = TickCount(), limit = (long)ms * 60 / 1000;
    if (limit < 1) limit = 1;
    for (;;) {
        unsigned char src[4];
        unsigned short sp = 0;
        int n;
        net_poll();
        n = net_recvfrom(sock, buf, cap, src, &sp);
        if (n > 0) {
            if (src[0] != x->ip[0] || src[1] != x->ip[1] ||
                src[2] != x->ip[2] || src[3] != x->ip[3]) continue;
            if (!*latched) { x->port = sp; *latched = 1; }
            else if (sp != (unsigned short)x->port) continue;
            return n;
        }
        if (TickCount() - t0 > limit) return -1;
    }
}

static int tf_errpkt(unoxfer_client *c, const unsigned char *p, int n)
{
    char msg[128];
    int i = 0;
    for (; i < n - 4 && i < (int)sizeof msg - 1 && p[4 + i]; i++) msg[i] = (char)p[4 + i];
    msg[i] = 0;
    /* code 1 is "file not found", which callers act on differently from a
     * refusal, so it keeps its own error rather than becoming a generic one. */
    return ux_failf(c, p[3] == 1 ? UNOXFER_ENOENT : UNOXFER_EPERM,
                    "tftp: %s", msg[0] ? msg : "error");
}

static int tf_get(unoxfer_client *c, const char *rpath, long long off,
                  int vol, const char *lpath, unoxfer_prog *p)
{
    tf *x = (tf *)c->impl;
    unsigned char pkt[TF_BLK + 8], *buf;
    long long capn = 0, got = 0;
    int sock, n, latched = 0, tries, rc;
    unsigned block = 1;
    const char *leaf = rpath;
    volatile int *cancel = p ? &p->cancel : 0;

    if (off != 0) return ux_fail(c, UNOXFER_EUNSUP, "tftp cannot resume");
    while (*leaf == '/') leaf++;

    buf = ux_stage_get(0, &capn);
    if (!buf) return ux_fail(c, UNOXFER_EIO, "the staging buffer is busy");

    sock = net_socket(SOCK_UDP);
    if (sock < 0 || net_bind(sock, 0) != 0) {
        ux_stage_put();
        if (sock >= 0) net_sock_close(sock);
        return ux_fail(c, UNOXFER_EIO, "no UDP socket");
    }
    if (p) { ux_cpy(p->file, (int)sizeof p->file, unoxfer_basename(rpath));
             p->done = 0; p->total = 0; }

    n = tf_request(pkt, (int)sizeof pkt, OP_RRQ, leaf);
    if (!n) { ux_stage_put(); net_sock_close(sock);
              return ux_fail(c, UNOXFER_EARG, "filename too long"); }
    net_sendto(sock, x->ip, (unsigned short)x->port, pkt, n);

    for (;;) {
        int len = -1;
        for (tries = 0; tries < TF_RETRIES && len < 0; tries++) {
            if (cancel && *cancel) { ux_stage_put(); net_sock_close(sock);
                                     return ux_fail(c, UNOXFER_ECANCEL, "cancelled"); }
            len = tf_wait(sock, x, pkt, (int)sizeof pkt, TF_TIMEOUT, &latched);
            if (len < 0 && block == 1)
                net_sendto(sock, x->ip, (unsigned short)x->port, pkt, n);  /* re-RRQ */
            else if (len < 0) {
                unsigned char ack[4];
                ack[0] = 0; ack[1] = OP_ACK;
                ack[2] = (unsigned char)((block - 1) >> 8); ack[3] = (unsigned char)(block - 1);
                net_sendto(sock, x->ip, (unsigned short)x->port, ack, 4);
            }
        }
        if (len < 0) { ux_stage_put(); net_sock_close(sock);
                       return ux_failf(c, UNOXFER_EIO, "tftp timed out at block %u", block); }
        if (len >= 4 && pkt[1] == OP_ERR) {
            rc = tf_errpkt(c, pkt, len);
            ux_stage_put(); net_sock_close(sock); return rc;
        }
        if (len < 4 || pkt[1] != OP_DATA) continue;
        {
            unsigned b = ((unsigned)pkt[2] << 8) | pkt[3];
            int dl = len - 4;
            unsigned char ack[4];
            if (b != block) {                     /* a duplicate: re-ack, skip */
                ack[0] = 0; ack[1] = OP_ACK; ack[2] = pkt[2]; ack[3] = pkt[3];
                net_sendto(sock, x->ip, (unsigned short)x->port, ack, 4);
                continue;
            }
            if (got + dl > capn) {
                ux_stage_put(); net_sock_close(sock);
                return ux_failf(c, UNOXFER_ETOOBIG,
                                "%s is larger than the %lld byte staging cap",
                                unoxfer_basename(rpath), capn);
            }
            memcpy(buf + got, pkt + 4, (unsigned long)dl);
            got += dl;
            if (p) p->done = (unsigned long long)got;
            ack[0] = 0; ack[1] = OP_ACK; ack[2] = pkt[2]; ack[3] = pkt[3];
            net_sendto(sock, x->ip, (unsigned short)x->port, ack, 4);
            block++;
            if (dl < TF_BLK) break;               /* a short block ends it     */
        }
    }
    net_sock_close(sock);
    rc = ux_commit_file(vol, lpath, buf, (long)got);
    ux_stage_put();
    if (rc != UNOXFER_OK) return ux_failf(c, rc, "write failed: %s", lpath);
    return UNOXFER_OK;
}

static int tf_put(unoxfer_client *c, int vol, const char *lpath,
                  const char *rpath, unoxfer_prog *p)
{
    tf *x = (tf *)c->impl;
    unsigned char pkt[TF_BLK + 8], *buf;
    long long capn = 0, sent = 0;
    long sz = uno_fs_size(vol, lpath), rd;
    int sock, n, latched = 0, tries;
    unsigned block = 0;
    int last_short = 0;             /* a block shorter than 512 has been sent */
    const char *leaf = rpath;
    volatile int *cancel = p ? &p->cancel : 0;

    if (sz < 0) return ux_failf(c, UNOXFER_ENOENT, "no such local file: %s", lpath);
    while (*leaf == '/') leaf++;
    buf = ux_stage_get(sz, &capn);
    if (!buf) return ux_fail(c, UNOXFER_EIO, "the staging buffer is busy");
    if (sz > capn) { ux_stage_put();
        return ux_failf(c, UNOXFER_ETOOBIG, "%s is over the %lld byte staging cap",
                        lpath, capn); }
    rd = uno_fs_read(vol, lpath, buf, sz);
    if (rd < 0) { ux_stage_put(); return ux_failf(c, UNOXFER_EIO, "read failed: %s", lpath); }

    sock = net_socket(SOCK_UDP);
    if (sock < 0 || net_bind(sock, 0) != 0) {
        ux_stage_put();
        if (sock >= 0) net_sock_close(sock);
        return ux_fail(c, UNOXFER_EIO, "no UDP socket");
    }
    if (p) { ux_cpy(p->file, (int)sizeof p->file, unoxfer_basename(lpath));
             p->done = 0; p->total = (unsigned long long)rd; }

    n = tf_request(pkt, (int)sizeof pkt, OP_WRQ, leaf);
    if (!n) { ux_stage_put(); net_sock_close(sock);
              return ux_fail(c, UNOXFER_EARG, "filename too long"); }
    net_sendto(sock, x->ip, (unsigned short)x->port, pkt, n);

    for (;;) {
        int len = -1, dl;
        for (tries = 0; tries < TF_RETRIES && len < 0; tries++) {
            if (cancel && *cancel) { ux_stage_put(); net_sock_close(sock);
                                     return ux_fail(c, UNOXFER_ECANCEL, "cancelled"); }
            len = tf_wait(sock, x, pkt, (int)sizeof pkt, TF_TIMEOUT, &latched);
            if (len < 0) {
                /* retransmit whatever we last said: the WRQ, or the block */
                if (block == 0) net_sendto(sock, x->ip, (unsigned short)x->port, pkt, n);
                else {
                    long long base = (long long)(block - 1) * TF_BLK;
                    int d = (int)(rd - base > TF_BLK ? TF_BLK : rd - base);
                    if (d < 0) d = 0;                  /* the empty terminator */
                    pkt[0] = 0; pkt[1] = OP_DATA;
                    pkt[2] = (unsigned char)(block >> 8); pkt[3] = (unsigned char)block;
                    if (d > 0) memcpy(pkt + 4, buf + base, (unsigned long)d);
                    net_sendto(sock, x->ip, (unsigned short)x->port, pkt, d + 4);
                }
            }
        }
        if (len < 0) { ux_stage_put(); net_sock_close(sock);
                       return ux_failf(c, UNOXFER_EIO, "tftp timed out at block %u", block); }
        if (len >= 4 && pkt[1] == OP_ERR) {
            int rc = tf_errpkt(c, pkt, len);
            ux_stage_put(); net_sock_close(sock); return rc;
        }
        if (len < 4 || pkt[1] != OP_ACK) continue;
        if ((((unsigned)pkt[2] << 8) | pkt[3]) != block) continue;   /* stale  */

        /* THE TERMINATOR.  A transfer ends with a block SHORTER than 512, so a
         * file that is an exact multiple of 512 needs a final EMPTY one - and
         * "stop when every byte has been sent" silently omits it, leaving the
         * server waiting forever for a file it has already received in full.
         * So the loop exits on the ack for a short block, never on the byte
         * count. */
        if (last_short) break;
        block++;
        dl = (int)(rd - sent > TF_BLK ? TF_BLK : rd - sent);
        pkt[0] = 0; pkt[1] = OP_DATA;
        pkt[2] = (unsigned char)(block >> 8); pkt[3] = (unsigned char)block;
        if (dl > 0) memcpy(pkt + 4, buf + sent, (unsigned long)dl);
        net_sendto(sock, x->ip, (unsigned short)x->port, pkt, dl + 4);
        sent += dl;
        last_short = dl < TF_BLK;
        if (p) p->done = (unsigned long long)sent;
    }
    ux_stage_put();
    net_sock_close(sock);
    return UNOXFER_OK;
}

const unoxfer_backend unoxfer_be_tftp = {
    "tftp",
    /* No LIST, no MKDIR, no DELETE, no RESUME, and no SIZE: TFTP has none of
     * them.  Saying so here is what makes `xfer pull … -r` over TFTP a refusal
     * with a reason instead of a job that starts and cannot walk. */
    UNOXFER_CAP_GET | UNOXFER_CAP_PUT,
    tf_open, tf_close, 0, 0, tf_get, tf_put, 0, 0
};
