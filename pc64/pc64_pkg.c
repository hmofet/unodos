/* pc64_pkg.c - unopkg: foreign packages that install as ordinary apps.
 *
 * Contract: pc64/UNOPKG.md.  Header: pc64_pkg.h, which carries the rationale.
 *
 * FOUR THINGS HAPPEN HERE and they are worth naming separately, because only
 * the first is about Android at all:
 *
 *   1. A ZIP READER.  An APK is a zip, a .deb is ar+tar and a .rpm is cpio;
 *      this file grows one reader per container and nothing else per format.
 *   2. A BINARY-XML READER.  AndroidManifest.xml inside an APK is AXML, not
 *      text: a string pool followed by a tree of chunks.  It is where the
 *      package name and the launcher activity live, and there is no other
 *      copy of either.
 *   3. AN INSTALL, which is three file writes and a rescan.
 *   4. A RUNTIME PROBE that answers "could this run here", honestly, on a
 *      machine that has no appliance.
 *
 * WHY THE ZIP READER IS LOCAL rather than unodoc's.  `unodoc/ud_zip.c` is a
 * better zip reader than this one and it is NOT linked into the kernel - it
 * lives inside the Office modules.  Pulling unodoc into the kernel to read a
 * central directory would cost far more than the eighty lines below, and
 * AGENTS.md's "consume, do not duplicate" rule is about not reimplementing a
 * neighbour's JOB, which is parsing OOXML.  What is copied deliberately is
 * ud_zip.c's hard-won lesson, and it is the one thing a naive reader gets
 * wrong: READ THE CENTRAL DIRECTORY, NEVER THE LOCAL HEADERS.  A local header
 * may carry zeroed sizes with the truth in a trailing data descriptor, which
 * is exactly what a streaming zip writer emits - and every APK is built by
 * one.
 *
 * WHAT IS NOT COPIED, AND WHY THE PACKAGE STAYS WHERE THE USER PUT IT.  The
 * layer under this one writes WHOLE FILES ONLY (`fat.h`: "create/overwrite a
 * file with exactly len bytes"), so copying a 100 MB APK would mean holding
 * 100 MB of kernel heap.  So an install RECORDS where the package is rather
 * than duplicating it, and the sidecar carries that path.  The consequence is
 * real and is stated in the UI rather than hidden: delete the APK and the app
 * reports that its package is missing.  This is the consumer that motivates
 * the write-at-offset request already filed with the unofs lane; when that
 * lands, an install can copy and this paragraph goes away.
 */
#include "pc64_pkg.h"
#include "pc64_fs.h"
#include "fat.h"
#include "uno_appdesc.h"
#include "uno_uuiapp.h"
#include "unovirt.h"
#include "unomedia.h"
#include <string.h>
#include <stdlib.h>

void pc64_shell_apps_rescan(void);

/* An APK's manifest is tens of KB; the central directory of a big one is a
 * few hundred.  Both are refused past a bound rather than trusted, because
 * both lengths come out of the file being examined. */
#define PKG_CD_MAX    (4L * 1024 * 1024)
#define PKG_PART_MAX  (2L * 1024 * 1024)
#define PKG_TPL_MAX   (512L * 1024)

/* ---- small string helpers ------------------------------------------------ */

static void sput(char *d, int max, const char *s)
{
    int i = 0;
    if (max <= 0) return;
    while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void scat(char *d, int max, const char *s)
{
    int i = 0;
    while (i < max && d[i]) i++;
    while (s && *s && i < max - 1) d[i++] = *s++;
    if (i < max) d[i] = 0;
}

static int seq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void sdec(char *d, int max, long v)
{
    char t[24]; int n = 0, i = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < 23);
    while (n && i < max - 1) d[i++] = t[--n];
    if (i < max) d[i] = 0;
}

/* ---- volume helpers ------------------------------------------------------ */

static long pkg_read_at(int vol, const char *path, long off,
                        unsigned char *buf, long max)
{
    return uno_fs_read_at(vol, path, off, buf, max);
}

static int pkg_write(int vol, const char *path, const unsigned char *b, long n)
{
    if (uno_fs_kind(vol) == 1)
        return uno_fat_write(uno_fs_fat_index(vol), path, b, n);
    return uno_fs_write(vol, path, b, n);
}

static int pkg_delete(int vol, const char *path)
{
    if (uno_fs_kind(vol) != 1) return 0;
    return uno_fat_delete(uno_fs_fat_index(vol), path);
}

/* Where installed things go on `vol`.  The installed-system layout wins when
 * it is present, matching every other consumer of the two-layout rule
 * (uno_mod_find, pc64_font.c); a dev stick has only the root layout. */
