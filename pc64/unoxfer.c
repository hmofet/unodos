/* ===========================================================================
 * UnoDOS/pc64 - unoxfer core: the backend registry, the site store, URL and
 * path handling, and the one staging buffer everything shares.
 *
 * Contract: UNOXFER.md.  This file holds the ONLY switch on unoxfer_proto in
 * the subsystem (open_for below).  If a second one ever appears, the seam has
 * leaked and the next protocol will cost a sweep instead of a row.
 * ======================================================================== */
#include "unoxfer_int.h"
#include "pc64_fs.h"
#include "fat.h"
#include "unoauto.h"        /* unoauto_test_register, at the foot of this file */
#include <stdarg.h>

void *malloc(unsigned long);
void  free(void *);
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
int   snprintf(char *, unsigned long, const char *, ...);
int   vsnprintf(char *, unsigned long, const char *, va_list);
void  net_poll(void);
int   net_dns_query(const char *host, unsigned char out[4]);
void  uno_pc64_delay_ms(int ms);
long  TickCount(void);

/* ---- weak links into two other lanes --------------------------------------
 * Both are REQUESTED (UNOAUTOMATE-REQUESTS.md, 2026-08-22) and neither exists
 * yet, so unoxfer must build and work without either and improve the moment
 * one lands.  That is the r8169_dbg_cmd pattern, and the fallback bodies are
 * not optional: this links as PE, where an UNDEFINED weak symbol does not
 * reliably resolve to a testable null the way it does in ELF.  So capability
 * is asked through a separate `_supported` predicate rather than by taking the
 * address of the function itself - one trivial extra symbol from the provider,
 * and no build-format folklore in the test.
 *
 * unofs: append to an existing file.  fat.c today can write a whole file and
 * cannot extend one, which is the entire reason for the per-file staging cap
 * (UNOXFER.md, "the single-file size cap").  When this lands the engine
 * streams in fixed windows and the cap disappears with no change here. */
__attribute__((weak)) int uno_fat_append(int vol, const char *path,
                                         const unsigned char *buf, long len)
{ (void)vol; (void)path; (void)buf; (void)len; return 0; }
__attribute__((weak)) int uno_fat_append_supported(void) { return 0; }

/* unossh: open a "subsystem" channel, which is all SFTP needs from the SSH
 * layer - channel_request() in unossh_auth.c already has exactly this shape,
 * so it is five lines beside ssh_exec().  Until it lands UNOXFER_SFTP reports
 * not-ready at open() and the app offers SCP, which does the same job over the
 * same connection. */
__attribute__((weak)) int ssh_subsystem(int handle, const char *name)
{ (void)handle; (void)name; return -1; }
__attribute__((weak)) int ssh_subsystem_supported(void) { return 0; }

/* ===========================================================================
 * Small string helpers.
 * ======================================================================== */
int ux_cpy(char *dst, int cap, const char *src)
{
    int i = 0;
    if (!dst || cap <= 0) return 0;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
    return src && src[i] ? 0 : 1;          /* 0 = it had to cut */
}

int ux_cat(char *dst, int cap, const char *src)
{
    int n = 0;
    if (!dst || cap <= 0) return 0;
    while (n < cap && dst[n]) n++;
    return ux_cpy(dst + n, cap - n, src);
}

int ux_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int ux_ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) == lower(*b);
}

unsigned long long ux_u64(const char *s)
{
    unsigned long long v = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') v = v * 10ull + (unsigned)(*s++ - '0');
    return v;
}

int ux_fail(unoxfer_client *c, int rc, const char *msg)
{
    if (c) ux_cpy(c->err, (int)sizeof c->err, msg);
    return rc;
}

int ux_failf(unoxfer_client *c, int rc, const char *fmt, ...)
{
    va_list ap;
    if (c) {
        va_start(ap, fmt);
        vsnprintf(c->err, sizeof c->err, fmt, ap);
        va_end(ap);
    }
    return rc;
}

/* ===========================================================================
 * Protocol identity.
 * ======================================================================== */
