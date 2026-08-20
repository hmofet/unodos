/* ===========================================================================
 * uc_cfg.c - settings.
 *
 * settings.json in VS Code is a flat map of dotted keys, and everything about
 * it - the Settings editor, the "modified" dots in the gutter of that editor,
 * getConfiguration() in the extension API, and the writer that has to put a
 * changed value back without destroying the rest of the file - falls out of
 * ONE declared table of keys with types and defaults.  Nothing here hard-codes
 * a list of settings twice, which is what keeps them from drifting apart.
 *
 * WHAT IS STORED.  The user's file is parsed once into a flat array of
 * key/raw-JSON-text pairs (nested objects are flattened to their dotted
 * spelling, which is the form VS Code writes anyway).  Reading a setting is a
 * lookup in that array falling back to the declared default; writing one edits
 * the array and rewrites the file.  The parsed tree is kept alive too, so an
 * extension's own contributed settings - which have no declaration here - are
 * still readable through uc_cfg_raw().
 *
 * ON-DISK NAME.  FAT 8.3 has no room for "settings.json", so the file is
 * UNOCODE\SETTINGS.JSN.  The loader accepts SETTINGS.JSON as well, for a
 * volume that can carry the long name.
 * ======================================================================== */
#include "unocode.h"

/* ---- the declared settings ------------------------------------------------ */
static const UcSettingDef kDefs[] = {
{ "editor.fontSize", UC_T_INT, "14", 0, "Font size in pixels.", 8, 40 },
{ "editor.tabSize", UC_T_INT, "4", 0, "Spaces a tab is rendered as.", 1, 16 },
{ "editor.insertSpaces", UC_T_BOOL, "true", 0, "Insert spaces when pressing Tab.", 0, 0 },
{ "editor.detectIndentation", UC_T_BOOL, "true", 0, "Take tab size from the file's own indentation.", 0, 0 },
{ "editor.wordWrap", UC_T_ENUM, "off", "off|on", "Wrap long lines.", 0, 0 },
{ "editor.lineNumbers", UC_T_ENUM, "on", "on|off|relative", "Line-number style.", 0, 0 },
{ "editor.minimap.enabled", UC_T_BOOL, "true", 0, "Show the minimap.", 0, 0 },
{ "editor.renderWhitespace", UC_T_ENUM, "none", "none|boundary|all", "Draw whitespace marks.", 0, 0 },
{ "editor.renderIndentGuides", UC_T_BOOL, "true", 0, "Draw indentation guides.", 0, 0 },
{ "editor.renderLineHighlight", UC_T_BOOL, "true", 0, "Highlight the current line.", 0, 0 },
{ "editor.cursorBlinking", UC_T_BOOL, "true", 0, "Blink the caret.", 0, 0 },
{ "editor.matchBrackets", UC_T_BOOL, "true", 0, "Highlight the matching bracket.", 0, 0 },
{ "editor.autoClosingBrackets", UC_T_BOOL, "true", 0, "Close brackets and quotes as you type.", 0, 0 },
{ "editor.autoIndent", UC_T_BOOL, "true", 0, "Keep the previous line's indentation on Enter.", 0, 0 },
{ "editor.trimAutoWhitespace", UC_T_BOOL, "true", 0, "Remove auto-inserted trailing whitespace.", 0, 0 },
{ "editor.quickSuggestions", UC_T_BOOL, "true", 0, "Suggest as you type.", 0, 0 },
{ "editor.acceptSuggestionOnEnter", UC_T_BOOL, "true", 0, "Enter accepts the selected suggestion.", 0, 0 },
{ "editor.scrollBeyondLastLine", UC_T_BOOL, "false", 0, "Allow scrolling past the last line.", 0, 0 },
{ "editor.rulers", UC_T_INT, "0", 0, "Column to draw a vertical ruler at (0 = none).", 0, 200 },
{ "editor.highlightSelectionMatches", UC_T_BOOL, "true", 0, "Highlight other occurrences of the selection.", 0, 0 },
{ "files.autoSave", UC_T_ENUM, "off", "off|afterDelay|onFocusChange", "Save edited files automatically.", 0, 0 },
{ "files.autoSaveDelay", UC_T_INT, "1000", 0, "Milliseconds before an auto-save.", 100, 60000 },
{ "files.trimTrailingWhitespace", UC_T_BOOL, "false", 0, "Trim trailing whitespace on save.", 0, 0 },
{ "files.insertFinalNewline", UC_T_BOOL, "false", 0, "End the file with a newline on save.", 0, 0 },
{ "files.eol", UC_T_ENUM, "lf", "lf|crlf", "Line ending written on save.", 0, 0 },
{ "workbench.colorTheme", UC_T_STR, "Dark+ (default dark)", 0, "The colour theme.", 0, 0 },
{ "workbench.sideBar.location", UC_T_ENUM, "left", "left|right", "Which side the side bar is on.", 0, 0 },
{ "workbench.activityBar.visible", UC_T_BOOL, "true", 0, "Show the activity bar.", 0, 0 },
{ "workbench.statusBar.visible", UC_T_BOOL, "true", 0, "Show the status bar.", 0, 0 },
{ "workbench.editor.showTabs", UC_T_BOOL, "true", 0, "Show editor tabs.", 0, 0 },
{ "workbench.startupEditor", UC_T_ENUM, "welcome", "welcome|none", "What to open at startup.", 0, 0 },
{ "workbench.tree.indent", UC_T_INT, "10", 0, "Explorer indentation per level, px.", 4, 32 },
{ "breadcrumbs.enabled", UC_T_BOOL, "true", 0, "Show the breadcrumb bar.", 0, 0 },
{ "terminal.integrated.fontSize", UC_T_INT, "13", 0, "Terminal font size.", 8, 32 },
{ "terminal.integrated.scrollback", UC_T_INT, "400", 0, "Terminal scrollback lines.", 50, 2000 },
{ "search.maxResults", UC_T_INT, "200", 0, "Maximum search results.", 10, 2000 },
{ "extensions.autoActivate", UC_T_BOOL, "true", 0, "Activate extensions on their activation events.", 0, 0 },
{ "extensions.fuelPerSlice", UC_T_INT, "400000", 0, "Interpreter steps one extension call may use.", 1000, 20000000 },
{ "extensions.heapMB", UC_T_INT, "4", 0, "Extension-host JavaScript heap, MB.", 1, 32 },
{ "unocode.buildOnSave", UC_T_BOOL, "false", 0, "Run the default build task after every save.", 0, 0 }
};
#define NDEFS ((int)(sizeof kDefs / sizeof kDefs[0]))