static void pkg_dirs(int vol, char *apps, int amax, char *pkg, int pmax)
{
    if (uno_fs_isdir(vol, "EFI\\UNODOS\\APPS")) {
        sput(apps, amax, "EFI\\UNODOS\\APPS");
        sput(pkg,  pmax, UNO_PKG_DIR_ESP);
    } else {
        sput(apps, amax, "APPS");
        sput(pkg,  pmax, UNO_PKG_DIR_ROOT);
    }
}

static void pkg_join(char *d, int max, const char *dir, const char *leaf)
{
    sput(d, max, dir); scat(d, max, "\\"); scat(d, max, leaf);
}

/* ---- the zip container --------------------------------------------------- */

typedef struct {
    int            vol;
    const char    *path;
    long           size;
    unsigned char *cd;          /* the whole central directory, in memory   */
    long           cdn;
    int            count;
} pkg_zip;

static unsigned g16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long g32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void zip_close(pkg_zip *z)
{
    if (z->cd) free(z->cd);
    z->cd = 0; z->cdn = 0; z->count = 0;
}

/* Find the end-of-central-directory record.  It is normally the last 22 bytes
 * of the file, but a zip comment can push it back by up to 64 KB, so the tail
 * is walked in windows rather than assumed. */
static int zip_eocd(int vol, const char *path, long size, long *cd_off,
                    long *cd_size, int *count)
{
    unsigned char win[4096];
    long back = 4096, guard = 0;

    while (back <= 66000L && guard++ < 20) {
        long at = size - back, n, i;
        if (at < 0) at = 0;
        n = pkg_read_at(vol, path, at, win, (long)sizeof win);
        if (n < 22) return 0;
        for (i = n - 22; i >= 0; i--) {
            if (win[i] == 0x50 && win[i + 1] == 0x4b
                && win[i + 2] == 0x05 && win[i + 3] == 0x06) {
                *count   = (int)g16(win + i + 10);
                *cd_size = (long)g32(win + i + 12);
                *cd_off  = (long)g32(win + i + 16);
                /* ZIP64 puts 0xFFFFFFFF in these; refuse rather than read
                 * from a place we computed out of a sentinel. */
                if (*cd_off < 0 || *cd_size < 0 || *cd_off >= size) return 0;
                return 1;
            }
        }
        if (at == 0) return 0;
        back += 4096 - 22;              /* overlap, so a straddling record is
                                           not missed at a window boundary  */
    }
    return 0;
}

static int zip_open(pkg_zip *z, int vol, const char *path)
{
    long cd_off = 0, cd_size = 0;
    int count = 0;

    z->vol = vol; z->path = path; z->cd = 0; z->cdn = 0; z->count = 0;
    z->size = uno_fs_size(vol, path);
    if (z->size < 22) return 0;
    if (!zip_eocd(vol, path, z->size, &cd_off, &cd_size, &count)) return 0;
    if (cd_size <= 0 || cd_size > PKG_CD_MAX) return 0;
    if (cd_off + cd_size > z->size) return 0;

    z->cd = (unsigned char *)malloc((unsigned long)cd_size);
    if (!z->cd) return 0;
    if (pkg_read_at(vol, path, cd_off, z->cd, cd_size) != cd_size) {
        zip_close(z); return 0;
    }
    z->cdn = cd_size; z->count = count;
    return 1;
}

/* Walk the central directory.  `*cur` starts at 0 and is advanced; returns 1
 * while entries remain.  Every length is bounds-checked against what was
 * actually read, because all of them came out of the file. */
static int zip_next(pkg_zip *z, long *cur, char *name, int namemax,
                    int *method, long *csize, long *usize, long *lho)
{
    unsigned char *e;
    long nl, xl, cl, i;

    if (*cur < 0 || *cur + 46 > z->cdn) return 0;
    e = z->cd + *cur;
    if (g32(e) != 0x02014b50UL) return 0;
    *method = (int)g16(e + 10);
    *csize  = (long)g32(e + 20);
    *usize  = (long)g32(e + 24);
    nl = (long)g16(e + 28);
    xl = (long)g16(e + 30);
    cl = (long)g16(e + 32);
    *lho    = (long)g32(e + 42);
    if (*cur + 46 + nl + xl + cl > z->cdn) return 0;
    for (i = 0; i < nl && i < namemax - 1; i++) name[i] = (char)e[46 + i];
    name[(i < namemax) ? i : namemax - 1] = 0;
    *cur += 46 + nl + xl + cl;
    return 1;
}

/* ---- inflate glue -------------------------------------------------------- */