static const struct { const char *name; int port; } kProto[UNOXFER_PROTO_COUNT] = {
    { "none",    0   },
    { "local",   0   },
    { "scp",     22  },
    { "sftp",    22  },
    { "http",    80  },
    { "https",   443 },
    { "webdav",  80  },
    { "webdavs", 443 },
    { "tftp",    69  }
};

const char *unoxfer_proto_name(unoxfer_proto p)
{
    if (p <= UNOXFER_NONE || p >= UNOXFER_PROTO_COUNT) return "none";
    return kProto[p].name;
}

int unoxfer_proto_port(unoxfer_proto p)
{
    if (p <= UNOXFER_NONE || p >= UNOXFER_PROTO_COUNT) return 0;
    return kProto[p].port;
}

unoxfer_proto unoxfer_proto_parse(const char *s)
{
    int i;
    if (!s || !*s) return UNOXFER_NONE;
    for (i = 1; i < UNOXFER_PROTO_COUNT; i++)
        if (ux_ieq(s, kProto[i].name)) return (unoxfer_proto)i;
    /* the aliases people actually type */
    if (ux_ieq(s, "ssh"))  return UNOXFER_SCP;
    if (ux_ieq(s, "dav"))  return UNOXFER_WEBDAV;
    if (ux_ieq(s, "davs")) return UNOXFER_WEBDAVS;
    return UNOXFER_NONE;
}

int unoxfer_proto_terminal(unoxfer_proto p)
{
    return p == UNOXFER_SCP || p == UNOXFER_SFTP || p == UNOXFER_LOCAL;
}

/* ===========================================================================
 * The registry.  THE one switch on the protocol enum.
 * ======================================================================== */
static const unoxfer_backend *backend_for(unoxfer_proto p)
{
    switch (p) {
    case UNOXFER_LOCAL:   return &unoxfer_be_local;
    case UNOXFER_SCP:     return &unoxfer_be_scp;
    case UNOXFER_SFTP:    return &unoxfer_be_sftp;
    case UNOXFER_HTTP:
    case UNOXFER_HTTPS:   return &unoxfer_be_http;
    case UNOXFER_WEBDAV:
    case UNOXFER_WEBDAVS: return &unoxfer_be_webdav;
    case UNOXFER_TFTP:    return &unoxfer_be_tftp;
    default:              return 0;
    }
}

/* SFTP is the interesting case: its backend IS linked and it still is not
 * usable, because unossh has no ssh_subsystem() yet.  Saying so here means the
 * app greys the option out and the verb refuses with a reason, instead of both
 * discovering it at connect time and blaming the server. */
int unoxfer_proto_ready(unoxfer_proto p)
{
    if (!backend_for(p)) return 0;
    if (p == UNOXFER_SFTP) return ssh_subsystem_supported();
    return 1;
}

const char *unoxfer_strerror(int rc)
{
    switch (rc) {
    case UNOXFER_OK:       return "ok";
    case UNOXFER_EIO:      return "transport failed";
    case UNOXFER_EAUTH:    return "authentication refused";
    case UNOXFER_ENOENT:   return "no such file or directory";
    case UNOXFER_EPERM:    return "refused by the server";
    case UNOXFER_EUNSUP:   return "not supported by this protocol";
    case UNOXFER_ENOSPC:   return "out of space";
    case UNOXFER_ETOOBIG:  return "file is larger than the staging buffer";
    case UNOXFER_ECANCEL:  return "cancelled";
    case UNOXFER_EARG:     return "bad argument";
    case UNOXFER_EHOSTKEY: return "HOST KEY MISMATCH";
    default:               return "failed";
    }
}

/* ===========================================================================
 * Clients.
 * ======================================================================== */
