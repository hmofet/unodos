/* uodesk_plat_common.c - the small pieces every backend needs the same way. */
#include <string.h>
#include <ctype.h>
#include "uodesk_plat.h"

/* "Word Document (*.doc;*.docx)" -> {"*.doc", "*.docx"}; how many */
int plat_type_patterns(const char *type, char pats[][16], int maxp)
{
    const char *p = type ? strrchr(type, '(') : 0;
    int n = 0;
    if (!p) return 0;
    p++;
    while (*p && *p != ')' && n < maxp) {
        int k = 0;
        while (*p == ' ' || *p == ';' || *p == ',') p++;
        while (*p && *p != ';' && *p != ',' && *p != ')' && *p != ' ' && k < 15)
            pats[n][k++] = *p++;
        pats[n][k] = 0;
        if (k) n++;
        while (*p && *p != ';' && *p != ',' && *p != ')') p++;   /* overlong */
    }
    return n;
}

/* which of `types` a chosen file's extension belongs to (0 if none does) */
int plat_type_of_path(const char *path, const char *const *types, int ntypes)
{
    const char *dot = path ? strrchr(path, '.') : 0;
    int t, i;
    if (!dot) return 0;
    for (t = 0; t < ntypes; t++) {
        char pats[8][16];
        int n = plat_type_patterns(types[t], pats, 8);
        for (i = 0; i < n; i++) {
            const char *a = dot, *b = strchr(pats[i], '.');
            if (!b) continue;
            while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
            if (!*a && !*b) return t;
        }
    }
    return 0;
}

/* fb_px is R,G,B,A in memory; Win32 DIBs and X11 TrueColor want B,G,R,X */
void plat_rgba_to_bgrx(unsigned char *dst, const fb_px *src, int npix)
{
    int i;
    for (i = 0; i < npix; i++) {
        fb_px p = src[i];
        dst[0] = (unsigned char)(p >> 16);
        dst[1] = (unsigned char)(p >> 8);
        dst[2] = (unsigned char)p;
        dst[3] = 0xFF;
        dst += 4;
    }
}
