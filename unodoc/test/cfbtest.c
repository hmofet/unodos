/* ===========================================================================
 * cfbtest - the host gate for unodoc's CFB container (docs/OFFICE97-PLAN.md
 * §4 phase 1).  Runs without booting the OS, over the same sources the .UNO
 * module compiles freestanding, built with build.sh's sanitizer set plus
 * ASan (the UnoAmp EQ lesson: a harness without the OS's flags tests
 * different code).
 *
 * Commands, all driven by test/run_tests.py:
 *   selftest              in-memory: round-trip, ordering, limits, and a
 *                         battery of deliberately corrupt containers
 *   ls FILE               print a canonical digest of a container's tree
 *   rt FILE               read FILE, rebuild it through the writer, reread,
 *                         and assert the two digests are identical
 *   rebuild IN OUT        read IN, rebuild through the writer, write OUT
 *                         (the LibreOffice oracle then opens both)
 *   fuzz FILE SEED N      N mutations of FILE: open + walk each.  Must never
 *                         crash and must always terminate.
 * ======================================================================== */
#include "unodoc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- allocator + tiny helpers --------------------------------------------- */
static void *t_alloc(unsigned long n) { return malloc(n ? n : 1); }

static int   g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); g_fail++; } } while (0)

/* a growable string, for building digests */
typedef struct { char *p; long n, cap; } sb;

static void sb_put(sb *s, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    long n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (long)sizeof tmp - 1) n = (long)sizeof tmp - 1;
    if (s->n + n + 1 > s->cap) {
        s->cap = (s->n + n + 1) * 2 + 256;
        s->p = (char *)realloc(s->p, (size_t)s->cap);
    }
    memcpy(s->p + s->n, tmp, (size_t)n);
    s->n += n;
    s->p[s->n] = 0;
}

static unsigned long long fnv(const unsigned char *b, long n)
{
    unsigned long long h = 1469598103934665603ULL;
    long i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static unsigned char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    *len = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    b = (unsigned char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 0; }
    fclose(f);
    *len = n;
    return b;
}

/* ---- the canonical digest of a container ----------------------------------
 * Depth-first in child order, one line per entry: indent, name, type, size
 * and an FNV-1a of the payload.  Two containers with the same digest hold
 * the same tree and the same bytes. */
static void digest_tree(ud_cfb *c, int id, int depth, sb *s)
{
    int k;
    if (depth > 64) { sb_put(s, "!!depth\n"); return; }
    for (k = ud_cfb_first(c, id); k != UD_CFB_NONE; k = ud_cfb_next(c, k)) {
        int t = ud_cfb_type(c, k);
        long n = ud_cfb_size(c, k);
        int i;
        for (i = 0; i < depth; i++) sb_put(s, "  ");
        if (t == UD_ENT_STREAM) {
            long got = 0;
            unsigned char *b = ud_cfb_load(c, k, &got);
            sb_put(s, "S %s %ld %016llx\n", ud_cfb_name(c, k), n,
                   b ? fnv(b, got) : 0ULL);
            ud_free(b);
        } else {
            sb_put(s, "D %s\n", ud_cfb_name(c, k));
            digest_tree(c, k, depth + 1, s);
        }
    }
}

static char *digest_of(const unsigned char *buf, long len)
{
    ud_src src;
    ud_cfb *c;
    sb s;
    memset(&s, 0, sizeof s);
    ud_src_mem(&src, buf, len);
    c = ud_cfb_open(&src);
    if (!c) { sb_put(&s, "OPEN-FAILED: %s\n", ud_error()); return s.p; }
    sb_put(&s, "v%d root=%s\n", ud_cfb_version(c), ud_cfb_name(c, UD_CFB_ROOT_ID));
    digest_tree(c, UD_CFB_ROOT_ID, 0, &s);
    ud_cfb_close(c);
    return s.p;
}