unoxfer_client *unoxfer_open(const unoxfer_site *site, char *err, int errcap)
{
    const unoxfer_backend *b;
    unoxfer_client *c;
    int rc;

    if (!site) { ux_cpy(err, errcap, "no site"); return 0; }
    b = backend_for(site->proto);
    if (!b) {
        snprintf(err, (unsigned long)errcap, "no backend for %s",
                 unoxfer_proto_name(site->proto));
        return 0;
    }
    if (!unoxfer_proto_ready(site->proto)) {
        snprintf(err, (unsigned long)errcap,
                 "%s is not available in this build (see UNOXFER.md)",
                 unoxfer_proto_name(site->proto));
        return 0;
    }
    c = (unoxfer_client *)malloc(sizeof *c);
    if (!c) { ux_cpy(err, errcap, "out of memory"); return 0; }
    memset(c, 0, sizeof *c);
    c->b = b;
    c->proto = site->proto;
    c->caps = b->caps;
    c->site = *site;
    ux_cpy(c->err, (int)sizeof c->err, "ok");

    rc = b->open ? b->open(c, site) : UNOXFER_EUNSUP;
    if (rc != UNOXFER_OK) {
        ux_cpy(err, errcap, c->err[0] ? c->err : unoxfer_strerror(rc));
        free(c);
        return 0;
    }
    ux_cpy(err, errcap, "ok");
    return c;
}

void unoxfer_close(unoxfer_client *c)
{
    if (!c) return;
    if (c->b && c->b->close) c->b->close(c);
    free(c);
}

unsigned      unoxfer_caps(unoxfer_client *c)        { return c ? c->caps : 0; }
const char   *unoxfer_error(unoxfer_client *c)       { return c ? c->err : "no client"; }
unoxfer_proto unoxfer_client_proto(unoxfer_client *c){ return c ? c->proto : UNOXFER_NONE; }

int unoxfer_list(unoxfer_client *c, const char *path,
                 unoxfer_ent *out, int max, int *total)
{
    if (total) *total = 0;
    if (!c) return UNOXFER_EARG;
    if (!(c->caps & UNOXFER_CAP_LIST) || !c->b->list)
        return ux_failf(c, UNOXFER_EUNSUP, "%s cannot list directories",
                        unoxfer_proto_name(c->proto));
    return c->b->list(c, path, out, max, total);
}

long long unoxfer_size(unoxfer_client *c, const char *rpath)
{
    if (!c) return UNOXFER_EARG;
    if (!(c->caps & UNOXFER_CAP_SIZE) || !c->b->size) return UNOXFER_EUNSUP;
    return c->b->size(c, rpath);
}

int unoxfer_get(unoxfer_client *c, const char *rpath,
                int vol, const char *lpath, unoxfer_prog *p)
{
    if (!c) return UNOXFER_EARG;
    if (!(c->caps & UNOXFER_CAP_GET) || !c->b->get)
        return ux_fail(c, UNOXFER_EUNSUP, "this protocol cannot download");
    return c->b->get(c, rpath, 0, vol, lpath, p);
}

int unoxfer_put(unoxfer_client *c, int vol, const char *lpath,
                const char *rpath, unoxfer_prog *p)
{
    if (!c) return UNOXFER_EARG;
    if (!(c->caps & UNOXFER_CAP_PUT) || !c->b->put)
        return ux_fail(c, UNOXFER_EUNSUP, "this protocol cannot upload");
    return c->b->put(c, vol, lpath, rpath, p);
}

int unoxfer_mkdir(unoxfer_client *c, const char *path)
{
    if (!c) return UNOXFER_EARG;
    if (!(c->caps & UNOXFER_CAP_MKDIR) || !c->b->mkdir)
        return ux_fail(c, UNOXFER_EUNSUP, "this protocol cannot create directories");
    return c->b->mkdir(c, path);
}

int unoxfer_del(unoxfer_client *c, const char *path)
{
    if (!c) return UNOXFER_EARG;
    if (!(c->caps & UNOXFER_CAP_DELETE) || !c->b->del)
        return ux_fail(c, UNOXFER_EUNSUP, "this protocol cannot delete");
    return c->b->del(c, path);
}

/* ===========================================================================
 * The staging buffer.
 *
 * ONE per machine.  The heap is 32 MB and shared with Studio's compile arena,
 * the browser and the Python VM, so two jobs each grabbing "16 MB, surely
 * that's fine" is how a file transfer takes the desktop down with it.  It is
 * allocated on first use and freed on last release: a box that never transfers
 * anything pays nothing for the capability.
 *
 * The default cap is deliberately modest and the allocator HALVES on failure
 * rather than giving up - a machine under memory pressure should transfer
 * slowly in small files, not refuse.
 * ======================================================================== */
