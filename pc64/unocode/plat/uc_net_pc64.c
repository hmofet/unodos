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
 * uc_net_pc64.c - uc_net.h answered by UnoDOS.
 *
 * This file is compiled into UNOCODE.UNO by pc64's build.sh and by nothing
 * else.  It lives in core/plat/ rather than beside the uc_*.c files because
 * the desktop build globs `core/uc_*.c`: a platform implementation sitting in
 * that glob would be compiled on a host that has none of these symbols, and
 * would fail at the linker rather than at the point of the mistake.
 *
 * It cannot be guarded with #ifdef UNO_PC64 either - the DESKTOP build defines
 * UNO_PC64 too, because the editor's own sources want it.  A directory is the
 * only separator here that actually separates.
 *
 * Everything below is a kernel export resolved by the module loader at load
 * time (pc64_modload.c's kExports).  There is no library and no libc: the
 * prototypes are declared here, exactly as apps/studio_ai.c declares its own.
 *
 * SEVEN OF THESE EXPORTS WERE ADDED FOR THIS FILE.  The four legacy tls_*
 * calls were exported; the handle API was not, so a module could only reach
 * the BLOCKING surface - which is why Studio's assistant freezes the desk
 * while it waits.  If a build fails here with undefined tls_open, the kernel
 * half of UCD-45 has not landed: see pc64/UNOCODE-UPSTREAM.md.
 * ======================================================================== */
#include "uc_net.h"

typedef unsigned char u8;
typedef unsigned short u16;

/* ---- the kernel's side ---------------------------------------------------- */
int  pc64_net_up(void);
void net_poll(void);
int  net_dns_query(const char *host, u8 out[4]);
int  net_dns_begin(const char *host, u8 out[4]);
int  net_dns_poll(u8 out[4]);
void net_dns_abort(void);

/* tls.h's handle API.  tls_conn is opaque to us, which is what lets a uc_conn
 * BE one rather than wrap one - no allocation, nothing to keep in step. */
typedef struct tls_conn tls_conn;
tls_conn *tls_open(const u8 dst[4], u16 port, const char *sni, int trust);
int  tls_open_error(void);
int  tls_poll(tls_conn *c);
int  tls_send(tls_conn *c, const void *data, int len);
int  tls_recv(tls_conn *c, void *buf, int cap);
void tls_free(tls_conn *c);
int  tls_conn_error(tls_conn *c);
int  tls_entropy_source(void);

/* tls.h's small enums, mirrored so this file needs none of pc64's headers.
 * They are ABI - they already cross the kernel/module boundary - and there are
 * five of them. */
#define TLS_TRUST_CA      1
#define TLS_PENDING       0
#define TLS_READY         1
#define TLS_EOF           2
#define TLS_ENOENTROPY  (-4)
#define TLS_ENT_NONE      0

/* The certificate errors are NOT mirrored, and that is deliberate.  Writing
 * them out by hand got all three wrong on the first attempt here, and two of
 * them wrong in a way that SWAPPED two messages - so a user with an untrusted
 * certificate would have been told their server name did not match, and gone
 * looking in the wrong place.  Nothing would have failed; the wrong sentence
 * is a perfectly good sentence.  Include the header instead and let the
 * compiler be right.  This costs one -Ibearssl/inc on one file in build.sh
 * and no link surface at all: the header is constants and structs. */
#include "bearssl_x509.h"

static int g_open_err;

/* ---- the link ------------------------------------------------------------- */

int uc_net_up(void)
{
    return pc64_net_up() ? 1 : 0;
}

void uc_net_pump(void)
{
    net_poll();
}

/* A dotted quad is answered here rather than asked of DNS, so a test endpoint
 * on a QEMU guestfwd address is reachable on a machine with no resolver at
 * all.  Deliberately strict: four parts, each 0..255, nothing else in the
 * string - "1.2.3.4.5" and "1.2.3.4x" are names, not addresses, and handing
 * either to the parser as if it were an address would silently connect
 * somewhere unintended. */
static int parse_quad(const char *s, u8 ip[4])
{
    int part = 0, val = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (++digits > 3 || val > 255) return 0;
        } else if (*s == '.' || *s == 0) {
            if (!digits || part > 3) return 0;
            ip[part++] = (u8)val;
            val = 0; digits = 0;
            if (*s == 0) break;
        } else {
            return 0;
        }
    }
    return part == 4;
}

/* Digits and dots only means it was MEANT as an address, so a parse_quad
 * failure makes it a malformed one rather than a name to look up.
 *
 * Carried here to match the hosted side, where it is not a nicety: glibc's
 * getaddrinfo("1.2.3") succeeds and yields 1.2.0.3 via the inet_aton
 * shorthand, so a strict parser in front of a permissive resolver protects
 * nothing.  net_dns_query may well be stricter, but the two platforms
 * answering the same string differently is its own bug, and this is one line. */
