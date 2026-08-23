/* ===========================================================================
 * unolog - the UnoDOS system log.  See UNOLOG.md.
 *
 * The record of what this machine did: levelled, kept in PRODUCTION builds,
 * readable by the person using the machine, and speakable to rsyslog in both
 * directions.
 *
 * Not the debug harness.  uno_debug.c keeps a raw byte ring inside the crash
 * stash and `uno_debug.h` turns every one of its hooks into ((void)0) for a
 * production build - right for forensics on a developer's failing machine,
 * wrong for a system log, which a shipped OS has to keep when nobody is
 * watching.  The two coexist and neither owns the other.
 * ======================================================================== */
#ifndef PC64_UNOLOG_H
#define PC64_UNOLOG_H

/* ---- severities: syslog's, not ours --------------------------------------
 * Interop is the requirement, so the wire values ARE the internal values. A
 * private scheme would only have to be mapped at both edges, and a mapping is
 * one more place for the meaning to drift. */
enum {
    LOG_EMERG   = 0,        /* the system is unusable                        */
    LOG_ALERT   = 1,        /* action must be taken immediately              */
    LOG_CRIT    = 2,        /* critical condition                            */
    LOG_ERR     = 3,        /* error                                         */
    LOG_WARNING = 4,        /* warning                                       */
    LOG_NOTICE  = 5,        /* normal but significant                        */
    LOG_INFO    = 6,        /* informational                                 */
    LOG_DEBUG   = 7         /* debug-level                                   */
};
#define UNOLOG_NSEV 8

/* ---- facilities ----------------------------------------------------------
 * A small local set, mapped onto syslog's numeric facilities on the way out.
 * APPEND ONLY, never renumber: the numbers reach a remote collector, and a
 * renumber retroactively relabels every line already sitting in it. */
enum {
    LF_KERNEL   = 0,
    LF_NET      = 1,
    LF_STORAGE  = 2,
    LF_BROWSER  = 3,
    LF_UI       = 4,
    LF_SECURITY = 5,
    LF_APP      = 6,
    LF_REMOTE   = 7         /* arrived from the network (see unolog_listen)   */
};
#define UNOLOG_NFAC 8

const char *unolog_sev_name(int sev);   /* "err", "notice", ... ; "?" if bad  */
const char *unolog_fac_name(int fac);   /* "net", "storage", ...              */

/* ---- writing -------------------------------------------------------------
 * Never blocks, never allocates, and is safe before the network or the
 * filesystem exist - the record goes to the ring and is written out when a
 * sink appears.  NOT safe from an interrupt handler: nothing in pc64 logs from
 * one, and making the ring interrupt-safe would cost every caller.
 *
 * A record above the configured level is dropped here, so the cost of a line
 * you are not keeping is one comparison. */
void unolog(int sev, int fac, const char *fmt, ...);

#define ulog_emerg(f, ...)  unolog(LOG_EMERG,   (f), __VA_ARGS__)
#define ulog_crit(f, ...)   unolog(LOG_CRIT,    (f), __VA_ARGS__)
#define ulog_err(f, ...)    unolog(LOG_ERR,     (f), __VA_ARGS__)
#define ulog_warn(f, ...)   unolog(LOG_WARNING, (f), __VA_ARGS__)
#define ulog_notice(f, ...) unolog(LOG_NOTICE,  (f), __VA_ARGS__)
#define ulog_info(f, ...)   unolog(LOG_INFO,    (f), __VA_ARGS__)
#define ulog_debug(f, ...)  unolog(LOG_DEBUG,   (f), __VA_ARGS__)

/* The tap the debug harness may one day call (a request is filed). Weak, so
 * both link green whichever lands first; a no-op until unolog is present. */
void unolog_tap(int sev, const char *line);

/* ---- lifecycle -----------------------------------------------------------
 * init: read \LOGS\LOG.CFG and open the ring. Safe to call before storage is
 * up; the config is re-read on the first tick that finds a volume.
 * tick: the pump. Once per frame from the shell - the periodic flush and the
 * syslog socket both happen HERE rather than in unolog(), so a caller pays
 * only for formatting a line.
 * shutdown: final flush; call before a deliberate restart or power-off. */
void unolog_init(void);
void unolog_tick(void);
void unolog_shutdown(void);

/* Write anything pending to \LOGS\SYSTEM.LOG now. Returns records written.
 * ERR and worse already force this, so callers rarely need it. */
int  unolog_flush(void);

/* The volume set was renumbered (a detach remounts every disk on native
 * drivers). Drop the cached file-sink volume so the next write re-resolves it
 * by identity - a stale index does not merely miss, it addresses a DIFFERENT
 * DISK. Cheap and idempotent; call it from the remap. */
void unolog_storage_remapped(void);

/* ---- what is kept, and what is sent --------------------------------------
 * Two thresholds because "what do I record" and "what do I ship over the
 * network" are different questions with different costs. */
int  unolog_level(void);                /* keep records at or below this      */
void unolog_set_level(int sev);
int  unolog_remote_level(void);         /* ... also send these               */
void unolog_set_remote_level(int sev);

/* Where to send. host is "1.2.3.4" or a name; port 0 means 514. An empty or
 * NULL host turns sending off. Returns 1 if accepted. */
int  unolog_set_remote(const char *host, int port);
const char *unolog_remote_host(void);   /* "" when off                        */
int  unolog_remote_port(void);

/* Be a collector: bind UDP 514 and file what arrives. OFF by default and
 * deliberately - it is an unauthenticated listener, so anything on the LAN can
 * fill the ring and the disk. Returns 1 if the socket came up. */
int  unolog_set_listen(int on);
int  unolog_listening(void);

/* Persist the four settings above to \LOGS\LOG.CFG. 1 = written. */
int  unolog_save_cfg(void);

/* ---- reading (the viewer, and anyone else) -------------------------------
 * Records are numbered from the first one this boot; the ring holds the last
 * UNOLOG_RING of them. `first` is the oldest still in memory, `next` is one
 * past the newest, so `next - first` is what a reader can see and a reader
 * that remembers `next` can poll for what it has not read.
 *
 * A sequence number rather than an index because the ring wraps: an index is
 * only meaningful until it is overwritten, and a viewer that scrolled while
 * the machine logged would silently show the wrong lines. */
typedef struct {
    unsigned long seq;
    int           sev, fac;
    unsigned long ms;                   /* uptime ms when it happened         */
    long long     wall;                 /* unix-ish seconds; 0 = no clock     */
    char          text[192];
    char          src[16];              /* sender's IP for LF_REMOTE, else "" */
} unolog_rec;

unsigned long unolog_first(void);
unsigned long unolog_next(void);
/* Copy record `seq` out. 1 = copied, 0 = no longer in the ring (or not yet). */
int unolog_get(unsigned long seq, unolog_rec *out);

/* Counters, for the viewer's summary line and for tests. */
unsigned long unolog_dropped(void);     /* over the level, or ring overrun    */
unsigned long unolog_sent(void);        /* datagrams to the syslog server     */
unsigned long unolog_received(void);    /* datagrams accepted as a collector  */

/* Format one record the way the file and the viewer show it. Returns length. */
int unolog_format(const unolog_rec *r, char *buf, int cap);

#endif