#define UX_STAGE_DEFAULT (8ll * 1024 * 1024)
#define UX_STAGE_MIN     (64ll * 1024)

static unsigned char *g_stage;
static long long      g_stage_len;
static long long      g_stage_cap = UX_STAGE_DEFAULT;
static int            g_stage_held;

long long unoxfer_stage_cap(void) { return g_stage_cap; }

void unoxfer_stage_set_cap(long long bytes)
{
    if (bytes < UX_STAGE_MIN) bytes = UX_STAGE_MIN;
    g_stage_cap = bytes;
}

int unoxfer_streaming(void) { return uno_fat_append_supported(); }

unsigned char *ux_stage_get(long long want, long long *got)
{
    long long n = want > 0 && want < g_stage_cap ? want : g_stage_cap;
    if (g_stage_held) { if (got) *got = 0; return 0; }
    if (g_stage && g_stage_len >= n) {
        g_stage_held = 1;
        if (got) *got = g_stage_len;
        return g_stage;
    }
    if (g_stage) { free(g_stage); g_stage = 0; g_stage_len = 0; }
    while (n >= UX_STAGE_MIN) {
        g_stage = (unsigned char *)malloc((unsigned long)n);
        if (g_stage) { g_stage_len = n; break; }
        n /= 2;
    }
    if (!g_stage) { if (got) *got = 0; return 0; }
    g_stage_held = 1;
    if (got) *got = g_stage_len;
    return g_stage;
}

void ux_stage_put(void)
{
    g_stage_held = 0;
    if (g_stage) { free(g_stage); g_stage = 0; g_stage_len = 0; }
}

/* ===========================================================================
 * The .PART commit.
 *
 * A file lands as <name>.PART and is renamed onto its real name only once the
 * last byte is there.  The rename IS the commit: a killed transfer leaves a
 * visibly incomplete file, never a truncated-looking real one, and a resume
 * measures the .PART rather than guessing.
 *
 * uno_fat_rename() takes a NEW LEAF NAME, not a new path, which is exactly
 * what is wanted here (the file does not move, it is only renamed in place)
 * and exactly the sort of signature that gets called wrong once.
 * ======================================================================== */
/* THE PARTIAL NAME HAS TO BE A LEGAL 8.3 NAME.
 *
 * The obvious spelling - append ".PART" - produces "HTTP1.TXT.PART": two dots
 * and a four-character extension, which FAT will not accept.  Every write of
 * every partial then failed, and the error surfaced as "write failed" against
 * the FINAL path, which is not where the bad name was.  Cost a debugging round;
 * this comment is the receipt.
 *
 * So: keep the directory and the base, replace the extension with "$$$" - the
 * DOS convention for a work file, and legal.  Two transfers into one directory
 * whose leaf names share a base would collide, which is survivable because the
 * engine commits one file at a time and the collision window is one file. */
int ux_partname(char *dst, int cap, const char *final)
{
    const char *leaf = unoxfer_basename(final);
    int dirlen = (int)(leaf - final), n = 0, i;

    if (!dst || cap < 16 || !final) return UNOXFER_EARG;
    if (dirlen >= cap - 12) return UNOXFER_EARG;
    for (i = 0; i < dirlen; i++) dst[n++] = final[i];
    for (i = 0; leaf[i] && leaf[i] != '.' && i < 8; i++) dst[n++] = leaf[i];
    if (n == dirlen) dst[n++] = 'X';          /* a dotfile has no base at all */
    dst[n++] = '.'; dst[n++] = '$'; dst[n++] = '$'; dst[n++] = '$';
    dst[n] = 0;
    return UNOXFER_OK;
}

int ux_append_part(int vol, const char *part, const unsigned char *buf, long len)
{
    if (!unoxfer_streaming()) return UNOXFER_EUNSUP;
    return uno_fat_append(vol, part, buf, len) ? UNOXFER_OK : UNOXFER_EIO;
}

