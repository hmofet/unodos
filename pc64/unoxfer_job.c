/* ===========================================================================
 * UnoDOS/pc64 - unoxfer's recursive transfer engine.
 *
 * PLAN, THEN RUN - Portage's SyncEngine, and the reason to keep the shape is
 * the reason it had it: the walk produces a file count and a byte total before
 * a single byte moves, so progress is a real percentage rather than a spinner,
 * and a job that is going to fail on a missing directory fails while planning
 * instead of three files in.
 *
 * A STEP MACHINE, NOT A LOOP.  unoxfer_job_tick() does ONE bounded unit of
 * work - list one directory, or move one file - and returns.  Everything about
 * this subsystem's front ends depends on that:
 *
 *   - the app calls it from the frame loop, so a stalled server costs a frame
 *   - the URC verb calls it from the remote tick, so `xfer pull` can RETURN
 *     immediately with a job id.  A URC command that blocked for the length of
 *     a multi-gigabyte transfer would park the dispatcher, miss the guard's
 *     deadline, and get the box hard-reset by its own dead-man's switch.
 *
 * The unit is a whole FILE rather than a slice of one, which is the honest
 * limit of this version: a single very large file still occupies one tick for
 * as long as it takes.  Bounding it further needs a resumable per-file reader
 * in every backend, and that in turn needs unofs's append (UNOXFER.md) - so
 * the seam is drawn where it can actually be held.
 * ======================================================================== */
#include "unoxfer_int.h"
#include "pc64_fs.h"

void *malloc(unsigned long);
void  free(void *);
void *realloc(void *, unsigned long);
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
int   snprintf(char *, unsigned long, const char *, ...);

/* ---- the plan ------------------------------------------------------------
 * Paths live in one string arena and items reference them by offset, because
 * a per-item char[256] would make a 2000-file plan half a megabyte of mostly
 * padding on a 32 MB machine.  The arena is CAPPED (below) and the cap is
 * REPORTED: a plan that would not fit is refused, never silently truncated -
 * a truncated plan transfers some of a directory and says it transferred the
 * directory, which is the worst failure this engine could have. */
#define UX_ARENA_CAP  (192 * 1024)
#define UX_ITEM_CAP   2048
#define UX_DIRQ_CAP   256

typedef struct {
    unsigned           rpath, lpath;      /* arena offsets                    */
    unsigned long long size;
    signed char        rc;                /* 0 pending, 1 done, else -UNOXFER */
    unsigned char      is_dir;
} ux_item;

typedef struct {
    unoxfer_job_state state;
    int               used;
    int               push;               /* 0 = pull (remote->local)         */
    int               recurse;
    int               vol;
    unoxfer_site      site;

    unoxfer_client   *rem;                /* the remote end                   */
    unoxfer_client   *loc;                /* the local end, for a push walk   */

    char             *arena;
    unsigned          arena_len;
    ux_item          *item;
    int               nitem, item_cap;

    /* the breadth-first directory queue, as arena offsets into (r,l) pairs */
    unsigned          dq_r[UX_DIRQ_CAP], dq_l[UX_DIRQ_CAP];
    int               dq_head, dq_tail;

    int               cursor;             /* which item is being moved        */
    unoxfer_prog      prog;
    unoxfer_job_stat  stat;
} ux_job;

static ux_job g_job[UNOXFER_MAXJOB];

/* ---- arena --------------------------------------------------------------- */
static unsigned arena_put(ux_job *j, const char *s)
{
    unsigned n = (unsigned)strlen(s) + 1, off = j->arena_len;
    if (off + n > UX_ARENA_CAP) return 0xFFFFFFFFu;
    memcpy(j->arena + off, s, n);
    j->arena_len += n;
    return off;
}

static const char *arena_get(ux_job *j, unsigned off)
{ return off == 0xFFFFFFFFu ? "" : j->arena + off; }