/* ---- rebuild: reader model -> writer model -> fresh bytes ------------------ */
static int copy_tree(ud_cfb *c, int src, ud_cfbw *w, int dst, int depth)
{
    int k;
    if (depth > 64) return 0;
    for (k = ud_cfb_first(c, src); k != UD_CFB_NONE; k = ud_cfb_next(c, k)) {
        int t = ud_cfb_type(c, k), nid;
        const unsigned char *cl = ud_cfb_clsid(c, k);
        if (t == UD_ENT_STORAGE) {
            nid = ud_cfbw_storage(w, dst, ud_cfb_name(c, k));
            if (nid == UD_CFB_NONE) return 0;
            if (cl) ud_cfbw_clsid(w, nid, cl);
            if (!copy_tree(c, k, w, nid, depth + 1)) return 0;
        } else if (t == UD_ENT_STREAM) {
            long n = 0;
            unsigned char *b = ud_cfb_load(c, k, &n);
            if (!b) return 0;
            nid = ud_cfbw_stream(w, dst, ud_cfb_name(c, k), b, n);
            ud_free(b);
            if (nid == UD_CFB_NONE) return 0;
            if (cl) ud_cfbw_clsid(w, nid, cl);
        }
    }
    return 1;
}

static unsigned char *rebuild(const unsigned char *in, long inlen, long *outlen)
{
    ud_src src;
    ud_cfb *c;
    ud_cfbw *w;
    unsigned char *out = 0;

    *outlen = 0;
    ud_src_mem(&src, in, inlen);
    c = ud_cfb_open(&src);
    if (!c) return 0;
    w = ud_cfbw_new();
    if (w) {
        const unsigned char *cl = ud_cfb_clsid(c, UD_CFB_ROOT_ID);
        if (cl) ud_cfbw_clsid(w, UD_CFB_ROOT_ID, cl);
        if (copy_tree(c, UD_CFB_ROOT_ID, w, UD_CFB_ROOT_ID, 0))
            out = ud_cfbw_serialize(w, outlen);
        ud_cfbw_free(w);
    }
    ud_cfb_close(c);
    return out;
}

/* ===========================================================================
 * selftest
 * ======================================================================== */
static unsigned char *pattern(long n, unsigned seed)
{
    unsigned char *b = (unsigned char *)malloc((size_t)(n ? n : 1));
    long i;
    for (i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        b[i] = (unsigned char)(seed >> 16);
    }
    return b;
}

/* open a serialized image and hand back the handle, keeping the buffer */
static ud_cfb *reopen(const unsigned char *buf, long len, ud_src *src)
{
    ud_src_mem(src, buf, len);
    return ud_cfb_open(src);
}

static void t_empty(void)
{
    ud_cfbw *w = ud_cfbw_new();
    unsigned char *img;
    long len = 0;
    ud_src src;
    ud_cfb *c;

    img = ud_cfbw_serialize(w, &len);
    CHECK(img != 0, "empty: serialize failed: %s", ud_error());
    if (!img) { ud_cfbw_free(w); return; }
    CHECK(len % 512 == 0, "empty: length %ld not a sector multiple", len);
    c = reopen(img, len, &src);
    CHECK(c != 0, "empty: reopen failed: %s", ud_error());
    if (c) {
        CHECK(ud_cfb_version(c) == 3, "empty: version %d", ud_cfb_version(c));
        CHECK(strcmp(ud_cfb_name(c, UD_CFB_ROOT_ID), "Root Entry") == 0,
              "empty: root name '%s'", ud_cfb_name(c, UD_CFB_ROOT_ID));
        CHECK(ud_cfb_first(c, UD_CFB_ROOT_ID) == UD_CFB_NONE,
              "empty: root has children");
        ud_cfb_close(c);
    }
    ud_free(img);
    ud_cfbw_free(w);
}

/* every size that straddles a boundary that matters: the 64-byte mini
 * sector, the 512-byte sector, and the 4096-byte mini/regular cutoff */
static const long SIZES[] = { 0, 1, 63, 64, 65, 511, 512, 513, 4095, 4096,
                              4097, 8191, 70000 };
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

