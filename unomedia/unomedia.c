/* ===========================================================================
 * unomedia core - the pieces every format family shares: the registered
 * allocator, the error surface, and the single open byte source (see
 * unomedia.h). The image dispatch lives in um_image.c and the audio
 * dispatch in um_audio.c, so a build links only the families it uses:
 * pc64's kernel takes core+audio (Music), PHOTOS.UNO takes core+image -
 * two separate instances, which is why "one open stream per instance"
 * never bites in practice.
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>

/* ---- allocator ------------------------------------------------------------ */
static void *(*g_alloc)(unsigned long);
static void  (*g_free)(void *);

void um_set_alloc(void *(*a)(unsigned long), void (*f)(void *))
{ g_alloc = a; g_free = f; }

void *um_alloc(unsigned long n) { return g_alloc ? g_alloc(n) : 0; }
void  um_free(void *p)          { if (p && g_free) g_free(p); }

/* ---- error surface -------------------------------------------------------- */
static const char *g_err = "";
const char *um_error(void)             { return g_err; }
void        um_set_error(const char *w){ g_err = w ? w : ""; }

/* ---- the open source (shared; owner-tagged so the two families cannot
 * silently steal it from each other within one instance) ------------------- */
static um_src g_src;
static int    g_owner;                  /* UM_OWNER_* , 0 = free */

int um_src_open(const um_src *src, int owner)
{
    if (g_owner && g_owner != owner) {
        um_set_error("another stream is open in this unomedia instance");
        return 0;
    }
    if (!src || !src->read) { um_set_error("bad byte source"); return 0; }
    g_src = *src;
    g_owner = owner;
    return 1;
}

void um_src_close(int owner)
{
    if (g_owner == owner) { g_owner = 0; memset(&g_src, 0, sizeof g_src); }
}

long um_size(void) { return g_owner ? g_src.size : 0; }

long um_read(long off, unsigned char *dst, long n)
{
    if (!g_owner || !g_src.read) return -1;
    if (off < 0 || n <= 0) return 0;
    if (off >= g_src.size) return 0;
    if (n > g_src.size - off) n = g_src.size - off;
    return g_src.read(g_src.ctx, off, dst, n);
}

/* ---- extension helper (shared by both dispatchers) ------------------------ */
void um_ext_of(const char *name, char *out /* [8] */)
{
    const char *dot = 0, *p;
    int i = 0;
    out[0] = 0;
    if (!name) return;
    for (p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return;
    for (p = dot + 1; *p && i < 7; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i++] = c;
    }
    out[i] = 0;
}