typedef struct { int vol; const char *path; long off, left; } inf_in;
typedef struct { unsigned char *p; long n, cap; } inf_out;

static long inf_read(void *ctx, unsigned char *dst, long max)
{
    inf_in *s = (inf_in *)ctx;
    long n;
    if (s->left <= 0) return 0;
    if (max > s->left) max = s->left;
    n = pkg_read_at(s->vol, s->path, s->off, dst, max);
    if (n <= 0) return 0;
    s->off += n; s->left -= n;
    return n;
}

static int inf_write(void *ctx, const unsigned char *p, long n)
{
    inf_out *o = (inf_out *)ctx;
    if (o->n + n > o->cap) return 0;         /* aborts the stream, by design */
    memcpy(o->p + o->n, p, (unsigned long)n);
    o->n += n;
    return 1;
}

/* Extract one member by name into a fresh buffer.  Returns its length, or -1.
 * The caller frees.  Stored (0) and deflate (8) only; anything else is
 * refused rather than mis-read. */
static long zip_get(pkg_zip *z, const char *want, unsigned char **out)
{
    char name[192];
    long cur = 0, csize, usize, lho;
    int method;
    unsigned char lh[30];
    long data, nl, xl;

    *out = 0;
    while (zip_next(z, &cur, name, (int)sizeof name,
                    &method, &csize, &usize, &lho)) {
        if (!seq(name, want)) continue;
        if (usize < 0 || usize > PKG_PART_MAX) return -1;
        if (lho < 0 || lho + 30 > z->size) return -1;
        if (pkg_read_at(z->vol, z->path, lho, lh, 30) != 30) return -1;
        if (g32(lh) != 0x04034b50UL) return -1;
        /* The local header's NAME and EXTRA lengths are the ones that matter -
         * they may differ from the central copy, and the payload starts after
         * them.  Its size fields are the ones that may lie; we do not use
         * them. */
        nl = (long)g16(lh + 26);
        xl = (long)g16(lh + 28);
        data = lho + 30 + nl + xl;
        if (data < 0 || data + csize > z->size) return -1;

        *out = (unsigned char *)malloc((unsigned long)(usize + 1));
        if (!*out) return -1;
        if (method == 0) {
            if (csize != usize
                || pkg_read_at(z->vol, z->path, data, *out, usize) != usize) {
                free(*out); *out = 0; return -1;
            }
        } else if (method == 8) {
            inf_in  in;
            inf_out o;
            in.vol = z->vol; in.path = z->path; in.off = data; in.left = csize;
            o.p = *out; o.n = 0; o.cap = usize;
            um_set_alloc(malloc, free);       /* idempotent kernel-heap wiring */
            if (!um_inflate(inf_read, &in, inf_write, &o, 0)) {
                free(*out); *out = 0; return -1;
            }
            usize = o.n;
        } else {
            free(*out); *out = 0; return -1;
        }
        (*out)[usize] = 0;
        return usize;
    }
    return -1;
}

/* ---- Android binary XML (AXML) ------------------------------------------- */

/* Chunk types, from AOSP's ResourceTypes.h.  Only the five that carry an
 * element tree are handled; everything else is skipped by its own length,
 * which is what makes an unknown chunk harmless. */
#define AX_STRPOOL   0x0001
#define AX_XML       0x0003
#define AX_START_EL  0x0102
#define AX_END_EL    0x0103

typedef struct {
    const unsigned char *b;
    long                 n;
    const unsigned char *offs;   /* string offset table                      */
    const unsigned char *strs;   /* string data                              */
    long                 strsn;
    int                  nstr;
    int                  utf8;
} axml;

static int axml_open(axml *a, const unsigned char *b, long n)
{
    long at;
    a->b = b; a->n = n; a->nstr = 0; a->offs = 0; a->strs = 0; a->strsn = 0;
    a->utf8 = 0;
    if (n < 8 || g16(b) != AX_XML) return 0;
    at = (long)g16(b + 2);                       /* header size              */
    while (at + 8 <= n) {
        long sz = (long)g32(b + at + 4);
        int  ty = (int)g16(b + at);
        if (sz < 8 || at + sz > n) return 0;
        if (ty == AX_STRPOOL) {
            long soff, dstart;
            if (sz < 28) return 0;
            a->nstr  = (int)g32(b + at + 8);
            a->utf8  = (g32(b + at + 16) & 0x100UL) ? 1 : 0;
            dstart   = (long)g32(b + at + 20);
            soff     = at + (long)g16(b + at + 2);
            if (a->nstr < 0 || a->nstr > 200000) return 0;
            if (soff + 4L * a->nstr > at + sz) return 0;
            if (dstart <= 0 || at + dstart > at + sz) return 0;
            a->offs  = b + soff;
            a->strs  = b + at + dstart;
            a->strsn = sz - dstart;
            return 1;
        }
        at += sz;
    }
    return 0;
}

