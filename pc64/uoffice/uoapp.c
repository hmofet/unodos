/* ===========================================================================
 * uoapp.c - the host seam, the unsaved-changes guard, the clipboard and the
 * CP-1252 conversions (see uoapp.h).                         [EXPERIMENTAL]
 *
 * Freestanding like the rest of the lane: no libc headers, the module's own
 * malloc/free, which pc64 exports and the desktop links from its libc.
 * ======================================================================== */
#include "uoapp.h"

void *malloc(unsigned long);
void  free(void *);

static const uoa_app  *g_app;
static const uoa_host *g_host;

void uoa_register(const uoa_app *a) { g_app = a; }
void uoa_set_host(const uoa_host *h) { g_host = h; }

static void s_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }

/* ---- the guard ---------------------------------------------------------------
 * One pending action, and whether a Save As the guard opened is outstanding.
 * An app has one document and one modal dialog at a time, so one of each is
 * all there can ever be. */
static int  g_pending;             /* UOA_* waiting on the prompt / Save As  */
static int  g_await_saveas;
static int  g_open_vol;
static char g_open_name[256];

int         uoa_open_vol(void)  { return g_open_vol; }
const char *uoa_open_name(void) { return g_open_name; }

static void quit_now(void)
{
    if (g_host && g_host->quit) g_host->quit();
}

static void run(int action)
{
    g_pending = UOA_NONE;
    g_await_saveas = 0;
    if (action == UOA_CLOSE) { quit_now(); return; }
    if (action != UOA_NONE && g_app && g_app->proceed) g_app->proceed(action);
}

int uoa_request(int action)
{
    char msg[320];
    int k = 0;
    const char *pre = "Do you want to save the changes you made to ", *nm;
    if (!g_app || !g_app->dirty || !g_app->dirty() || !g_app->dl) {
        run(action);
        return 1;
    }
    g_pending = action;
    g_await_saveas = 0;
    nm = g_app->doc_name ? g_app->doc_name() : "";
    while (*pre && k < (int)sizeof msg - 2) msg[k++] = *pre++;
    while (nm && *nm && k < (int)sizeof msg - 2) msg[k++] = *nm++;
    msg[k++] = '?';
    msg[k] = 0;
    uod_msgbox(g_app->dl, g_app->app ? g_app->app : "", msg, UOD_MB_YESNOCANCEL,
               g_app->frame_w ? g_app->frame_w() : 640,
               g_app->frame_h ? g_app->frame_h() : 400);
    if (g_app->prompted) g_app->prompted();
    return 0;
}

void uoa_prompt_closed(void)
{
    int res, act = g_pending;
    if (!g_app || !g_app->dl) return;
    res = uod_result(g_app->dl);
    if (res == UOD_ID_NO) { run(act); return; }
    if (res != UOD_ID_YES) { g_pending = UOA_NONE; return; }   /* Cancel, Esc */
    switch (g_app->save ? g_app->save() : 0) {
    case 1:  run(act); break;
    case 2:  g_await_saveas = 1; break;       /* Save As is up; wait for it */
    default: g_pending = UOA_NONE; break;     /* it failed: keep the document */
    }
}

void uoa_save_as_done(int ok)
{
    if (!g_await_saveas) return;
    g_await_saveas = 0;
    if (ok) run(g_pending);
    else g_pending = UOA_NONE;
}

int  uoa_key_mods(void) { return g_host && g_host->mods ? g_host->mods() : 0; }

int  uoa_can_exit(void) { return g_host && g_host->quit; }
void uoa_exit(void) { if (uoa_can_exit()) uoa_request(UOA_CLOSE); }

/* ---- the host's side ----------------------------------------------------------- */
int uoa_host_close(void)
{
    if (!g_app || !g_app->dirty || !g_app->dirty()) return 1;
    /* already asking (the close box pressed twice): leave the prompt alone */
    if (g_pending != UOA_NONE) return 0;
    uoa_request(UOA_CLOSE);
    return 0;
}

void uoa_host_open(int vol, const char *name)
{
    g_open_vol = vol;
    s_cpy(g_open_name, name, (int)sizeof g_open_name);
    if (g_pending != UOA_NONE) return;        /* a prompt is up: not now      */
    uoa_request(UOA_OPEN_FILE);
}

int uoa_host_dirty(void) { return g_app && g_app->dirty && g_app->dirty(); }