static void t_roundtrip(void)
{
    ud_cfbw *w = ud_cfbw_new();
    unsigned char *img, *pat[NSIZES];
    char names[NSIZES][32];
    long len = 0;
    ud_src src;
    ud_cfb *c;
    int i, sub, deep;

    sub  = ud_cfbw_storage(w, UD_CFB_ROOT_ID, "ObjectPool");
    deep = ud_cfbw_storage(w, sub, "_1234567890");
    CHECK(sub != UD_CFB_NONE && deep != UD_CFB_NONE, "roundtrip: storages");

    for (i = 0; i < NSIZES; i++) {
        sprintf(names[i], "s%02d", i);
        pat[i] = pattern(SIZES[i], (unsigned)(i + 1) * 7919u);
        CHECK(ud_cfbw_stream(w, i % 3 == 0 ? UD_CFB_ROOT_ID :
                                (i % 3 == 1 ? sub : deep),
                             names[i], pat[i], SIZES[i]) != UD_CFB_NONE,
              "roundtrip: add %s: %s", names[i], ud_error());
    }
    /* the two names Office actually writes with a leading control byte */
    CHECK(ud_cfbw_stream(w, UD_CFB_ROOT_ID, "\005SummaryInformation",
                         "suminfo", 7) != UD_CFB_NONE, "roundtrip: \\005 name");
    CHECK(ud_cfbw_stream(w, deep, "\001Ole", "ole", 3) != UD_CFB_NONE,
          "roundtrip: \\001 name");

    img = ud_cfbw_serialize(w, &len);
    CHECK(img != 0, "roundtrip: serialize: %s", ud_error());
    if (!img) goto out;

    c = reopen(img, len, &src);
    CHECK(c != 0, "roundtrip: reopen: %s", ud_error());
    if (!c) { ud_free(img); goto out; }

    for (i = 0; i < NSIZES; i++) {
        const char *dir = i % 3 == 0 ? "" :
                          (i % 3 == 1 ? "ObjectPool/" : "ObjectPool/_1234567890/");
        char path[80];
        int id;
        long got = 0;
        unsigned char *b;
        sprintf(path, "/%s%s", dir, names[i]);
        id = ud_cfb_find(c, path);
        CHECK(id != UD_CFB_NONE, "roundtrip: %s missing", path);
        if (id == UD_CFB_NONE) continue;
        CHECK(ud_cfb_size(c, id) == SIZES[i], "roundtrip: %s size %ld != %ld",
              path, ud_cfb_size(c, id), SIZES[i]);
        b = ud_cfb_load(c, id, &got);
        CHECK(b && got == SIZES[i] && (SIZES[i] == 0 ||
              memcmp(b, pat[i], (size_t)SIZES[i]) == 0),
              "roundtrip: %s content differs", path);
        ud_free(b);
    }
    CHECK(ud_cfb_find(c, "/\005SummaryInformation") != UD_CFB_NONE,
          "roundtrip: \\005 lookup");
    CHECK(ud_cfb_find(c, "ObjectPool/_1234567890/\001Ole") != UD_CFB_NONE,
          "roundtrip: \\001 lookup (no leading slash)");
    CHECK(ud_cfb_find(c, "/nope") == UD_CFB_NONE, "roundtrip: phantom found");
    CHECK(ud_cfb_type(c, ud_cfb_find(c, "/ObjectPool")) == UD_ENT_STORAGE,
          "roundtrip: ObjectPool not a storage");
    /* parents link back */
    {
        int id = ud_cfb_find(c, "ObjectPool/_1234567890");
        CHECK(ud_cfb_parent(c, id) == ud_cfb_find(c, "ObjectPool"),
              "roundtrip: parent link");
    }
    ud_cfb_close(c);

    /* Rebuilding must preserve the tree exactly, and must then be a fixed
       point.  It is NOT byte-identical to the first image: the reader hands
       entries back in CFB name order while the model was built in insertion
       order, so directory ids and therefore sector assignments differ.  The
       second rebuild reads in the same order it writes, so from there on the
       serialiser must be a pure, stable function of the model - which is the
       property that makes a diff against a saved file meaningful. */
    {
        long r1 = 0, r2 = 0;
        unsigned char *a = rebuild(img, len, &r1), *b = 0;
        CHECK(a != 0, "roundtrip: rebuild failed: %s", ud_error());
        if (a) {
            char *d0 = digest_of(img, len), *d1 = digest_of(a, r1);
            CHECK(d0 && d1 && strcmp(d0, d1) == 0,
                  "roundtrip: rebuild changed the tree");
            free(d0); free(d1);
            b = rebuild(a, r1, &r2);
            CHECK(b != 0, "roundtrip: second rebuild failed: %s", ud_error());
            if (b) {
                CHECK(r2 == r1 && memcmp(a, b, (size_t)r1) == 0,
                      "roundtrip: rebuild is not idempotent (%ld vs %ld)", r1, r2);
                ud_free(b);
            }
            ud_free(a);
        }
    }
    ud_free(img);
out:
    for (i = 0; i < NSIZES; i++) free(pat[i]);
    ud_cfbw_free(w);
}