/* Copy string `idx` out of the pool as ASCII.  Anything outside ASCII becomes
 * '?': every string this file actually matches on (a package name, a class
 * name, an intent action) is ASCII by Android's own rules, and a label that
 * is not is replaced wholesale by the caller. */
static int axml_str(const axml *a, long idx, char *out, int max)
{
    long off;
    const unsigned char *p;
    int i = 0;

    if (max > 0) out[0] = 0;
    if (!a->offs || idx < 0 || idx >= a->nstr || max <= 1) return 0;
    off = (long)g32(a->offs + 4 * idx);
    if (off < 0 || off + 2 > a->strsn) return 0;
    p = a->strs + off;

    if (a->utf8) {
        long n;
        int  hdr = 0;
        /* two lengths, each 1 or 2 bytes: the char count then the byte count */
        if ((p[0] & 0x80)) hdr += 2; else hdr += 1;
        if ((p[hdr] & 0x80)) { n = (long)(((p[hdr] & 0x7f) << 8) | p[hdr + 1]);
                               hdr += 2; }
        else                 { n = (long)p[hdr]; hdr += 1; }
        if (off + hdr + n > a->strsn) return 0;
        for (i = 0; i < n && i < max - 1; i++) {
            unsigned char c = p[hdr + i];
            out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
    } else {
        long n = (long)g16(p);
        long base = 2;
        if (n & 0x8000L) { n = ((n & 0x7fffL) << 16) | (long)g16(p + 2);
                           base = 4; }
        if (off + base + 2 * n > a->strsn) return 0;
        for (i = 0; i < n && i < max - 1; i++) {
            unsigned u = g16(p + base + 2 * i);
            out[i] = (u >= 0x20 && u < 0x7f) ? (char)u : '?';
        }
    }
    out[i] = 0;
    return i;
}

/* One attribute of a START_ELEMENT, by its (namespace-ignored) name.
 *
 * IGNORING THE NAMESPACE IS DELIBERATE.  Every attribute this file wants is
 * either bare (`package`) or in the android namespace (`name`, `label`,
 * `versionName`), and no manifest element carries two attributes with the
 * same local name in different namespaces.  Resolving namespaces properly
 * would mean matching a URI string on every attribute of every element, for
 * an answer that is already known. */
static int axml_attr(const axml *a, long el, const char *want,
                     char *out, int max)
{
    long ext = el + 16, astart, asize, i;
    int  ac;

    if (max > 0) out[0] = 0;
    if (ext + 20 > a->n) return 0;
    astart = (long)g16(a->b + ext + 8);
    asize  = (long)g16(a->b + ext + 10);
    ac     = (int)g16(a->b + ext + 12);
    if (asize < 20 || ac < 0 || ac > 4096) return 0;
    if (ext + astart + (long)ac * asize > a->n) return 0;

    for (i = 0; i < ac; i++) {
        const unsigned char *at = a->b + ext + astart + i * asize;
        char nm[48];
        unsigned long raw, data;
        int dtype;
        axml_str(a, (long)g32(at + 4), nm, (int)sizeof nm);
        if (!seq(nm, want)) continue;
        raw   = g32(at + 8);
        dtype = (int)at[15];
        data  = g32(at + 16);
        if (raw != 0xFFFFFFFFUL) { axml_str(a, (long)raw, out, max); return 1; }
        if (dtype == 0x03)       { axml_str(a, (long)data, out, max); return 1; }
        if (dtype == 0x10)       { sdec(out, max, (long)data); return 1; }
        return 0;                  /* a reference (a @string/...): not ours  */
    }
    return 0;
}

/* ---- reading a manifest into a uno_pkg_info ------------------------------ */

/* "org.mozilla.firefox" -> "firefox"; also the fallback display name. */
static void last_component(const char *pkg, char *out, int max)
{
    int i = 0, last = 0;
    while (pkg[i]) { if (pkg[i] == '.') last = i + 1; i++; }
    sput(out, max, pkg + last);
}

static void id_from(const char *s, char *out, int max)
{
    int i = 0, o = 0;
    while (s[i] && o < max - 1) {
        char c = s[i++];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == '-')
            out[o++] = c;
    }
    out[o] = 0;
    if (!o) sput(out, max, "foreign");
}

static void title_case(const char *s, char *out, int max)
{
    int i = 0;
    sput(out, max, s);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    (void)i;
}