static int item_add(ux_job *j, unsigned r, unsigned l,
                    unsigned long long size, int is_dir)
{
    if (j->nitem >= j->item_cap) {
        int cap = j->item_cap ? j->item_cap * 2 : 64;
        ux_item *n;
        if (cap > UX_ITEM_CAP) return 0;
        n = (ux_item *)realloc(j->item, (unsigned long)cap * sizeof *n);
        if (!n) return 0;
        j->item = n; j->item_cap = cap;
    }
    j->item[j->nitem].rpath = r;
    j->item[j->nitem].lpath = l;
    j->item[j->nitem].size = size;
    j->item[j->nitem].rc = 0;
    j->item[j->nitem].is_dir = (unsigned char)is_dir;
    j->nitem++;
    return 1;
}

/* Two levels of teardown, and the distinction matters.
 *
 * job_hangup() drops the CONNECTIONS - sockets, SSH channels, the staging
 * buffer's holder - the moment a job stops needing them, which is as soon as
 * it ends however it ends.  job_free() additionally throws away the PLAN.
 *
 * They are separate because `xfer log <id>` reads the plan, and the whole
 * reason a job has a stable id is so a client that lost the link can come back
 * and ask what happened.  Freeing the plan when the job ends would make every
 * failed job report "unknown id" - which is exactly the moment somebody wants
 * to know.  So the plan lives until it is reaped, or until the table needs the
 * slot for a new job. */
static void job_hangup(ux_job *j)
{
    if (j->rem) { unoxfer_close(j->rem); j->rem = 0; }
    if (j->loc) { unoxfer_close(j->loc); j->loc = 0; }
}

static void job_free(ux_job *j)
{
    job_hangup(j);
    if (j->arena) { free(j->arena); j->arena = 0; }
    if (j->item)  { free(j->item);  j->item = 0; }
    j->arena_len = 0; j->nitem = 0; j->item_cap = 0;
}

static int job_fail(ux_job *j, int rc, const char *msg)
{
    j->state = UNOXFER_JOB_FAILED;
    j->stat.state = UNOXFER_JOB_FAILED;
    j->stat.rc = rc;
    ux_cpy(j->stat.msg, (int)sizeof j->stat.msg, msg);
    job_hangup(j);
    return rc;
}

/* ===========================================================================
 * Starting a job.
 * ======================================================================== */
static int job_slot(void)
{
    int i;
    for (i = 0; i < UNOXFER_MAXJOB; i++) if (!g_job[i].used) return i;
    /* reuse the oldest FINISHED slot rather than refusing: a client that never
     * reaps should degrade to losing old results, not to being unable to
     * start a transfer. */
    for (i = 0; i < UNOXFER_MAXJOB; i++)
        if (g_job[i].state == UNOXFER_JOB_DONE ||
            g_job[i].state == UNOXFER_JOB_FAILED ||
            g_job[i].state == UNOXFER_JOB_CANCELLED) { job_free(&g_job[i]); return i; }
    return -1;
}