static int looks_numeric(const char *s)
{
    for (; *s; s++)
        if ((*s < '0' || *s > '9') && *s != '.') return 0;
    return 1;
}

int uc_net_resolve(const char *host, unsigned char ip[4])
{
    if (!host || !*host) return 0;
    if (parse_quad(host, ip)) return 1;
    if (looks_numeric(host)) return 0;      /* a broken address, not a name */
    if (!uc_net_up()) return 0;
    return net_dns_query(host, ip) ? 1 : 0;
}

/* The non-blocking pair maps almost one-for-one onto the resolver's own,
 * because that split was made upstream for this caller.  All this layer adds
 * is the literal-address shortcut and the numeric guard, so a caller gets the
 * same answers from both forms. */
int uc_net_resolve_begin(const char *host, unsigned char ip[4])
{
    int rc;
    if (!host || !*host) return UC_NET_ENODNS;
    if (parse_quad(host, ip)) return 1;
    if (looks_numeric(host)) return UC_NET_ENODNS;
    if (!uc_net_up()) return UC_NET_ENOLINK;

    rc = net_dns_begin(host, ip);
    if (rc == 1)  return 1;                 /* the resolver's own cache        */
    if (rc == 0)  return 0;
    if (rc == -2) return UC_NET_EBUSY;
    return UC_NET_ENODNS;
}

int uc_net_resolve_poll(unsigned char ip[4])
{
    int rc = net_dns_poll(ip);
    if (rc == 1) return 1;
    if (rc == 0) return 0;
    return UC_NET_ENODNS;
}

void uc_net_resolve_end(void) { net_dns_abort(); }

int uc_net_entropy_ok(void)
{
    return tls_entropy_source() != TLS_ENT_NONE;
}

/* ---- a session ------------------------------------------------------------ */

uc_conn *uc_tls_open(const unsigned char ip[4], unsigned short port,
                     const char *sni)
{
    tls_conn *c;

    /* Ask about entropy before opening, not after failing.  tls_open() would
     * refuse anyway, but "no usable random source on this machine" and "that
     * server did not answer" are different sentences and the user can only act
     * on one of them. */
    if (!uc_net_entropy_ok()) { g_open_err = UC_NET_ENOENTROPY; return 0; }
    if (!uc_net_up())         { g_open_err = UC_NET_ENOLINK;    return 0; }

    c = tls_open(ip, (u16)port, sni, TLS_TRUST_CA);
    if (!c) {
        g_open_err = (tls_open_error() == TLS_ENOENTROPY)
                     ? UC_NET_ENOENTROPY : UC_NET_ERR;
        return 0;
    }
    g_open_err = 0;
    return (uc_conn *)c;
}

int uc_net_open_error(void)
{
    return g_open_err;
}

int uc_tls_poll(uc_conn *c)
{
    int r;
    if (!c) return UC_NET_ERR;
    r = tls_poll((tls_conn *)c);
    if (r == TLS_PENDING) return UC_NET_PENDING;
    if (r == TLS_READY)   return UC_NET_READY;
    if (r == TLS_EOF)     return UC_NET_EOF;

    /* A negative tls_poll means the session died.  Say WHY where we can: the
     * three certificate outcomes are the ones a user has any chance of acting
     * on, and "your clock is wrong" is a far better message than "connection
     * failed" for the machine that has just come up with no RTC battery. */
    switch (tls_conn_error((tls_conn *)c)) {
    case BR_ERR_X509_EXPIRED:
    case BR_ERR_X509_NOT_TRUSTED:
    case BR_ERR_X509_BAD_SERVER_NAME: return UC_NET_ETRUST;
    default:                          return UC_NET_ERR;
    }
}

int uc_tls_send(uc_conn *c, const void *data, int len)
{
    if (!c || len < 0) return UC_NET_ERR;
    return tls_send((tls_conn *)c, data, len);
}

int uc_tls_recv(uc_conn *c, void *buf, int cap)
{
    if (!c || cap <= 0) return UC_NET_ERR;
    return tls_recv((tls_conn *)c, buf, cap);
}

void uc_tls_free(uc_conn *c)
{
    if (c) tls_free((tls_conn *)c);
}

const char *uc_net_error(uc_conn *c)
{
    if (!c) return "no connection";
    switch (tls_conn_error((tls_conn *)c)) {
    case 0:                           return "no error";
    case BR_ERR_X509_EXPIRED:         return "the server's certificate has "
                                             "expired, or this machine's clock "
                                             "is wrong";
    case BR_ERR_X509_NOT_TRUSTED:     return "the server's certificate is not "
                                             "signed by a trusted authority";
    case BR_ERR_X509_BAD_SERVER_NAME: return "the server's certificate is for "
                                             "a different name";
    default:                          return "the secure connection failed";
    }
}
