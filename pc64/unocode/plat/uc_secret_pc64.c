/*
 * VENDORED FILE - DO NOT EDIT HERE.
 *
 * UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
 * An edit made here is lost at the next sync, and until then it silently
 * forks the editor away from the tree the desktop builds are cut from.
 *
 * Change it there; bring it back with pc64/tools/sync_unocode.py.
 * See pc64/UNOCODE-UPSTREAM.md.
 */
/* ===========================================================================
 * uc_secret_pc64.c - uc_secret.h answered by UnoDOS (UCD-48).
 *
 * Compiled into UNOCODE.UNO by pc64's build.sh and by nothing else, for the
 * same reason as uc_net_pc64.c beside it: the desktop build globs core/uc_*.c
 * and must not pick a platform file up.
 *
 * The store is UNOCODE\SECRETS.TXT on the preferred volume - A PLAIN FILE ON
 * FAT, WHICH PROTECTS NOTHING.  FAT has no owners and no permissions, so
 * anything cleverer here would be theatre: an "encrypted" file whose key must
 * also live on the same unprotected disk is plaintext with extra steps.  The
 * honest move is the one Studio's AI.CFG made - say so - and this seam says it
 * through uc_secret_plaintext(), which the UI turns into a warning instead of
 * a reassurance.
 *
 * Values are hex-encoded on the line (`name SP hex NL`) so a value holding a
 * space or a newline cannot corrupt the record it sits in - the same format
 * the desktop's file-backed platforms use.
 * ======================================================================== */
#include "uc_secret.h"

/* ---- the kernel's side (module-loader exports, as uc_net_pc64.c does) ----- */
int   uno_fs_pref_vol(void);
int   uno_fs_writable(int vol);
int   uno_fs_isdir(int vol, const char *path);
int   uno_fs_mkdir(int vol, const char *path);
long  uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
int   uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);

unsigned long strlen(const char *s);
int   strncmp(const char *a, const char *b, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);

#define SEC_PATH  "UNOCODE\\SECRETS.TXT"
#define SEC_DIR   "UNOCODE"
#define SEC_CAP   8192            /* the whole store; ~7 keys of full length */

static char g_file[SEC_CAP];      /* the file as loaded                      */
static char g_out[SEC_CAP];       /* the file as rewritten                   */

static int vol_ready(void)
{
    int vol = uno_fs_pref_vol();
    if (vol < 0 || !uno_fs_writable(vol)) return -1;
    if (!uno_fs_isdir(vol, SEC_DIR)) uno_fs_mkdir(vol, SEC_DIR);
    return vol;
}

static long load(int vol)
{
    long n = uno_fs_read(vol, SEC_PATH, (unsigned char *)g_file, SEC_CAP - 1);
    if (n < 0) n = 0;
    g_file[n] = 0;
    return n;
}

/* Find `name`'s line in g_file.  Returns the offset of the hex, or -1; the
 * line's [start,end) go to *ls / *le for the rewriter. */
static int find_line(const char *name, long n, int *ls, int *le)
{
    unsigned long nl = strlen(name);
    int i = 0;
    while (i < (int)n) {
        int e = i;
        while (e < (int)n && g_file[e] != '\n') e++;
        if (!strncmp(g_file + i, name, nl) && g_file[i + nl] == ' ') {
            *ls = i;
            *le = (e < (int)n) ? e + 1 : e;
            return i + (int)nl + 1;
        }
        i = e + 1;
    }
    return -1;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int uc_secret_set(const char *name, const char *value)
{
    static const char d[] = "0123456789abcdef";
    int vol = vol_ready(), ls, le, at, o = 0, i;
    long n;
    unsigned long nl, vl;
    if (vol < 0 || !name || !name[0] || !value) return 0;
    nl = strlen(name);
    vl = strlen(value);
    if (nl > 64 || vl >= UC_SECRET_MAX) return 0;
    n = load(vol);
    at = find_line(name, n, &ls, &le);
    /* keep everything except the old line */
    if (at >= 0) {
        memcpy(g_out, g_file, (unsigned long)ls);
        o = ls;
        memcpy(g_out + o, g_file + le, (unsigned long)(n - le));
        o += (int)(n - le);
    } else {
        memcpy(g_out, g_file, (unsigned long)n);
        o = (int)n;
    }
    if (o + (int)nl + 1 + (int)vl * 2 + 2 > SEC_CAP) return 0;
    memcpy(g_out + o, name, nl);
    o += (int)nl;
    g_out[o++] = ' ';
    for (i = 0; i < (int)vl; i++) {
        g_out[o++] = d[(unsigned char)value[i] >> 4];
        g_out[o++] = d[(unsigned char)value[i] & 15];
    }
    g_out[o++] = '\n';
    return uno_fs_write(vol, SEC_PATH, (unsigned char *)g_out, o) >= 0;
}

int uc_secret_get(const char *name, char *out, int cap)
{
    int vol = uno_fs_pref_vol(), ls, le, at, o = 0;
    long n;
    if (cap > 0) out[0] = 0;
    if (vol < 0 || !name || cap <= 0) return 0;
    n = load(vol);
    at = find_line(name, n, &ls, &le);
    if (at < 0) return 0;
    while (at + 1 < (int)n && o < cap - 1) {
        int hi = hexv(g_file[at]), lo = hexv(g_file[at + 1]);
        if (hi < 0 || lo < 0) break;
        out[o++] = (char)(hi << 4 | lo);
        at += 2;
    }
    out[o] = 0;
    return 1;
}

int uc_secret_del(const char *name)
{
    int vol = vol_ready(), ls, le, at;
    long n;
    if (vol < 0 || !name) return 0;
    n = load(vol);
    at = find_line(name, n, &ls, &le);
    if (at < 0) return 1;
    memcpy(g_out, g_file, (unsigned long)ls);
    memcpy(g_out + ls, g_file + le, (unsigned long)(n - le));
    return uno_fs_write(vol, SEC_PATH, (unsigned char *)g_out,
                        ls + (n - le)) >= 0;
}

const char *uc_secret_store_name(void)
{ return "SECRETS.TXT on this volume (plain text)"; }

int uc_secret_plaintext(void) { return 1; }