static int apk_manifest(pkg_zip *z, uno_pkg_info *out)
{
    unsigned char *m = 0;
    long n = zip_get(z, "AndroidManifest.xml", &m);
    axml a;
    long at, depth = 0, act_depth = 0;
    char cur_act[128], launcher[128], label[64], pkg[128], ver[24];
    int saw_main = 0, saw_launch = 0, have = 0;

    if (n < 0 || !m) return 0;
    if (!axml_open(&a, m, n)) { free(m); return 0; }

    cur_act[0] = launcher[0] = label[0] = pkg[0] = ver[0] = 0;
    at = (long)g16(m + 2);
    while (at + 8 <= n) {
        long sz = (long)g32(m + at + 4);
        int  ty = (int)g16(m + at);
        if (sz < 8 || at + sz > n) break;

        if (ty == AX_START_EL) {
            char el[48];
            axml_str(&a, (long)g32(m + at + 20), el, (int)sizeof el);
            depth++;
            if (depth == 1 && seq(el, "manifest")) {
                axml_attr(&a, at, "package", pkg, (int)sizeof pkg);
                axml_attr(&a, at, "versionName", ver, (int)sizeof ver);
            } else if (seq(el, "application")) {
                axml_attr(&a, at, "label", label, (int)sizeof label);
            } else if (seq(el, "activity") || seq(el, "activity-alias")) {
                act_depth = depth; saw_main = saw_launch = 0;
                axml_attr(&a, at, "name", cur_act, (int)sizeof cur_act);
            } else if (act_depth && seq(el, "action")) {
                char v[64];
                if (axml_attr(&a, at, "name", v, (int)sizeof v)
                    && seq(v, "android.intent.action.MAIN")) saw_main = 1;
            } else if (act_depth && seq(el, "category")) {
                char v[64];
                if (axml_attr(&a, at, "name", v, (int)sizeof v)
                    && seq(v, "android.intent.category.LAUNCHER")) saw_launch = 1;
            }
        } else if (ty == AX_END_EL) {
            if (act_depth && depth == act_depth) {
                if (saw_main && saw_launch && !have && cur_act[0]) {
                    sput(launcher, (int)sizeof launcher, cur_act);
                    have = 1;
                }
                act_depth = 0;
            }
            depth--;
        }
        at += sz;
    }
    free(m);

    if (!pkg[0]) return 0;

    sput(out->target, (int)sizeof out->target, "android:");
    scat(out->target, (int)sizeof out->target, pkg);
    if (launcher[0]) {
        scat(out->target, (int)sizeof out->target, "/");
        scat(out->target, (int)sizeof out->target, launcher);
    }
    sput(out->version, (int)sizeof out->version, ver);

    {   /* The id and the display name.  A literal label is used when the
         * manifest has one; most real apps put theirs in resources.arsc as a
         * reference, so the ordinary answer is the package's last component
         * title-cased - which is "Firefox" for org.mozilla.firefox and a
         * reasonable guess for everything else.  The guest knows the true
         * label and can correct it once there is a channel to ask over. */
        char base[64];
        last_component(pkg, base, (int)sizeof base);
        id_from(base, out->id, (int)sizeof out->id);
        if (label[0] && label[0] != '?') sput(out->name, (int)sizeof out->name, label);
        else title_case(base, out->name, (int)sizeof out->name);
    }
    return 1;
}

/* Which ABIs the package carries.  A JNI library for the wrong architecture
 * is not a slow app, it is a crash at the first call into it, so this decides
 * whether the package is installable at all. */
static void apk_arch(pkg_zip *z, uno_pkg_info *out)
{
    char name[192];
    long cur = 0, csize, usize, lho;
    int method, any = 0;

    out->arch_ok = 0; out->arch[0] = 0;
    while (zip_next(z, &cur, name, (int)sizeof name,
                    &method, &csize, &usize, &lho)) {
        if (name[0] != 'l' || name[1] != 'i' || name[2] != 'b'
            || name[3] != '/') continue;
        any = 1;
        if (name[4] == 'x' && name[5] == '8' && name[6] == '6'
            && name[7] == '_' && name[8] == '6' && name[9] == '4'
            && name[10] == '/') { out->arch_ok = 1; sput(out->arch, 32, "x86_64"); return; }
        if (!out->arch[0]) {
            int i = 4, o = 0;
            while (name[i] && name[i] != '/' && o < 31) out->arch[o++] = name[i++];
            out->arch[o] = 0;
        }
    }
    /* No lib/ at all means pure Java/Kotlin, which runs on any ABI. */
    if (!any) { out->arch_ok = 1; sput(out->arch, 32, "any"); }
}

