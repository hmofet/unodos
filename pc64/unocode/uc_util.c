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

/* ---- the assistant's file context (UCD-58) ---------------------------------
 * Without this the model is asked about code it cannot see, and it says so:
 * "I don't see a program in your message. Could you please paste the code"
 * was the live answer to "in one sentence, what does this program do?" with
 * the file open on the other half of the screen.
 *
 * TRUNCATION IS SAID IN THE TEXT, not just returned in the flag.  A model
 * handed the first 24 KB of a 40 KB file and told nothing will answer about
 * the whole file with complete confidence, and the part it never saw is
 * exactly the part the reader cannot check.
 */
static void ctx_put(char *out, int *p, int cap, const char *s)
{
    if (!s) return;
    while (*s && *p < cap - 1) out[(*p)++] = *s++;
    out[*p] = 0;
}

int uc_ctx_file(char *out, int cap, const char *name, const char *text,
                int len, int *truncated)
{
    int p = 0, room, n, i;
    char num[24];

    if (truncated) *truncated = 0;
    if (cap <= 0) return 0;
    out[0] = 0;
    if (!text || len <= 0) return 0;
    if (!name || !name[0]) name = "an untitled file";

    ctx_put(out, &p, cap, "The user is editing a file called ");
    ctx_put(out, &p, cap, name);
    ctx_put(out, &p, cap, " in UnoCode. Its contents follow.\n\n----8<---- ");
    ctx_put(out, &p, cap, name);
    ctx_put(out, &p, cap, " ----8<----\n");

    /* Reserve the footer's room BEFORE copying, so the sentence that says the
     * file was cut can never itself be the thing that gets cut. */
    room = cap - 1 - p - 96;
    if (room < 0) room = 0;
    n = len > room ? room : len;
    for (i = 0; i < n; i++) out[p + i] = text[i];
    p += n;
    out[p] = 0;

    if (n < len) {
        if (truncated) *truncated = 1;
        ctx_put(out, &p, cap, "\n----8<---- truncated: the first ");
        uc_itoa(num, n);
        ctx_put(out, &p, cap, num);
        ctx_put(out, &p, cap, " bytes of ");
        uc_itoa(num, len);
        ctx_put(out, &p, cap, num);
        ctx_put(out, &p, cap, " ----8<----\n");
    }
    return p;
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
