/* ===========================================================================
 * uodesk_fs.c - pc64's uno_fs_* surface over the host's folders.
 *
 * The apps address files the way pc64_fs.h does: a VOLUME number and a name
 * in that volume's root.  uofile's dialog is built on exactly that (a Look-in
 * combo of volumes, a flat list of names), so the desktop mapping is a list
 * of folders - Documents, Desktop and the home folder by default, or whatever
 * --dir named - each one a volume.
 *
 * What the listing shows is narrowed to what the suite can open.  uofile
 * holds 64 names of up to 31 characters and cannot descend into a folder, so
 * a real Desktop listed raw would push every document off the end behind
 * shortcuts and screenshots, and a long name would be truncated into one that
 * no longer opens.  Office documents (and extensionless files, see
 * office_name) only, names that fit, sorted.
 *
 * One more name is served by every volume: the bundled fonts.  pc64_font.c
 * looks its faces up on the volumes (the ESP, on pc64); here they live in a
 * fonts/ folder beside the executable (Contents/Resources on a Mac), and a
 * user's own SANS.TTF in Documents should not replace the UI font.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "uodesk.h"

#define MAXVOL   8
#define MAXLIST  256
#define NAMECAP  32          /* uofile.c's NAMELEN: a name must fit, NUL too */
#define PATHCAP  1024

typedef struct { char path[PATHCAP]; char label[32]; } vol_t;
static vol_t g_vol[MAXVOL];
static int   g_nvol;
static char  g_fontdir[PATHCAP];

static char  g_list[MAXLIST][NAMECAP];
static int   g_nlist, g_listvol = -1;

/* ---- paths ----------------------------------------------------------------- */
static void s_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }

static int join(char *out, int cap, const char *dir, const char *name)
{
    int n = (int)strlen(dir), i, k;
    if (n + (int)strlen(name) + 2 > cap) return 0;
    memcpy(out, dir, (size_t)n);
    if (n && out[n - 1] != '/' && out[n - 1] != '\\') out[n++] = '/';
    for (i = 0, k = n; name[i]; i++, k++)
        out[k] = name[i] == '\\' ? '/' : name[i];   /* pc64 names use '\' */
    out[k] = 0;
    return 1;
}

