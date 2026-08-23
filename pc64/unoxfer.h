/* ===========================================================================
 * UnoDOS/pc64 - unoxfer (UnoTransfer): the file-transfer subsystem.
 *
 * Contract + rationale: UNOXFER.md.  Read that first; this header is the
 * surface, not the argument.
 *
 * Headless by design, for the same reason unossh is: there are three front
 * ends and only one of them has a screen.  The windowed app, the terminal,
 * and the `xfer` URC verb all consume this, and the verb has to work on a box
 * with no desktop drawn at all.
 *
 * ONE SEAM, MANY BACKENDS.  Everything above unoxfer_client talks to the seven
 * calls below and never to a protocol.  unoxfer_open() is the only place in
 * the subsystem that reads unoxfer_proto; adding a protocol is one
 * unoxfer_backend and one row in its table.  This is Portage's ITransferClient
 * and its factory, and it is the whole reason that app could grow from two
 * protocols to nine without the UI learning about any of them.
 *
 * NOTHING HERE BLOCKS FOR LONG.  Listing and metadata are bounded round trips
 * and block with a timeout.  Bulk transfer does not: it is a step machine
 * (unoxfer_job_*) that does a slice and returns, so a stalled server costs the
 * caller one frame - and, over URC, does not park the dispatcher past the
 * guard's deadline and get the box hard-reset by its own dead-man's switch.
 * ======================================================================== */
#ifndef PC64_UNOXFER_H
#define PC64_UNOXFER_H

/* ---- protocols -----------------------------------------------------------
 * Appended-to, never reordered: a saved site stores the numeric value.  The
 * v1 set is what the existing stack already reaches (unossh, pc64_http,
 * netsock).  FTP/FTPS, S3, Azure and SMB are absent because they are unwritten,
 * not because anything blocks them - see UNOXFER.md. */
typedef enum {
    UNOXFER_NONE = 0,
    UNOXFER_LOCAL,          /* a volume on this machine (pc64_fs / fat)      */
    UNOXFER_SCP,            /* SCP over unossh's exec channel                */
    UNOXFER_SFTP,           /* SFTP; weak-linked, see unoxfer_proto_ready()  */
    UNOXFER_HTTP,
    UNOXFER_HTTPS,
    UNOXFER_WEBDAV,
    UNOXFER_WEBDAVS,
    UNOXFER_TFTP,
    UNOXFER_PROTO_COUNT
} unoxfer_proto;

/* "sftp", "https", ... for the UI and the verb.  Never a bare number in a
 * report: a protocol id in a log line is a thing the reader has to go and
 * look up. */
const char   *unoxfer_proto_name(unoxfer_proto p);
unoxfer_proto unoxfer_proto_parse(const char *s);
int           unoxfer_proto_port(unoxfer_proto p);      /* conventional port */
/* 1 when a backend for this protocol is actually linked in AND its
 * dependencies are present.  SFTP answers 0 until unossh lands
 * ssh_subsystem(), which is the honest answer and not an error at connect. */
int           unoxfer_proto_ready(unoxfer_proto p);
/* 1 for the protocols that can host an interactive terminal (SSH-based, and
 * local).  Portage's IsTerminalCapable(). */
int           unoxfer_proto_terminal(unoxfer_proto p);

/* ---- capabilities --------------------------------------------------------
 * Asked BEFORE a plan is made, not discovered by a transfer that fails in the
 * middle.  A backend that cannot list (TFTP, HTTP) must say so here, so
 * "pull -r" against it is refused up front with a reason rather than started
 * and abandoned three files in. */
enum {
    UNOXFER_CAP_LIST    = 1 << 0,   /* directories can be enumerated         */
    UNOXFER_CAP_GET     = 1 << 1,
    UNOXFER_CAP_PUT     = 1 << 2,
    UNOXFER_CAP_MKDIR   = 1 << 3,
    UNOXFER_CAP_DELETE  = 1 << 4,
    UNOXFER_CAP_RESUME  = 1 << 5,   /* a transfer can start at a byte offset */
    UNOXFER_CAP_SIZE    = 1 << 6    /* a file's size is knowable before it   */
};

/* ---- errors --------------------------------------------------------------
 * Negative, small, and distinct where the caller would act differently.  Every
 * call that can fail also leaves a human sentence in the client's error slot
 * (unoxfer_error), because "-4" in a status line helps nobody. */
enum {
    UNOXFER_OK       =  0,
    UNOXFER_EIO      = -1,          /* the transport broke                   */
    UNOXFER_EAUTH    = -2,          /* credentials refused                   */
    UNOXFER_ENOENT   = -3,
    UNOXFER_EPERM    = -4,          /* the far end said no                   */
    UNOXFER_EUNSUP   = -5,          /* this backend cannot do that at all    */
    UNOXFER_ENOSPC   = -6,
    UNOXFER_ETOOBIG  = -7,          /* over the staging cap; see UNOXFER.md  */
    UNOXFER_ECANCEL  = -8,
    UNOXFER_EARG     = -9,
    UNOXFER_EHOSTKEY = -10          /* known-hosts MISMATCH: stop, do not ask */
};
const char *unoxfer_strerror(int rc);

