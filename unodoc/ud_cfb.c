/* ===========================================================================
 * ud_cfb.c - the Compound File Binary container [MS-CFB], read AND write.
 *
 * A CFB file is a FAT filesystem inside a file.  After a 512-byte header
 * come fixed-size sectors (512 in version 3, which is what Office 97
 * writes; 4096 in version 4, which we read).  A sector allocation table
 * (the FAT) chains them.  Streams below a 4096-byte cutoff do not get whole
 * sectors: they are packed into a single "mini stream" - itself an ordinary
 * sector chain hanging off the root directory entry - and chained by a
 * second table, the mini FAT.  The FAT's own sector numbers are listed in
 * the DIFAT: 109 slots in the header, then chained DIFAT sectors.  On top
 * of all that sits a directory of storages (folders) and streams (files),
 * held as a red-black tree of 128-byte entries.
 *
 * READING is hostile-input work.  This file is the first thing that touches
 * a .doc off a USB stick, so: every sector index is range-checked before
 * use, every chain walk is step-bounded so a loop terminates instead of
 * hanging, every declared size is clamped to what the file can physically
 * hold, and the directory's sibling trees are flattened with a visited
 * bitmap so a cyclic or self-referencing tree cannot spin.  Nothing here
 * trusts a number because the file said so.
 *
 * WRITING is never in place.  A caller builds a model (ud_cfbw_*) and this
 * file serialises a fresh container from it: streams laid out, mini vs
 * regular chosen by the cutoff, FAT / mini FAT / DIFAT / directory built to
 * match, and the directory emitted as a balanced, all-black tree in CFB
 * name order (length first, then uppercased code units).  The ordering is
 * the part real Office actually checks; the colouring it ignores.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include <string.h>

/* ---- on-disk constants ---------------------------------------------------- */
#define CFB_SIG0     0xE011CFD0u    /* D0 CF 11 E0 */
#define CFB_SIG1     0xE11AB1A1u    /* A1 B1 1A E1 */

#define MAXREGSECT   0xFFFFFFFAu    /* last addressable sector number        */
#define DIFSECT      0xFFFFFFFCu
#define FATSECT      0xFFFFFFFDu
#define ENDOFCHAIN   0xFFFFFFFEu
#define FREESECT     0xFFFFFFFFu
#define NOSTREAM     0xFFFFFFFFu

#define DIRENT_SZ    128
#define MINI_SZ      64
#define MINI_SHIFT   6

/* header field offsets */
#define H_SIG        0x00
#define H_CLSID      0x08
#define H_MINORVER   0x18
#define H_MAJORVER   0x1A
#define H_BYTEORDER  0x1C
#define H_SECSHIFT   0x1E
#define H_MINISHIFT  0x20
#define H_NDIRSECT   0x28
#define H_NFATSECT   0x2C
#define H_DIRSTART   0x30
#define H_TRANSIG    0x34
#define H_CUTOFF     0x38
#define H_MFATSTART  0x3C
#define H_NMFATSECT  0x40
#define H_DIFATSTART 0x44
#define H_NDIFATSECT 0x48
#define H_DIFAT      0x4C          /* 109 * u32, to 0x200                    */
#define H_DIFAT_N    109

/* directory entry field offsets */
#define D_NAME       0x00          /* 64 bytes UTF-16LE                      */
#define D_NAMELEN    0x40          /* u16, BYTES incl. the terminator        */
#define D_TYPE       0x42
#define D_COLOR      0x43
#define D_LEFT       0x44
#define D_RIGHT      0x48
#define D_CHILD      0x4C
#define D_CLSID      0x50
#define D_STATE      0x60
#define D_CTIME      0x64
#define D_MTIME      0x6C
#define D_START      0x74
#define D_SIZE       0x78          /* u64 (v3: high dword must be zero)      */

/* ===========================================================================
 * READER
 * ======================================================================== */

typedef struct {
    char          name[UD_NAME_BUF];
    unsigned char clsid[16];
    unsigned char type;
    unsigned char mini;            /* payload lives in the mini stream       */
    uint32_t      start;
    long          size;            /* clamped to what the file can hold      */
    int32_t       left, right, kid;   /* as read from the file               */
    int           first, next;     /* flattened child list / next sibling    */
    int           parent;
} ud_dirent;

/* a "where did the last walk of this chain get to" cursor: without it a
 * sequential read of an N-sector stream is O(N^2) chain steps. */
typedef struct {
    uint32_t start;
    long     blk;
    uint32_t sect;
    int      valid;
} ud_cur;

struct ud_cfb {
    ud_src     src;
    int        ver;                /* 3 or 4                                 */
    long       ssz;                /* sector size in bytes                   */
    int        sshift;
    uint32_t   nsect;              /* sectors physically present in the file */
    long       cutoff;

    uint32_t  *fat;   uint32_t fatn;
    uint32_t  *mfat;  uint32_t mfatn;

    ud_dirent *dir;   int dirn;

    uint32_t   ministart;          /* root's chain = the mini stream         */
    long       minisize;

    ud_cur     rcur;               /* user regular streams                   */
    ud_cur     mcur;               /* the mini stream's own backing chain    */
    ud_cur     micur;              /* mini FAT walks                         */
};

/* ---- chain walking --------------------------------------------------------
 * Returns the sector number `blk` steps along the chain starting at `start`,
 * or ENDOFCHAIN if the chain ends, leaves the table, or loops.  The step
 * budget (one table's worth) is what turns a cyclic FAT from a hang into a
 * clean short read. */