#ifdef _WIN32
static int widen(const char *s, wchar_t *w, int cap)
{ return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) > 0; }
static FILE *u8open(const char *path, const char *mode)
{
    wchar_t wp[PATHCAP], wm[8];
    if (!widen(path, wp, PATHCAP) || !widen(mode, wm, 8)) return 0;
    return _wfopen(wp, wm);
}
static int is_dir_path(const char *path)
{
    wchar_t wp[PATHCAP];
    DWORD a;
    if (!widen(path, wp, PATHCAP)) return 0;
    a = GetFileAttributesW(wp);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int replace_file(const char *from, const char *to)
{
    wchar_t wf[PATHCAP], wt[PATHCAP];
    if (!widen(from, wf, PATHCAP) || !widen(to, wt, PATHCAP)) return 0;
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) != 0;
}
static void remove_file(const char *p)
{ wchar_t w[PATHCAP]; if (widen(p, w, PATHCAP)) DeleteFileW(w); }
#else
static FILE *u8open(const char *path, const char *mode) { return fopen(path, mode); }
static int is_dir_path(const char *path)
{ struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
static int replace_file(const char *from, const char *to) { return rename(from, to) == 0; }
static void remove_file(const char *p) { remove(p); }
#endif

void uodesk_fs_add_dir(const char *path, const char *label)
{
    vol_t *v;
    const char *base;
    int i, n;
    if (!path || !*path || g_nvol >= MAXVOL || !is_dir_path(path)) return;
    for (i = 0; i < g_nvol; i++) if (!strcmp(g_vol[i].path, path)) return;
    v = &g_vol[g_nvol++];
    s_cpy(v->path, path, PATHCAP);
    n = (int)strlen(v->path);             /* no trailing separator */
    while (n > 1 && (v->path[n - 1] == '/' || v->path[n - 1] == '\\')) v->path[--n] = 0;
    if (label) { s_cpy(v->label, label, sizeof v->label); return; }
    base = v->path;
    for (i = 0; v->path[i]; i++)
        if (v->path[i] == '/' || v->path[i] == '\\') base = v->path + i + 1;
    s_cpy(v->label, *base ? base : v->path, sizeof v->label);
}

static void add_known(const char *home, const char *sub, const char *label)
{
    char p[PATHCAP];
    if (!home || !*home) return;
    if (!sub) { uodesk_fs_add_dir(home, label); return; }
    if (join(p, PATHCAP, home, sub)) uodesk_fs_add_dir(p, label);
}

void uodesk_fs_init(const char *base)
{
    int explicit_dirs = g_nvol > 0;
    if (base) join(g_fontdir, PATHCAP, base, "fonts");
    if (explicit_dirs) return;
#ifdef _WIN32
    {
        wchar_t w[MAX_PATH];
        char    u[PATHCAP];
        if (SHGetFolderPathW(0, CSIDL_PERSONAL, 0, 0, w) == S_OK &&
            WideCharToMultiByte(CP_UTF8, 0, w, -1, u, PATHCAP, 0, 0) > 0)
            uodesk_fs_add_dir(u, "Documents");
        if (SHGetFolderPathW(0, CSIDL_DESKTOPDIRECTORY, 0, 0, w) == S_OK &&
            WideCharToMultiByte(CP_UTF8, 0, w, -1, u, PATHCAP, 0, 0) > 0)
            uodesk_fs_add_dir(u, "Desktop");
        add_known(getenv("USERPROFILE"), 0, "Home");
    }
#else
    add_known(getenv("HOME"), "Documents", "Documents");
    add_known(getenv("HOME"), "Desktop", "Desktop");
    add_known(getenv("HOME"), 0, "Home");
#endif
    if (!g_nvol) {                     /* nothing resolved: the working dir */
        char cwd[PATHCAP];
#ifdef _WIN32
        wchar_t w[PATHCAP];
        if (GetCurrentDirectoryW(PATHCAP, w) &&
            WideCharToMultiByte(CP_UTF8, 0, w, -1, cwd, PATHCAP, 0, 0) > 0)
#else
        if (getcwd(cwd, sizeof cwd))
#endif
            uodesk_fs_add_dir(cwd, "Current folder");
    }
}

/* ---- the bundled fonts ------------------------------------------------------ */
static const char *font_file(const char *name)
{
    static const char *const map[][2] = {
        { "CHICAGO.TTF", "ChiKareGo2.ttf" }, { "SANS.TTF", "Sans.ttf" },
        { "MONO.TTF", "Mono.ttf" },          { "UBUNTU.TTF", "Ubuntu.ttf" },
    };
    unsigned i;
    if (!strncmp(name, "EFI\\UNODOS\\", 11)) name += 11;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(name, map[i][0])) return map[i][1];
    return 0;
}

static int resolve(int vol, const char *name, char *out)
{
    const char *f;
    if (!name || !*name) return 0;
    if ((f = font_file(name)) != 0)
        return g_fontdir[0] && join(out, PATHCAP, g_fontdir, f);
    if (vol < 0 || vol >= g_nvol) return 0;
    /* a name is a name in the volume's root: nothing climbs out of it */
    if (strstr(name, "..") || name[0] == '/' || name[0] == '\\' || strchr(name, ':'))
        return 0;
    return join(out, PATHCAP, g_vol[vol].path, name);
}

