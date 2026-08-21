/* ===========================================================================
 * uc_util.c - the string helpers every other UnoCode file uses.
 *
 * They live in their own translation unit for one reason: the host test
 * (tools/uc_test.c) links uc_json.c and uc_rx.c and nothing else, and both of
 * those need uc_scpy/uc_scat/uc_itoa.  Putting them in uc_main.c would drag
 * the whole workbench - and the toolkit, and the framebuffer - into a test
 * that exists to check a parser.
 * ======================================================================== */
#include "unocode.h"

void uc_scpy(char *d, const char *s, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    if (!s) { d[0] = 0; return; }
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void uc_scat(char *d, const char *s, int cap)
{
    int i = 0, j;
    if (cap <= 0 || !s) return;
    while (d[i] && i < cap - 1) i++;
    for (j = 0; s[j] && i < cap - 1; j++) d[i++] = s[j];
    d[i] = 0;
}

static int uc_low(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int uc_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (uc_low((unsigned char)*a) != uc_low((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int uc_starts(const char *s, const char *pfx)
{
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}

int uc_ends_icase(const char *s, const char *suffix)
{
    int ls = (int)strlen(s), lx = (int)strlen(suffix), i;
    if (lx > ls) return 0;
    for (i = 0; i < lx; i++)
        if (uc_low((unsigned char)s[ls - lx + i]) != uc_low((unsigned char)suffix[i]))
            return 0;
    return 1;
}

void uc_upper(char *s)
{
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

int uc_itoa(char *out, long v)
{
    char t[24];
    int n = 0, i = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) out[i++] = '-';
    while (n) out[i++] = t[--n];
    out[i] = 0;
    return i;
}

int uc_is_word(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* ---- UTF-8 -------------------------------------------------------------------
 * The document stays a BYTE buffer.  UTF-8 is what files hold and what the
 * clipboard carries, and transcoding on every load and save to store something
 * wider would buy nothing; what the rest of the editor needs instead is a way
 * to walk those bytes a CHARACTER at a time, so a caret lands between
 * characters, Backspace removes one, and a column counts one.
 *
 * The decoder itself lives in pc64/uno_utf8.h, not here, because pc64_font.c
 * needs it too and sits on the other side of a link boundary - it is in the
 * kernel, this file ships inside UNOCODE.UNO.  These are the editor's names
 * for it; see that header for why it decodes strictly. */
#include "uno_utf8.h"

int uc_u8_get(const char *s, int n, int *cp) { return uno_u8_get(s, n, cp); }
int uc_u8_len(int cp)                        { return uno_u8_len(cp); }
int uc_u8_put(int cp, char *out)             { return uno_u8_put(cp, out); }
int uc_u8_align(const char *s, int i)        { return uno_u8_align(s, i); }
int uc_cp_width(int cp)                      { return uno_cp_width(cp); }

/* The offset of the character before `i`, or 0. */
int uc_u8_back(const char *s, int i)
{
    if (i <= 0) return 0;
    return uno_u8_align(s, i - 1);
}