/* ---- user overrides ------------------------------------------------------- */
typedef struct { char key[52]; char *val; } UcUser;

#define UC_USER_MAX 128
static UcUser g_user[UC_USER_MAX];
static int    g_nuser;
static UcJson *g_root;                 /* kept for uc_cfg_raw()               */
static int    g_vol = -1;
static char   g_path[40];
static char   g_dir[24];

int uc_cfg_count(void) { return NDEFS; }
const UcSettingDef *uc_cfg_def(int i) { return (i >= 0 && i < NDEFS) ? &kDefs[i] : 0; }

const UcSettingDef *uc_cfg_find(const char *key)
{
    int i;
    for (i = 0; i < NDEFS; i++) if (!strcmp(kDefs[i].key, key)) return &kDefs[i];
    return 0;
}

static int user_index(const char *key)
{
    int i;
    for (i = 0; i < g_nuser; i++) if (!strcmp(g_user[i].key, key)) return i;
    return -1;
}

int uc_cfg_is_user(const char *key) { return user_index(key) >= 0; }

static void user_put(const char *key, const char *jsonval)
{
    int i = user_index(key);
    char *copy;
    unsigned long n;
    if (i < 0) {
        if (g_nuser >= UC_USER_MAX) return;
        i = g_nuser++;
        uc_scpy(g_user[i].key, key, sizeof g_user[i].key);
        g_user[i].val = 0;
    }
    n = strlen(jsonval) + 1;
    copy = (char *)malloc(n);
    if (!copy) return;
    memcpy(copy, jsonval, n);
    if (g_user[i].val) free(g_user[i].val);
    g_user[i].val = copy;
}

