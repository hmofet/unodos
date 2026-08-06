/* ===========================================================================
 * unonet host gate: several TLS sessions at once, none of them blocking.
 *
 * Builds the REAL tls.c and the REAL BearSSL against a POSIX shim standing in
 * for netsock, and runs N genuine TLS 1.2 handshakes to tools/tls_echo_server.py
 * from ONE pump loop. No QEMU, no UEFI, seconds to run.
 *
 * Why this gate exists at all: tls.c used to hold ONE module-global BearSSL
 * context and drive it through blocking read/write callbacks, so "two sessions
 * at once" was not a slow path, it was unrepresentable - and nothing in the
 * tree could have noticed, because the QEMU spectest area runs on a NULL NIC
 * and the only real-TLS harness (nettest.py) is a screenshot a human reads.
 *
 * The three things it asserts, in order of what would actually regress:
 *   1. N sessions are open, handshaking and readable SIMULTANEOUSLY.
 *   2. Their application data does not cross over - each gets its own bytes
 *      back, which is the failure a shared engine or a shared buffer gives.
 *   3. Nothing blocks: a poll on a session with nothing pending returns at
 *      once, so one silent peer cannot stall the others.
 * Then it runs the LEGACY blocking API over the same server, because that API
 * is exported to .UNO modules and is what apps/studio_ai.c uses.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "netsock.h"
#include "tls.h"

/* ======================= the netsock shim ================================ *
 * Deliberately mimics the real one's SHAPE, not just its signatures: a send
 * carries at most one 512-byte segment and refuses (-1) when the transport
 * will not take it right now, exactly as net.c's one-segment-in-flight rule
 * does. A shim that accepted 16 KB in one call would let a pump bug through. */

#define SHIM_N 16
static struct { int used, fd, state; } g_sk[SHIM_N];

static int sk_ok(int s) { return s >= 0 && s < SHIM_N && g_sk[s].used; }

int net_socket(int type)
{
    int i, fd;
    if (type != SOCK_TCP) return -1;
    for (i = 0; i < SHIM_N; i++) if (!g_sk[i].used) break;
    if (i == SHIM_N) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    g_sk[i].used = 1; g_sk[i].fd = fd; g_sk[i].state = TCP_CLOSED;
    return i;
}

