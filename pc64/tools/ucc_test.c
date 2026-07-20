/* ===========================================================================
 * ucc_test - the host-side battery for the UnoC compiler.
 *
 * Each positive case is a UnoC snippet defining `long test(long a, long b)`.
 * The harness appends a `uno_app_main` wrapper (the module entry the emitter
 * requires), compiles the snippet with ucc, then LOADS THE .UNO ITSELF with
 * a reimplementation of pc64_modload.c's mod_load (mmap+PROT_EXEC, CRC,
 * rebase, named-import resolve against ms_abi host stubs) and calls the
 * entry through an ms_abi function pointer.  So every case exercises
 * lexer -> parser -> codegen -> container -> loader -> real execution,
 * including the Microsoft-x64 ABI boundary the pc64 kernel will use.
 *
 * Negative cases assert that a snippet fails with a diagnostic on the
 * expected line.
 *
 * Build (WSL/Linux):
 *   gcc -O1 -Wall -o ucc_test tools/ucc_test.c apps/ucc.c apps/ucc_x64.c \
 *       -DUCC_KEXPORTS_H='"../build/apps/ucc_kexports.h"'
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../apps/ucc.h"

#define MS __attribute__((ms_abi))

/* ---- ms_abi stubs standing in for the kernel exports ----------------------- */
static MS void *s_memcpy(void *d, const void *s, unsigned long n) { return memcpy(d, s, n); }
static MS void *s_memmove(void *d, const void *s, unsigned long n) { return memmove(d, s, n); }
static MS void *s_memset(void *d, int c, unsigned long n) { return memset(d, c, n); }
static MS int   s_memcmp(const void *a, const void *b, unsigned long n) { return memcmp(a, b, n); }
static MS unsigned long s_strlen(const char *s) { return strlen(s); }
static MS char *s_strcpy(char *d, const char *s) { return strcpy(d, s); }
static MS char *s_strncpy(char *d, const char *s, unsigned long n) { return strncpy(d, s, n); }
static MS char *s_strcat(char *d, const char *s) { return strcat(d, s); }
static MS int   s_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static MS int   s_strncmp(const char *a, const char *b, unsigned long n) { return strncmp(a, b, n); }
static long g_ticks = 60042;
static MS long  s_TickCount(void) { return g_ticks; }
static MS short s_Random(void) { return 12345; }
static MS char *s_NewPtr(long n) { return malloc((size_t)n); }
static MS void  s_DisposePtr(char *p) { free(p); }

/* Toolbox geometry/drawing stubs (SetRect family is real - apps depend on
 * the stores; the paint calls just count) */
typedef struct { short top, left, bottom, right; } SRect;
typedef struct { short v, h; } SPoint;
static long g_paints;
static MS void s_SetRect(SRect *r, short l, short t, short ri, short b)
{ r->left = l; r->top = t; r->right = ri; r->bottom = b; }
static MS void s_OffsetRect(SRect *r, short dh, short dv)
{ r->left += dh; r->right += dh; r->top += dv; r->bottom += dv; }
static MS void s_InsetRect(SRect *r, short dh, short dv)
{ r->left += dh; r->right -= dh; r->top += dv; r->bottom -= dv; }
static MS void s_PaintRect(const SRect *r) { (void)r; g_paints++; }
static MS void s_FrameRect(const SRect *r) { (void)r; g_paints++; }
static MS void s_InvertRect(const SRect *r) { (void)r; g_paints++; }
static MS void s_PaintOval(const SRect *r) { (void)r; g_paints++; }
static MS void s_FrameOval(const SRect *r) { (void)r; g_paints++; }
static MS void s_MoveTo(short h, short v) { (void)h; (void)v; }
static MS void s_LineTo(short h, short v) { (void)h; (void)v; g_paints++; }
static MS void s_PenNormal(void) {}
static MS void s_PenMode(short m) { (void)m; }
static MS void s_RGBForeColor(const void *c) { (void)c; }
static MS void s_GetMouse(SPoint *p) { p->v = 100; p->h = 120; }
static MS unsigned char s_StillDown(void) { return 0; }

static const struct { const char *name; void *fn; } kStubs[] = {
    {"memcpy", (void *)s_memcpy}, {"memmove", (void *)s_memmove},
    {"memset", (void *)s_memset}, {"memcmp", (void *)s_memcmp},
    {"strlen", (void *)s_strlen}, {"strcpy", (void *)s_strcpy},
    {"strncpy", (void *)s_strncpy}, {"strcat", (void *)s_strcat},
    {"strcmp", (void *)s_strcmp}, {"strncmp", (void *)s_strncmp},
    {"TickCount", (void *)s_TickCount}, {"Random", (void *)s_Random},
    {"NewPtr", (void *)s_NewPtr}, {"DisposePtr", (void *)s_DisposePtr},
    {"SetRect", (void *)s_SetRect}, {"OffsetRect", (void *)s_OffsetRect},
    {"InsetRect", (void *)s_InsetRect}, {"PaintRect", (void *)s_PaintRect},
    {"FrameRect", (void *)s_FrameRect}, {"InvertRect", (void *)s_InvertRect},
    {"PaintOval", (void *)s_PaintOval}, {"FrameOval", (void *)s_FrameOval},
    {"MoveTo", (void *)s_MoveTo}, {"LineTo", (void *)s_LineTo},
    {"PenNormal", (void *)s_PenNormal}, {"PenMode", (void *)s_PenMode},
    {"RGBForeColor", (void *)s_RGBForeColor}, {"GetMouse", (void *)s_GetMouse},
    {"StillDown", (void *)s_StillDown},
};

/* ---- host mod_load (mirrors pc64_modload.c checks 1:1) ---------------------- */
typedef struct {
    unsigned int magic;
    unsigned short abi, flags;
    unsigned int entry, mem_size, file_size, nreloc;
    unsigned int imp_rva, imp_count;
    unsigned long long pref_base;
    unsigned int crc, rsv;
} UnoModHdr;