int ux_commit_file(int vol, const char *final, const unsigned char *buf, long len)
{
    char part[UNOXFER_PATHLEN + 16];
    const char *leaf;

    if (ux_partname(part, (int)sizeof part, final) != UNOXFER_OK)
        return UNOXFER_EARG;
    if (!uno_fs_writable(vol)) return UNOXFER_EPERM;
    if (buf && !uno_fs_write(vol, part, buf, len)) return UNOXFER_ENOSPC;

    /* Rename onto the real name.  A native-FAT volume has uno_fat_rename; the
     * RAM disk and firmware SFS do not, so on those the .PART dance cannot
     * commit and the file is written straight to its final name.  That is a
     * weaker guarantee, stated rather than hidden: those volumes are the ones
     * that do not survive a power cycle anyway. */
    if (uno_fs_kind(vol) == 1) {
        int fv = uno_fs_fat_index(vol);
        leaf = unoxfer_basename(final);
        if (fv >= 0) {
            uno_fat_delete(fv, final);            /* an old copy would block it */
            if (uno_fat_rename(fv, part, leaf)) return UNOXFER_OK;
            return UNOXFER_EIO;
        }
    }
    if (buf && !uno_fs_write(vol, final, buf, len)) return UNOXFER_ENOSPC;
    return UNOXFER_OK;
}

/* ===========================================================================
 * Paths.
 *
 * Two joins, not one.  A remote path is '/'-separated and case-sensitive; a
 * FAT path is '\'-separated, upper-cased and 8.3-constrained.  Every transfer
 * bug that ever ate a directory came from one function trying to be both.
 * ======================================================================== */
static int join(char *dst, int cap, const char *dir, const char *leaf, char sep)
{
    int n = 0;
    if (!dst || cap <= 1) return 0;
    dst[0] = 0;
    if (dir && *dir) {
        if (!ux_cpy(dst, cap, dir)) return 0;
        n = (int)strlen(dst);
        while (n > 1 && (dst[n - 1] == '/' || dst[n - 1] == '\\')) dst[--n] = 0;
    }
    if (!leaf || !*leaf) return 1;
    if (n == 0 || (n == 1 && (dst[0] == '/' || dst[0] == '\\'))) {
        if (n == 0) { dst[n++] = sep; dst[n] = 0; }
    } else {
        if (n >= cap - 1) return 0;
        dst[n++] = sep; dst[n] = 0;
    }
    while (*leaf == '/' || *leaf == '\\') leaf++;
    return ux_cat(dst, cap, leaf);
}

int unoxfer_rjoin(char *dst, int cap, const char *dir, const char *leaf)
{ return join(dst, cap, dir, leaf, '/'); }

int unoxfer_ljoin(char *dst, int cap, const char *dir, const char *leaf)
{ return join(dst, cap, dir, leaf, '\\'); }

const char *unoxfer_basename(const char *path)
{
    const char *p, *last = path;
    if (!path) return "";
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') last = p + 1;
    return last;
}

/* Map a remote leaf onto something FAT will take.  Returns 1 when it had to
 * change something, so a caller can REPORT the rename: silently landing
 * "my long report.txt" as "MYLONG~1.TXT" and saying nothing is how a sync
 * appears to have worked and did not. */
int unoxfer_fatname(char *dst, int cap, const char *leaf)
{
    static const char *bad = "\"*+,:;<=>?[]|\\/ ";
    char base[16], ext[8];
    int nb = 0, ne = 0, changed = 0, i;
    const char *dot = 0, *p;

    if (!dst || cap < 13 || !leaf) return 0;
    for (p = leaf; *p; p++) if (*p == '.') dot = p;

    for (p = leaf; *p && (!dot || p < dot); p++) {
        int ch = (unsigned char)*p;
        const char *q;
        if (ch < 32 || ch > 126) { changed = 1; continue; }
        for (q = bad; *q; q++) if (ch == *q) break;
        if (*q) { ch = '_'; changed = 1; }
        if (ch >= 'a' && ch <= 'z') { ch -= 32; changed = 1; }
        if (nb < 8) base[nb++] = (char)ch; else changed = 1;
    }
    if (dot) for (p = dot + 1; *p; p++) {
        int ch = (unsigned char)*p;
        const char *q;
        if (ch < 32 || ch > 126) { changed = 1; continue; }
        for (q = bad; *q; q++) if (ch == *q) break;
        if (*q) { ch = '_'; changed = 1; }
        if (ch >= 'a' && ch <= 'z') { ch -= 32; changed = 1; }
        if (ne < 3) ext[ne++] = (char)ch; else changed = 1;
    }
    if (nb == 0) { base[nb++] = 'X'; changed = 1; }
    for (i = 0; i < nb; i++) dst[i] = base[i];
    if (ne) { dst[nb++] = '.'; for (i = 0; i < ne; i++) dst[nb + i] = ext[i]; nb += ne; }
    dst[nb] = 0;
    return changed;
}

