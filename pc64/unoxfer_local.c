/* ===========================================================================
 * UnoDOS/pc64 - unoxfer's LOCAL backend: a volume on this machine.
 *
 * Portage has one of these too (LocalTransferClient), and for the same
 * non-obvious reason: it makes "copy from this folder to that server" and
 * "copy from this folder to that folder" the SAME code path.  Without it the
 * app grows a second, local-only transfer routine that then has its own
 * progress reporting, its own cancel, and its own bugs.
 *
 * It is also what makes the URC verb's `push` work: the source of a push is a
 * local volume, and a local volume is just another client.
 * ======================================================================== */
#include "unoxfer_int.h"
#include "pc64_fs.h"
#include "fat.h"

void *malloc(unsigned long);
void  free(void *);
void *memset(void *, int, unsigned long);
unsigned long strlen(const char *);

typedef struct { int vol; } lx;

static int lx_open(unoxfer_client *c, const unoxfer_site *s)
{
    lx *x;
    int nv = uno_fs_volumes();
    if (s->vol < 0 || s->vol >= nv)
        return ux_failf(c, UNOXFER_EARG, "volume %d does not exist (%d volumes)",
                        s->vol, nv);
    x = (lx *)malloc(sizeof *x);
    if (!x) return ux_fail(c, UNOXFER_EIO, "out of memory");
    x->vol = s->vol;
    c->impl = x;
    if (!uno_fs_writable(s->vol))
        c->caps &= ~(unsigned)(UNOXFER_CAP_PUT | UNOXFER_CAP_MKDIR | UNOXFER_CAP_DELETE);
    /* Only native FAT has subdirectories.  The RAM disk is FLAT, so a
     * recursive walk of it is a walk of one level - saying that through caps
     * is better than a listing that quietly returns nothing for "\SUB". */
    if (uno_fs_kind(s->vol) != 1)
        c->caps &= ~(unsigned)UNOXFER_CAP_MKDIR;
    return UNOXFER_OK;
}

static void lx_close(unoxfer_client *c)
{
    if (c->impl) { free(c->impl); c->impl = 0; }
}

static int lx_list(unoxfer_client *c, const char *path,
                   unoxfer_ent *out, int max, int *total)
{
    lx *x = (lx *)c->impl;
    int fv = uno_fs_fat_index(x->vol), n = 0, i, wrote = 0;

    if (fv >= 0) {
        /* the native path: metadata AND subdirectory entries */
        static uno_fat_entry ents[128];
        int want = max < 128 ? max : 128;
        n = uno_fat_list_ex(fv, path && *path ? path : "", ents, want);
        if (total) *total = n;
        for (i = 0; i < n && i < want && wrote < max; i++) {
            memset(&out[wrote], 0, sizeof out[wrote]);
            ux_cpy(out[wrote].name, (int)sizeof out[wrote].name, ents[i].name);
            out[wrote].is_dir = (unsigned char)(ents[i].is_dir != 0);
            out[wrote].size = ents[i].is_dir ? 0ull : (unsigned long long)ents[i].size;
            wrote++;
        }
        return wrote;
    }

    /* RAM disk / firmware SFS: names only, root only.  uno_fs_list_dir gives
     * no size and no dir flag, so every entry reports as a zero-length file.
     * That is a real limitation of those volumes and it is reported as one -
     * a plan built on it will transfer the files and find no directories,
     * which is exactly what is there. */
    n = uno_fs_list_begin(x->vol);
    if (total) *total = n;
    for (i = 0; i < n && wrote < max; i++) {
        char nm[64];
        if (!uno_fs_list_get(x->vol, i, nm, (int)sizeof nm)) continue;
        memset(&out[wrote], 0, sizeof out[wrote]);
        ux_cpy(out[wrote].name, (int)sizeof out[wrote].name, nm);
        out[wrote].size = (unsigned long long)uno_fs_size(x->vol, nm);
        wrote++;
    }
    return wrote;
}