static uint32_t chain_at(const uint32_t *tab, uint32_t tabn, uint32_t start,
                         long blk, ud_cur *cur)
{
    uint32_t s;
    long i, steps = 0;

    if (blk < 0) return ENDOFCHAIN;
    if (cur && cur->valid && cur->start == start && cur->blk <= blk) {
        s = cur->sect; i = cur->blk;
    } else {
        s = start;    i = 0;
    }
    while (i < blk) {
        if (s > MAXREGSECT || s >= tabn) return ENDOFCHAIN;
        s = tab[s];
        i++;
        if (++steps > (long)tabn + 1) return ENDOFCHAIN;   /* cycle guard */
    }
    if (s > MAXREGSECT) return ENDOFCHAIN;
    if (cur) { cur->valid = 1; cur->start = start; cur->blk = blk; cur->sect = s; }
    return s;
}

/* Read `n` bytes at `off` from a chain of whole sectors. */
static long read_regular(ud_cfb *c, uint32_t start, long size, long off,
                         unsigned char *dst, long n, ud_cur *cur)
{
    long got = 0;

    if (off < 0 || n <= 0 || size <= 0 || off >= size) return 0;
    if (n > size - off) n = size - off;
    while (n > 0) {
        long blk   = off / c->ssz;
        long inner = off % c->ssz;
        long chunk = c->ssz - inner;
        uint32_t s;
        long r;

        if (chunk > n) chunk = n;
        s = chain_at(c->fat, c->fatn, start, blk, cur);
        if (s > MAXREGSECT || s >= c->nsect) break;
        r = ud_src_read(&c->src, ((long)s + 1) * c->ssz + inner, dst, chunk);
        if (r <= 0) break;
        dst += r; off += r; n -= r; got += r;
        if (r < chunk) break;
    }
    return got;
}

/* Read from a mini-stream chain: walk the mini FAT to a 64-byte mini sector,
 * then read that range out of the mini stream, which is itself an ordinary
 * sector chain. */
static long read_mini(ud_cfb *c, uint32_t start, long size, long off,
                      unsigned char *dst, long n)
{
    long got = 0;

    if (off < 0 || n <= 0 || size <= 0 || off >= size) return 0;
    if (n > size - off) n = size - off;
    while (n > 0) {
        long blk   = off >> MINI_SHIFT;
        long inner = off & (MINI_SZ - 1);
        long chunk = MINI_SZ - inner;
        uint32_t ms;
        long r;

        if (chunk > n) chunk = n;
        ms = chain_at(c->mfat, c->mfatn, start, blk, &c->micur);
        if (ms > MAXREGSECT || ms >= c->mfatn) break;
        r = read_regular(c, c->ministart, c->minisize,
                         ((long)ms << MINI_SHIFT) + inner, dst, chunk,
                         &c->mcur);
        if (r <= 0) break;
        dst += r; off += r; n -= r; got += r;
        if (r < chunk) break;
    }
    return got;
}

/* ---- header --------------------------------------------------------------- */
static int parse_header(ud_cfb *c, const unsigned char *h)
{
    uint16_t sshift, mshift;

    if (ud_rd32(h + H_SIG) != CFB_SIG0 || ud_rd32(h + H_SIG + 4) != CFB_SIG1) {
        ud_set_error("not a compound file (bad signature)");
        return 0;
    }
    if (ud_rd16(h + H_BYTEORDER) != 0xFFFE) {
        ud_set_error("compound file: unsupported byte order");
        return 0;
    }
    sshift = ud_rd16(h + H_SECSHIFT);
    mshift = ud_rd16(h + H_MINISHIFT);
    if (sshift == 9)       c->ver = 3;
    else if (sshift == 12) c->ver = 4;
    else { ud_set_error("compound file: unsupported sector size"); return 0; }
    if (mshift != MINI_SHIFT) {
        ud_set_error("compound file: unsupported mini sector size");
        return 0;
    }
    c->sshift = (int)sshift;
    c->ssz    = 1L << sshift;
    c->cutoff = (long)ud_rd32(h + H_CUTOFF);
    if (c->cutoff != UD_CFB_MINI_CUTOFF) {
        /* [MS-CFB] fixes this at 4096; anything else is a broken writer, and
           honouring it would put streams in the wrong table. */
        ud_set_error("compound file: non-standard mini stream cutoff");
        return 0;
    }
    if (c->src.size < c->ssz * 2) {
        ud_set_error("compound file: truncated (no sectors)");
        return 0;
    }
    c->nsect = (uint32_t)(c->src.size / c->ssz - 1);
    return 1;
}

/* ---- the FAT, via the DIFAT ------------------------------------------------ */
/* Pull one FAT sector's worth of entries into c->fat[slot * ents ...]. */
static int fat_sector_in(ud_cfb *c, uint32_t fs, uint32_t slot,
                         unsigned char *sec)
{
    uint32_t ents = (uint32_t)(c->ssz / 4), k;

    if (fs > MAXREGSECT || fs >= c->nsect) return 0;
    if (ud_src_read(&c->src, ((long)fs + 1) * c->ssz, sec, c->ssz) != c->ssz)
        return 0;
    for (k = 0; k < ents; k++)
        c->fat[slot * ents + k] = ud_rd32(sec + k * 4);
    return 1;
}