static int job_start(const unoxfer_site *site, int push, const char *rpath,
                     int vol, const char *lpath, int recurse)
{
    char err[160];
    int id = job_slot();
    ux_job *j;
    unsigned r, l;

    if (id < 0) return UNOXFER_ENOSPC;
    j = &g_job[id];
    memset(j, 0, sizeof *j);
    j->used = 1;
    j->push = push;
    j->recurse = recurse;
    j->vol = vol;
    j->site = *site;
    j->state = UNOXFER_JOB_PLANNING;
    j->stat.state = UNOXFER_JOB_PLANNING;

    if (!uno_fs_writable(vol) && !push)
        return job_fail(j, UNOXFER_EPERM, "that volume is read-only");

    j->arena = (char *)malloc(UX_ARENA_CAP);
    if (!j->arena) return job_fail(j, UNOXFER_EIO, "out of memory");

    j->rem = unoxfer_open(site, err, (int)sizeof err);
    if (!j->rem) return job_fail(j, UNOXFER_EIO, err);

    /* Refuse a recursive job the protocol cannot walk, HERE, before anything
     * moves.  TFTP has no listing at all; a "recursive TFTP pull" is not a
     * slow job, it is an impossible one, and finding that out after three
     * files is worse than being told now. */
    if (recurse && !(unoxfer_caps(j->rem) & UNOXFER_CAP_LIST))
        return job_fail(j, UNOXFER_EUNSUP,
                        "this protocol cannot list directories, so -r is impossible");

    if (push) {
        unoxfer_site ls;
        memset(&ls, 0, sizeof ls);
        ls.proto = UNOXFER_LOCAL;
        ls.vol = vol;
        ux_cpy(ls.name, (int)sizeof ls.name, "local");
        j->loc = unoxfer_open(&ls, err, (int)sizeof err);
        if (!j->loc) return job_fail(j, UNOXFER_EIO, err);
    }

    r = arena_put(j, rpath);
    l = arena_put(j, lpath);
    if (r == 0xFFFFFFFFu || l == 0xFFFFFFFFu)
        return job_fail(j, UNOXFER_EARG, "path too long");

    if (recurse) {
        j->dq_r[j->dq_tail] = r;
        j->dq_l[j->dq_tail] = l;
        j->dq_tail++;
        /* The destination root itself has to exist before anything lands in
         * it, so it is item zero. */
        if (!item_add(j, r, l, 0, 1)) return job_fail(j, UNOXFER_ENOSPC, "plan too large");
    } else {
        long long sz = unoxfer_size(j->rem, rpath);
        if (!item_add(j, r, l, sz > 0 ? (unsigned long long)sz : 0ull, 0))
            return job_fail(j, UNOXFER_ENOSPC, "plan too large");
        if (sz > 0) j->stat.bytes_total = (unsigned long long)sz;
        j->stat.files_total = 1;
        j->state = UNOXFER_JOB_RUNNING;
        j->stat.state = UNOXFER_JOB_RUNNING;
    }
    return id;
}

int unoxfer_job_pull(const unoxfer_site *site, const char *rpath,
                     int vol, const char *lpath, int recurse)
{ return job_start(site, 0, rpath, vol, lpath, recurse); }

int unoxfer_job_push(const unoxfer_site *site, int vol, const char *lpath,
                     const char *rpath, int recurse)
{ return job_start(site, 1, rpath, vol, lpath, recurse); }

/* ===========================================================================
 * The planning step: list ONE directory and fold it into the plan.
 * ======================================================================== */
#define UX_LISTMAX 128