/* ---- a site --------------------------------------------------------------
 * What you need to reach a host, and no secret.  SSH-based sites name a key in
 * unossh's encrypted store (`key`), which is where the secret lives and stays;
 * this struct is safe to write to disk and safe to print.  That rule is
 * Portage's ConnectionProfile/CredentialTarget split, and the reason to keep
 * it is the same: a config file that can hold a secret eventually does. */
#define UNOXFER_NAMELEN 32
#define UNOXFER_HOSTLEN 96
#define UNOXFER_PATHLEN 192

typedef struct {
    char          name[UNOXFER_NAMELEN];
    unoxfer_proto proto;
    char          host[UNOXFER_HOSTLEN];   /* or a full URL for the http kin */
    int           port;                    /* 0 = the protocol's default     */
    char          user[UNOXFER_NAMELEN];
    char          key[UNOXFER_NAMELEN];    /* unossh key name, or empty      */
    char          root[UNOXFER_PATHLEN];   /* opening directory, "/" if none */
    int           vol;                     /* LOCAL only: which volume       */
} unoxfer_site;

/* ---- a listing entry ----------------------------------------------------- */
typedef struct {
    char          name[64];
    unsigned long long size;
    unsigned      mtime;               /* unix seconds; 0 = unknown          */
    unsigned char is_dir;
} unoxfer_ent;

/* ---- progress ------------------------------------------------------------
 * Handed to the bulk calls and updated in place as bytes move.  `cancel` is
 * read by the backend between slices: setting it from another context is how
 * `xfer cancel` and the app's Stop button reach a transfer that is mid-file. */
typedef struct {
    char               file[64];
    unsigned long long done;
    unsigned long long total;          /* 0 when the server would not say    */
    volatile int       cancel;
} unoxfer_prog;

/* ---- the client ---------------------------------------------------------- */
typedef struct unoxfer_client unoxfer_client;

/* Open and authenticate.  Returns NULL on failure with a sentence in `err`,
 * which is a caller-owned buffer because a failure to open has no client to
 * hang an error on. */
unoxfer_client *unoxfer_open(const unoxfer_site *site, char *err, int errcap);
void            unoxfer_close(unoxfer_client *c);
unsigned        unoxfer_caps(unoxfer_client *c);
const char     *unoxfer_error(unoxfer_client *c);
unoxfer_proto   unoxfer_client_proto(unoxfer_client *c);

/* List `path`.  Returns the number of entries WRITTEN (<= max) and, through
 * `total` (nullable), how many there were - so a caller can tell a complete
 * listing from a truncated one and never present the second as the first.
 * uno_fs_list_dir() has the same shape for the same reason. */
int unoxfer_list(unoxfer_client *c, const char *path,
                 unoxfer_ent *out, int max, int *total);

/* One file's size without fetching it, or a negative error.  Used by the plan
 * phase so a job knows its byte total before it moves any.  Backends without
 * UNOXFER_CAP_SIZE answer UNOXFER_EUNSUP and the plan carries 0. */
long long unoxfer_size(unoxfer_client *c, const char *rpath);

/* Pull `rpath` to `lpath` on volume `vol`, push the other way.  BLOCKING for
 * one file, bounded by the staging cap - the recursive, non-blocking, whole-
 * directory form is unoxfer_job_* below, and is what every front end uses.
 * `p` (nullable) is updated as bytes move and read for cancellation. */
int unoxfer_get(unoxfer_client *c, const char *rpath,
                int vol, const char *lpath, unoxfer_prog *p);
int unoxfer_put(unoxfer_client *c, int vol, const char *lpath,
                const char *rpath, unoxfer_prog *p);

int unoxfer_mkdir(unoxfer_client *c, const char *path);
int unoxfer_del  (unoxfer_client *c, const char *path);

/* ---- the staging buffer --------------------------------------------------
 * v1 stages one file at a time because fat.c can write a whole file and
 * cannot append to one (UNOXFER.md, "the single-file size cap").  The cap is
 * per FILE; a job is unbounded, because the buffer is reused.
 *
 * unoxfer_streaming() reports which mode is live: 1 once unofs's
 * uno_fat_append() strong symbol is linked and the cap no longer applies.  A
 * front end should say so rather than let someone discover it at 16 MB. */
long long unoxfer_stage_cap(void);
void      unoxfer_stage_set_cap(long long bytes);
int       unoxfer_streaming(void);