/* CFB orders siblings by NAME LENGTH first, then by uppercased code units -
 * the rule real Office checks and the one everybody gets wrong. */
static void t_ordering(void)
{
    /* distinct case-insensitively, since that is how CFB compares them */
    static const char *N[] = { "zz", "a", "B", "AA", "bb", "aaa", "Z",
                               "\005Doc", "m", "MM", "ab", "cd" };
    const int n = (int)(sizeof N / sizeof N[0]);
    ud_cfbw *w = ud_cfbw_new();
    unsigned char *img;
    long len = 0;
    ud_src src;
    ud_cfb *c;
    int i, k, cnt = 0;

    for (i = 0; i < n; i++)
        CHECK(ud_cfbw_stream(w, UD_CFB_ROOT_ID, N[i], "x", 1) != UD_CFB_NONE,
              "ordering: add %s: %s", N[i], ud_error());
    /* a duplicate differing only in case must be refused */
    CHECK(ud_cfbw_stream(w, UD_CFB_ROOT_ID, "AB", "x", 1) == UD_CFB_NONE,
          "ordering: case-insensitive duplicate accepted");
    CHECK(ud_cfbw_stream(w, UD_CFB_ROOT_ID, "a/b", "x", 1) == UD_CFB_NONE,
          "ordering: name with '/' accepted");
    CHECK(ud_cfbw_stream(w, UD_CFB_ROOT_ID,
                         "0123456789012345678901234567890123", "x", 1)
          == UD_CFB_NONE, "ordering: over-long name accepted");

    img = ud_cfbw_serialize(w, &len);
    CHECK(img != 0, "ordering: serialize: %s", ud_error());
    if (!img) { ud_cfbw_free(w); return; }
    c = reopen(img, len, &src);
    CHECK(c != 0, "ordering: reopen: %s", ud_error());
    if (c) {
        const char *prev = 0;
        for (k = ud_cfb_first(c, UD_CFB_ROOT_ID); k != UD_CFB_NONE;
             k = ud_cfb_next(c, k)) {
            const char *nm = ud_cfb_name(c, k);
            if (prev)
                CHECK(ud_name_cmp(prev, nm) < 0,
                      "ordering: '%s' before '%s'", prev, nm);
            prev = nm;
            cnt++;
        }
        CHECK(cnt == n, "ordering: enumerated %d of %d", cnt, n);
        for (i = 0; i < n; i++)
            CHECK(ud_cfb_child(c, UD_CFB_ROOT_ID, N[i]) != UD_CFB_NONE,
                  "ordering: '%s' not findable", N[i]);
        /* lookup is case-insensitive, as CFB defines it */
        CHECK(ud_cfb_child(c, UD_CFB_ROOT_ID, "ZZ") ==
              ud_cfb_child(c, UD_CFB_ROOT_ID, "zz"),
              "ordering: case-insensitive lookup");
        ud_cfb_close(c);
    }
    ud_free(img);
    ud_cfbw_free(w);
}

/* Enough data to push the FAT past the 109 slots in the header, so the
 * DIFAT chain itself is exercised - the part of the format that is never hit
 * by a small test file and always hit by a real presentation. */