static void step_plan(ux_job *j)
{
    static unoxfer_ent ents[UX_LISTMAX];
    unoxfer_client *walker = j->push ? j->loc : j->rem;
    const char *rdir, *ldir;
    char rbuf[UNOXFER_PATHLEN], lbuf[UNOXFER_PATHLEN];
    int n, total = 0, i;

    if (j->dq_head >= j->dq_tail) {                 /* the walk is finished */
        j->state = UNOXFER_JOB_RUNNING;
        j->stat.state = UNOXFER_JOB_RUNNING;
        j->cursor = 0;
        return;
    }
    rdir = arena_get(j, j->dq_r[j->dq_head]);
    ldir = arena_get(j, j->dq_l[j->dq_head]);
    j->dq_head++;

    /* The walker lists whichever side is the SOURCE: a pull walks the remote
     * tree, a push walks the local one.  Everything below is direction-blind
     * because of that one line, which is the whole reason the local volume is
     * an unoxfer_client and not a special case. */
    n = unoxfer_list(walker, j->push ? ldir : rdir, ents, UX_LISTMAX, &total);
    if (n < 0) { job_fail(j, n, unoxfer_error(walker)); return; }
    if (total > n) {
        job_fail(j, UNOXFER_ENOSPC,
                 "a directory has more than 128 entries; this version cannot "
                 "plan it, and half a directory is not an answer");
        return;
    }

    for (i = 0; i < n; i++) {
        unsigned r, l;
        char fat[16];
        if (ux_eq(ents[i].name, ".") || ux_eq(ents[i].name, "..")) continue;

        if (!unoxfer_rjoin(rbuf, (int)sizeof rbuf, rdir, ents[i].name)) continue;
        /* The LOCAL name is mapped onto something FAT will accept; the remote
         * name is kept verbatim.  The mapping is recorded in the plan so the
         * log can show both, rather than landing MYLONG~1.TXT and saying
         * nothing. */
        unoxfer_fatname(fat, (int)sizeof fat, ents[i].name);
        if (!unoxfer_ljoin(lbuf, (int)sizeof lbuf, ldir, fat)) continue;

        r = arena_put(j, rbuf);
        l = arena_put(j, lbuf);
        if (r == 0xFFFFFFFFu || l == 0xFFFFFFFFu) {
            job_fail(j, UNOXFER_ENOSPC, "the plan's path arena is full");
            return;
        }
        if (!item_add(j, r, l, ents[i].size, ents[i].is_dir)) {
            job_fail(j, UNOXFER_ENOSPC, "the plan is larger than this version can hold");
            return;
        }
        if (ents[i].is_dir) {
            if (j->dq_tail >= UX_DIRQ_CAP) {
                job_fail(j, UNOXFER_ENOSPC, "the directory tree is too deep/wide to plan");
                return;
            }
            j->dq_r[j->dq_tail] = r;
            j->dq_l[j->dq_tail] = l;
            j->dq_tail++;
        } else {
            j->stat.files_total++;
            j->stat.bytes_total += ents[i].size;
        }
    }
}

/* ===========================================================================
 * The running step: move ONE item.
 * ======================================================================== */
static void step_run(ux_job *j)
{
    ux_item *it;
    const char *rp, *lp;
    int rc;

    while (j->cursor < j->nitem && j->item[j->cursor].rc != 0) j->cursor++;
    if (j->cursor >= j->nitem) {
        j->state = j->stat.errors ? UNOXFER_JOB_FAILED : UNOXFER_JOB_DONE;
        j->stat.state = j->state;
        if (j->stat.errors && !j->stat.msg[0])
            ux_cpy(j->stat.msg, (int)sizeof j->stat.msg, "some files failed - see `xfer log`");
        job_hangup(j);
        return;
    }
    it = &j->item[j->cursor];
    rp = arena_get(j, it->rpath);
    lp = arena_get(j, it->lpath);

    if (it->is_dir) {
        /* Create the DESTINATION directory: local for a pull, remote for a
         * push.  Idempotent on both sides, so a re-run of a partly-done job
         * does not fail on the directories it already made. */
        if (j->push) rc = unoxfer_mkdir(j->rem, rp);
        else         rc = uno_fs_isdir(j->vol, lp) ? UNOXFER_OK
                        : (uno_fs_mkdir(j->vol, lp) ? UNOXFER_OK : UNOXFER_EPERM);
        it->rc = (signed char)(rc == UNOXFER_OK ? 1 : rc);
        if (rc != UNOXFER_OK) {
            j->stat.errors++;
            if (!j->stat.msg[0])
                snprintf(j->stat.msg, sizeof j->stat.msg, "mkdir failed: %s",
                         j->push ? rp : lp);
        }
        j->cursor++;
        return;
    }

    ux_cpy(j->stat.cur, (int)sizeof j->stat.cur, unoxfer_basename(j->push ? lp : rp));
    j->prog.done = 0;
    j->prog.total = it->size;
    ux_cpy(j->prog.file, (int)sizeof j->prog.file, j->stat.cur);

    if (j->push) rc = unoxfer_put(j->rem, j->vol, lp, rp, &j->prog);
    else         rc = unoxfer_get(j->rem, rp, j->vol, lp, &j->prog);

    it->rc = (signed char)(rc == UNOXFER_OK ? 1 : rc);
    if (rc == UNOXFER_OK) {
        j->stat.files_done++;
        /* Count what actually moved, not what the listing claimed: a server
         * whose `ls` size disagrees with the bytes it sends should make the
         * total drift, not make the progress bar lie. */
        j->stat.bytes_done += j->prog.done ? j->prog.done : it->size;
    } else {
        j->stat.errors++;
        if (rc == UNOXFER_ECANCEL) {
            j->state = UNOXFER_JOB_CANCELLED;
            j->stat.state = UNOXFER_JOB_CANCELLED;
            ux_cpy(j->stat.msg, (int)sizeof j->stat.msg, "cancelled");
            job_hangup(j);
            return;
        }
        if (!j->stat.msg[0])
            ux_cpy(j->stat.msg, (int)sizeof j->stat.msg, unoxfer_error(j->rem));
    }
    j->cursor++;
}