/* ---- the site store ------------------------------------------------------
 * Sites persist beside unossh's store, on the first NATIVE volume rather than
 * the first writable one - volume 0 is the RAM disk, and a store written there
 * is gone at power-off while every save appears to have worked.  That is the
 * exact bug unossh's header records; there is no reason to ship it twice. */
#define UNOXFER_MAXSITE 16
int unoxfer_site_set   (const unoxfer_site *s);
int unoxfer_site_get   (const char *name, unoxfer_site *out);
int unoxfer_site_list  (int idx, char *name, int cap);
int unoxfer_site_delete(const char *name);
int unoxfer_store_persistent(void);   /* 0 = the store is on the RAM disk    */

/* ---- URLs and paths ------------------------------------------------------
 * unoxfer_parse_url() fills a site from "sftp://user@host:22/path" and
 * friends; it returns 0 or a negative error.  It REFUSES a URL carrying a
 * password ("user:pw@host"): accepting one means either storing it or
 * pretending to, and the store deliberately has nowhere to put it. */
int unoxfer_parse_url(const char *url, unoxfer_site *out);

/* POSIX join for the remote side, DOS join for the local side.  Separate
 * because they are separate: a remote path is '/'-separated and case-sensitive
 * and a FAT path is '\'-separated and is not, and every transfer bug that ever
 * ate a directory came from one function trying to be both. */
int unoxfer_rjoin(char *dst, int cap, const char *dir, const char *leaf);
int unoxfer_ljoin(char *dst, int cap, const char *dir, const char *leaf);
/* The last component of a path, either flavour. */
const char *unoxfer_basename(const char *path);
/* A remote leaf name mapped onto something FAT will accept (8.3-safe, upper
 * case, illegal bytes replaced).  Returns 1 if it had to change anything, so a
 * caller can report the rename instead of silently landing a different name. */
int unoxfer_fatname(char *dst, int cap, const char *leaf);

/* ---- the recursive engine ------------------------------------------------
 * PLAN then RUN, taken from Portage's SyncEngine.  The walk produces a file
 * count and a byte total before a byte moves, so progress is a percentage
 * rather than a spinner, and a job that will fail on a missing directory fails
 * while planning instead of three files in.
 *
 * Jobs are owned by this subsystem and referred to by a small integer id that
 * is stable for the life of the job, because the URC client that started one
 * and the app that watches it are not the same process and cannot share a
 * pointer. */
#define UNOXFER_MAXJOB 4

typedef enum {
    UNOXFER_JOB_IDLE = 0,
    UNOXFER_JOB_PLANNING,
    UNOXFER_JOB_RUNNING,
    UNOXFER_JOB_DONE,
    UNOXFER_JOB_FAILED,
    UNOXFER_JOB_CANCELLED
} unoxfer_job_state;

typedef struct {
    unoxfer_job_state state;
    int                files_done, files_total;
    unsigned long long bytes_done, bytes_total;
    int                errors;
    int                rc;                 /* the failing UNOXFER_E* code    */
    char               cur[64];            /* the file in flight             */
    char               msg[96];            /* why it failed, if it did       */
} unoxfer_job_stat;

/* Start a pull (remote -> local) or a push.  `recurse` walks directories;
 * without it a directory argument is an error rather than a silent single
 * file.  Returns a job id >= 0, or a negative UNOXFER_E*.  RETURNS AT ONCE:
 * the job is planned and run by unoxfer_job_tick(). */
int unoxfer_job_pull(const unoxfer_site *site, const char *rpath,
                     int vol, const char *lpath, int recurse);
int unoxfer_job_push(const unoxfer_site *site, int vol, const char *lpath,
                     const char *rpath, int recurse);

/* Advance every live job by one bounded slice.  Called once per shell frame
 * (pc64_uui.c) and from the URC tick, so a job progresses whether or not
 * anybody has the app open. */
void unoxfer_job_tick(void);

int  unoxfer_job_status(int id, unoxfer_job_stat *out);
int  unoxfer_job_cancel(int id);
/* The per-file result list, one line at a time: "ok <bytes> <name>" or
 * "err <code> <name>".  Returns the length written, or -1 past the end. */
int  unoxfer_job_log(int id, int line, char *out, int cap);
/* Release a finished job's slot.  A finished job is kept until it is reaped
 * (or the table needs the slot) so a client that reconnects after a drop can
 * still read the result - which is the whole point of the id. */
void unoxfer_job_reap(int id);

/* ---- the automation verb -------------------------------------------------
 * The unoautomate pass-through, same shape as ssh_dbg_cmd / r8169_dbg_cmd:
 * unoautomate lands one weak stub and one dispatch clause, the sub-verb
 * grammar and the output format are ours.  Returns the reply length, or -1 for
 * a bad command. */
int unoxfer_cmd(const char *line, char *out, int cap);

/* Register the live transport gates into SPECTEST's `network` area.  No-op in
 * a production build.  Called once from uno_pc64_init. */
void unoxfer_register_tests(void);

#endif