static int load_fat(ud_cfb *c, const unsigned char *h)
{
    uint32_t nfat = ud_rd32(h + H_NFATSECT);
    uint32_t ndif = ud_rd32(h + H_NDIFATSECT);
    uint32_t difs = ud_rd32(h + H_DIFATSTART);
    uint32_t ents = (uint32_t)(c->ssz / 4);
    uint32_t got = 0, i, guard;
    unsigned char *sec, *dsec;

    if (nfat == 0 || nfat > c->nsect) {
        ud_set_error("compound file: bad FAT sector count");
        return 0;
    }
    c->fatn = nfat * ents;
    c->fat  = (uint32_t *)ud_alloc((unsigned long)c->fatn * 4);
    sec     = (unsigned char *)ud_alloc((unsigned long)c->ssz);
    dsec    = (unsigned char *)ud_alloc((unsigned long)c->ssz);
    if (!c->fat || !sec || !dsec) {
        ud_free(sec); ud_free(dsec);
        ud_set_error("out of memory (FAT)");
        return 0;
    }

    /* the 109 slots carried in the header */
    for (i = 0; i < H_DIFAT_N && got < nfat; i++) {
        if (!fat_sector_in(c, ud_rd32(h + H_DIFAT + i * 4), got, sec)) break;
        got++;
    }
    /* then the chained DIFAT sectors: ents-1 FAT sector numbers each, with
       the next DIFAT sector in the last slot.  The walk is capped at the
       sectors that physically exist, so a self-referencing DIFAT stops. */
    guard = ndif > c->nsect ? c->nsect : ndif;
    for (i = 0; i < guard && got < nfat; i++) {
        uint32_t k;
        if (difs > MAXREGSECT || difs >= c->nsect) break;
        if (ud_src_read(&c->src, ((long)difs + 1) * c->ssz, dsec, c->ssz) != c->ssz)
            break;
        for (k = 0; k + 1 < ents && got < nfat; k++) {
            if (!fat_sector_in(c, ud_rd32(dsec + k * 4), got, sec)) break;
            got++;
        }
        difs = ud_rd32(dsec + (ents - 1) * 4);
    }
    ud_free(sec);
    ud_free(dsec);

    if (got < nfat) {
        ud_set_error("compound file: DIFAT does not describe every FAT sector");
        return 0;
    }
    return 1;
}

/* ---- the mini FAT ---------------------------------------------------------- */
static int load_minifat(ud_cfb *c, const unsigned char *h)
{
    uint32_t nmf  = ud_rd32(h + H_NMFATSECT);
    uint32_t s    = ud_rd32(h + H_MFATSTART);
    uint32_t ents = (uint32_t)(c->ssz / 4);
    uint32_t i;
    unsigned char *sec;

    if (nmf == 0 || nmf > c->nsect) { c->mfat = 0; c->mfatn = 0; return 1; }
    c->mfatn = nmf * ents;
    c->mfat  = (uint32_t *)ud_alloc((unsigned long)c->mfatn * 4);
    if (!c->mfat) { ud_set_error("out of memory (mini FAT)"); return 0; }
    for (i = 0; i < c->mfatn; i++) c->mfat[i] = FREESECT;

    sec = (unsigned char *)ud_alloc((unsigned long)c->ssz);
    if (!sec) { ud_set_error("out of memory (sector buffer)"); return 0; }
    for (i = 0; i < nmf; i++) {
        uint32_t k;
        if (s > MAXREGSECT || s >= c->nsect) break;
        if (ud_src_read(&c->src, ((long)s + 1) * c->ssz, sec, c->ssz) != c->ssz)
            break;
        for (k = 0; k < ents; k++) c->mfat[i * ents + k] = ud_rd32(sec + k * 4);
        s = (s < c->fatn) ? c->fat[s] : ENDOFCHAIN;
    }
    ud_free(sec);
    /* a short mini FAT is survivable: the tail stays FREESECT, so streams
       that reach into it end early rather than reading garbage. */
    return 1;
}

/* ---- the directory --------------------------------------------------------- */
static void parse_dirent(ud_cfb *c, ud_dirent *e, const unsigned char *p)
{
    uint32_t nlen = ud_rd16(p + D_NAMELEN);
    uint64_t sz;
    int chars, i;

    memset(e, 0, sizeof *e);
    e->left = e->right = e->kid = (int32_t)NOSTREAM;
    e->first = e->next = e->parent = UD_CFB_NONE;

    e->type = p[D_TYPE];
    if (e->type != UD_ENT_STORAGE && e->type != UD_ENT_STREAM &&
        e->type != UD_ENT_ROOT) {
        e->type = UD_ENT_EMPTY;
        return;
    }
    if (nlen < 2 || nlen > 64 || (nlen & 1)) { e->type = UD_ENT_EMPTY; return; }

    chars = (int)(nlen / 2) - 1;
    if (chars > UD_NAME_MAX) chars = UD_NAME_MAX;
    for (i = 0; i < chars; i++) {
        uint16_t u = ud_rd16(p + D_NAME + i * 2);
        if (!u) break;                       /* early terminator: honour it */
        e->name[i] = (char)ud_uc_to_cp1252(u);
    }
    e->name[i] = 0;
    if (!e->name[0]) { e->type = UD_ENT_EMPTY; return; }

    memcpy(e->clsid, p + D_CLSID, 16);
    e->left  = (int32_t)ud_rd32(p + D_LEFT);
    e->right = (int32_t)ud_rd32(p + D_RIGHT);
    e->kid   = (int32_t)ud_rd32(p + D_CHILD);
    e->start = ud_rd32(p + D_START);

    sz = ud_rd64(p + D_SIZE);
    if (c->ver == 3) sz &= 0xFFFFFFFFu;      /* v3 must zero the high dword */

    /* mini-vs-regular is decided from the DECLARED size, before clamping, so
       a fuzzed size cannot silently move a stream into the other table. */
    e->mini = (unsigned char)(e->type == UD_ENT_STREAM &&
                              sz < (uint64_t)c->cutoff);
    {
        long maxb = (long)c->nsect * c->ssz;
        e->size = (sz > (uint64_t)maxb) ? maxb : (long)sz;
    }
    if (e->type == UD_ENT_STORAGE) { e->size = 0; e->start = 0; }
}

