/* ===========================================================================
 * UnoDOS/pc64 - UNOAUTOMATE remote channel (unoauto_remote): a bidirectional
 * link between the running OS and the PC you develop from, over the pc64 TCP
 * stack.  The remote-logging + remote-control half of unoautomate.
 *
 * WHAT IT DOES
 *   - streams every unoauto LOG channel to the dev PC (remote logging)
 *   - accepts commands FROM the dev PC (probe / launch / key / py / ...)
 *   - lets either end send commands and free-form messages to the other
 *
 * SHAPE
 *   pc64's TCP stack is single-connection and client-only, so pc64 DIALS OUT
 *   to a listener on the dev PC.  The dev PC's address is a STRESS.CFG key,
 *   `remote=<ip>:<port>` (broadcast auto-discovery waits on the fuller ARP/UDP
 *   stack - see UNOAUTOMATE-REQUESTS.md).  Plaintext, LAN-only by intent.
 *
 *   The wire protocol (URC) is newline-delimited text frames, symmetric both
 *   directions - see REMOTE.md.  One TCP connection is shared with the
 *   Browser/AI apps, so the remote link and those are mutually exclusive.
 *
 *   Compiled only under -DUNO_DEBUG (same gate as the rest of unoautomate);
 *   in a production build every entry point compiles away.  [EXPERIMENTAL].
 * ======================================================================== */
#ifndef UNOAUTO_REMOTE_H
#define UNOAUTO_REMOTE_H

#ifdef UNO_DEBUG

/* Read the STRESS.CFG `remote=<ip>:<port>` key and arm the connector.  A no-op
 * if the key is absent or already armed.  Call once the boot net test has
 * released the single TCP connection (from the automate/nettest-finish path). */
void unoauto_remote_boot(void);

/* Pump one step: net_poll, advance the connect/retry machine, drain inbound
 * command lines, flush queued outbound frames.  Call every shell frame.
 * Cooperative + non-blocking; does nothing until armed. */
void unoauto_remote_tick(void);

/* 1 once the link is established (HELLO exchanged), else 0. */
int  unoauto_remote_active(void);

/* Queue one frame `TYPE text\n` for the dev PC (e.g. type "MSG").  Returns the
 * bytes queued, or -1 if the link is down or the outbound queue is full. */
int  unoauto_remote_send(const char *type, const char *text);

/* Pop the next inbound MSG payload into buf (NUL-terminated), for a Python
 * consumer.  Returns its length, or 0 when the inbound queue is empty. */
int  unoauto_remote_recv(char *buf, int cap);

/* Tear the link down (BYE + close) and disarm. */
void unoauto_remote_stop(void);

#else /* !UNO_DEBUG: everything compiles away */
#define unoauto_remote_boot()        ((void)0)
#define unoauto_remote_tick()        ((void)0)
#define unoauto_remote_active()      (0)
#define unoauto_remote_send(t, x)    (-1)
#define unoauto_remote_recv(b, c)    (0)
#define unoauto_remote_stop()        ((void)0)
#endif

#endif /* UNOAUTO_REMOTE_H */