/* ---- listing ---------------------------------------------------------------- */
static int office_name(const char *n)
{
    static const char *const ext[] = {
        ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".txt", ".csv", ".rtf"
    };
    const char *dot = strrchr(n, '.');
    unsigned i;
    if (n[0] == '.' || n[0] == '~' || (int)strlen(n) >= NAMECAP) return 0;
    /* no extension: what a plain Ctrl+S on an untitled document writes
     * ("Document1"), so it has to be listed or it could never be reopened */
    if (!dot) return 1;
    for (i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        const char *a = dot, *b = ext[i];
        while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

static int ci_cmp(const void *a, const void *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    while (*x && tolower(*x) == tolower(*y)) { x++; y++; }
    return tolower(*x) - tolower(*y);
}

static void take(const char *n)
{
    if (g_nlist < MAXLIST && office_name(n)) s_cpy(g_list[g_nlist++], n, NAMECAP);
}

int uno_fs_volumes(void) { return g_nvol; }
const char *uno_fs_volume_name(int vol)
{ return vol >= 0 && vol < g_nvol ? g_vol[vol].label : ""; }

int uno_fs_list_begin(int vol)
{
    g_nlist = 0; g_listvol = vol;
    if (vol < 0 || vol >= g_nvol) return 0;
#ifdef _WIN32
    {
        WIN32_FIND_DATAW fd;
        wchar_t pat[PATHCAP];
        char    u[PATHCAP * 2];
        HANDLE  h;
        if (!join(u, PATHCAP, g_vol[vol].path, "*") || !widen(u, pat, PATHCAP)) return 0;
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN))
                continue;
            if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, u, sizeof u, 0, 0) > 0)
                take(u);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(g_vol[vol].path);
        struct dirent *de;
        char p[PATHCAP];
        if (!d) return 0;
        while ((de = readdir(d)) != 0) {
            if (!office_name(de->d_name)) continue;
            if (!join(p, PATHCAP, g_vol[vol].path, de->d_name) || is_dir_path(p)) continue;
            take(de->d_name);
        }
        closedir(d);
    }
#endif
    qsort(g_list, (size_t)g_nlist, NAMECAP, ci_cmp);
    return g_nlist;
}

int uno_fs_list_get(int vol, int i, char *name, int cap)
{
    if (vol != g_listvol || i < 0 || i >= g_nlist || !name || cap <= 0) return 0;
    s_cpy(name, g_list[i], cap);
    return 1;
}

int uno_fs_isdir(int vol, const char *name)
{
    char p[PATHCAP];
    return resolve(vol, name, p) && is_dir_path(p);
}

/* ---- file I/O --------------------------------------------------------------- */
long uno_fs_size(int vol, const char *name)
{
    char p[PATHCAP];
    FILE *f;
    long n;
    if (!resolve(vol, name, p) || is_dir_path(p) || !(f = u8open(p, "rb"))) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    n = ftell(f);
    fclose(f);
    return n;
}

long uno_fs_read(int vol, const char *name, unsigned char *buf, long max)
{
    char p[PATHCAP];
    FILE *f;
    size_t n;
    if (!buf || max <= 0 || !resolve(vol, name, p) || is_dir_path(p)) return -1;
    if (!(f = u8open(p, "rb"))) return -1;
    n = fread(buf, 1, (size_t)max, f);
    fclose(f);
    return (long)n;
}

/* Written beside the target and renamed over it, so a failed save (a full
 * disk, a yanked USB stick) leaves the previous document intact rather than
 * a truncated one - which is the one guarantee a Save button implies. */
int uno_fs_write(int vol, const char *name, const unsigned char *buf, long len)
{
    char p[PATHCAP], tmp[PATHCAP + 8];
    FILE *f;
    int ok;
    if (!buf || len < 0 || font_file(name) || !resolve(vol, name, p)) return 0;
    snprintf(tmp, sizeof tmp, "%s.~tmp", p);
    if (!(f = u8open(tmp, "wb"))) return 0;
    ok = fwrite(buf, 1, (size_t)len, f) == (size_t)len;
    ok = (fclose(f) == 0) && ok;
    if (ok) ok = replace_file(tmp, p);
    if (!ok) remove_file(tmp);
    return ok;
}

/* ---- the headless check ------------------------------------------------------ */
int uodesk_write_ppm(const char *path, const fb_px *px, int w, int h)
{
    FILE *f = u8open(path, "wb");
    int i;
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(px[i]);
        rgb[1] = (unsigned char)(px[i] >> 8);
        rgb[2] = (unsigned char)(px[i] >> 16);
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0;
}