static int load_dir(ud_cfb *c, const unsigned char *h)
{
    uint32_t start = ud_rd32(h + H_DIRSTART);
    long per   = c->ssz / DIRENT_SZ;
    long nsec  = 0;
    uint32_t s;
    long i;
    unsigned char *sec;

    /* count the chain first (bounded by the sectors that exist) */
    s = start;
    while (s <= MAXREGSECT && s < c->nsect && nsec <= (long)c->nsect) {
        nsec++;
        s = (s < c->fatn) ? c->fat[s] : ENDOFCHAIN;
    }
    if (nsec == 0) { ud_set_error("compound file: empty directory"); return 0; }
    if (nsec > (long)c->nsect) {
        ud_set_error("compound file: looping directory chain");
        return 0;
    }

    c->dirn = (int)(nsec * per);
    c->dir  = (ud_dirent *)ud_alloc((unsigned long)c->dirn * sizeof(ud_dirent));
    if (!c->dir) { ud_set_error("out of memory (directory)"); return 0; }

    sec = (unsigned char *)ud_alloc((unsigned long)c->ssz);
    if (!sec) { ud_set_error("out of memory (sector buffer)"); return 0; }
    s = start;
    for (i = 0; i < nsec; i++) {
        long k;
        if (s > MAXREGSECT || s >= c->nsect) break;
        if (ud_src_read(&c->src, ((long)s + 1) * c->ssz, sec, c->ssz) != c->ssz)
            break;
        for (k = 0; k < per; k++)
            parse_dirent(c, &c->dir[i * per + k], sec + k * DIRENT_SZ);
        s = (s < c->fatn) ? c->fat[s] : ENDOFCHAIN;
    }
    /* any sectors we could not read stay zeroed => UD_ENT_EMPTY */
    for (; i < nsec; i++) {
        long k;
        for (k = 0; k < per; k++) {
            memset(&c->dir[i * per + k], 0, sizeof(ud_dirent));
            c->dir[i * per + k].first  = UD_CFB_NONE;
            c->dir[i * per + k].next   = UD_CFB_NONE;
            c->dir[i * per + k].parent = UD_CFB_NONE;
        }
    }
    ud_free(sec);

    if (c->dir[0].type != UD_ENT_ROOT) {
        ud_set_error("compound file: directory 0 is not the root storage");
        return 0;
    }
    c->ministart = c->dir[0].start;
    c->minisize  = c->dir[0].size;
    return 1;
}

/* Flatten every storage's red-black sibling tree into a first/next child
 * list.  One shared visited bitmap across all storages: an entry belongs to
 * exactly one tree, so a node reached twice is corruption (or a deliberate
 * cycle) and is dropped rather than followed. */
static int flatten_dir(ud_cfb *c)
{
    unsigned char *seen;
    int *stack;
    int i;

    seen  = (unsigned char *)ud_alloc((unsigned long)c->dirn);
    stack = (int *)ud_alloc((unsigned long)(c->dirn + 1) * sizeof(int));
    if (!seen || !stack) {
        ud_free(seen); ud_free(stack);
        ud_set_error("out of memory (directory walk)");
        return 0;
    }
    memset(seen, 0, (unsigned long)c->dirn);

    for (i = 0; i < c->dirn; i++) {
        int node, sp = 0, tail = UD_CFB_NONE;
        if (c->dir[i].type != UD_ENT_STORAGE && c->dir[i].type != UD_ENT_ROOT)
            continue;
        node = (int)c->dir[i].kid;

#define OK(n) ((n) >= 0 && (n) < c->dirn && !seen[n] && \
               c->dir[n].type != UD_ENT_EMPTY)
        for (;;) {
            while (OK(node)) {
                seen[node] = 1;
                stack[sp++] = node;
                node = (int)c->dir[node].left;
            }
            if (sp == 0) break;
            node = stack[--sp];
            c->dir[node].parent = i;
            if (tail == UD_CFB_NONE) c->dir[i].first = node;
            else                     c->dir[tail].next = node;
            tail = node;
            node = (int)c->dir[node].right;
        }
#undef OK
    }
    ud_free(seen);
    ud_free(stack);
    return 1;
}

ud_cfb *ud_cfb_open(const ud_src *src)
{
    unsigned char hdr[512];
    ud_cfb *c;

    ud_set_error("");
    if (!src || !src->read) { ud_set_error("bad byte source"); return 0; }
    if (src->size < 512) { ud_set_error("not a compound file (too small)"); return 0; }
    if (ud_src_read(src, 0, hdr, 512) != 512) {
        ud_set_error("compound file: short read on the header");
        return 0;
    }
    c = (ud_cfb *)ud_alloc(sizeof(ud_cfb));
    if (!c) { ud_set_error("out of memory"); return 0; }
    memset(c, 0, sizeof *c);
    c->src = *src;

    if (!parse_header(c, hdr) || !load_fat(c, hdr) || !load_minifat(c, hdr) ||
        !load_dir(c, hdr) || !flatten_dir(c)) {
        ud_cfb_close(c);
        return 0;
    }
    return c;
}

void ud_cfb_close(ud_cfb *c)
{
    if (!c) return;
    ud_free(c->fat);
    ud_free(c->mfat);
    ud_free(c->dir);
    ud_free(c);
}

/* ---- accessors ------------------------------------------------------------- */
static int id_ok(const ud_cfb *c, int id)
{ return c && id >= 0 && id < c->dirn && c->dir[id].type != UD_ENT_EMPTY; }

