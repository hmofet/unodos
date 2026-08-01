/* ===========================================================================
 * UnoDOS/pc64 - unossh: the SSH client subsystem.
 *
 * Headless by design. Two front ends consume this and neither is privileged:
 * the GUI app, and the unoautomate verb that lets the harness which already
 * commands this machine log into others and command those. That second one is
 * why the core cannot live inside a window: it has to work on a box with no
 * desktop drawn.
 *
 * Sits directly on BearSSL primitives plus netsock. NOT on pc64/tls.*, whose
 * state is four file-scope statics and whose API takes no handle - it is a
 * single global connection and always was.
 *
 * Algorithms, smallest modern set that current OpenSSH will talk to:
 *   kex        curve25519-sha256
 *   host key   ssh-ed25519
 *   cipher     aes256-ctr
 *   mac        hmac-sha2-256
 *
 * This header carries the wire and key-exchange surface, which is pure
 * computation and therefore host-testable. The connection state machine and
 * its socket I/O land on top of it.
 * ======================================================================== */
#ifndef PC64_UNOSSH_H
#define PC64_UNOSSH_H

/* ---- connections --------------------------------------------------------
 * The handshake BLOCKS with a timeout: it is a bounded operation with a fixed
 * message order and nothing useful to do in between. Packet reads afterwards
 * take a timeout of their own, so a shell session driven from the shell's
 * frame loop passes 0 and never stalls the desktop.
 *
 * ssh_handshake() proves the peer holds the private half of the host key it
 * presented. It does NOT check that the key is the one you expected - there is
 * no known-hosts store until ssh-d. ssh_host_fingerprint() is the SHA-256 of
 * the key blob that such a store will compare. */
#define SSH_MAXCONN 4

int  ssh_connect(const char *host, int port);   /* -> handle, or -1 */
int  ssh_handshake(int handle);                 /* 0 = transport is up */
int  ssh_send(int handle, const unsigned char *payload, int n);
int  ssh_recv(int handle, const unsigned char **payload, int *n, int timeout_ms);
void ssh_close(int handle);

const char          *ssh_error(int handle);
const char          *ssh_server_ident(int handle);
const unsigned char *ssh_host_fingerprint(int handle);   /* 32 bytes */
const unsigned char *ssh_session_id(int handle);         /* 32 bytes */
int                  ssh_is_encrypted(int handle);

/* ---- byte buffers -------------------------------------------------------
 * One append-only writer and one bounds-checked reader, because every wire
 * bug in this protocol is either a length that was not checked or a length
 * that was written in the wrong units. Both track an error flag rather than
 * returning a status per call: a truncated read poisons the reader and every
 * later get returns zero, so a caller can parse a whole message and test once
 * at the end instead of after every field. */
typedef struct {
    unsigned char *p;
    int cap;
    int len;
    int err;                 /* 1 = ran out of room; contents undefined */
} ssh_buf;

typedef struct {
    const unsigned char *p;
    int len;
    int pos;
    int err;                 /* 1 = ran off the end; every get returns 0 */
} ssh_rd;

void ssh_buf_init(ssh_buf *b, unsigned char *store, int cap);
void ssh_put_u8  (ssh_buf *b, unsigned v);
void ssh_put_u32 (ssh_buf *b, unsigned v);
void ssh_put_raw (ssh_buf *b, const unsigned char *d, int n);
/* RFC 4251 string: uint32 length then the bytes */
void ssh_put_str (ssh_buf *b, const unsigned char *d, int n);
void ssh_put_cstr(ssh_buf *b, const char *s);
/* RFC 4251 mpint: a SIGNED big-endian integer, so a positive value whose top
 * bit is set gains a leading zero byte and zero encodes as an empty string.
 * Getting this wrong is the classic way an exchange hash silently disagrees. */
void ssh_put_mpint(ssh_buf *b, const unsigned char *d, int n);

void ssh_rd_init(ssh_rd *r, const unsigned char *d, int n);
unsigned ssh_get_u8 (ssh_rd *r);
unsigned ssh_get_u32(ssh_rd *r);
/* returns a pointer INTO the reader's buffer and its length; no copy */
const unsigned char *ssh_get_str(ssh_rd *r, int *n);
int ssh_rd_left(const ssh_rd *r);

/* ---- packet framing -----------------------------------------------------
 * RFC 4253 section 6. packet_length counts padding_length + payload +
 * padding, and NOT itself or the MAC. The encrypted region is
 * length+padlen+payload+padding, which must be a multiple of the cipher block
 * size (8 minimum) and at least 16 bytes; padding is 4..255. */
#define SSH_PKT_MIN_PAD 4
int ssh_pad_len(int payload_len, int blocksize);

/* ---- key exchange -------------------------------------------------------
 * X25519 through BearSSL. Note the byte order, which is not symmetric and is
 * not what you would guess: BearSSL takes the POINT little-endian (RFC 7748's
 * own encoding) but the SCALAR big-endian, and clamps the scalar itself.
 * These wrappers take and return everything in RFC 7748 little-endian, so a
 * caller never has to know that. */
int ssh_x25519_base(unsigned char pub[32], const unsigned char sec[32]);
int ssh_x25519(unsigned char out[32], const unsigned char sec[32],
               const unsigned char peer[32]);

/* The exchange hash H = SHA256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S
 * || K), every field a string except K which is an mpint. Built into `out`
 * (32 bytes). Version strings are passed WITHOUT their trailing CR LF, which
 * is what the protocol hashes. */
typedef struct {
    const char *v_c, *v_s;               /* client, server ident strings   */
    const unsigned char *i_c; int i_c_len;   /* client KEXINIT payload     */
    const unsigned char *i_s; int i_s_len;   /* server KEXINIT payload     */
    const unsigned char *k_s; int k_s_len;   /* server host key blob       */
    const unsigned char *q_c;                /* client ephemeral, 32 bytes */
    const unsigned char *q_s;                /* server ephemeral, 32 bytes */
    const unsigned char *k;   int k_len;     /* shared secret, big-endian  */
} ssh_exch;
void ssh_exchange_hash(unsigned char out[32], const ssh_exch *e);

/* RFC 4253 section 7.2 key derivation: KEY = HASH(K || H || <letter> ||
 * session_id), extended by KEY += HASH(K || H || KEY) until long enough.
 * `letter` is 'A'..'F'. */
void ssh_derive_key(unsigned char *out, int outlen, unsigned char letter,
                    const unsigned char *k, int k_len,
                    const unsigned char h[32],
                    const unsigned char session_id[32]);

#endif