void uoa_host_tick(void)
{
    static int last = -1;
    int d;
    if (!g_app || !g_app->dirty || !g_host || !g_host->modified) return;
    d = g_app->dirty() ? 1 : 0;
    if (d != last) { last = d; g_host->modified(d); }
}

/* ---- the clipboard ---------------------------------------------------------------- */
static char *g_clip;                /* the in-app clipboard, when no host has one */

int uoa_clip_set(const char *utf8)
{
    long n = 0, i;
    if (!utf8) utf8 = "";
    if (g_host && g_host->clip_set) return g_host->clip_set(utf8);
    while (utf8[n]) n++;
    if (g_clip) free(g_clip);
    g_clip = (char *)malloc((unsigned long)n + 1);
    if (!g_clip) return 0;
    for (i = 0; i <= n; i++) g_clip[i] = utf8[i];
    return 1;
}

char *uoa_clip_get(void)
{
    char *out;
    long n = 0, i;
    if (g_host && g_host->clip_get) {
        char probe[1];
        long need = g_host->clip_get(probe, 1);
        if (need <= 0) return 0;
        out = (char *)malloc((unsigned long)need + 1);
        if (!out) return 0;
        g_host->clip_get(out, need + 1);
        return out;
    }
    if (!g_clip || !g_clip[0]) return 0;
    while (g_clip[n]) n++;
    out = (char *)malloc((unsigned long)n + 1);
    if (!out) return 0;
    for (i = 0; i <= n; i++) out[i] = g_clip[i];
    return out;
}

/* ---- CP-1252 ---------------------------------------------------------------------
 * Only 0x80..0x9F differ from Latin-1.  The five slots CP-1252 leaves
 * undefined (0x81 0x8D 0x8F 0x90 0x9D) pass through as C1 controls, which is
 * what unodoc does (ud_cp1252_to_uc), so a byte always survives the trip. */
static const unsigned short k1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

int uoa_1252_to_uc(int b)
{
    b &= 0xFF;
    return (b >= 0x80 && b < 0xA0) ? k1252[b - 0x80] : b;
}

int uoa_uc_to_1252(int uc)
{
    int i;
    if (uc < 0) return -1;
    if (uc < 0x80 || (uc >= 0xA0 && uc < 0x100)) return uc;
    for (i = 0; i < 32; i++) if (k1252[i] == uc) return 0x80 + i;
    return -1;
}

static int put_u8(int cp, char *o)
{
    if (cp < 0x80)  { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6));
                      o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | (cp >> 12));
    o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;                           /* CP-1252 is all inside the BMP */
}

int uoa_to_utf8(const char *s, long n, char *out, int cap)
{
    long i;
    int k = 0;
    if (!out || cap <= 0) return 0;
    for (i = 0; s && (n < 0 ? s[i] != 0 : i < n); i++) {
        char b[3];
        int m = put_u8(uoa_1252_to_uc((unsigned char)s[i]), b), j;
        if (k + m > cap - 1) break;
        for (j = 0; j < m; j++) out[k++] = b[j];
    }
    out[k] = 0;
    return k;
}

/* Strict, the way pc64/uno_utf8.h is: an over-long form, a surrogate or a
 * truncated tail is ONE bad byte, so the caller's step is always safe. */
int uoa_u8_get(const char *s, int n, int *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    int c, need, i, v;
    if (n <= 0) { *cp = 0; return 0; }
    c = p[0];
    if (c < 0x80)                { *cp = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; }
    else                         { *cp = 0xFFFD; return 1; }
    if (n <= need) { *cp = 0xFFFD; return 1; }
    for (i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    if ((need == 1 && v < 0x80) || (need == 2 && v < 0x800) ||
        (need == 3 && v < 0x10000) || v > 0x10FFFF ||
        (v >= 0xD800 && v <= 0xDFFF)) { *cp = 0xFFFD; return 1; }
    *cp = v;
    return need + 1;
}

long uoa_from_utf8(const char *u, char *out, long cap)
{
    long k = 0, n = 0, i = 0;
    if (!out || cap <= 0) return 0;
    while (u && u[n]) n++;
    while (i < n && k < cap - 1) {
        int cp, step = uoa_u8_get(u + i, (int)(n - i > 4 ? 4 : n - i), &cp), b;
        i += step > 0 ? step : 1;
        if (cp == '\r' && i < n && u[i] == '\n') continue;   /* CRLF -> LF */
        b = uoa_uc_to_1252(cp);
        out[k++] = (char)(b < 0 ? '?' : b);
    }
    out[k] = 0;
    return k;
}