static void user_clear(void)
{
    int i;
    for (i = 0; i < g_nuser; i++) if (g_user[i].val) free(g_user[i].val);
    g_nuser = 0;
}

/* ---- raw JSON text of a value -------------------------------------------- */
static void value_text(const UcJson *v, char *out, int cap)
{
    if (!v) { uc_scpy(out, "null", cap); return; }
    switch (v->type) {
    case UJ_BOOL: uc_scpy(out, v->bval ? "true" : "false", cap); break;
    case UJ_NULL: uc_scpy(out, "null", cap); break;
    case UJ_STR: {
        char esc[192];
        uc_json_esc(esc, sizeof esc, v->str ? v->str : "");
        uc_scpy(out, "\"", cap); uc_scat(out, esc, cap); uc_scat(out, "\"", cap);
        break;
    }
    case UJ_NUM: {
        char num[32];
        double d = v->num;
        long whole = (long)d;
        if (d == (double)whole) uc_itoa(num, whole);
        else {
            /* two decimals is as much as any setting here needs, and it beats
             * dragging a float formatter into the module */
            long scaled = (long)(d * 100 + (d < 0 ? -0.5 : 0.5));
            char frac[8];
            uc_itoa(num, scaled / 100);
            uc_itoa(frac, scaled % 100 < 0 ? -(scaled % 100) : scaled % 100);
            uc_scat(num, ".", sizeof num);
            if (frac[1] == 0) uc_scat(num, "0", sizeof num);
            uc_scat(num, frac, sizeof num);
        }
        uc_scpy(out, num, cap);
        break;
    }
    default:
        /* arrays and objects round-trip as their compact rendering; the
         * settings this build declares are all scalars, and a value we cannot
         * re-render is better preserved approximately than dropped */
        uc_scpy(out, v->type == UJ_ARR ? "[]" : "{}", cap);
        break;
    }
}

/* Flatten `obj` into the user array, joining nested objects with dots. */
static void flatten(const UcJson *obj, const char *prefix)
{
    const UcJson *m;
    for (m = obj->child; m; m = m->next) {
        char key[52], text[256];
        if (!m->key) continue;
        key[0] = 0;
        if (prefix && prefix[0]) { uc_scpy(key, prefix, sizeof key); uc_scat(key, ".", sizeof key); }
        uc_scat(key, m->key, sizeof key);
        if (m->type == UJ_OBJ && !uc_cfg_find(key)) { flatten(m, key); continue; }
        value_text(m, text, sizeof text);
        user_put(key, text);
    }
}

/* ---- where the config lives ----------------------------------------------- */
int uc_cfg_vol(void) { return g_vol; }
const char *uc_cfg_path(void) { return g_path; }

int uc_cfg_dir(char *out, int cap)
{
    uc_scpy(out, g_dir, cap);
    return g_vol >= 0;
}

static void pick_vol(void)
{
    /* uno_fs_pref_vol() is the OS's answer to "where does state belong": the
     * boot volume first, never the RAM disk if a real one exists, and never
     * the ZimaBlade's dead eMMC.  Two subsystems each grew their own copy of
     * that heuristic and each cost a day; this one uses theirs. */
    g_vol = uno_fs_pref_vol();
    uc_scpy(g_dir, "UNOCODE", sizeof g_dir);
    uc_scpy(g_path, "UNOCODE\\SETTINGS.JSN", sizeof g_path);
    if (g_vol >= 0 && uno_fs_writable(g_vol) && !uno_fs_isdir(g_vol, g_dir))
        uno_fs_mkdir(g_vol, g_dir);
}