/* ===========================================================================
 * URLs.
 *
 * "sftp://user@host:2222/srv/media" -> a site.  A URL carrying a PASSWORD
 * ("user:pw@host") is REFUSED rather than parsed: accepting one means either
 * storing it or pretending to, and the store deliberately has nowhere to put
 * it.  Refusing is the only answer that does not lie.
 * ======================================================================== */
int unoxfer_parse_url(const char *url, unoxfer_site *out)
{
    char scheme[16];
    const char *s = url, *at, *colon, *slash, *p;
    int n = 0;

    if (!url || !out) return UNOXFER_EARG;
    memset(out, 0, sizeof *out);

    /* scheme */
    for (p = s; *p && *p != ':' && n < (int)sizeof scheme - 1; p++) scheme[n++] = *p;
    scheme[n] = 0;
    if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
        out->proto = unoxfer_proto_parse(scheme);
        if (out->proto == UNOXFER_NONE) return UNOXFER_EARG;
        s = p + 3;
    } else {
        return UNOXFER_EARG;                 /* a bare host is a site, not a URL */
    }

    /* authority ends at the first '/' */
    slash = s;
    while (*slash && *slash != '/') slash++;

    /* user[:pw]@ */
    at = 0;
    for (p = s; p < slash; p++) if (*p == '@') at = p;
    if (at) {
        for (p = s; p < at; p++)
            if (*p == ':') return UNOXFER_EPERM;   /* a password in a URL: no */
        n = (int)(at - s);
        if (n >= UNOXFER_NAMELEN) n = UNOXFER_NAMELEN - 1;
        memcpy(out->user, s, (unsigned long)n);
        out->user[n] = 0;
        s = at + 1;
    }

    /* host[:port] - the LAST colon, so a bracketed v6 literal keeps its own */
    colon = 0;
    for (p = s; p < slash; p++) if (*p == ':') colon = p;
    n = (int)((colon ? colon : slash) - s);
    if (n <= 0) return UNOXFER_EARG;
    if (n >= UNOXFER_HOSTLEN) n = UNOXFER_HOSTLEN - 1;
    memcpy(out->host, s, (unsigned long)n);
    out->host[n] = 0;
    if (colon) out->port = (int)ux_u64(colon + 1);
    if (!out->port) out->port = unoxfer_proto_port(out->proto);

    ux_cpy(out->root, (int)sizeof out->root, *slash ? slash : "/");
    ux_cpy(out->name, (int)sizeof out->name, out->host);
    return UNOXFER_OK;
}

/* ===========================================================================
 * Address resolution.
 *
 * net_dns_query() sends a DNS QUERY, including for "10.0.2.2" - which no
 * resolver will answer and which every harness, every LAN NAS and every
 * router-shaped target is named by.  pc64_http's own note says it "accepts an
 * IP literal", so the check belongs on this side of the call too, once, rather
 * than in each backend.
 *
 * The parse is strict about the shape (four decimal octets, nothing else),
 * because a lenient one is how "1.2.3" quietly becomes 1.2.0.3 - which is a
 * REACHABLE address, so the failure is a connection to the wrong machine
 * rather than an error.
 * ======================================================================== */
int ux_resolve(const char *host, unsigned char ip[4])
{
    const char *p = host;
    int part = 0, v, digits;

    if (!host || !ip) return 0;
    for (part = 0; part < 4; part++) {
        v = 0; digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) { v = v * 10 + (*p++ - '0'); digits++; }
        if (!digits || v > 255) break;
        ip[part] = (unsigned char)v;
        if (part < 3) { if (*p != '.') break; p++; }
        else if (*p == 0) return 1;              /* a complete dotted quad    */
        else break;
    }
    return net_dns_query(host, ip);
}