static void t_difat(void)
{
    const int NS = 160;
    const long SZ = 100000;
    ud_cfbw *w = ud_cfbw_new();
    unsigned char *img;
    long len = 0;
    ud_src src;
    ud_cfb *c;
    int i;

    for (i = 0; i < NS; i++) {
        char nm[16];
        unsigned char *b = (unsigned char *)ud_alloc((unsigned long)SZ);
        long j;
        sprintf(nm, "big%03d", i);
        for (j = 0; j < SZ; j++) b[j] = (unsigned char)((j * 31 + i) & 0xFF);
        CHECK(ud_cfbw_stream_take(w, UD_CFB_ROOT_ID, nm, b, SZ) != UD_CFB_NONE,
              "difat: add %s: %s", nm, ud_error());
    }
    img = ud_cfbw_serialize(w, &len);
    CHECK(img != 0, "difat: serialize: %s", ud_error());
    if (!img) { ud_cfbw_free(w); return; }
    /* 16 MB of payload => ~250 FAT sectors => the header's 109 slots overflow */
    CHECK(len > 16L * 1000 * 1000, "difat: image only %ld bytes", len);
    c = reopen(img, len, &src);
    CHECK(c != 0, "difat: reopen: %s", ud_error());
    if (c) {
        for (i = 0; i < NS; i += 37) {
            char nm[16];
            int id;
            long got = 0, j;
            unsigned char *b;
            int bad = 0;
            sprintf(nm, "/big%03d", i);
            id = ud_cfb_find(c, nm);
            CHECK(id != UD_CFB_NONE, "difat: %s missing", nm);
            if (id == UD_CFB_NONE) continue;
            b = ud_cfb_load(c, id, &got);
            CHECK(b && got == SZ, "difat: %s short (%ld)", nm, got);
            if (b) {
                for (j = 0; j < got; j++)
                    if (b[j] != (unsigned char)((j * 31 + i) & 0xFF)) { bad = 1; break; }
                CHECK(!bad, "difat: %s content differs at %ld", nm, j);
                ud_free(b);
            }
        }
        ud_cfb_close(c);
    }
    ud_free(img);
    ud_cfbw_free(w);
}

/* Random access into a big stream must agree with a whole-stream load: this
 * is what catches an off-by-one in the chain cursor. */
static void t_random_access(void)
{
    const long SZ = 300000;
    ud_cfbw *w = ud_cfbw_new();
    unsigned char *pat = pattern(SZ, 424242u), *img;
    long len = 0;
    ud_src src;
    ud_cfb *c;

    ud_cfbw_stream(w, UD_CFB_ROOT_ID, "big", pat, SZ);
    ud_cfbw_stream(w, UD_CFB_ROOT_ID, "small", pat, 2000);
    img = ud_cfbw_serialize(w, &len);
    CHECK(img != 0, "random: serialize: %s", ud_error());
    if (img) {
        c = reopen(img, len, &src);
        CHECK(c != 0, "random: reopen: %s", ud_error());
        if (c) {
            int big = ud_cfb_find(c, "big"), sml = ud_cfb_find(c, "small");
            unsigned seed = 99u;
            int t;
            unsigned char buf[777];
            /* backwards, forwards and straddling, on both tables */
            for (t = 0; t < 400; t++) {
                int mini = t & 1;
                long cap = mini ? 2000 : SZ;
                long off, n, got;
                seed = seed * 1103515245u + 12345u;
                off = (long)((seed >> 8) % (unsigned)(cap + 100));
                seed = seed * 1103515245u + 12345u;
                n = (long)((seed >> 8) % 777u) + 1;
                got = ud_cfb_read(c, mini ? sml : big, off, buf, n);
                if (off >= cap) {
                    CHECK(got == 0, "random: read past end returned %ld", got);
                } else {
                    long want = n > cap - off ? cap - off : n;
                    CHECK(got == want, "random: got %ld want %ld at %ld",
                          got, want, off);
                    CHECK(memcmp(buf, pat + off, (size_t)got) == 0,
                          "random: bytes differ at %ld", off);
                }
            }
            CHECK(ud_cfb_read(c, big, -5, buf, 10) == 0, "random: negative off");
            CHECK(ud_cfb_read(c, big, 0, buf, 0) == 0, "random: zero length");
            CHECK(ud_cfb_read(c, 9999, 0, buf, 10) == 0, "random: bad id");
            ud_cfb_close(c);
        }
        ud_free(img);
    }
    free(pat);
    ud_cfbw_free(w);
}

/* ---- corrupt containers ----------------------------------------------------
 * Each case patches a known-good image.  The requirement is not "rejects" -
 * a reader may salvage - it is "terminates, stays inside its buffers, and
 * never hands the caller bytes it did not read".  ASan/UBSan enforce the
 * middle clause; the harness enforces the other two. */