void unoxfer_job_tick(void)
{
    int i;
    for (i = 0; i < UNOXFER_MAXJOB; i++) {
        ux_job *j = &g_job[i];
        if (!j->used) continue;
        if (j->state == UNOXFER_JOB_PLANNING) step_plan(j);
        else if (j->state == UNOXFER_JOB_RUNNING) step_run(j);
    }
}

/* ===========================================================================
 * Status, cancel, log, reap.
 * ======================================================================== */
int unoxfer_job_status(int id, unoxfer_job_stat *out)
{
    if (id < 0 || id >= UNOXFER_MAXJOB || !g_job[id].used) return UNOXFER_EARG;
    if (out) {
        *out = g_job[id].stat;
        /* Fold the in-flight file's progress into the byte count so a single
         * large file does not read as 0% for its whole duration - the number a
         * watcher checks is "is this moving", and per-file granularity answers
         * "no" for minutes at a time. */
        if (g_job[id].state == UNOXFER_JOB_RUNNING)
            out->bytes_done += g_job[id].prog.done;
    }
    return UNOXFER_OK;
}

int unoxfer_job_cancel(int id)
{
    ux_job *j;
    if (id < 0 || id >= UNOXFER_MAXJOB || !g_job[id].used) return UNOXFER_EARG;
    j = &g_job[id];
    if (j->state == UNOXFER_JOB_DONE || j->state == UNOXFER_JOB_FAILED ||
        j->state == UNOXFER_JOB_CANCELLED) return UNOXFER_OK;
    /* Set the flag the backend reads BETWEEN slices, then mark the job.  The
     * transfer in flight notices at its next read; it is not torn down from
     * under itself, which is what would leave a socket and a .PART behind. */
    j->prog.cancel = 1;
    j->state = UNOXFER_JOB_CANCELLED;
    j->stat.state = UNOXFER_JOB_CANCELLED;
    ux_cpy(j->stat.msg, (int)sizeof j->stat.msg, "cancelled");
    job_hangup(j);
    return UNOXFER_OK;
}

int unoxfer_job_log(int id, int line, char *out, int cap)
{
    ux_job *j;
    ux_item *it;
    if (id < 0 || id >= UNOXFER_MAXJOB || !g_job[id].used) return -1;
    j = &g_job[id];
    if (line < 0 || line >= j->nitem || !j->item) return -1;
    it = &j->item[line];
    if (it->rc == 1)
        return snprintf(out, (unsigned long)cap, "ok %llu %s", it->size,
                        arena_get(j, j->push ? it->lpath : it->rpath));
    if (it->rc == 0)
        return snprintf(out, (unsigned long)cap, "pending %s",
                        arena_get(j, j->push ? it->lpath : it->rpath));
    return snprintf(out, (unsigned long)cap, "err %d (%s) %s", it->rc,
                    unoxfer_strerror(it->rc),
                    arena_get(j, j->push ? it->lpath : it->rpath));
}

void unoxfer_job_reap(int id)
{
    if (id < 0 || id >= UNOXFER_MAXJOB) return;
    job_free(&g_job[id]);
    memset(&g_job[id], 0, sizeof g_job[id]);
}