/* ===========================================================================
 * The site store.
 *
 * A flat, versioned record file.  It goes on uno_fs_pref_vol(), which is the
 * one place in the tree that knows "the boot volume first, then a native FAT
 * one, and the RAM disk only as a last resort" - the heuristic three
 * subsystems each got wrong separately (see pc64_fs.h).  Using it rather than
 * re-deriving it is the entire point of its existing.
 * ======================================================================== */
#define UX_STORE_MAGIC 0x5846524cu          /* 'XFRL' */
#define UX_STORE_VER   1
#define UX_STORE_FILE  "XFERSITE.BIN"

typedef struct {
    unsigned magic, ver, count, reserved;
    unoxfer_site site[UNOXFER_MAXSITE];
} ux_store;

static ux_store g_store;
static int      g_store_loaded;

static int store_vol(void) { return uno_fs_pref_vol(); }

int unoxfer_store_persistent(void)
{
    int v = store_vol();
    return uno_fs_kind(v) == 1 ? 1 : 0;     /* 0 = the RAM disk, i.e. not     */
}

static void store_load(void)
{
    long n;
    if (g_store_loaded) return;
    g_store_loaded = 1;
    memset(&g_store, 0, sizeof g_store);
    g_store.magic = UX_STORE_MAGIC;
    g_store.ver = UX_STORE_VER;
    n = uno_fs_read(store_vol(), UX_STORE_FILE,
                    (unsigned char *)&g_store, (long)sizeof g_store);
    if (n < (long)(4 * sizeof(unsigned)) ||
        g_store.magic != UX_STORE_MAGIC || g_store.ver != UX_STORE_VER) {
        memset(&g_store, 0, sizeof g_store);
        g_store.magic = UX_STORE_MAGIC;
        g_store.ver = UX_STORE_VER;
    }
    if (g_store.count > UNOXFER_MAXSITE) g_store.count = UNOXFER_MAXSITE;
}

static int store_save(void)
{
    return uno_fs_write(store_vol(), UX_STORE_FILE,
                        (const unsigned char *)&g_store, (long)sizeof g_store)
           ? UNOXFER_OK : UNOXFER_EIO;
}

int unoxfer_site_set(const unoxfer_site *s)
{
    unsigned i;
    if (!s || !s->name[0]) return UNOXFER_EARG;
    store_load();
    for (i = 0; i < g_store.count; i++)
        if (ux_eq(g_store.site[i].name, s->name)) { g_store.site[i] = *s; return store_save(); }
    if (g_store.count >= UNOXFER_MAXSITE) return UNOXFER_ENOSPC;
    g_store.site[g_store.count++] = *s;
    return store_save();
}

int unoxfer_site_get(const char *name, unoxfer_site *out)
{
    unsigned i;
    if (!name || !out) return UNOXFER_EARG;
    store_load();
    for (i = 0; i < g_store.count; i++)
        if (ux_eq(g_store.site[i].name, name)) { *out = g_store.site[i]; return UNOXFER_OK; }
    return UNOXFER_ENOENT;
}

int unoxfer_site_list(int idx, char *name, int cap)
{
    store_load();
    if (idx < 0 || (unsigned)idx >= g_store.count) return -1;
    ux_cpy(name, cap, g_store.site[idx].name);
    return (int)g_store.count;
}

int unoxfer_site_delete(const char *name)
{
    unsigned i;
    store_load();
    for (i = 0; i < g_store.count; i++)
        if (ux_eq(g_store.site[i].name, name)) {
            for (; i + 1 < g_store.count; i++) g_store.site[i] = g_store.site[i + 1];
            g_store.count--;
            memset(&g_store.site[g_store.count], 0, sizeof g_store.site[0]);
            return store_save();
        }
    return UNOXFER_ENOENT;
}

/* ===========================================================================
 * The wait loop every backend needs, written once.
 *
 * Getting "pump the NIC while you wait" wrong is how a transfer stalls forever
 * on a link that is working perfectly: the bytes are sitting in the driver's
 * ring and nobody called net_poll().  So there is one of these, and backends
 * call it rather than writing their own.
 * ======================================================================== */