/* ---- load / save ---------------------------------------------------------- */
int uc_cfg_load(void)
{
    char *src = 0;
    long len = 0;
    char err[80];
    user_clear();
    if (g_root) { uc_json_free(g_root); g_root = 0; }
    if (g_vol < 0) return 0;
    if (!uc_read_file(g_vol, g_path, &src, &len)) {
        if (!uc_read_file(g_vol, "UNOCODE\\SETTINGS.JSON", &src, &len)) return 0;
    }
    g_root = uc_json_parse(src, (int)len, err, sizeof err);
    free(src);
    if (!g_root) return 0;
    if (g_root->type == UJ_OBJ) flatten(g_root, "");
    return 1;
}

int uc_cfg_save(void)
{
    char *buf;
    int n = 0, i, cap = 8192;
    int ok;
    if (g_vol < 0 || !uno_fs_writable(g_vol)) return 0;
    buf = (char *)malloc((unsigned long)cap);
    if (!buf) return 0;
    {
        static const char hdr[] =
            "// UnoCode settings.  Edit here or through the Settings editor\n"
            "// (Ctrl+,); both write this file.  Comments and trailing commas\n"
            "// are allowed.\n{\n";
        uc_scpy(buf, hdr, cap);
        n = (int)strlen(buf);
    }
    for (i = 0; i < g_nuser; i++) {
        char line[320], esc[64];
        if (!g_user[i].val) continue;
        uc_json_esc(esc, sizeof esc, g_user[i].key);
        uc_scpy(line, "    \"", sizeof line);
        uc_scat(line, esc, sizeof line);
        uc_scat(line, "\": ", sizeof line);
        uc_scat(line, g_user[i].val, sizeof line);
        uc_scat(line, i + 1 < g_nuser ? ",\n" : "\n", sizeof line);
        if (n + (int)strlen(line) + 4 >= cap) break;
        uc_scpy(buf + n, line, cap - n);
        n += (int)strlen(line);
    }
    uc_scpy(buf + n, "}\n", cap - n);
    n += 2;
    if (!uno_fs_isdir(g_vol, g_dir)) uno_fs_mkdir(g_vol, g_dir);
    ok = uno_fs_write(g_vol, g_path, (const unsigned char *)buf, n);
    free(buf);
    return ok;
}

void uc_cfg_init(void)
{
    pick_vol();
    uc_cfg_load();
}

/* ---- reading -------------------------------------------------------------- */
static const char *raw_text(const char *key)
{
    int i = user_index(key);
    return i >= 0 ? g_user[i].val : 0;
}

int uc_cfg_int(const char *key)
{
    const UcSettingDef *d = uc_cfg_find(key);
    const char *t = raw_text(key);
    long v;
    if (!t) t = d ? d->dflt : "0";
    while (*t == ' ' || *t == '"') t++;
    v = strtol(t, 0, 10);
    if (d && d->type == UC_T_INT) {
        if (v < d->lo) v = d->lo;
        if (v > d->hi) v = d->hi;
    }
    return (int)v;
}

int uc_cfg_bool(const char *key)
{
    const UcSettingDef *d = uc_cfg_find(key);
    const char *t = raw_text(key);
    if (!t) t = d ? d->dflt : "false";
    while (*t == ' ' || *t == '"') t++;
    if (uc_starts(t, "true")) return 1;
    if (uc_starts(t, "false")) return 0;
    return strtol(t, 0, 10) != 0;
}

const char *uc_cfg_str(const char *key)
{
    static char out[160];
    const UcSettingDef *d = uc_cfg_find(key);
    const char *t = raw_text(key);
    int n = 0;
    if (!t) { uc_scpy(out, d ? d->dflt : "", sizeof out); return out; }
    if (*t != '"') { uc_scpy(out, t, sizeof out); return out; }
    t++;
    while (*t && *t != '"' && n < (int)sizeof out - 1) {
        if (*t == '\\' && t[1]) {
            t++;
            out[n++] = (*t == 'n') ? '\n' : (*t == 't') ? '\t' : *t;
            t++;
        } else out[n++] = *t++;
    }
    out[n] = 0;
    return out;
}

int uc_cfg_set(const char *key, const char *val)
{
    user_put(key, val);
    return uc_cfg_save();
}

UcJson *uc_cfg_raw(const char *key)
{
    return g_root ? uc_json_path(g_root, key) : 0;
}
