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
 * uc_net.h - the network seam, and the whole of it.
 *
 * The editor reaches the outside world through these eleven calls and nothing
 * else.  UnoDOS answers them from its own TCP stack and BearSSL build
 * (core/plat/uc_net_pc64.c, over the kernel's exports); a hosted build answers
 * them from sockets and its own BearSSL (host/host_net.c).  Neither side of
 * that is visible from here, which is the point.
 *
 * THIS IS A SOCKET SEAM, NOT A REQUEST SEAM.  It moves bytes over TLS and says
 * nothing about HTTP: no verbs, no headers, no bodies.  That belongs one layer
 * up in uc_http.c, where both platforms share it.  The mistake to avoid is
 * pc64_http.c, which is the browser's client and hard-codes a form-encoded
 * POST with no way to add a header - so the one other thing in the OS that
 * needed an API key in a header (Studio's assistant) had to go around it and
 * write its own transport instead of using it.  A seam that assumes the shape
 * of today's request is a seam the next caller cannot use.
 *
 * EVERY CALL IS NON-BLOCKING.  There is no uc_tls_connect() that returns once
 * the handshake is done, on purpose: the editor has a frame to draw, and a
 * connect to a server on another continent is tens of frames long.  You open a
 * handle, you pump it from wherever you already run a frame, and you look at
 * what it says.  pc64's tls.h offers exactly this shape alongside a blocking
 * one, and the blocking one is why Studio's assistant freezes the desk while
 * it thinks.  We take the other.
 *
 * TRUST IS NOT A PARAMETER.  Every session is CA-validated against the bundled
 * roots with the peer's name checked, and there is no argument, setting or
 * config key that turns that off.  pc64's tls.h can also pin a single key,
 * which is right for its own test server and wrong here.  A "just this once"
 * flag in a config file is a flag an attacker can also write.
 * ======================================================================== */
#ifndef UC_NET_H
#define UC_NET_H

/* ---- what a poll can say -------------------------------------------------- */
enum {
    UC_NET_PENDING = 0,   /* connecting or handshaking - call again next frame */
    UC_NET_READY   = 1,   /* the handshake is done; send and recv are live     */
    UC_NET_EOF     = 2    /* the peer closed - drain recv, THEN free           */
};

/* Failures.  Negative, distinct, and each one nameable in a message a user can
 * act on: "no network" and "your clock is wrong" want different sentences, and
 * a single -1 makes both of them "it didn't work". */
enum {
    UC_NET_ERR        = -1,   /* generic transport failure                     */
    UC_NET_ENOLINK    = -2,   /* no link, and none could be brought up         */
    UC_NET_ENODNS     = -3,   /* the name did not resolve                      */
    UC_NET_ENOENTROPY = -4,   /* no usable RNG - TLS refuses rather than guess */
    UC_NET_ENOMEM     = -5,
    UC_NET_ETRUST     = -6    /* the certificate did not validate              */
};

/* ---- the link ------------------------------------------------------------- */

/* Bring the network up if it is not already.  Idempotent, and it may take a
 * while the first time (a DHCP lease on a cold link), so call it before you
 * need it rather than inside a frame that has to finish.  Returns 1 if a link
 * is up, 0 if there is no usable device.  A hosted build says 1 without doing
 * anything: the OS owns the link there. */
int uc_net_up(void);

/* Resolve a host name to an IPv4 address.  Returns 1 on success.  Accepts a
 * dotted-quad literal without asking anybody, which is what makes a local test
 * server reachable with no DNS at all. */
int uc_net_resolve(const char *host, unsigned char ip[4]);

/* Is there a random source good enough to hand to TLS?  0 means uc_tls_open()
 * will refuse with UC_NET_ENOENTROPY, and it means it on a real machine: pc64
 * falls back to conditioned CPU timing jitter and health-tests it, and a box
 * that fails both that and RDRAND gets no TLS at all.  Ask FIRST, so the
 * refusal can say what is actually wrong instead of "connection failed". */
int uc_net_entropy_ok(void);

/* ---- a session ------------------------------------------------------------ */

typedef struct uc_conn uc_conn;

/* Open a TLS session to ip:port, validating the certificate against the
 * bundled CA roots and checking it names `sni`.  Returns a handle IMMEDIATELY
 * - nothing has connected and nothing has been validated yet - or NULL, in
 * which case uc_net_open_error() says why.  `sni` is copied.
 *
 * The name and the address are separate arguments so that a test endpoint can
 * be reached by address while its certificate is still checked against the
 * real name.  That is the only concession to testing here, and it is a safe
 * one: a rogue address cannot present a certificate for a name it does not
 * hold. */
uc_conn *uc_tls_open(const unsigned char ip[4], unsigned short port,
                     const char *sni);
int uc_net_open_error(void);        /* why the last uc_tls_open() gave NULL   */

/* Advance this session as far as the transport allows right now, and never a
 * byte further.  Returns UC_NET_PENDING / UC_NET_READY / UC_NET_EOF, or a
 * negative UC_NET_* on failure.  Call uc_net_pump() yourself; this does not,
 * so one loop can drive several sessions. */
int uc_tls_poll(uc_conn *c);

/* Queue plaintext for encryption and sending.  Returns the bytes ACCEPTED,
 * which may be fewer than asked - call again with the rest - or 0 if the
 * session cannot take any this instant.  Negative on failure.  Never blocks,
 * so a caller that treats a short write as an error will drop most of a
 * request body the first time one arrives. */
int uc_tls_send(uc_conn *c, const void *data, int len);

/* Take decrypted bytes.  Returns the count copied, or 0 if none has arrived
 * yet - which is NOT an error and NOT end of stream, it is the ordinary answer
 * between frames.  Negative on failure.  UC_NET_EOF from uc_tls_poll() is what
 * says the peer is done. */
int uc_tls_recv(uc_conn *c, void *buf, int cap);

/* Close politely if the transport still allows it, then free the handle and
 * its socket.  Safe on NULL, and safe on a session that never finished
 * connecting - which is what a cancelled request is. */
void uc_tls_free(uc_conn *c);

/* Why this session failed, as a short sentence for a human.  Never NULL.  It
 * exists because the numbers underneath are BearSSL's BR_ERR_*, and "BR_ERR_
 * X509_EXPIRED" in the UI is a bug report we will have to translate anyway. */
const char *uc_net_error(uc_conn *c);

/* ---- the pump ------------------------------------------------------------- */

/* Give the network stack a turn.  On UnoDOS this is net_poll(), and NOTHING
 * moves without it - a session polled in a loop that never pumps sits at
 * UC_NET_PENDING for ever.  On a hosted build the OS is already doing this and
 * the call does nothing.  Call it once per frame, not once per session. */
void uc_net_pump(void);

#endif