/* PACE THE POLL.  net.c derives part of its notion of time from how often
 * net_poll() is called (`g_ticks * 5u` when there is no TSC base, and the DHCP
 * and TCP retransmit stages are commented as "net_poll is called at ~5 ms"), so
 * a TIGHT spin does not merely waste cycles - it runs the stack's timers
 * hundreds of times too fast and retransmits a SYN into a NIC that cannot drain
 * it.  The symptom is a socket stuck in SYN_SENT against a peer that is plainly
 * reachable, which is exactly how this was found: the same address and port
 * that `nst` connects to in two seconds would not connect here in thirty.
 *
 * So: pump, and when NOTHING happened, yield ~2 ms before pumping again.  The
 * `idle` argument is what keeps a bulk read at full speed - a loop that is
 * moving bytes never sleeps. */
void ux_pump(int idle)
{
    net_poll();
    if (idle) uno_pc64_delay_ms(2);
}

int ux_wait(int (*ready)(void *), void *ctx, int ms)
{
    long t0 = TickCount(), ticks = (long)ms * 60 / 1000;
    if (ticks < 1) ticks = 1;
    for (;;) {
        ux_pump(1);
        if (ready && ready(ctx)) return 1;
        if (TickCount() - t0 > ticks) return 0;
    }
}

/* ===========================================================================
 * SPECTEST registration.
 *
 * The live transport gates go in the `network` area, beside unossh's, and only
 * in a debug build - unoauto_test_register is weak-stubbed away in production,
 * so this is a call that costs a shipped image nothing.  The host-side tests
 * (tools/xfer_test.c) cover the parsing and framing that need no device; what
 * belongs HERE is only what needs a real server on the other end.
 * ======================================================================== */
/* Prove the seam is wired end to end without needing a server: open a LOCAL
 * client on the preferred volume, list it, and check the capability bits agree
 * with what the volume can actually do.  It is a small test and it catches the
 * failure that matters - a backend table row that does not line up with its
 * vtable, which is silent until the day somebody uses that protocol. */
static int ux_t_local(void *ctx)
{
    static unoxfer_ent ents[8];
    unoxfer_site s;
    unoxfer_client *c;
    char err[160];
    int n, total = 0, ok = 1;

    (void)ctx;
    memset(&s, 0, sizeof s);
    s.proto = UNOXFER_LOCAL;
    s.vol = uno_fs_pref_vol();
    ux_cpy(s.name, (int)sizeof s.name, "selftest");

    c = unoxfer_open(&s, err, (int)sizeof err);
    if (!c) return 1;
    n = unoxfer_list(c, "", ents, 8, &total);
    if (n < 0) ok = 0;
    if (!(unoxfer_caps(c) & UNOXFER_CAP_GET)) ok = 0;
    /* A read-only volume must NOT advertise put: that is the exact class of
     * bug the capability bits exist to prevent. */
    if (!uno_fs_writable(s.vol) && (unoxfer_caps(c) & UNOXFER_CAP_PUT)) ok = 0;
    unoxfer_close(c);
    return ok ? 0 : 1;
}

/* Parsing has no device and no network in it, so it is checked here too rather
 * than only on the host: a URL that carries a password must be REFUSED, and
 * that is a security property, not a convenience. */
static int ux_t_url(void *ctx)
{
    unoxfer_site s;
    (void)ctx;
    if (unoxfer_parse_url("scp://arin@nas.lan:2222/srv/media", &s) != UNOXFER_OK) return 1;
    if (s.proto != UNOXFER_SCP || s.port != 2222) return 1;
    if (!ux_eq(s.host, "nas.lan") || !ux_eq(s.user, "arin")) return 1;
    if (!ux_eq(s.root, "/srv/media")) return 1;
    if (unoxfer_parse_url("scp://arin:hunter2@nas.lan/x", &s) != UNOXFER_EPERM) return 1;
    if (unoxfer_parse_url("nas.lan/x", &s) == UNOXFER_OK) return 1;
    return 0;
}

void unoxfer_register_tests(void)
{
    unoauto_test_register("network", "xfer:local", ux_t_local);
    unoauto_test_register("network", "xfer:url",   ux_t_url);
}