static unsigned int mod_crc32(const unsigned char *p, long n)
{
    unsigned int c = 0xFFFFFFFFu; long i; int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static const char *g_loaderr;

static void *host_mod_load(const unsigned char *buf, long n)
{
    const UnoModHdr *h = (const UnoModHdr *)buf;
    unsigned char *base;
    const unsigned int *rel;
    unsigned int i;

    g_loaderr = 0;
    if (n < (long)sizeof *h)               { g_loaderr = "short";      return 0; }
    if (h->magic != 0x314F4E55u)           { g_loaderr = "bad magic";  return 0; }
    if (h->abi != 1)                       { g_loaderr = "bad abi";    return 0; }
    if ((long)sizeof *h + h->file_size + 4ll * h->nreloc != n)
                                           { g_loaderr = "bad size";   return 0; }
    if (h->entry >= h->mem_size || h->file_size > h->mem_size)
                                           { g_loaderr = "bad hdr";    return 0; }
    if (mod_crc32(buf + sizeof *h, n - (long)sizeof *h) != h->crc)
                                           { g_loaderr = "bad crc";    return 0; }

    base = mmap(0, (h->mem_size + 4095u) & ~4095u,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)                { g_loaderr = "mmap";       return 0; }
    memcpy(base, buf + sizeof *h, h->file_size);
    memset(base + h->file_size, 0, h->mem_size - h->file_size);

    rel = (const unsigned int *)(buf + sizeof *h + h->file_size);
    for (i = 0; i < h->nreloc; i++) {
        if (rel[i] + 8u > h->mem_size)     { g_loaderr = "bad reloc";  return 0; }
        *(unsigned long long *)(base + rel[i]) +=
            (unsigned long long)base - h->pref_base;
    }
    for (i = 0; i < h->imp_count; i++) {
        char *rec = (char *)base + h->imp_rva + 32u * i;
        void *fn = 0;
        unsigned int j;
        if (h->imp_rva + 32u * (i + 1) > h->mem_size) { g_loaderr = "bad imp"; return 0; }
        rec[23] = 0;
        for (j = 0; j < sizeof kStubs / sizeof kStubs[0]; j++)
            if (!strcmp(kStubs[j].name, rec)) { fn = kStubs[j].fn; break; }
        if (!fn) { g_loaderr = rec; return 0; }
        *(unsigned long long *)(rec + 24) = (unsigned long long)fn;
    }
    return base + h->entry;
}

/* ---- the test driver ---------------------------------------------------------*/
typedef MS long (*TestFn)(long a, long b);

static long g_incn;
static long inc_read(void *ctx, const char *name, char *buf, long max)
{
    /* tests may register one in-memory include file */
    const char **inc = ctx;
    long n;
    if (!inc || !inc[0] || strcmp(name, inc[0])) return -1;
    n = (long)strlen(inc[1]);
    if (n > max) return -1;
    memcpy(buf, inc[1], (size_t)n);
    g_incn++;
    return n;
}

#define WRAP "\nlong long uno_app_main(long long a, long long b) { return test(a, b); }\n"

static int g_pass, g_fail;
static unsigned char g_out[1 << 20];
static void *g_work;
#define WORKLEN (8L << 20)

static void run_case(const char *name, const char *src, long a, long b,
                     long expect, const char **inc)
{
    char full[65536];
    UccDiag diags[8];
    int nd = 0;
    long n;
    void *entry;
    long got;

    snprintf(full, sizeof full, "%s%s", src, WRAP);
    n = ucc_compile(full, (long)strlen(full), "TEST.C",
                    inc ? inc_read : 0, (void *)inc,
                    g_work, WORKLEN, g_out, sizeof g_out, diags, 8, &nd);
    if (n < 0) {
        printf("FAIL %-28s compile: %s (line %d)\n", name,
               nd ? diags[0].msg : "?", nd ? diags[0].line : 0);
        g_fail++;
        return;
    }
    entry = host_mod_load(g_out, n);
    if (!entry) {
        printf("FAIL %-28s load: %s\n", name, g_loaderr);
        g_fail++;
        return;
    }
    got = ((TestFn)entry)(a, b);
    if (got != expect) {
        printf("FAIL %-28s got %ld want %ld\n", name, got, expect);
        g_fail++;
        return;
    }
    g_pass++;
}

static void run_err(const char *name, const char *src, int wantline,
                    const char *wantmsg)
{
    char full[65536];
    UccDiag diags[8];
    int nd = 0;
    long n;

    snprintf(full, sizeof full, "%s%s", src, WRAP);
    n = ucc_compile(full, (long)strlen(full), "TEST.C", 0, 0,
                    g_work, WORKLEN, g_out, sizeof g_out, diags, 8, &nd);
    if (n >= 0 || !nd) {
        printf("FAIL %-28s expected an error, compiled fine\n", name);
        g_fail++;
        return;
    }
    if (wantline && diags[0].line != wantline) {
        printf("FAIL %-28s error on line %d, want %d (%s)\n", name,
               diags[0].line, wantline, diags[0].msg);
        g_fail++;
        return;
    }
    if (wantmsg && !strstr(diags[0].msg, wantmsg)) {
        printf("FAIL %-28s error '%s', want '%s'\n", name, diags[0].msg, wantmsg);
        g_fail++;
        return;
    }
    g_pass++;
}

#define T(name, expect, src)          run_case(name, src, 0, 0, expect, 0)
#define TAB(name, a, b, expect, src)  run_case(name, src, a, b, expect, 0)

/* ============================================================================
 * SDK acceptance: compile sdk/<file>, load it, hand it a stub KernelApi and
 * drive draw/key/tick like the desktop would.  The host-side mirrors of the
 * ABI structs use ms_abi function pointers so the boundary is the real one.
 * ========================================================================== */
typedef unsigned char HBoolean;
typedef struct { short v, h; } HPoint;
typedef struct { short top, left, bottom, right; } HRect;
typedef struct { unsigned short red, green, blue; } HRGB;
typedef struct { HBoolean used; short proc; HRect bounds; const char *title; } HWin;
typedef struct { unsigned char midi, dur; } HNote;
typedef struct { unsigned char r, g, b, mono; } HGameRGB;

typedef struct {
    short abi_version;
    HRGB *palette, *black;
    short tbar_h;
    MS void (*uno_fill)(HRect *, short);
    MS void (*uno_box)(HRect *, short);
    MS void (*uno_invert)(HRect *);
    MS void (*text_at)(short, short, const char *, short, short, HBoolean);
    MS void (*text_at_max)(short, short, const char *, short, short);
    MS void (*fill_rgb)(HRect *, const HGameRGB *);
    MS void (*fmt_u)(long, char *);
    MS void (*put2)(long, char *);
    MS long (*now_secs)(void);
    MS void (*draw_window)(HWin *);
    MS HWin *(*find_app_window)(short);
    MS void (*launch_app)(short);
    MS void (*repaint_all)(void);
    MS short (*topmost_proc)(void);
    MS HBoolean (*fat12_mount)(void);
    MS void (*fat12_list)(void);
    MS long (*fat12_read)(const char *, unsigned char *, long);
    MS HBoolean (*fat12_write)(const char *, const unsigned char *, long);
    short *fat_count;
    unsigned char (*fat_name)[13];
    long *fat_sizes;
    MS void (*music_open_chan)(void);
    MS void (*music_note_on)(short, short);
    MS void (*music_quiet)(void);
    MS void (*music_start)(void);
    MS void (*music_stop)(void);
    MS void (*gm_start)(const HNote *, short, short);
    MS void (*gm_stop)(void);
    MS int  (*display_res_count)(void);
    MS void (*display_res_get)(int, short *, short *, short *, HBoolean *);
    MS void (*display_res_set)(int);
} HKernelApi;

typedef struct {
    MS void (*draw)(HWin *);
    MS HBoolean (*key)(char, short, HBoolean);
    MS void (*click)(HWin *, HPoint);
    MS void (*tick)(void);
    MS void (*opened)(void);
    MS void (*closed)(void);
    const char *win_title;
    short win_rect[4];
} HAppInterface;

typedef MS const HAppInterface *(*HEntry)(const HKernelApi *);

static long g_fills, g_texts, g_boxes, g_gm_starts, g_draw_windows;
static HWin g_win;

static MS void  k_uno_fill(HRect *r, short c) { (void)r; (void)c; g_fills++; }
static MS void  k_uno_box(HRect *r, short c) { (void)r; (void)c; g_boxes++; }
static MS void  k_uno_invert(HRect *r) { (void)r; }
static MS void  k_text_at(short x, short y, const char *s, short fg, short bg,
                          HBoolean op)
{ (void)x; (void)y; (void)fg; (void)bg; (void)op; if (s) g_texts++; }
static MS void  k_text_at_max(short x, short y, const char *s, short fg, short mw)
{ (void)x; (void)y; (void)fg; (void)mw; if (s) g_texts++; }
static MS void  k_fill_rgb(HRect *q, const HGameRGB *c)
{ (void)q; (void)c; g_fills++; }
static MS void  k_fmt_u(long v, char *out) { sprintf(out, "%ld", v); }
static MS void  k_put2(long v, char *out) { sprintf(out, "%02ld", v); }
static MS long  k_now_secs(void) { return 1000; }
static MS void  k_draw_window(HWin *w) { (void)w; g_draw_windows++; }
static MS HWin *k_find_app_window(short proc) { (void)proc; return &g_win; }
static MS void  k_launch_app(short proc) { (void)proc; }
static MS void  k_repaint_all(void) {}
static MS short k_topmost_proc(void) { return 0; }
static MS HBoolean k_fat12_mount(void) { return 1; }
static MS void  k_fat12_list(void) {}
static MS long  k_fat12_read(const char *n, unsigned char *b, long m)
{ (void)n; (void)b; (void)m; return -1; }
static MS HBoolean k_fat12_write(const char *n, const unsigned char *b, long l)
{ (void)n; (void)b; (void)l; return 1; }
static MS void  k_music0(void) {}
static MS void  k_music_note_on(short m, short d) { (void)m; (void)d; }
static MS void  k_gm_start(const HNote *n, short c, short o)
{ (void)n; (void)c; (void)o; g_gm_starts++; }
static MS int   k_res_count(void) { return 1; }
static MS void  k_res_get(int i, short *w, short *h, short *z, HBoolean *a)
{ (void)i; *w = 640; *h = 480; *z = 1; *a = 1; }
static MS void  k_res_set(int i) { (void)i; }

static HRGB g_pal[4] = { {0,0,40000}, {0,50000,50000}, {50000,0,50000},
                         {65535,65535,65535} };
static HRGB g_black;
static short g_fat_count;
static unsigned char g_fat_names[24][13];
static long g_fat_sizes[24];

static HKernelApi g_kapi;

static void kapi_init(void)
{
    g_kapi.abi_version = 1;
    g_kapi.palette = g_pal; g_kapi.black = &g_black; g_kapi.tbar_h = 14;
    g_kapi.uno_fill = k_uno_fill; g_kapi.uno_box = k_uno_box;
    g_kapi.uno_invert = k_uno_invert;
    g_kapi.text_at = k_text_at; g_kapi.text_at_max = k_text_at_max;
    g_kapi.fill_rgb = k_fill_rgb;
    g_kapi.fmt_u = k_fmt_u; g_kapi.put2 = k_put2;
    g_kapi.now_secs = k_now_secs;
    g_kapi.draw_window = k_draw_window;
    g_kapi.find_app_window = k_find_app_window;
    g_kapi.launch_app = k_launch_app;
    g_kapi.repaint_all = k_repaint_all;
    g_kapi.topmost_proc = k_topmost_proc;
    g_kapi.fat12_mount = k_fat12_mount;
    g_kapi.fat12_list = k_fat12_list;
    g_kapi.fat12_read = k_fat12_read;
    g_kapi.fat12_write = k_fat12_write;
    g_kapi.fat_count = &g_fat_count;
    g_kapi.fat_name = g_fat_names;
    g_kapi.fat_sizes = g_fat_sizes;
    g_kapi.music_open_chan = k_music0;
    g_kapi.music_note_on = k_music_note_on;
    g_kapi.music_quiet = k_music0;
    g_kapi.music_start = k_music0;
    g_kapi.music_stop = k_music0;
    g_kapi.gm_start = k_gm_start;
    g_kapi.gm_stop = k_music0;
    g_kapi.display_res_count = k_res_count;
    g_kapi.display_res_get = k_res_get;
    g_kapi.display_res_set = k_res_set;
}

static long sdk_inc(void *ctx, const char *name, char *buf, long max)
{
    char path[256];
    FILE *f;
    long n;
    (void)ctx;
    snprintf(path, sizeof path, "sdk/%s", name);
    f = fopen(path, "rb");
    if (!f) return -1;
    n = (long)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}

static void run_app(const char *file, const char *title)
{
    char path[256];
    static char src[262144];
    FILE *f;
    long n, uno;
    UccDiag diags[8];
    int nd = 0, i;
    void *entry;
    const HAppInterface *iface;

    snprintf(path, sizeof path, "sdk/%s", file);
    f = fopen(path, "rb");
    if (!f) { printf("FAIL app:%-22s missing %s\n", file, path); g_fail++; return; }
    n = (long)fread(src, 1, sizeof src - 1, f);
    fclose(f);

    uno = ucc_compile(src, n, file, sdk_inc, 0, g_work, WORKLEN,
                      g_out, sizeof g_out, diags, 8, &nd);
    if (uno < 0) {
        printf("FAIL app:%-22s compile: %s (%s line %d)\n", file,
               nd ? diags[0].msg : "?", nd ? diags[0].file : "?",
               nd ? diags[0].line : 0);
        g_fail++;
        return;
    }
    entry = host_mod_load(g_out, uno);
    if (!entry) { printf("FAIL app:%-22s load: %s\n", file, g_loaderr); g_fail++; return; }

    kapi_init();
    g_fills = g_texts = g_boxes = g_gm_starts = g_draw_windows = 0;
    iface = ((HEntry)entry)((const HKernelApi *)&g_kapi);
    if (!iface || !iface->win_title || strcmp(iface->win_title, title)) {
        printf("FAIL app:%-22s bad iface/title\n", file);
        g_fail++;
        return;
    }
    g_win.used = 1; g_win.proc = 5;
    g_win.bounds.left = (short)iface->win_rect[0];
    g_win.bounds.top = (short)iface->win_rect[1];
    g_win.bounds.right = (short)iface->win_rect[2];
    g_win.bounds.bottom = (short)iface->win_rect[3];
    g_win.title = iface->win_title;

    if (iface->opened) iface->opened();
    if (!iface->key || !iface->key('n', 0, 0)) {
        printf("FAIL app:%-22s key('n') not handled\n", file);
        g_fail++;
        return;
    }
    for (i = 0; i < 120; i++) {                  /* run 2 emulated seconds */
        g_ticks += 5;
        if (iface->tick) iface->tick();
    }
    iface->draw(&g_win);
    if (g_fills < 2 || g_texts < 1) {
        printf("FAIL app:%-22s draw made no calls (fills=%ld texts=%ld)\n",
               file, g_fills, g_texts);
        g_fail++;
        return;
    }
    if (iface->closed) iface->closed();
    printf("  ok app:%-22s %s\n", file, ucc_summary());
    g_pass++;
}

int main(void)
{
    g_work = malloc(WORKLEN);
    if (!g_work) return 2;

    /* ---- integers + arithmetic ------------------------------------------ */
    T("return-42", 42, "long test(long a, long b) { return 42; }");
    T("add", 5, "long test(long a, long b) { return 2 + 3; }");
    T("arith-mix", 26, "long test(long a, long b) { return 2 * 3 + 4 * 5; }");
    T("paren", 70, "long test(long a, long b) { return 2 * (3 + 4) * 5; }");
    T("sub-neg", -4, "long test(long a, long b) { return 3 - 7; }");
    T("div", 6, "long test(long a, long b) { return 45 / 7; }");
    T("mod", 3, "long test(long a, long b) { return 45 % 7; }");
    T("neg-div", -6, "long test(long a, long b) { return -45 / 7; }");
    T("unary-minus", -10, "long test(long a, long b) { return -(4 + 6); }");
    T("bitops", 0x1234 ^ 0x00FF, "long test(long a, long b) { return 0x1234 ^ 0x00FF; }");
    T("and-or", (0x1234 & 0xFF0) | 0x8000,
      "long test(long a, long b) { return (0x1234 & 0xFF0) | 0x8000; }");
    T("shifts", (1 << 12) + (1024 >> 3),
      "long test(long a, long b) { return (1 << 12) + (1024 >> 3); }");
    T("bitnot", ~0x55, "long test(long a, long b) { return ~0x55; }");
    T("hex-char", 'A' + 010, "long test(long a, long b) { return 'A' + 010; }");
    TAB("params", 33, 44, 77, "long test(long a, long b) { return a + b; }");
    TAB("param-swap", 10, 3, 7, "long test(long a, long b) { return a - b; }");

    /* 32-bit int semantics: wraparound + unsigned compare */
    T("int-wrap", 1,
      "int test(long a, long b) { int x = 2147483647; x = x + 1;"
      " return x == -2147483647 - 1; }");
    T("uint-wrap", 1,
      "long test(long a, long b) { unsigned x = 1; x = x - 2;"
      " return x == 4294967295U; }");
    T("uint-cmp", 1,
      "long test(long a, long b) { unsigned x = 1; x = x - 2;"
      " return x > 1000000; }");
    T("int-div-neg", -3,
      "long test(long a, long b) { int x = -10; return x / 3; }");
    T("uns-shr", 0x7FFFFFFF,
      "long test(long a, long b) { unsigned x = 0xFFFFFFFE; return x >> 1; }");
    T("sgn-shr", -1,
      "long test(long a, long b) { int x = -2; return x >> 1; }");

    /* ---- comparisons + logic --------------------------------------------- */
    T("cmp-chain", 1, "long test(long a, long b) { return 3 < 5 == 1; }");
    T("cmp-ge", 1, "long test(long a, long b) { return 5 >= 5; }");
    T("cmp-gt", 0, "long test(long a, long b) { return 5 > 5; }");
    T("logand", 1, "long test(long a, long b) { return 2 && 3; }");
    T("logand-0", 0, "long test(long a, long b) { return 2 && 0; }");
    T("logor", 1, "long test(long a, long b) { return 0 || 3; }");
    T("lognot", 1, "long test(long a, long b) { return !0; }");
    TAB("shortcircuit", 5, 0, 5,
        "long g; long boom(void) { g = 99; return 1; }"
        "long test(long a, long b) { if (b && boom()) return 0; return a + g; }");
    T("ternary", 7, "long test(long a, long b) { return 1 ? 7 : 9; }");
    T("ternary-else", 9, "long test(long a, long b) { return 0 ? 7 : 9; }");

    /* ---- control flow ------------------------------------------------------ */
    T("if-else", 1, "long test(long a, long b) { if (3 > 2) return 1; return 2; }");
    T("while-sum", 55,
      "long test(long a, long b) { long s = 0; long i = 1;"
      " while (i <= 10) { s = s + i; i = i + 1; } return s; }");
    T("for-sum", 55,
      "long test(long a, long b) { long s = 0;"
      " for (long i = 1; i <= 10; i++) s += i; return s; }");
    T("do-while", 10,
      "long test(long a, long b) { long i = 0; do { i++; } while (i < 10); return i; }");
    T("break", 5,
      "long test(long a, long b) { long i;"
      " for (i = 0; i < 100; i++) if (i == 5) break; return i; }");
    T("continue", 25,
      "long test(long a, long b) { long s = 0;"
      " for (long i = 0; i < 10; i++) { if (i % 2 == 0) continue; s += i; } return s; }");
    T("nested-loop", 100,
      "long test(long a, long b) { long s = 0;"
      " for (long i = 0; i < 10; i++) for (long j = 0; j < 10; j++) s++; return s; }");
    TAB("switch", 3, 0, 30,
        "long test(long a, long b) { switch (a) {"
        " case 1: return 10; case 2: return 20; case 3: return 30;"
        " default: return -1; } }");
    TAB("switch-dflt", 9, 0, -1,
        "long test(long a, long b) { switch (a) {"
        " case 1: return 10; default: return -1; } }");
    TAB("switch-fall", 2, 0, 12,
        "long test(long a, long b) { long s = 0; switch (a) {"
        " case 2: s += 2; case 1: s += 10; break; case 5: s = 99; } return s; }");
    T("empty-stmt", 4, "long test(long a, long b) { ;;; return 4; }");

    /* ---- functions ---------------------------------------------------------- */
    T("call", 12,
      "long three(void) { return 3; }"
      "long test(long a, long b) { return three() * 4; }");
    T("call-args", 22,
      "long madd(long x, long y, long z) { return x * y + z; }"
      "long test(long a, long b) { return madd(4, 5, 2); }");
    T("call-8args", 36,
      "long s8(long p, long q, long r, long s, long t, long u, long v, long w)"
      " { return p + q + r + s + t + u + v + w; }"
      "long test(long a, long b) { return s8(1, 2, 3, 4, 5, 6, 7, 8); }");
    T("stack-args-order", 87654321,
      "long pick(long p, long q, long r, long s, long t, long u, long v, long w)"
      " { return w*10000000 + v*1000000 + u*100000 + t*10000 + s*1000 + r*100 + q*10 + p; }"
      "long test(long a, long b) { return pick(1, 2, 3, 4, 5, 6, 7, 8); }");
    T("recursion", 120,
      "long fact(long n) { if (n <= 1) return 1; return n * fact(n - 1); }"
      "long test(long a, long b) { return fact(5); }");
    T("fib", 55,
      "long fib(long n) { if (n < 2) return n; return fib(n-1) + fib(n-2); }"
      "long test(long a, long b) { return fib(10); }");
    T("fwd-proto", 9,
      "long later(long x);"
      "long test(long a, long b) { return later(3); }"
      "long later(long x) { return x * x; }");
    T("void-fn", 21,
      "long g; void bump(void) { g = g + 21; }"
      "long test(long a, long b) { bump(); return g; }");
    T("deep-call-align", 15,
      "long f1(long x) { return x + 1; }"
      "long f2(long x) { return f1(x) + f1(x + 1); }"
      "long test(long a, long b) { return f2(3) + f2(0) * 2; }");

    /* the ABI boundary: kernel-style import in an expression context */
    T("import-call", 60042,
      "long TickCount(void);"
      "long test(long a, long b) { return TickCount(); }");
    T("import-strlen", 5,
      "unsigned long strlen(const char *s);"
      "long test(long a, long b) { return strlen(\"hello\"); }");
    T("import-in-expr", 60052,
      "long TickCount(void);"
      "long test(long a, long b) { return 4 + TickCount() + 3 * 2; }");
    T("import-nested-args", 10,
      "int strcmp(const char *x, const char *y);"
      "unsigned long strlen(const char *s);"
      "long test(long a, long b) {"
      " return strlen(\"hi\") + (strcmp(\"aa\", \"ab\") < 0) * 8 - 0; }");

    /* ---- locals / assignment ops -------------------------------------------- */
    T("compound-ops", 30,
      "long test(long a, long b) { long x = 10;"
      " x += 5; x -= 1; x *= 4; x /= 2; x %= 100; x |= 16; x &= 63; x ^= 2;"
      " return x; }");
    T("compound-shifts", 24,
      "long test(long a, long b) { long x = 3; x <<= 4; x >>= 1; return x; }");
    T("preinc", 6, "long test(long a, long b) { long x = 5; return ++x; }");
    T("postinc", 5,
      "long test(long a, long b) { long x = 5; long y = x++; return y + (x == 6) - 1; }");
    T("postdec", 5,
      "long test(long a, long b) { long x = 5; long y = x--; return y + (x == 4) - 1; }");
    T("comma", 7, "long test(long a, long b) { long x; x = (1, 2, 7); return x; }");
    T("multi-decl", 6, "long test(long a, long b) { long x = 1, y = 2, z = 3; return x+y+z; }");
    T("shadow", 3,
      "long test(long a, long b) { long x = 1; { long x = 2; } return x + 2; }");
    T("char-arith", 3,
      "long test(long a, long b) { char c = 'a'; char d = 'd'; return d - c; }");
    T("uchar-wrap", 44,
      "long test(long a, long b) { unsigned char c = 200; c += 100; return c; }");
    T("short-trunc", -6,
      "long test(long a, long b) { short s = 65530; return s; }");

    /* ---- pointers ----------------------------------------------------------- */
    T("ptr-basic", 7,
      "long test(long a, long b) { long x = 7; long *p = &x; return *p; }");
    T("ptr-write", 9,
      "long test(long a, long b) { long x = 7; long *p = &x; *p = 9; return x; }");
    T("ptr-arith", 30,
      "long test(long a, long b) { long v[4]; long *p = v;"
      " v[0]=10; v[1]=20; v[2]=30; p = p + 2; return *p; }");
    T("ptr-diff", 3,
      "long test(long a, long b) { long v[8]; return &v[5] - &v[2]; }");
    T("ptr-index-neg", 20,
      "long test(long a, long b) { long v[4]; long *p = &v[3];"
      " v[1] = 20; return p[-2]; }");
    T("ptr-compound", 2,
      "long test(long a, long b) { long v[4]; long *p = v;"
      " p += 3; p -= 1; return p - v; }");
    T("ptr-postinc", 21,
      "long test(long a, long b) { long v[3]; long *p = v;"
      " v[0]=1; v[1]=20; long x = *p++; return x + *p; }");
    T("ptr-ptr", 5,
      "long test(long a, long b) { long x = 5; long *p = &x; long **q = &p;"
      " return **q; }");
    T("ptr-cast-int", 1,
      "long test(long a, long b) { long x = 3; long *p = &x;"
      " long v = (long)p; return v == (long)&x; }");
    T("null-ptr", 1,
      "long test(long a, long b) { long *p = 0; return p == 0; }");

    /* ---- arrays -------------------------------------------------------------- */
    T("arr-sum", 60,
      "long test(long a, long b) { long v[3]; v[0]=10; v[1]=20; v[2]=30;"
      " long s = 0; for (long i = 0; i < 3; i++) s += v[i]; return s; }");
    T("arr-2d", 42,
      "long test(long a, long b) { long m[3][4];"
      " m[1][2] = 40; m[2][3] = 2; return m[1][2] + m[2][3]; }");
    T("arr-decay-fn", 6,
      "long sum(long *v, long n) { long s = 0;"
      " for (long i = 0; i < n; i++) s += v[i]; return s; }"
      "long test(long a, long b) { long v[3]; v[0]=1; v[1]=2; v[2]=3;"
      " return sum(v, 3); }");
    T("sizeof-arr", 12, "long test(long a, long b) { long v[3]; return sizeof(v); }");
    T("sizeof-2d", 48, "long test(long a, long b) { long m[3][4]; return sizeof m; }");
    T("sizeof-elem", 4, "long test(long a, long b) { long m[3][4]; return sizeof m[0][0]; }");
    T("sizeof-types", 1 + 2 + 4 + 4 + 8 + 8,     /* LLP64: long is 4 */
      "long test(long a, long b) { return sizeof(char) + sizeof(short)"
      " + sizeof(int) + sizeof(long) + sizeof(long long) + sizeof(char *); }");
    T("long-is-4", 1,
      "long test(long a, long b) { return sizeof(long) == 4"
      " && sizeof(unsigned long) == 4 && sizeof(long long) == 8; }");
    T("llong-shift", 1,
      "long test(long a, long b) { long long x = 1LL << 40;"
      " return (x >> 40) == 1 && x > 0x7FFFFFFF; }");
    T("llong-gvar", 1,
      "long long big = 0x123456789AB;"
      "long test(long a, long b) { return big == 0x123456789ABLL; }");
    T("char-arr-str", 'e',
      "char *strcpy(char *d, const char *s);"
      "long test(long a, long b) { char s[6]; strcpy(s, \"hello\"); return s[1]; }");
    T("str-literal-idx", 'l',
      "long test(long a, long b) { return \"hello\"[2]; }");

    /* ---- structs / unions ----------------------------------------------------- */
    T("struct-basic", 30,
      "struct P { long x, y; };"
      "long test(long a, long b) { struct P p; p.x = 10; p.y = 20;"
      " return p.x + p.y; }");
    T("struct-ptr", 30,
      "struct P { long x, y; };"
      "long test(long a, long b) { struct P p; struct P *q = &p;"
      " q->x = 10; q->y = 20; return p.x + p.y; }");
    T("struct-nested", 7,
      "struct In { short a, b; }; struct Out { struct In i; long z; };"
      "long test(long a, long b) { struct Out o; o.i.b = 7; return o.i.b; }");
    T("struct-copy", 15,
      "struct P { long x, y, z; };"
      "long test(long a, long b) { struct P p; struct P q;"
      " p.x = 4; p.y = 5; p.z = 6; q = p; return q.x + q.y + q.z; }");
    T("struct-init-copy", 15,
      "struct P { long x, y, z; };"
      "long test(long a, long b) { struct P p; p.x = 4; p.y = 5; p.z = 6;"
      " struct P q = p; return q.x + q.y + q.z; }");
    T("struct-arr", 12,
      "struct P { short x; short y; };"
      "long test(long a, long b) { struct P v[4]; v[2].x = 5; v[2].y = 7;"
      " return v[2].x + v[2].y; }");
    T("struct-layout", 8,                        /* LLP64: long aligns to 4 */
      "struct M { char c; long q; };"
      "long test(long a, long b) { return sizeof(struct M); }");
    T("struct-fnarg", 30,
      "struct P { long x, y; };"
      "long sum(struct P *p) { return p->x + p->y; }"
      "long test(long a, long b) { struct P p; p.x = 10; p.y = 20;"
      " return sum(&p); }");
    T("union-basic", 1,
      "union U { long q; char c[8]; };"
      "long test(long a, long b) { union U u; u.q = 0x41; return u.c[0] == 'A'; }");
    T("short-members", 9,
      "struct R { short top, left, bottom, right; };"
      "long test(long a, long b) { struct R r; r.top = 4; r.right = 5;"
      " return r.top + r.right; }");

    /* ---- typedef / enum -------------------------------------------------------- */
    T("typedef", 30,
      "typedef struct { long x, y; } P; typedef P *PP;"
      "long test(long a, long b) { P p; PP q = &p; q->x = 10; q->y = 20;"
      " return p.x + p.y; }");
    T("typedef-scalar", 5,
      "typedef unsigned char Boolean;"
      "long test(long a, long b) { Boolean f = 5; return f; }");
    T("enum-basic", 6,
      "enum { C_BLUE = 0, C_CYAN = 1, C_MAG = 2, C_WHITE = 3 };"
      "long test(long a, long b) { return C_CYAN + C_MAG + C_WHITE; }");
    T("enum-assign", 29,
      "enum { A = 10, B, C = 3, D, E = B + D };"
      "long test(long a, long b) { return E + B + C; }");   /* 15+11+3 */
    T("enum-tagged", 2,
      "enum Dir { N, E, S, W }; "
      "long test(long a, long b) { enum Dir d = S; return d; }");

    /* ---- globals + initializers ------------------------------------------------- */
    T("gvar", 11,
      "long g; long test(long a, long b) { g = 11; return g; }");
    T("gvar-init", 42,
      "long g = 42; long test(long a, long b) { return g; }");
    T("gvar-arr-init", 60,
      "long v[3] = { 10, 20, 30 };"
      "long test(long a, long b) { return v[0] + v[1] + v[2]; }");
    T("gvar-arr-unsized", 5,
      "short v[] = { 1, 2, 3, 4, 5 };"
      "long test(long a, long b) { return sizeof v / sizeof v[0]; }");
    T("gvar-2d-init", 8,
      "signed char m[2][4] = { {0,1,2,3}, {4,5,6,7} };"
      "long test(long a, long b) { return m[0][1] + m[1][3]; }");
    T("gvar-struct-init", 30,
      "struct P { long x, y; }; struct P p = { 10, 20 };"
      "long test(long a, long b) { return p.x + p.y; }");
    T("gvar-struct-arr", 31,
      "struct C { unsigned char r, g, bl, mono; };"
      "struct C pal[3] = { {1,2,3,4}, {5,6,7,8}, {9,10,11,12} };"
      "long test(long a, long b) { return pal[1].g + pal[2].r + pal[0].mono"
      " + pal[2].mono; }");   /* 6+9+4+12 */
    T("gvar-str-ptr", 'r',
      "const char *s = \"world\";"
      "long test(long a, long b) { return s[2]; }");
    T("gvar-str-arr", 5,
      "char s[] = \"hello\";"
      "unsigned long strlen(const char *s);"
      "long test(long a, long b) { return strlen(s); }");
    T("gvar-ptr-tbl", 'b',
      "const char *tbl[3] = { \"alpha\", \"beta\", \"gamma\" };"
      "long test(long a, long b) { return tbl[1][0]; }");
    T("gvar-addr-init", 33,
      "long x = 33; long *p = &x;"
      "long test(long a, long b) { return *p; }");
    T("gvar-arr-addr", 21,
      "long v[4] = { 20, 21, 22, 23 }; long *p = &v[1];"
      "long test(long a, long b) { return *p; }");
    T("fnptr-tbl", 12,
      "long f1(long x) { return x + 1; } long f2(long x) { return x * 2; }"
      "long (*ops[2])(long) = { f1, f2 };"
      "long test(long a, long b) { return ops[0](5) + ops[1](3); }");
    T("fnptr-var", 9,
      "long sq(long x) { return x * x; }"
      "long test(long a, long b) { long (*f)(long) = sq; return f(3); }");
    T("static-local", 3,
      "long bump(void) { static long n = 0; n++; return n; }"
      "long test(long a, long b) { bump(); bump(); return bump(); }");
    T("static-gvar", 17,
      "static long g = 17; long test(long a, long b) { return g; }");
    T("local-brace-init", 6,
      "long test(long a, long b) { long v[3] = { 1, 2, 3 };"
      " return v[0] + v[1] + v[2]; }");
    T("local-struct-brace", 30,
      "struct P { long x, y; };"
      "long test(long a, long b) { struct P p = { 10, 20 }; return p.x + p.y; }");
    T("bss-zero", 0,
      "long big[100];"
      "long test(long a, long b) { long s = 0;"
      " for (long i = 0; i < 100; i++) s += big[i]; return s; }");

    /* ---- preprocessor ------------------------------------------------------------ */
    T("define-const", 30,
      "#define TEN 10\n#define THIRTY (TEN * 3)\n"
      "long test(long a, long b) { return THIRTY; }");
    T("define-nested", 8,
      "#define A B\n#define B C\n#define C 8\n"
      "long test(long a, long b) { return A; }");
    T("ifdef", 5,
      "#define ON\n#ifdef ON\nlong test(long a, long b) { return 5; }\n"
      "#else\nlong test(long a, long b) { return 6; }\n#endif\n");
    T("ifndef-else", 6,
      "#ifdef OFF\nlong test(long a, long b) { return 5; }\n"
      "#else\nlong test(long a, long b) { return 6; }\n#endif\n");
    T("nested-cond", 3,
      "#define X\n#ifdef X\n#ifdef Y\nlong bad;\n#else\n"
      "long test(long a, long b) { return 3; }\n#endif\n#endif\n");
    T("undef", 4,
      "#define F\n#undef F\n#ifndef F\nlong test(long a, long b) { return 4; }\n"
      "#endif\n");
    T("define-selfref", 30,
      "struct K { long fill; };\n#define fill k.fill\n"
      "struct K k;\n"
      "long test(long a, long b) { fill = 30; return fill; }");
    {
        static const char *inc[2] = { "DEFS.H",
            "#ifndef DEFS_H\n#define DEFS_H\n"
            "#define MAGIC 77\ntypedef struct { long v; } Box;\n#endif\n" };
        run_case("include",
            "#include \"DEFS.H\"\n#include \"DEFS.H\"\n"
            "long test(long a, long b) { Box bx; bx.v = MAGIC; return bx.v; }",
            0, 0, 77, inc);
    }

    /* ---- kernel-flavored patterns (dostris idioms) -------------------------------- */
    T("kapi-vtable", 60042,
      "typedef struct { long (*now)(void); long pad; } K;"
      "long TickCount(void);"
      "static K k;"
      "long test(long a, long b) { k.now = TickCount; return k.now(); }");
    T("fnptr-member-call", 25,
      "typedef struct { long (*op)(long, long); } Ops;"
      "long mul(long x, long y) { return x * y; }"
      "static Ops ops = { mul };"
      "long test(long a, long b) { return ops.op(5, 5); }");
    T("iface-static-init", 3,
      "typedef struct { long (*d)(void); const char *t; short r[4]; } IF;"
      "long dr(void) { return 1; }"
      "static const IF kI = { dr, \"Dostris\", { 20, 10, 330, 388 } };"
      "long test(long a, long b) { return kI.d() + (kI.t[0] == 'D') + (kI.r[2] == 330); }");
    T("board-2d", 200,
      "static unsigned char board[20][10];"
      "long test(long a, long b) { long n = 0;"
      " for (long r = 0; r < 20; r++) for (long c = 0; c < 10; c++)"
      " { board[r][c] = 1; n += board[r][c]; } return n; }");
    T("shape-tbl", 1,
      "static const signed char kS[2][4][8] = {"
      " { {0,1,1,1,2,1,3,1}, {2,0,2,1,2,2,2,3}, {0,2,1,2,2,2,3,2}, {1,0,1,1,1,2,1,3} },"
      " { {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1} } };"
      "long test(long a, long b) { const signed char *sh = kS[1][2];"
      " return sh[0] == 1 && sh[1] == 0 && kS[0][3][7] == 3; }");
    T("rand7", 1,
      "static unsigned long seed = 1;"
      "short rand7(void) { seed = seed * 1103515245UL + 12345UL;"
      " return (short)((seed >> 16) % 7); }"
      "long test(long a, long b) { short v = rand7();"
      " return v >= 0 && v < 7; }");
    T("memcpy-rows", 9,
      "void *memcpy(void *d, const void *s, unsigned long n);"
      "static unsigned char bd[4][3];"
      "long test(long a, long b) { bd[0][0]=1; bd[0][1]=3; bd[0][2]=5;"
      " memcpy(bd[2], bd[0], 3); return bd[2][0] + bd[2][1] + bd[2][2]; }");
    T("sizeof-div-idiom", 20,
      "typedef struct { unsigned char m; unsigned char d; } Note;"
      "static const Note kK[] = { {76,16},{71,10},{72,10},{74,16},{72,10},"
      " {71,10},{69,16},{69,10},{72,10},{76,16},{74,10},{72,10},{71,26},"
      " {72,10},{74,16},{76,16},{72,16},{69,16},{69,33},{0,10} };\n"
      "#define NK (short)(sizeof(kK)/sizeof(kK[0]))\n"
      "long test(long a, long b) { return NK; }");
    T("cast-chains", -2,
      "long test(long a, long b) { unsigned long long s = 0xFFFFFFFFFFFFFFFEULL;"
      " return (long)(long long)s; }");
    T("bool-ret", 1,
      "typedef unsigned char Boolean;"
      "Boolean fits(long c) { return (Boolean)(c >= 0 && c < 10); }"
      "long test(long a, long b) { return fits(3) && !fits(12); }");

    /* ---- SDK acceptance: SAMPLE.C + the Dostris port run as real apps ------- */
    run_app("SAMPLE.C", "Sample");
    run_app("DOSTRIS.C", "Dostris");

    /* ---- negative cases ------------------------------------------------------------ */
    run_err("err-syntax", "long test(long a, long b) { return 1 +; }", 1, 0);
    run_err("err-undecl", "long test(long a, long b) { return nope; }", 1,
            "undeclared");
    run_err("err-line", "\n\nlong test(long a, long b) {\n  return bad;\n}", 4,
            "undeclared");
    run_err("err-bad-import",
            "long not_an_export(void);"
            "long test(long a, long b) { return not_an_export(); }", 0,
            "undefined function");
    run_err("err-varargs", "long f(long a, ...);", 1, "varargs");
    run_err("err-fnlike-macro", "#define SQ(x) ((x)*(x))\n", 1,
            "function-like");
    run_err("err-struct-byval",
            "struct P { long x, y, z; };"        /* 12 bytes: no register class */
            "long f(struct P p);"
            "long test(long a, long b) { return 0; }", 1, "pointer");
    T("small-struct-byval", 220,
      "typedef struct { short v, h; } Point;"
      "long addpt(Point p) { return p.v + p.h; }"
      "long test(long a, long b) { Point p; p.v = 100; p.h = 120;"
      " return addpt(p); }");
    T("small-struct-import", 220,
      "typedef struct { short v, h; } Point;"
      "void GetMouse(Point *p);"
      "long test(long a, long b) { Point p; GetMouse(&p); return p.v + p.h; }");
    run_err("err-missing-semi", "long g = 5\nlong test(long a, long b) { return g; }",
            2, 0);
    run_err("err-bad-case",
            "long test(long a, long b) { case 3: return 1; }", 1, "outside");

    printf("\nucc_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