int  ud_cfb_version(const ud_cfb *c) { return c ? c->ver : 0; }
int  ud_cfb_count  (const ud_cfb *c) { return c ? c->dirn : 0; }
int  ud_cfb_type   (const ud_cfb *c, int id)
{ return id_ok(c, id) ? c->dir[id].type : UD_ENT_EMPTY; }
const char *ud_cfb_name(const ud_cfb *c, int id)
{ return id_ok(c, id) ? c->dir[id].name : ""; }
long ud_cfb_size(const ud_cfb *c, int id)
{ return id_ok(c, id) ? c->dir[id].size : 0; }
int  ud_cfb_parent(const ud_cfb *c, int id)
{ return id_ok(c, id) ? c->dir[id].parent : UD_CFB_NONE; }
const unsigned char *ud_cfb_clsid(const ud_cfb *c, int id)
{ return id_ok(c, id) ? c->dir[id].clsid : 0; }
int  ud_cfb_first(const ud_cfb *c, int parent)
{ return id_ok(c, parent) ? c->dir[parent].first : UD_CFB_NONE; }
int  ud_cfb_next(const ud_cfb *c, int id)
{ return id_ok(c, id) ? c->dir[id].next : UD_CFB_NONE; }

int ud_cfb_child(const ud_cfb *c, int parent, const char *name)
{
    int k;
    if (!id_ok(c, parent) || !name) return UD_CFB_NONE;
    for (k = c->dir[parent].first; k != UD_CFB_NONE; k = c->dir[k].next)
        if (ud_name_cmp(c->dir[k].name, name) == 0) return k;
    return UD_CFB_NONE;
}

int ud_cfb_find(const ud_cfb *c, const char *path)
{
    char comp[UD_NAME_BUF];
    int id = UD_CFB_ROOT_ID;

    if (!c || !path) return UD_CFB_NONE;
    while (*path == '/') path++;
    while (*path) {
        int n = 0;
        while (*path && *path != '/') {
            if (n < UD_NAME_MAX) comp[n++] = *path;
            path++;
        }
        comp[n] = 0;
        while (*path == '/') path++;
        if (!n) continue;
        id = ud_cfb_child(c, id, comp);
        if (id == UD_CFB_NONE) return UD_CFB_NONE;
    }
    return id;
}

long ud_cfb_read(ud_cfb *c, int id, long off, void *dst, long n)
{
    ud_dirent *e;
    if (!id_ok(c, id) || !dst) return 0;
    e = &c->dir[id];
    if (e->type == UD_ENT_STORAGE) return 0;
    if (e->mini)
        return read_mini(c, e->start, e->size, off, (unsigned char *)dst, n);
    return read_regular(c, e->start, e->size, off, (unsigned char *)dst, n,
                        &c->rcur);
}

unsigned char *ud_cfb_load(ud_cfb *c, int id, long *len)
{
    long sz, got;
    unsigned char *buf;

    if (len) *len = 0;
    if (!id_ok(c, id)) { ud_set_error("no such stream"); return 0; }
    sz = c->dir[id].size;
    if (sz > UD_MAX_STREAM) {
        ud_set_error("stream too large for this build");
        return 0;
    }
    buf = (unsigned char *)ud_alloc((unsigned long)sz + 1);
    if (!buf) { ud_set_error("out of memory (stream)"); return 0; }
    got = sz ? ud_cfb_read(c, id, 0, buf, sz) : 0;
    if (got < sz) memset(buf + got, 0, (unsigned long)(sz - got));
    buf[sz] = 0;
    if (len) *len = sz;
    return buf;
}

/* ===========================================================================
 * WRITER
 * ======================================================================== */

typedef struct {
    char           name[UD_NAME_BUF];
    unsigned char  clsid[16];
    int            type;
    unsigned char *data;
    long           len;
    int            parent;
    int            kid_first, kid_next;   /* insertion order                 */
    /* filled in by the serialiser */
    int            left, right, kid;      /* the emitted tree                */
    uint32_t       start;
    long           emit_size;
} ud_went;

struct ud_cfbw {
    ud_went *e;
    int      n, cap;
};

ud_cfbw *ud_cfbw_new(void)
{
    ud_cfbw *w = (ud_cfbw *)ud_alloc(sizeof(ud_cfbw));
    if (!w) { ud_set_error("out of memory"); return 0; }
    memset(w, 0, sizeof *w);
    w->cap = 8;
    w->e   = (ud_went *)ud_alloc((unsigned long)w->cap * sizeof(ud_went));
    if (!w->e) { ud_free(w); ud_set_error("out of memory"); return 0; }
    memset(w->e, 0, (unsigned long)w->cap * sizeof(ud_went));
    /* entry 0 is always the root storage, named exactly "Root Entry" */
    memcpy(w->e[0].name, "Root Entry", 11);
    w->e[0].type      = UD_ENT_ROOT;
    w->e[0].parent    = UD_CFB_NONE;
    w->e[0].kid_first = UD_CFB_NONE;
    w->e[0].kid_next  = UD_CFB_NONE;
    w->n = 1;
    return w;
}

void ud_cfbw_free(ud_cfbw *w)
{
    int i;
    if (!w) return;
    for (i = 0; i < w->n; i++) ud_free(w->e[i].data);
    ud_free(w->e);
    ud_free(w);
}

static int name_ok(const char *name)
{
    unsigned long i, n;
    if (!name || !name[0]) return 0;
    n = strlen(name);
    if (n > UD_NAME_MAX) return 0;
    for (i = 0; i < n; i++) {
        char ch = name[i];
        /* [MS-CFB]: these four are forbidden in an entry name */
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '!') return 0;
    }
    return 1;
}