static unsigned char *good_image(long *len)
{
    ud_cfbw *w = ud_cfbw_new();
    unsigned char *img, *pat = pattern(9000, 5u);
    int sub;
    ud_cfbw_stream(w, UD_CFB_ROOT_ID, "WordDocument", pat, 9000);
    ud_cfbw_stream(w, UD_CFB_ROOT_ID, "\0051Table", pat, 500);
    sub = ud_cfbw_storage(w, UD_CFB_ROOT_ID, "ObjectPool");
    ud_cfbw_stream(w, sub, "\001Ole", pat, 20);
    img = ud_cfbw_serialize(w, len);
    free(pat);
    ud_cfbw_free(w);
    return img;
}

/* Open and exhaustively walk an image.  Returns 1 if it opened at all.  The
 * point is the walk, not the answer: under ASan/UBSan any out-of-bounds or
 * undefined step inside unodoc aborts the process here. */
static int walk_all(const unsigned char *buf, long len)
{
    char *d = digest_of(buf, len);
    int opened = d && strncmp(d, "OPEN-FAILED", 11) != 0;
    free(d);
    return opened;
}

static unsigned char *patched(const unsigned char *good, long len, long off,
                              const unsigned char *bytes, int n)
{
    unsigned char *b = (unsigned char *)malloc((size_t)len);
    memcpy(b, good, (size_t)len);
    if (off >= 0 && off + n <= len) memcpy(b + off, bytes, (size_t)n);
    return b;
}

static void expect_reject(const unsigned char *good, long len, long off,
                          const unsigned char *bytes, int n, const char *what)
{
    unsigned char *b = patched(good, len, off, bytes, n);
    ud_src s;
    ud_cfb *c;
    ud_src_mem(&s, b, len);
    c = ud_cfb_open(&s);
    CHECK(c == 0, "corrupt: %s accepted", what);
    ud_cfb_close(c);
    free(b);
}

static void t_corrupt(void)
{
    static const unsigned char B_FF[4]  = { 0xFF, 0xFF, 0xFF, 0xFF };
    static const unsigned char B_EOC[4] = { 0xFE, 0xFF, 0xFF, 0xFF };
    static const unsigned char B_00[4]  = { 0x00, 0x00, 0x00, 0x00 };
    static const unsigned char B_ODD[4] = { 0xF0, 0x0D, 0x00, 0x00 };
    static const unsigned char B_ONE[1] = { 0x07 };
    long len = 0, i;
    unsigned char *good = good_image(&len);
    unsigned seed = 7u;

    CHECK(good != 0, "corrupt: could not build a good image");
    if (!good) return;

    /* Header damage: these must be REFUSED, not guessed at.  Guessing here
       is how a parser ends up interpreting arbitrary bytes as sector
       geometry. */
    expect_reject(good, len, 0x00, B_FF, 4, "bad signature");
    expect_reject(good, len, 0x1E, B_ONE, 1, "sector shift 7");
    expect_reject(good, len, 0x20, B_ONE, 1, "mini sector shift 7");
    expect_reject(good, len, 0x1C, B_00, 2, "wrong byte order mark");
    expect_reject(good, len, 0x38, B_00, 4, "mini cutoff 0");
    expect_reject(good, len, 0x2C, B_00, 4, "zero FAT sectors");
    expect_reject(good, len, 0x2C, B_FF, 4, "absurd FAT sector count");
    {   /* files too small to hold anything */
        ud_src s; ud_cfb *c;
        ud_src_mem(&s, good, 400);  c = ud_cfb_open(&s);
        CHECK(c == 0, "corrupt: 400-byte file accepted"); ud_cfb_close(c);
        ud_src_mem(&s, good, 512);  c = ud_cfb_open(&s);
        CHECK(c == 0, "corrupt: header-only file accepted"); ud_cfb_close(c);
        ud_src_mem(&s, good, 0);    c = ud_cfb_open(&s);
        CHECK(c == 0, "corrupt: empty file accepted"); ud_cfb_close(c);
    }

    /* Truncation at every sector boundary: never a crash, never a hang. */
    for (i = 512; i < len; i += 512) walk_all(good, i);

    /* Structural damage.  A reader is allowed to salvage what it can, so the
       requirement is only that it terminates and stays in bounds - which is
       exactly what the chain-step budget and the visited bitmap buy. */
    {
        const unsigned char *vals[4];
        int v;
        vals[0] = B_FF; vals[1] = B_EOC; vals[2] = B_00; vals[3] = B_ODD;
        /* one hostile u32 at a time, over a ~20% sample of the image: this
           reaches FAT entries, mini FAT entries, directory sector/size/name
           fields and sibling pointers without a combinatorial blowup */
        for (i = 0; i + 4 <= len; i += 4) {
            seed = seed * 1103515245u + 12345u;
            if ((seed >> 16) % 5u) continue;
            for (v = 0; v < 4; v++) {
                unsigned char *b = patched(good, len, i, vals[v], 4);
                walk_all(b, len);
                free(b);
            }
        }
    }
    free(good);
}

