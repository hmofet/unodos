/* ===========================================================================
 * UnoDOS/pc64 - unoxfer's private surface: the backend vtable and the shared
 * helpers the backends use.  NOT a public header - nothing outside unoxfer*.c
 * includes this.  The public contract is unoxfer.h.
 * ======================================================================== */
#ifndef PC64_UNOXFER_INT_H
#define PC64_UNOXFER_INT_H
#include "unoxfer.h"

/* ---- the vtable ----------------------------------------------------------
 * One row per protocol.  Every entry may be NULL except open/close: a NULL
 * slot is UNOXFER_EUNSUP, reported through caps BEFORE anything is planned,
 * rather than a crash or a transfer that fails halfway.
 *
 * `impl` is the backend's own state, allocated in open() and freed in close().
 * The core never looks inside it. */
typedef struct unoxfer_backend {
    const char *name;
    unsigned    caps;                       /* the static ceiling; open() may
                                               clear bits it finds missing   */
    int  (*open) (unoxfer_client *c, const unoxfer_site *s);
    void (*close)(unoxfer_client *c);
    int  (*list) (unoxfer_client *c, const char *path,
                  unoxfer_ent *out, int max, int *total);
    long long (*size)(unoxfer_client *c, const char *rpath);
    /* get/put move ONE file.  `off` is the byte to start at (resume); a
     * backend without UNOXFER_CAP_RESUME is never called with off != 0. */
    int  (*get)  (unoxfer_client *c, const char *rpath, long long off,
                  int vol, const char *lpath, unoxfer_prog *p);
    int  (*put)  (unoxfer_client *c, int vol, const char *lpath,
                  const char *rpath, unoxfer_prog *p);
    int  (*mkdir)(unoxfer_client *c, const char *path);
    int  (*del)  (unoxfer_client *c, const char *path);
} unoxfer_backend;

struct unoxfer_client {
    const unoxfer_backend *b;
    void         *impl;
    unsigned      caps;
    unoxfer_proto proto;
    unoxfer_site  site;
    char          err[160];
};

/* Each backend exports exactly one of these. */
extern const unoxfer_backend unoxfer_be_local;
extern const unoxfer_backend unoxfer_be_scp;
extern const unoxfer_backend unoxfer_be_sftp;
extern const unoxfer_backend unoxfer_be_http;
extern const unoxfer_backend unoxfer_be_webdav;
extern const unoxfer_backend unoxfer_be_tftp;

/* ---- shared helpers (unoxfer.c) ------------------------------------------ */

/* Set the client's error sentence.  Returns `rc` so a backend can write
 *     return ux_fail(c, UNOXFER_EIO, "connect refused");
 * which is the shape that stops a failure path from forgetting to say why. */
int  ux_fail(unoxfer_client *c, int rc, const char *msg);
int  ux_failf(unoxfer_client *c, int rc, const char *fmt, ...);

/* THE staging buffer.  One per machine, not one per client: two concurrent
 * jobs each holding 8 MB out of a 32 MB heap is how a transfer takes the
 * desktop down with it.  ux_stage_get() hands out the single buffer and fails
 * cleanly if somebody already has it; ux_stage_put() returns it.  The buffer
 * is allocated on first use and freed when the last holder lets go, so a box
 * that never transfers anything pays nothing. */
unsigned char *ux_stage_get(long long want, long long *got);
void           ux_stage_put(void);

/* Write a whole staged file to a volume, through the .PART -> rename commit
 * that UNOXFER.md describes.  `final` is the real DOS path; the caller never
 * writes the final name itself. */
int  ux_commit_file(int vol, const char *final, const unsigned char *buf, long len);
/* The .PART path for a final path, so resume can measure what is already
 * there.  Returns 0 on success. */
int  ux_partname(char *dst, int cap, const char *final);
/* Append to a .PART if unofs's uno_fat_append() is linked, else fail with
 * UNOXFER_EUNSUP so the caller falls back to staging.  See unoxfer_streaming(). */
int  ux_append_part(int vol, const char *part, const unsigned char *buf, long len);

/* Bounded, allocation-free string helpers.  The tree already has snprintf,
 * but these say what they mean at the call site and never truncate silently:
 * ux_cat returns 0 if it had to cut, which callers building a path check. */
int  ux_cpy(char *dst, int cap, const char *src);
int  ux_cat(char *dst, int cap, const char *src);
int  ux_eq (const char *a, const char *b);
int  ux_ieq(const char *a, const char *b);          /* ASCII case-insensitive */
/* strtoull without the locale baggage; stops at the first non-digit. */
unsigned long long ux_u64(const char *s);

/* Poll the network for up to `ms`, returning early the moment `ready()` says
 * so.  Every backend needs this loop and none of them should write it again:
 * getting the "pump the NIC while you wait" part wrong is how a transfer
 * stalls forever on a link that is working fine. */
int  ux_wait(int (*ready)(void *), void *ctx, int ms);

#endif