static int w_add(ud_cfbw *w, int parent, const char *name, int type,
                 unsigned char *data, long len)
{
    int id, k, tail;

    if (!w) { ud_set_error("no writer"); return UD_CFB_NONE; }
    if (parent < 0 || parent >= w->n ||
        (w->e[parent].type != UD_ENT_ROOT &&
         w->e[parent].type != UD_ENT_STORAGE)) {
        ud_set_error("cfb write: parent is not a storage");
        return UD_CFB_NONE;
    }
    if (!name_ok(name)) {
        ud_set_error("cfb write: illegal entry name");
        return UD_CFB_NONE;
    }
    for (k = w->e[parent].kid_first; k != UD_CFB_NONE; k = w->e[k].kid_next)
        if (ud_name_cmp(w->e[k].name, name) == 0) {
            ud_set_error("cfb write: duplicate name in one storage");
            return UD_CFB_NONE;
        }
    if (len < 0) len = 0;

    if (w->n == w->cap) {
        int nc = w->cap * 2;
        ud_went *ne = (ud_went *)ud_alloc((unsigned long)nc * sizeof(ud_went));
        if (!ne) { ud_set_error("out of memory (cfb model)"); return UD_CFB_NONE; }
        memcpy(ne, w->e, (unsigned long)w->n * sizeof(ud_went));
        memset(ne + w->n, 0, (unsigned long)(nc - w->n) * sizeof(ud_went));
        ud_free(w->e);
        w->e = ne; w->cap = nc;
    }
    id = w->n++;
    memset(&w->e[id], 0, sizeof(ud_went));
    memcpy(w->e[id].name, name, strlen(name) + 1);
    w->e[id].type      = type;
    w->e[id].data      = data;
    w->e[id].len       = len;
    w->e[id].parent    = parent;
    w->e[id].kid_first = UD_CFB_NONE;
    w->e[id].kid_next  = UD_CFB_NONE;

    tail = UD_CFB_NONE;
    for (k = w->e[parent].kid_first; k != UD_CFB_NONE; k = w->e[k].kid_next)
        tail = k;
    if (tail == UD_CFB_NONE) w->e[parent].kid_first = id;
    else                     w->e[tail].kid_next    = id;
    return id;
}

int ud_cfbw_storage(ud_cfbw *w, int parent, const char *name)
{ return w_add(w, parent, name, UD_ENT_STORAGE, 0, 0); }

int ud_cfbw_stream(ud_cfbw *w, int parent, const char *name,
                   const void *data, long len)
{
    unsigned char *copy = 0;
    int id;
    if (len < 0) len = 0;
    if (len > 0) {
        if (!data) { ud_set_error("cfb write: null stream data"); return UD_CFB_NONE; }
        copy = (unsigned char *)ud_alloc((unsigned long)len);
        if (!copy) { ud_set_error("out of memory (stream copy)"); return UD_CFB_NONE; }
        memcpy(copy, data, (unsigned long)len);
    }
    id = w_add(w, parent, name, UD_ENT_STREAM, copy, len);
    if (id == UD_CFB_NONE) ud_free(copy);
    return id;
}

int ud_cfbw_stream_take(ud_cfbw *w, int parent, const char *name,
                        unsigned char *data, long len)
{
    if (len < 0) len = 0;
    if (len > 0 && !data) {
        ud_set_error("cfb write: null stream data");
        return UD_CFB_NONE;
    }
    return w_add(w, parent, name, UD_ENT_STREAM, len ? data : 0, len);
}

int ud_cfbw_clsid(ud_cfbw *w, int id, const unsigned char clsid[16])
{
    if (!w || id < 0 || id >= w->n || !clsid) return 0;
    memcpy(w->e[id].clsid, clsid, 16);
    return 1;
}

/* ---- directory ordering ---------------------------------------------------- */
static void kid_sift(ud_cfbw *w, int *a, int lo, int n)
{
    int root = lo;
    while (root * 2 + 1 < n) {
        int ch = root * 2 + 1;
        if (ch + 1 < n &&
            ud_name_cmp(w->e[a[ch]].name, w->e[a[ch + 1]].name) < 0) ch++;
        if (ud_name_cmp(w->e[a[root]].name, w->e[a[ch]].name) >= 0) return;
        { int t = a[root]; a[root] = a[ch]; a[ch] = t; }
        root = ch;
    }
}

static void kid_sort(ud_cfbw *w, int *a, int n)
{
    int i;
    for (i = n / 2 - 1; i >= 0; i--) kid_sift(w, a, i, n);
    for (i = n - 1; i > 0; i--) {
        int t = a[0]; a[0] = a[i]; a[i] = t;
        kid_sift(w, a, 0, i);
    }
}

/* A perfectly balanced BST over the sorted children.  Recursion depth is
 * log2(n), so a storage would need 2^60 children to be a stack problem. */
static int build_bst(ud_cfbw *w, const int *a, int lo, int hi)
{
    int mid, id;
    if (lo > hi) return UD_CFB_NONE;
    mid = lo + (hi - lo) / 2;
    id  = a[mid];
    w->e[id].left  = build_bst(w, a, lo, mid - 1);
    w->e[id].right = build_bst(w, a, mid + 1, hi);
    return id;
}

static int build_trees(ud_cfbw *w)
{
    int i, maxk = 0, *a;

    for (i = 0; i < w->n; i++) {
        int k, cnt = 0;
        w->e[i].left = w->e[i].right = w->e[i].kid = UD_CFB_NONE;
        for (k = w->e[i].kid_first; k != UD_CFB_NONE; k = w->e[k].kid_next) cnt++;
        if (cnt > maxk) maxk = cnt;
    }
    if (maxk == 0) return 1;
    a = (int *)ud_alloc((unsigned long)maxk * sizeof(int));
    if (!a) { ud_set_error("out of memory (directory sort)"); return 0; }
    for (i = 0; i < w->n; i++) {
        int k, cnt = 0;
        if (w->e[i].kid_first == UD_CFB_NONE) continue;
        for (k = w->e[i].kid_first; k != UD_CFB_NONE; k = w->e[k].kid_next)
            a[cnt++] = k;
        kid_sort(w, a, cnt);
        w->e[i].kid = build_bst(w, a, 0, cnt - 1);
    }
    ud_free(a);
    return 1;
}