int net_connect(int s, const unsigned char dst[4], unsigned short dport)
{
    struct sockaddr_in a;
    if (!sk_ok(s)) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(dport);
    memcpy(&a.sin_addr, dst, 4);
    if (connect(g_sk[s].fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        g_sk[s].state = TCP_CLOSED;
        return -1;
    }
    g_sk[s].state = TCP_SYN_SENT;
    return 0;
}

int net_sock_state(int s)
{
    if (!sk_ok(s)) return TCP_CLOSED;
    if (g_sk[s].state == TCP_SYN_SENT) {          /* has the connect landed? */
        fd_set w;
        struct timeval tv;
        FD_ZERO(&w); FD_SET(g_sk[s].fd, &w);
        tv.tv_sec = 0; tv.tv_usec = 0;
        if (select(g_sk[s].fd + 1, 0, &w, 0, &tv) > 0) {
            int e = 0; socklen_t el = sizeof e;
            getsockopt(g_sk[s].fd, SOL_SOCKET, SO_ERROR, &e, &el);
            g_sk[s].state = e ? TCP_CLOSED : TCP_ESTABLISHED;
        }
    }
    return g_sk[s].state;
}

int net_send(int s, const void *data, int len)
{
    ssize_t n;
    if (!sk_ok(s) || net_sock_state(s) != TCP_ESTABLISHED) return -1;
    if (len > 512) len = 512;                     /* one segment, as net.c does */
    n = send(g_sk[s].fd, data, (size_t)len, 0);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
    g_sk[s].state = TCP_DONE;
    return -1;
}

int net_recv(int s, void *buf, int cap)
{
    ssize_t n;
    if (!sk_ok(s)) return 0;
    n = recv(g_sk[s].fd, buf, (size_t)cap, 0);
    if (n > 0) return (int)n;
    if (n == 0) { g_sk[s].state = TCP_DONE; return 0; }        /* peer FIN */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    g_sk[s].state = TCP_DONE;
    return 0;
}

void net_sock_close(int s)
{
    if (!sk_ok(s)) return;
    close(g_sk[s].fd);
    g_sk[s].used = 0; g_sk[s].state = TCP_CLOSED;
}

int net_sock_count(void)
{ int i, n = 0; for (i = 0; i < SHIM_N; i++) if (g_sk[i].used) n++; return n; }

/* The kernel pumps the NIC here. POSIX does it for us, so this is where the
 * gate proves the OTHER half of the contract: the handle API must not need it. */
static long g_polls;
void net_poll(void) { g_polls++; }

/* tls_entropy.c folds the link's frame counters in as UNCREDITED diversity -
 * they add no entropy to its budget, so a constant is a faithful stand-in. */
u32 net_tx_frames(void) { return 0; }
u32 net_rx_frames(void) { return 0; }

/* ---- the rest of the kernel the TLS glue touches ------------------------- */
void uno_pc64_delay_ms(int ms)
{ struct timespec t; t.tv_sec = ms / 1000; t.tv_nsec = (long)(ms % 1000) * 1000000L; nanosleep(&t, 0); }

void uno_pc64_time(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    time_t now = time(0);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    *y = tmv.tm_year + 1900; *mo = tmv.tm_mon + 1; *d = tmv.tm_mday;
    *h = tmv.tm_hour; *mi = tmv.tm_min; *s = tmv.tm_sec;
}

long unoauto_deadline_left_ms(void) { return -1; }      /* no budget armed */

/* ============================== the gate ================================= */

#define NCONN 4
#define MSGLEN 300              /* > one 512-byte segment once encrypted */

static int g_fail;
static void ck(int ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_fail = 1;
}

static long long now_ms(void)
{ struct timeval tv; gettimeofday(&tv, 0); return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000; }

int main(int argc, char **argv)
{
    unsigned char ip[4] = { 127, 0, 0, 1 };
    unsigned short port = (argc > 1) ? (unsigned short)atoi(argv[1]) : 0;
    tls_conn *c[NCONN];
    char out[NCONN][MSGLEN], in[NCONN][MSGLEN + 1];
    int sent[NCONN], got[NCONN], ready[NCONN];
    int i, rounds = 0, all_ready = 0, peak_hs = 0;
    long long t0;

    if (!port) { fprintf(stderr, "usage: tls_conc_test <port>\n"); return 2; }
    printf("tls_conn concurrency gate (%d sessions, 127.0.0.1:%u)\n", NCONN, port);

    /* 1. open every session BEFORE pumping any. With the old module-global
     *    context the second tls_open would have destroyed the first. */
    for (i = 0; i < NCONN; i++) {
        memset(out[i], 'A' + i, sizeof out[i]);
        memcpy(out[i], "CONN", 4); out[i][4] = (char)('0' + i);
        sent[i] = got[i] = ready[i] = 0;
        c[i] = tls_open(ip, port, "unodos-pc64", TLS_TRUST_PINNED);
        if (!c[i]) { printf("  tls_open %d failed (err %d)\n", i, tls_open_error()); return 1; }
    }
    ck(net_sock_count() == NCONN, "N sockets open at once, one per session");

    /* 2. one loop drives all of them. Nothing here waits on a particular
     *    session, which is the property the old blocking callbacks denied. */
    t0 = now_ms();
    for (;;) {
        int hs_now = 0, done = 0;
        rounds++;
        for (i = 0; i < NCONN; i++) {
            int r = tls_poll(c[i]);
            if (r < 0) { printf("  session %d failed: rc %d, BR_ERR %d\n", i, r, tls_conn_error(c[i])); return 1; }
            if (r == TLS_READY) { if (!ready[i]) { ready[i] = 1; all_ready++; } }
            if (!ready[i]) hs_now++;      /* still mid-connect or mid-handshake */

            if (ready[i] && sent[i] < MSGLEN) {
                int n = tls_send(c[i], out[i] + sent[i], MSGLEN - sent[i]);
                if (n < 0) { printf("  session %d send failed\n", i); return 1; }
                sent[i] += n;
            }
            if (got[i] < MSGLEN) {
                int n = tls_recv(c[i], in[i] + got[i], MSGLEN - got[i]);
                if (n < 0) { printf("  session %d closed early (%d/%d)\n", i, got[i], MSGLEN); return 1; }
                got[i] += n;
            }
            if (got[i] >= MSGLEN) done++;
        }
        if (hs_now > peak_hs) peak_hs = hs_now;
        if (done == NCONN) break;
        if (now_ms() - t0 > 20000) { printf("  TIMEOUT after %d rounds\n", rounds); return 1; }
    }

    ck(all_ready == NCONN, "every session completed its own handshake");
    ck(peak_hs == NCONN, "all N were mid-handshake at once, not one after another");

    /* 3. no cross-talk: each session gets ITS bytes back, not its neighbour's. */
    {   int clean = 1;
        for (i = 0; i < NCONN; i++) if (memcmp(in[i], out[i], MSGLEN)) clean = 0;
        ck(clean, "each session echoed its own payload, no cross-talk");
    }
    {   int v = 1;
        for (i = 0; i < NCONN; i++) if (tls_conn_version(c[i]) != 0x0303) v = 0;
        ck(v, "every session negotiated TLS 1.2 (0x0303)");
    }
    {   int distinct = 1, j;
        for (i = 0; i < NCONN; i++) for (j = i + 1; j < NCONN; j++)
            if (tls_conn_sock(c[i]) == tls_conn_sock(c[j])) distinct = 0;
        ck(distinct, "each session owns a distinct socket");
    }

    /* 4. an idle session must return, not wait. A blocking recv here would
     *    have been the whole bug: one slow peer stalling everyone else. */
    {   char junk[64];
        long long a = now_ms();
        for (i = 0; i < NCONN; i++) (void)tls_recv(c[i], junk, sizeof junk);
        ck(now_ms() - a < 100, "polling N idle sessions returns immediately");
    }
    ck(g_polls == 0, "the handle API never needed net_poll of its own");

    for (i = 0; i < NCONN; i++) tls_free(c[i]);
    ck(net_sock_count() == 0, "every socket freed on close");

    /* 5. the legacy blocking API, unchanged, over the same server. */
    {   char buf[64];
        int rc = tls_connect(ip, port, "unodos-pc64");
        ck(rc == 0, "legacy tls_connect still handshakes");
        if (rc == 0) {
            ck(tls_write("UNODOS-TLS", 10) == 10, "legacy tls_write");
            {   int n = tls_read(buf, sizeof buf);
                ck(n == 10 && !memcmp(buf, "UNODOS-TLS", 10), "legacy tls_read echoes back");
            }
            ck(tls_version() == 0x0303, "legacy tls_version reports TLS 1.2");
            ck(tls_last_error() == 0, "legacy tls_last_error clean");
            tls_close();
        }
        ck(net_sock_count() == 0, "legacy close frees its socket too");
    }

    printf("%s (%d rounds)\n", g_fail ? ">> tls concurrency gate FAILED" : ">> tls concurrency gate OK", rounds);
    return g_fail;
}