static void selftest(void)
{
    t_empty();
    t_roundtrip();
    t_ordering();
    t_difat();
    t_random_access();
    t_corrupt();
    if (g_fail) printf("cfbtest: %d FAILURES\n", g_fail);
    else        printf("cfbtest: selftest OK\n");
}

/* ===========================================================================
 * main
 * ======================================================================== */
int main(int argc, char **argv)
{
    ud_set_alloc(t_alloc, free);

    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        selftest();
        return g_fail ? 1 : 0;
    }
    if (argc >= 3 && strcmp(argv[1], "ls") == 0) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        char *d;
        if (!b) { printf("ERR: cannot read %s\n", argv[2]); return 2; }
        d = digest_of(b, n);
        fputs(d ? d : "", stdout);
        free(d); free(b);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "rt") == 0) {
        long n = 0, rn = 0;
        unsigned char *b = slurp(argv[2], &n), *r;
        char *d1, *d2;
        int rc = 0;
        if (!b) { printf("ERR: cannot read %s\n", argv[2]); return 2; }
        r = rebuild(b, n, &rn);
        if (!r) { printf("ERR: rebuild failed: %s\n", ud_error()); free(b); return 1; }
        d1 = digest_of(b, n);
        d2 = digest_of(r, rn);
        if (!d1 || !d2 || strcmp(d1, d2) != 0) {
            printf("ERR: digest differs after rebuild\n--- original ---\n%s"
                   "--- rebuilt ---\n%s", d1 ? d1 : "", d2 ? d2 : "");
            rc = 1;
        } else {
            printf("OK %ld -> %ld bytes, tree identical\n", n, rn);
        }
        free(d1); free(d2); free(r); free(b);
        return rc;
    }
    if (argc >= 4 && strcmp(argv[1], "rebuild") == 0) {
        long n = 0, rn = 0;
        unsigned char *b = slurp(argv[2], &n), *r;
        FILE *f;
        if (!b) { printf("ERR: cannot read %s\n", argv[2]); return 2; }
        r = rebuild(b, n, &rn);
        if (!r) { printf("ERR: rebuild failed: %s\n", ud_error()); free(b); return 1; }
        f = fopen(argv[3], "wb");
        if (!f) { printf("ERR: cannot write %s\n", argv[3]); free(r); free(b); return 2; }
        fwrite(r, 1, (size_t)rn, f);
        fclose(f);
        printf("OK %ld -> %ld bytes\n", n, rn);
        free(r); free(b);
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "fuzz") == 0) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        unsigned seed = (unsigned)strtoul(argv[3], 0, 10);
        long iters = strtol(argv[4], 0, 10), t;
        long opened = 0;
        if (!b || n < 512) { printf("ERR: cannot read %s\n", argv[2]); free(b); return 2; }
        for (t = 0; t < iters; t++) {
            unsigned char *m = (unsigned char *)malloc((size_t)n);
            int k, nmut;
            memcpy(m, b, (size_t)n);
            seed = seed * 1103515245u + 12345u;
            nmut = (int)((seed >> 16) % 12u) + 1;
            for (k = 0; k < nmut; k++) {
                long off;
                seed = seed * 1103515245u + 12345u;
                off = (long)((seed >> 8) % (unsigned long)n);
                seed = seed * 1103515245u + 12345u;
                m[off] = (unsigned char)(seed >> 16);
            }
            opened += walk_all(m, n);
            free(m);
        }
        printf("OK %ld mutations, %ld opened\n", iters, opened);
        free(b);
        return 0;
    }

    printf("usage: cfbtest selftest | ls FILE | rt FILE | rebuild IN OUT"
           " | fuzz FILE SEED N\n");
    return 2;
}