/* ---- serialise -------------------------------------------------------------
 * Fixed sector size 512 (version 3) - what Office 97 writes, and what every
 * reader in the world has seen most of. */
#define W_SSZ      512L
#define W_FATENTS  (W_SSZ / 4)          /* 128 FAT entries per sector        */
#define W_DIRPER   (W_SSZ / DIRENT_SZ)  /* 4 directory entries per sector    */
#define W_MINIPER  (W_SSZ / MINI_SZ)    /* 8 mini sectors per sector         */

static void put_dirent(unsigned char *p, const ud_went *e, int color,
                       uint32_t start, long size)
{
    unsigned long i, n;

    memset(p, 0, DIRENT_SZ);
    n = strlen(e->name);
    for (i = 0; i < n; i++)
        ud_wr16(p + D_NAME + i * 2, ud_cp1252_to_uc((unsigned char)e->name[i]));
    ud_wr16(p + D_NAME + n * 2, 0);
    ud_wr16(p + D_NAMELEN, (uint16_t)((n + 1) * 2));
    p[D_TYPE]  = (unsigned char)e->type;
    p[D_COLOR] = (unsigned char)color;
    ud_wr32(p + D_LEFT,  e->left  == UD_CFB_NONE ? NOSTREAM : (uint32_t)e->left);
    ud_wr32(p + D_RIGHT, e->right == UD_CFB_NONE ? NOSTREAM : (uint32_t)e->right);
    ud_wr32(p + D_CHILD, e->kid   == UD_CFB_NONE ? NOSTREAM : (uint32_t)e->kid);
    memcpy(p + D_CLSID, e->clsid, 16);
    ud_wr32(p + D_START, start);
    ud_wr64(p + D_SIZE, (uint64_t)size);
}