/* ---- probe --------------------------------------------------------------- */

static int ends_icase(const char *s, const char *suf)
{
    int ls = 0, lf = 0, i;
    while (s[ls]) ls++;
    while (suf[lf]) lf++;
    if (ls < lf) return 0;
    for (i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

int uno_pkg_probe(int vol, const char *path, uno_pkg_info *out)
{
    pkg_zip z;
    int ok;

    memset(out, 0, sizeof *out);
    if (!path || !ends_icase(path, ".APK")) return -1;
    out->kind = UNO_PKG_APK;
    out->size = uno_fs_size(vol, path);
    if (out->size <= 0) return -1;
    if (!zip_open(&z, vol, path)) return -1;

    ok = apk_manifest(&z, out);
    if (ok) apk_arch(&z, out);
    zip_close(&z);
    if (!ok) return -1;
    return out->arch_ok ? 1 : 0;
}

/* ---- the shim ------------------------------------------------------------ */

/* Rewrite the descriptor block inside a template image.  The block is found
 * by its own magic rather than through the module header, because mkuno.py
 * guarantees at build time that a module carries EXACTLY ONE - it refuses a
 * second - so the search cannot be ambiguous, and this file then needs to
 * know nothing about UnoModHdr's layout. */
static int shim_desc(unsigned char *img, long n, const uno_pkg_info *info,
                     const char *iconspec)
{
    long i, at = -1, room;
    char body[UNO_APPDESC_MAX];
    long bn;

    for (i = 0; i + 8 <= n; i++) {
        if (img[i] == 'U' && img[i + 1] == 'A' && img[i + 2] == 'P'
            && img[i + 3] == 'P' && g16(img + i + 4) == UNO_APPDESC_VER) {
            at = i; break;
        }
    }
    if (at < 0) return 0;
    room = (long)g16(img + at + 6);          /* what the template reserved   */
    if (room < 16 || at + room > n) return 0;

    body[0] = 0;
    scat(body, (int)sizeof body, "id: ");    scat(body, (int)sizeof body, info->id);
    scat(body, (int)sizeof body, "\nname: "); scat(body, (int)sizeof body, info->name);
    scat(body, (int)sizeof body, "\nshort: ");scat(body, (int)sizeof body, info->name);
    scat(body, (int)sizeof body, "\nicon: "); scat(body, (int)sizeof body, iconspec);
    scat(body, (int)sizeof body, "\ncat: other\nrank: 120\n");
    /* Unknown keys are ignored by the reader and kept for the next version of
     * it - uno_appdesc.h calls that the format's extension point, and this is
     * the first user of it. */
    scat(body, (int)sizeof body, "kind: foreign\ntarget: ");
    scat(body, (int)sizeof body, info->target);
    scat(body, (int)sizeof body, "\n");

    bn = 0; while (body[bn]) bn++;
    if (8 + bn + 1 > room) return 0;         /* would run past the section   */

    memcpy(img + at + 8, body, (unsigned long)bn);
    img[at + 8 + bn] = 0;
    /* Shrink the length to what was written.  A SHORTER block is safe by
     * construction: every reader takes exactly `len` bytes, and the bytes
     * beyond it are still inside the same reserved run. */
    img[at + 6] = (unsigned char)((8 + bn + 1) & 0xff);
    img[at + 7] = (unsigned char)(((8 + bn + 1) >> 8) & 0xff);
    return 1;
}

static int shim_blob(unsigned char *img, long n, const uno_pkg_info *info)
{
    static const char mark[] = "UNOPKG-TARGET-v1";
    long i, at = -1;
    int k = 0, cap;

    for (i = 0; i + 16 <= n; i++) {
        int j = 0;
        while (j < 16 && img[i + j] == (unsigned char)mark[j]) j++;
        if (j == 16) { at = i; break; }
    }
    if (at < 0) return 0;
    /* The template declares 320 bytes; only the tail after the marker is
     * ours to write, and it is bounded here rather than trusted. */
    cap = 320 - 16;
    if (at + 16 + cap > n) cap = (int)(n - at - 16);
    if (cap < 64) return 0;
    memset(img + at + 16, 0, (unsigned long)cap);
    while (info->target[k] && k < cap - 40) { img[at + 16 + k] = (unsigned char)info->target[k]; k++; }
    img[at + 16 + k] = 0; k++;
    { int j = 0;
      while (info->name[j] && k < cap - 1) img[at + 16 + k++] = (unsigned char)info->name[j++]; }
    img[at + 16 + k] = 0;
    return 1;
}

/* 8.3 base name from an id: "firefox" -> "FIREFOX". */
static void base83(const char *id, char *out, int max)
{
    int i = 0, o = 0;
    while (id[i] && o < 8 && o < max - 1) {
        char c = id[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[o++] = c;
    }
    out[o] = 0;
    if (!o) sput(out, max, "FOREIGN");
}

/* ---- install / remove ---------------------------------------------------- */

static int find_template(int *vol, char *path, int max)
{
    int nv = uno_fs_volumes(), v, pass;
    for (pass = 0; pass < 2; pass++) {
        char p[64];
        sput(p, (int)sizeof p, pass ? UNO_PKG_DIR_ESP : UNO_PKG_DIR_ROOT);
        scat(p, (int)sizeof p, "\\" UNO_PKG_TPL_FILE);
        for (v = pass; v < nv; v++)
            if (uno_fs_size(v, p) >= 48) {
                *vol = v; sput(path, max, p); return 1;
            }
    }
    return 0;
}

int uno_pkg_install(int vol, const char *path, const uno_pkg_info *info,
                    void (*progress)(int pct, const char *msg),
                    char *err, int errmax)
{
    int tvol, dvol;
    char tpath[64], apps[40], pkgd[40], base[16];
    char shimp[80], sidep[80];
    unsigned char *img = 0;
    long n;

    if (err && errmax) err[0] = 0;
    if (progress) progress(5, "Reading the package");

    if (!info->arch_ok) {
        sput(err, errmax, "That package has no x86_64 code.");
        return 0;
    }
    if (!find_template(&tvol, tpath, (int)sizeof tpath)) {
        sput(err, errmax, "The shim template PKG\\FSHIM.UNO is missing.");
        return 0;
    }
    dvol = uno_fs_pref_vol();
    if (dvol < 0 || !uno_fs_writable(dvol)) {
        sput(err, errmax, "No writable volume to install onto.");
        return 0;
    }
    pkg_dirs(dvol, apps, (int)sizeof apps, pkgd, (int)sizeof pkgd);
    base83(info->id, base, (int)sizeof base);

    if (progress) progress(25, "Preparing the app");
    n = uno_fs_size(tvol, tpath);
    if (n <= 0 || n > PKG_TPL_MAX) {
        sput(err, errmax, "The shim template is not a usable size.");
        return 0;
    }
    img = (unsigned char *)malloc((unsigned long)n);
    if (!img) { sput(err, errmax, "Out of memory."); return 0; }
    if (pkg_read_at(tvol, tpath, 0, img, n) != n) {
        free(img); sput(err, errmax, "Could not read the shim template.");
        return 0;
    }

    if (!shim_blob(img, n, info)) {
        free(img);
        sput(err, errmax, "The template carries no target slot (rebuild it).");
        return 0;
    }
    if (!shim_desc(img, n, info, "generic")) {
        free(img);
        sput(err, errmax, "The template's descriptor has no room.");
        return 0;
    }

    /* The sidecar first: if the shim lands and this does not, there is an app
     * icon with nothing behind it.  In the other order the worst case is an
     * orphan record, which the next install overwrites and which nothing
     * reads without a shim beside it. */
    if (progress) progress(55, "Recording the package");
    uno_fs_mkdir(dvol, pkgd);
    {
        char rec[512], num[24];
        rec[0] = 0;
        scat(rec, (int)sizeof rec, "kind: apk\nid: ");
        scat(rec, (int)sizeof rec, info->id);
        scat(rec, (int)sizeof rec, "\nname: ");
        scat(rec, (int)sizeof rec, info->name);
        scat(rec, (int)sizeof rec, "\ntarget: ");
        scat(rec, (int)sizeof rec, info->target);
        scat(rec, (int)sizeof rec, "\nversion: ");
        scat(rec, (int)sizeof rec, info->version);
        /* WHERE THE PACKAGE IS, not a copy of it - see the file header. */
        scat(rec, (int)sizeof rec, "\nsrcvol: ");
        sdec(num, (int)sizeof num, (long)vol);
        scat(rec, (int)sizeof rec, num);
        scat(rec, (int)sizeof rec, "\nsrcpath: ");
        scat(rec, (int)sizeof rec, path);
        scat(rec, (int)sizeof rec, "\nbytes: ");
        sdec(num, (int)sizeof num, info->size);
        scat(rec, (int)sizeof rec, num);
        scat(rec, (int)sizeof rec, "\n");
        pkg_join(sidep, (int)sizeof sidep, pkgd, base);
        scat(sidep, (int)sizeof sidep, ".PKG");
        { long L = 0; while (rec[L]) L++;
          if (!pkg_write(dvol, sidep, (const unsigned char *)rec, L)) {
              free(img); sput(err, errmax, "Could not write the package record.");
              return 0;
          } }
    }

    if (progress) progress(80, "Installing");
    pkg_join(shimp, (int)sizeof shimp, apps, base);
    scat(shimp, (int)sizeof shimp, ".UNO");
    if (!pkg_write(dvol, shimp, img, n)) {
        free(img);
        pkg_delete(dvol, sidep);
        sput(err, errmax, "Could not write the app.");
        return 0;
    }
    free(img);

    if (progress) progress(95, "Refreshing the desktop");
    pc64_shell_apps_rescan();
    if (progress) progress(100, "Installed");
    return 1;
}

int uno_pkg_installed(const char *id)
{
    char base[16], file[24];
    base83(id, base, (int)sizeof base);
    sput(file, (int)sizeof file, base);
    scat(file, (int)sizeof file, ".UNO");
    return uno_mod_present(file);
}

int uno_pkg_remove(const char *id)
{
    int nv = uno_fs_volumes(), v, pass, hit = 0;
    char base[16];
    base83(id, base, (int)sizeof base);

    for (pass = 0; pass < 2; pass++) {
        for (v = pass; v < nv; v++) {
            char apps[40], pkgd[40], p[80];
            if (uno_fs_kind(v) != 1 || !uno_fs_writable(v)) continue;
            pkg_dirs(v, apps, (int)sizeof apps, pkgd, (int)sizeof pkgd);
            pkg_join(p, (int)sizeof p, apps, base); scat(p, (int)sizeof p, ".UNO");
            if (uno_fs_size(v, p) > 0 && pkg_delete(v, p)) hit = 1;
            pkg_join(p, (int)sizeof p, pkgd, base); scat(p, (int)sizeof p, ".PKG");
            if (uno_fs_size(v, p) > 0) pkg_delete(v, p);
        }
    }
    if (hit) pc64_shell_apps_rescan();
    return hit;
}

/* ---- the runtime probe ---------------------------------------------------
 *
 * This is a STUB BACKEND and says so, but what it reports is not a stub: on a
 * machine with virtualization locked off in firmware, or with too little RAM
 * for the carve, or with no Android image staged, the answer here is the
 * answer forever, and it is the one the L1 version must not lose.  Each of
 * those is a different problem with a different fix, so each gets its own
 * sentence rather than being flattened into "cannot start".
 */
#define ANDROID_IMG "EFI\\UNODOS\\VM\\ANDROID.IMG"
#define ANDROID_MIN_CARVE_MB 1536u

static int android_img_present(void)
{
    int nv = uno_fs_volumes(), v;
    for (v = 1; v < nv; v++) if (uno_fs_size(v, ANDROID_IMG) > 0) return 1;
    return 0;
}

static int is_android(const char *target)
{
    return target && target[0] == 'a' && target[1] == 'n' && target[2] == 'd'
        && target[3] == 'r' && target[4] == 'o' && target[5] == 'i'
        && target[6] == 'd' && target[7] == ':';
}

void uno_pkg_runtime_str(const char *target, char *buf, int max)
{
    unsigned blockers = 0;

    if (max > 0) buf[0] = 0;
    if (!is_android(target)) {
        sput(buf, max, "No runtime is registered for this package.");
        return;
    }
    if (!uno_vmm_eligible(&blockers)) {
        sput(buf, max, "Appliances unavailable: ");
        scat(buf, max, uno_vmm_blocker_str(blockers));
        return;
    }
    if (uno_vmm_carve_mb() < ANDROID_MIN_CARVE_MB) {
        char n[24];
        sput(buf, max, "This machine's appliance memory is ");
        sdec(n, (int)sizeof n, (long)uno_vmm_carve_mb());
        scat(buf, max, n);
        scat(buf, max, " MB; Android needs 1536 MB.");
        return;
    }
    if (!android_img_present()) {
        sput(buf, max, "The Android runtime image is not installed on this machine.");
        return;
    }
    sput(buf, max, "The Android runtime is available but not yet connected.");
}

int uno_pkg_launch(const char *target, char *msg, int msgmax)
{
    if (msgmax > 0) msg[0] = 0;
    if (!target || !target[0]) {
        sput(msg, msgmax, "This app carries no launch target.");
        return 0;
    }
    /* No channel yet (plan phases P3/P4).  Reporting the runtime's real state
     * is the whole of the backend today, and deliberately: a launch that
     * silently did nothing would be indistinguishable from a guest that had
     * started and drawn nothing. */
    sput(msg, msgmax, "Not started: no runtime is connected yet.");
    return 0;
}
