/* ===========================================================================
 * UnoDOS/pc64 - key bindings and small app preferences.  See uno_binds.h.
 * ======================================================================== */
#include <string.h>
#include "uno_binds.h"
#include "hid_kbd.h"      /* UNO_KH_* - the bitmap these bindings feed */
#include "pc64_fs.h"      /* uno_fs_read / uno_fs_write / uno_fs_pref_vol */

#define CFG_NAME "UNOPREF.CFG"
#define NACT     8        /* the eight UNO_KH_* actions */
#define NKEY     2        /* keys per action; a menu prints at most two */
#define NPREF    8
#define PREF_NAME 16
#define PREF_VAL  24

typedef struct { int action; int k[NKEY]; } bind_row;

/* The shipped bindings, and they are EXACTLY what this machine did before any
 * of this existed: arrows, F or either Ctrl, Space or E, comma, period.  A
 * default that "improves" on that would be a change to how every app reads the
 * keyboard, arriving inside a commit about a menu. */
static const bind_row kDefault[NACT] = {
    { UNO_KH_UP,     { UNO_BK_UP,    0 } },
    { UNO_KH_DOWN,   { UNO_BK_DOWN,  0 } },
    { UNO_KH_RIGHT,  { UNO_BK_RIGHT, 0 } },
    { UNO_KH_LEFT,   { UNO_BK_LEFT,  0 } },
    { UNO_KH_FIRE,   { 'f',          UNO_BK_CTRL } },
    { UNO_KH_USE,    { ' ',          'e' } },
    { UNO_KH_SLEFT,  { ',',          0 } },
    { UNO_KH_SRIGHT, { '.',          0 } },
};

static bind_row g_bind[NACT];
static struct { char name[PREF_NAME]; char val[PREF_VAL]; } g_pref[NPREF];
static int g_npref;
static int g_loaded;                 /* 0 = untouched, 1 = defaults + file */
static int g_loading;                /* re-entry guard for the lazy load */

/* ---- store ---------------------------------------------------------------- */
static void set_defaults(void)
{
    int i, j;
    for (i = 0; i < NACT; i++) {
        g_bind[i].action = kDefault[i].action;
        for (j = 0; j < NKEY; j++) g_bind[i].k[j] = kDefault[i].k[j];
    }
    g_npref = 0;
}

static int num(const char *v)
{
    int n = 0, neg = 0;
    if (*v == '-') { neg = 1; v++; }
    while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
    return neg ? -n : n;
}

static bind_row *row(int action)
{
    int i;
    for (i = 0; i < NACT; i++) if (g_bind[i].action == action) return &g_bind[i];
    return 0;
}

static void cfg_apply(char *buf, long len)
{
    long i = 0;
    while (i < len) {
        char *line = buf + i, *eq;
        long j = i;
        while (j < len && buf[j] != '\n' && buf[j] != '\r') j++;
        buf[j] = 0;
        i = j + 1;
        while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || !*line) continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        while (*eq == ' ') eq++;
        if (!strncmp(line, "bind.", 5)) {
            bind_row *r = row(num(line + 5));
            int n = 0;
            if (!r) continue;
            r->k[0] = r->k[1] = 0;
            while (*eq && n < NKEY) {
                r->k[n++] = num(eq);
                while (*eq && *eq != ' ') eq++;
                while (*eq == ' ') eq++;
            }
        } else if (!strncmp(line, "pref.", 5)) {
            const char *nm = line + 5;
            if (g_npref < NPREF && strlen(nm) < PREF_NAME &&
                strlen(eq) < PREF_VAL) {
                strcpy(g_pref[g_npref].name, nm);
                strcpy(g_pref[g_npref].val, eq);
                g_npref++;
            }
        }
    }
}

/* Lazy, and never from a make/break handler.  The guard matters: uno_fs_read
 * can end up asking the keyboard nothing at all, but it CAN take a while, and
 * a second entry while the first is mid-read would parse into a half-set
 * table. */