unsigned char *ud_cfbw_serialize(ud_cfbw *w, long *len)
{
    long aN = 0, bN, cN, dN, fN = 0, gN = 0, base, total, minibytes = 0;
    long A, B, C, D, E, F;
    long nminisec, i;
    uint32_t *fat = 0, *mfat = 0;
    unsigned char *out = 0;
    long outsz;
    int it, ok = 0;

    if (len) *len = 0;
    if (!w) { ud_set_error("no writer"); return 0; }
    if (!build_trees(w)) return 0;

    /* --- 1. place every payload -------------------------------------------- */
    for (i = 0; i < w->n; i++) {
        ud_went *e = &w->e[i];
        e->start = ENDOFCHAIN;
        e->emit_size = 0;
        if (e->type != UD_ENT_STREAM || e->len <= 0) continue;
        if (e->len < UD_CFB_MINI_CUTOFF) {
            e->start = (uint32_t)(minibytes / MINI_SZ);
            minibytes += (e->len + MINI_SZ - 1) / MINI_SZ * MINI_SZ;
        } else {
            e->start = (uint32_t)aN;
            aN += (e->len + W_SSZ - 1) / W_SSZ;
        }
        e->emit_size = e->len;
    }
    nminisec = minibytes / MINI_SZ;
    bN = (minibytes + W_SSZ - 1) / W_SSZ;
    cN = (nminisec + W_FATENTS - 1) / W_FATENTS;
    dN = (w->n + W_DIRPER - 1) / W_DIRPER;
    if (dN < 1) dN = 1;

    /* --- 2. the FAT sizes itself, including its own sectors ---------------- */
    base = aN + bN + cN + dN;
    for (it = 0; it < 64; it++) {
        long nf = (base + fN + gN + W_FATENTS - 1) / W_FATENTS;
        long ng = nf > H_DIFAT_N ? (nf - H_DIFAT_N + (W_FATENTS - 2)) /
                                   (W_FATENTS - 1) : 0;
        if (nf == fN && ng == gN) break;
        fN = nf; gN = ng;
    }
    if ((base + fN + gN + W_FATENTS - 1) / W_FATENTS != fN) {
        ud_set_error("cfb write: FAT sizing did not converge");
        return 0;
    }
    A = 0; B = A + aN; C = B + bN; D = C + cN; E = D + dN; F = E + fN;
    total = F + gN;

    /* --- 3. build the two allocation tables -------------------------------- */
    fat = (uint32_t *)ud_alloc((unsigned long)fN * W_FATENTS * 4);
    if (!fat) { ud_set_error("out of memory (FAT)"); goto done; }
    for (i = 0; i < fN * W_FATENTS; i++) fat[i] = FREESECT;

    if (cN) {
        mfat = (uint32_t *)ud_alloc((unsigned long)cN * W_FATENTS * 4);
        if (!mfat) { ud_set_error("out of memory (mini FAT)"); goto done; }
        for (i = 0; i < cN * W_FATENTS; i++) mfat[i] = FREESECT;
    }
    for (i = 0; i < w->n; i++) {
        ud_went *e = &w->e[i];
        long k, ns;
        if (e->type != UD_ENT_STREAM || e->len <= 0) continue;
        if (e->len < UD_CFB_MINI_CUTOFF) {
            ns = (e->len + MINI_SZ - 1) / MINI_SZ;
            for (k = 0; k < ns; k++)
                mfat[e->start + k] = (k + 1 == ns) ? ENDOFCHAIN
                                                   : (uint32_t)(e->start + k + 1);
        } else {
            ns = (e->len + W_SSZ - 1) / W_SSZ;
            for (k = 0; k < ns; k++)
                fat[A + e->start + k] = (k + 1 == ns) ? ENDOFCHAIN
                                                      : (uint32_t)(A + e->start + k + 1);
        }
    }
    for (i = 0; i < bN; i++) fat[B + i] = (i + 1 == bN) ? ENDOFCHAIN : (uint32_t)(B + i + 1);
    for (i = 0; i < cN; i++) fat[C + i] = (i + 1 == cN) ? ENDOFCHAIN : (uint32_t)(C + i + 1);
    for (i = 0; i < dN; i++) fat[D + i] = (i + 1 == dN) ? ENDOFCHAIN : (uint32_t)(D + i + 1);
    for (i = 0; i < fN; i++) fat[E + i] = FATSECT;
    for (i = 0; i < gN; i++) fat[F + i] = DIFSECT;

    /* --- 4. emit ------------------------------------------------------------ */
    outsz = (1 + total) * W_SSZ;
    out = (unsigned char *)ud_alloc((unsigned long)outsz);
    if (!out) { ud_set_error("out of memory (output)"); goto done; }
    memset(out, 0, (unsigned long)outsz);

#define SEC(n) (out + ((n) + 1) * W_SSZ)

    /* header */
    ud_wr32(out + H_SIG,     CFB_SIG0);
    ud_wr32(out + H_SIG + 4, CFB_SIG1);
    ud_wr16(out + H_MINORVER,  0x003E);
    ud_wr16(out + H_MAJORVER,  3);
    ud_wr16(out + H_BYTEORDER, 0xFFFE);
    ud_wr16(out + H_SECSHIFT,  9);
    ud_wr16(out + H_MINISHIFT, MINI_SHIFT);
    ud_wr32(out + H_NDIRSECT,  0);           /* MUST be 0 in version 3       */
    ud_wr32(out + H_NFATSECT,  (uint32_t)fN);
    ud_wr32(out + H_DIRSTART,  (uint32_t)D);
    ud_wr32(out + H_TRANSIG,   0);
    ud_wr32(out + H_CUTOFF,    UD_CFB_MINI_CUTOFF);
    ud_wr32(out + H_MFATSTART, cN ? (uint32_t)C : ENDOFCHAIN);
    ud_wr32(out + H_NMFATSECT, (uint32_t)cN);
    ud_wr32(out + H_DIFATSTART, gN ? (uint32_t)F : ENDOFCHAIN);
    ud_wr32(out + H_NDIFATSECT, (uint32_t)gN);
    for (i = 0; i < H_DIFAT_N; i++)
        ud_wr32(out + H_DIFAT + i * 4,
                i < fN ? (uint32_t)(E + i) : FREESECT);

    /* stream payloads */
    for (i = 0; i < w->n; i++) {
        ud_went *e = &w->e[i];
        if (e->type != UD_ENT_STREAM || e->len <= 0) continue;
        if (e->len < UD_CFB_MINI_CUTOFF)
            memcpy(SEC(B) + (long)e->start * MINI_SZ, e->data, (unsigned long)e->len);
        else
            memcpy(SEC(A + (long)e->start), e->data, (unsigned long)e->len);
    }

    /* mini FAT, then the FAT, then the DIFAT sectors */
    for (i = 0; i < cN * W_FATENTS; i++) ud_wr32(SEC(C) + i * 4, mfat[i]);
    for (i = 0; i < fN * W_FATENTS; i++) ud_wr32(SEC(E) + i * 4, fat[i]);
    for (i = 0; i < gN; i++) {
        long k;
        unsigned char *p = SEC(F + i);
        for (k = 0; k < W_FATENTS - 1; k++) {
            long fs = H_DIFAT_N + i * (W_FATENTS - 1) + k;
            ud_wr32(p + k * 4, fs < fN ? (uint32_t)(E + fs) : FREESECT);
        }
        ud_wr32(p + (W_FATENTS - 1) * 4,
                (i + 1 == gN) ? ENDOFCHAIN : (uint32_t)(F + i + 1));
    }

    /* directory: entry 0 is the root, whose "stream" IS the mini stream */
    for (i = 0; i < dN * W_DIRPER; i++) {
        unsigned char *p = SEC(D) + i * DIRENT_SZ;
        if (i >= w->n) {                       /* padding to a whole sector  */
            memset(p, 0, DIRENT_SZ);
            ud_wr32(p + D_LEFT,  NOSTREAM);
            ud_wr32(p + D_RIGHT, NOSTREAM);
            ud_wr32(p + D_CHILD, NOSTREAM);
            continue;
        }
        {
            ud_went *e = &w->e[i];
            uint32_t st;
            long sz;
            if (e->type == UD_ENT_ROOT) {
                st = bN ? (uint32_t)B : ENDOFCHAIN;
                sz = minibytes;
            } else if (e->type == UD_ENT_STORAGE) {
                st = 0; sz = 0;
            } else if (e->len <= 0) {
                st = ENDOFCHAIN; sz = 0;
            } else if (e->len < UD_CFB_MINI_CUTOFF) {
                st = e->start;                 /* a MINI sector index        */
                sz = e->len;
            } else {
                st = (uint32_t)(A + (long)e->start);
                sz = e->len;
            }
            /* all-black: readers ignore the colouring, and an all-black tree
               is the arrangement every implementation accepts. */
            put_dirent(p, e, 1, st, sz);
        }
    }
#undef SEC

    ok = 1;
    if (len) *len = outsz;

done:
    ud_free(fat);
    ud_free(mfat);
    if (!ok) { ud_free(out); return 0; }
    return out;
}