static long long lx_size(unoxfer_client *c, const char *p)
{
    lx *x = (lx *)c->impl;
    long n = uno_fs_size(x->vol, p);
    return n < 0 ? (long long)UNOXFER_ENOENT : (long long)n;
}

/* A local "get" is a copy from this volume into `vol`.  It goes through the
 * same staging buffer and the same .PART commit as a network transfer, rather
 * than through uno_fs_copytree: this way a copy that is cancelled halfway
 * behaves identically to a download that is, which is one fewer thing for the
 * app's Stop button to get wrong. */
static int lx_get(unoxfer_client *c, const char *rpath, long long off,
                  int vol, const char *lpath, unoxfer_prog *p)
{
    lx *x = (lx *)c->impl;
    unsigned char *buf;
    long long capn = 0;
    long n, sz = uno_fs_size(x->vol, rpath);
    int rc;

    (void)off;
    if (sz < 0) return ux_failf(c, UNOXFER_ENOENT, "no such file: %s", rpath);
    buf = ux_stage_get(sz, &capn);
    if (!buf) return ux_fail(c, UNOXFER_EIO, "the staging buffer is busy");
    if (sz > capn) { ux_stage_put(); return ux_failf(c, UNOXFER_ETOOBIG,
        "%s is %ld bytes, over the %ld byte staging cap", rpath, sz, (long)capn); }

    n = uno_fs_read(x->vol, rpath, buf, sz);
    if (n < 0) { ux_stage_put(); return ux_failf(c, UNOXFER_EIO, "read failed: %s", rpath); }
    if (p) { p->done = (unsigned long long)n; p->total = (unsigned long long)n; }
    rc = ux_commit_file(vol, lpath, buf, n);
    ux_stage_put();
    if (rc != UNOXFER_OK) return ux_failf(c, rc, "write failed: %s", lpath);
    return UNOXFER_OK;
}

static int lx_put(unoxfer_client *c, int vol, const char *lpath,
                  const char *rpath, unoxfer_prog *p)
{
    /* Push into a local volume: the same copy, sources swapped.  Reusing the
     * one implementation is deliberate; two near-identical copy loops is how
     * one of them ends up without the cancel check. */
    lx *x = (lx *)c->impl;
    unoxfer_client tmp = *c;
    lx tx;
    int rc;
    tx.vol = vol;
    tmp.impl = &tx;
    rc = lx_get(&tmp, lpath, 0, x->vol, rpath, p);
    if (rc != UNOXFER_OK) ux_cpy(c->err, (int)sizeof c->err, tmp.err);
    return rc;
}

static int lx_mkdir(unoxfer_client *c, const char *path)
{
    lx *x = (lx *)c->impl;
    if (uno_fs_isdir(x->vol, path)) return UNOXFER_OK;      /* idempotent */
    return uno_fs_mkdir(x->vol, path) ? UNOXFER_OK
         : ux_failf(c, UNOXFER_EPERM, "mkdir failed: %s", path);
}

static int lx_del(unoxfer_client *c, const char *path)
{
    lx *x = (lx *)c->impl;
    int fv = uno_fs_fat_index(x->vol);
    if (fv < 0) return ux_fail(c, UNOXFER_EUNSUP, "this volume cannot delete");
    return uno_fat_delete(fv, path) ? UNOXFER_OK
         : ux_failf(c, UNOXFER_ENOENT, "delete failed: %s", path);
}

const unoxfer_backend unoxfer_be_local = {
    "local",
    UNOXFER_CAP_LIST | UNOXFER_CAP_GET | UNOXFER_CAP_PUT |
    UNOXFER_CAP_MKDIR | UNOXFER_CAP_DELETE | UNOXFER_CAP_SIZE,
    lx_open, lx_close, lx_list, lx_size, lx_get, lx_put, lx_mkdir, lx_del
};