static void load(void)
{
    static unsigned char buf[1024];
    int v;
    long n;
    if (g_loaded || g_loading) return;
    g_loading = 1;
    set_defaults();
    v = uno_fs_pref_vol();
    if (v >= 0) {
        n = uno_fs_read(v, CFG_NAME, buf, (long)sizeof buf - 1);
        if (n > 0) { buf[n] = 0; cfg_apply((char *)buf, n); }
    }
    g_loaded = 1;
    g_loading = 0;
}

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static char *put_int(char *p, int v)
{
    char t[12]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static void save(void)
{
    static unsigned char buf[1024];
    char *p = (char *)buf;
    int i, j, v;
    if (!g_loaded) return;
    p = put_str(p, "# UnoDOS key bindings and app preferences.\r\n");
    p = put_str(p, "# Delete this file to go back to the defaults.\r\n");
    for (i = 0; i < NACT; i++) {
        p = put_str(p, "bind."); p = put_int(p, g_bind[i].action);
        *p++ = '=';
        for (j = 0; j < NKEY; j++) {
            if (!g_bind[i].k[j]) continue;
            if (j) *p++ = ' ';
            p = put_int(p, g_bind[i].k[j]);
        }
        *p++ = '\r'; *p++ = '\n';
    }
    for (i = 0; i < g_npref; i++) {
        p = put_str(p, "pref."); p = put_str(p, g_pref[i].name);
        *p++ = '='; p = put_str(p, g_pref[i].val);
        *p++ = '\r'; *p++ = '\n';
    }
    v = uno_fs_pref_vol();
    if (v >= 0) uno_fs_write(v, CFG_NAME, buf, (long)(p - (char *)buf));
}

void uno_binds_reload(void) { if (!g_loading) g_loaded = 0; }

/* ---- the keyboard path ---------------------------------------------------- */
int uno_bind_bits(int keyid)
{
    int i, j, m = 0;
    if (!keyid) return 0;
    load();
    for (i = 0; i < NACT; i++)
        for (j = 0; j < NKEY; j++)
            if (g_bind[i].k[j] == keyid) { m |= g_bind[i].action; break; }
    return m;
}

int uno_bind_keyid(int uni, int scan)
{
    switch (scan) {
    case 1: return UNO_BK_UP;
    case 2: return UNO_BK_DOWN;
    case 3: return UNO_BK_RIGHT;
    case 4: return UNO_BK_LEFT;
    default: break;
    }
    if (uni >= 'A' && uni <= 'Z') uni += 32;      /* bind the key, not the case */
    if (uni >= 32 && uni < 127) return uni;
    return 0;
}

/* ---- the menu's side ------------------------------------------------------ */
static const char *key_word(int k)
{
    switch (k) {
    case UNO_BK_UP:    return "Up";
    case UNO_BK_DOWN:  return "Down";
    case UNO_BK_RIGHT: return "Right";
    case UNO_BK_LEFT:  return "Left";
    case UNO_BK_CTRL:  return "Ctrl";
    case ' ':          return "Space";
    default:           return 0;
    }
}

int uno_bind_name(int action, char *buf, int cap)
{
    bind_row *r;
    int j, n = 0;
    if (cap <= 0) return 0;
    buf[0] = 0;
    load();
    r = row(action);
    if (!r) return 0;
    for (j = 0; j < NKEY; j++) {
        const char *w;
        int k = r->k[j];
        if (!k) continue;
        if (n && n + 3 < cap) { buf[n++] = ' '; buf[n++] = '/'; buf[n++] = ' '; }
        w = key_word(k);
        if (w) {
            while (*w && n < cap - 1) buf[n++] = *w++;
        } else if (n < cap - 1) {
            /* a character key prints as the key cap: upper case for a letter */
            buf[n++] = (char)((k >= 'a' && k <= 'z') ? k - 32 : k);
        }
    }
    buf[n] = 0;
    return n;
}

int uno_bind_set(int action, int keyid)
{
    bind_row *r;
    int i, j;
    if (!keyid) return 0;
    load();
    /* Use is read as a key EVENT here, not from the bitmap this table feeds,
     * so a rebind would be stored and then do nothing.  Say no instead. */
    if (action == UNO_KH_USE) return 0;
    r = row(action);
    if (!r) return 0;
    for (i = 0; i < NACT; i++)
        for (j = 0; j < NKEY; j++)
            if (g_bind[i].k[j] == keyid) g_bind[i].k[j] = 0;
    r->k[0] = keyid;
    r->k[1] = 0;
    save();
    return 1;
}

void uno_bind_reset(void)
{
    load();
    set_defaults();
    save();
}

/* ---- preferences ---------------------------------------------------------- */
int uno_pref_get(const char *name, char *buf, int cap)
{
    int i, n = 0;
    if (cap <= 0) return 0;
    buf[0] = 0;
    load();
    for (i = 0; i < g_npref; i++)
        if (!strcmp(g_pref[i].name, name)) {
            const char *v = g_pref[i].val;
            while (*v && n < cap - 1) buf[n++] = *v++;
            buf[n] = 0;
            return n;
        }
    return 0;
}

int uno_pref_set(const char *name, const char *value)
{
    int i;
    if (!name || !value) return 0;
    if (strlen(name) >= PREF_NAME || strlen(value) >= PREF_VAL) return 0;
    load();
    for (i = 0; i < g_npref; i++)
        if (!strcmp(g_pref[i].name, name)) {
            strcpy(g_pref[i].val, value);
            save();
            return 1;
        }
    if (g_npref >= NPREF) return 0;
    strcpy(g_pref[g_npref].name, name);
    strcpy(g_pref[g_npref].val, value);
    g_npref++;
    save();
    return 1;
}
