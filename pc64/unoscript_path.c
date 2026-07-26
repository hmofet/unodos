/* ===========================================================================
 * unoscript_path - pure path helpers (see unoscript_path.h).  Freestanding: no
 * libc beyond what is inlined here, so it links into both the OS image and the
 * standalone host test unchanged.
 * ======================================================================== */
#include "unoscript_path.h"

/* --- tiny inlined string ops (no libc dependency) ---------------------- */
static int p_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* decimal of an unsigned long into buf (min 1 digit); returns length. */
static int p_utoa(unsigned long v, char *b)
{
    char t[24]; int i = 0, n = 0;
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i) b[n++] = t[--i];
    b[n] = 0;
    return n;
}

int uscp_has_traversal(const char *s)
{
    int i = 0;
    if (!s || !s[0]) return 1;                 /* empty path is unsafe        */
    for (;;) {
        int start = i;
        /* scan one component up to '/' or end */
        while (s[i] && s[i] != '/') i++;
        {
            int comp = i - start;              /* component length            */
            if (comp == 0) return 1;           /* "", "//", trailing '/'      */
            if (comp == 1 && s[start] == '.') return 1;              /* "."   */
            if (comp == 2 && s[start] == '.' && s[start + 1] == '.')
                return 1;                                            /* ".."  */
        }
        if (!s[i]) return 0;                   /* reached the end, all clean  */
        i++;                                   /* step over the '/'           */
    }
}

int uscp_home_name(unsigned long uid, const char *rel, char *out, int cap)
{
    static const char PFX[] = "USERS/";
    int n = 0, i, rl;
    char idb[24];
    int idn = p_utoa(uid, idb);
    if (!rel || !out || cap <= 0) { if (out && cap > 0) out[0] = 0; return -1; }
    rl = p_len(rel);
    /* "USERS/" + <uid> + "/" + rel + NUL */
    if ((int)(sizeof PFX - 1) + idn + 1 + rl + 1 > cap) { out[0] = 0; return -1; }
    for (i = 0; i < (int)(sizeof PFX - 1); i++) out[n++] = PFX[i];
    for (i = 0; i < idn; i++) out[n++] = idb[i];
    out[n++] = '/';
    for (i = 0; i < rl; i++) out[n++] = rel[i];
    out[n] = 0;
    return n;
}

int uscp_under_home(unsigned long uid, const char *name)
{
    static const char PFX[] = "USERS/";
    char idb[24];
    int idn, i, k = 0;
    if (!name) return 0;
    for (i = 0; i < (int)(sizeof PFX - 1); i++)
        if (name[k++] != PFX[i]) return 0;
    idn = p_utoa(uid, idb);
    for (i = 0; i < idn; i++)
        if (name[k++] != idb[i]) return 0;
    if (name[k++] != '/') return 0;            /* exact "USERS/<uid>/"        */
    return name[k] != 0;                        /* something after the prefix  */
}

int uscp_split_abs(const char *path, char *lab, int labcap, const char **rest)
{
    int n = 0;
    if (!path || path[0] != '/' || !lab || labcap <= 0 || !rest) return -1;
    path++;                                    /* skip leading '/'            */
    while (*path && *path != '/') {
        if (n >= labcap - 1) return -1;        /* label overflow              */
        lab[n++] = *path++;
    }
    lab[n] = 0;
    if (n == 0) return -1;                     /* "/" or "//..." -> empty lab */
    if (*path != '/') return -1;               /* need a '/rest' after label  */
    path++;                                    /* skip the separator          */
    if (!*path) return -1;                     /* no rest                     */
    *rest = path;
    return 0;
}
