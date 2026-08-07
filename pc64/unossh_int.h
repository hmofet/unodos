/* ===========================================================================
 * unossh - the connection object, shared between the transport (unossh.c) and
 * the auth/channel layer (unossh_auth.c).
 *
 * Internal: nothing outside unossh includes this. The public surface is
 * unossh.h, which is handle-based precisely so that callers cannot reach in
 * here and start depending on the layout.
 * ======================================================================== */
#ifndef PC64_UNOSSH_INT_H
#define PC64_UNOSSH_INT_H
#include "bearssl_block.h"

#define SSH_RBCAP 16384              /* received channel data waiting to be read */

typedef struct {
    int used, sock, encrypted;
    unsigned seq_out, seq_in;
    br_aes_ct64_ctr_keys enc, dec;
    unsigned char iv_out[12], iv_in[12];
    unsigned cc_out, cc_in;
    unsigned char mac_out[32], mac_in[32];
    unsigned char sess_id[32], h[32];
    unsigned char hostkey[32], hostfp[32];
    char v_s[256];
    unsigned char *rx, *tx;
    unsigned char *pay; int paylen;
    char err[96];

    /* ---- auth + the session channel (unossh_auth.c) --------------------- */
    /* host_verified records that ssh_verify_host() has consulted known-hosts
     * for THIS connection. The transport refuses auth/channel traffic until it
     * is set, so a caller that forgets to check the host key cannot leak the
     * publickey signature or a password to an unverified peer. */
    int host_verified;
    int authed;
    int ch_state;                    /* 0 = none/closed, 1 = open            */
    unsigned ch_local, ch_remote;
    unsigned win_out, win_in;        /* flow control, both directions        */
    unsigned maxpkt_out;
    int ch_eof, ch_exit;
    unsigned char *rb;               /* SSH_RBCAP, allocated with rx/tx      */
    int rb_off, rb_len;
} ssh_conn;

ssh_conn *uns_get(int handle);
void      uns_err(ssh_conn *c, const char *m);
int       uns_send(ssh_conn *c, const unsigned char *payload, int n);
int       uns_recv(ssh_conn *c, int timeout_ms);

#endif
