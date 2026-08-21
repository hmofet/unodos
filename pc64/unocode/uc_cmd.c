/* ===========================================================================
 * uc_cmd.c - commands, keybindings, `when` clauses and the quick-input
 * overlays (the Command Palette, Go to File, Go to Line, the theme picker).
 *
 * THE COMMAND IS THE UNIT.  Nothing in UnoCode is bound to a key directly: a
 * key resolves to a COMMAND ID, and the id is looked up in a registry that
 * built-ins and extensions populate the same way.  That is the whole reason
 * keybindings.json can rebind anything, why the palette can list everything,
 * and why an extension's command is a first-class citizen rather than a
 * second mechanism bolted alongside.
 *
 * KEYBINDINGS RESOLVE LAST-WINS.  The table is scanned from the END, so a user
 * entry in keybindings.json beats an extension's, which beats the default -
 * exactly VS Code's rule - and `-command` removes a binding rather than
 * needing the defaults to be editable.
 *
 * THE `when` CLAUSE is a real (small) boolean expression over context keys,
 * not a fixed enum of situations.  Without it, "Escape" would have to be one
 * command that knows about the find widget, the suggestion list, multiple
 * cursors and the palette; with it, each of those binds Escape under its own
 * condition and none of them knows the others exist.
 * ======================================================================== */
#include "unocode.h"

/* F-keys, above the UI_KEY_* range so the two spaces cannot collide.  The
 * shell hands the module an EFI SimpleTextIn scan code, where F1..F12 are
 * 0x0B..0x16 - see uc_main.c's key hook. */
#define UC_KEY_F1  0x200
#define UC_KEY_F(n) (UC_KEY_F1 + (n) - 1)

/* ---- the registry ------------------------------------------------------- */
static UcCommand g_cmd[UC_CMD_MAX];
static int       g_ncmd;
static UcKeybind g_key[UC_KEYS_MAX];
static int       g_nkey;

int uc_cmd_count(void) { return g_ncmd; }
UcCommand *uc_cmd_at(int i) { return (i >= 0 && i < g_ncmd) ? &g_cmd[i] : 0; }
int uc_keys_count(void) { return g_nkey; }
UcKeybind *uc_keybind_at(int i) { return (i >= 0 && i < g_nkey) ? &g_key[i] : 0; }

int uc_cmd_find(const char *id)
{
    int i;
    for (i = 0; i < g_ncmd; i++) if (!strcmp(g_cmd[i].id, id)) return i;
    return -1;
}

int uc_cmd_register(const char *id, const char *title, const char *cat,
                    UcCmdFn fn, int ext, int jsid)
{
    int i = uc_cmd_find(id);
    if (i < 0) {
        if (g_ncmd >= UC_CMD_MAX) return -1;
        i = g_ncmd++;
        memset(&g_cmd[i], 0, sizeof g_cmd[i]);
        uc_scpy(g_cmd[i].id, id, sizeof g_cmd[i].id);
    }
    uc_scpy(g_cmd[i].title, title, sizeof g_cmd[i].title);
    uc_scpy(g_cmd[i].cat, cat ? cat : "", sizeof g_cmd[i].cat);
    g_cmd[i].fn = fn;
    g_cmd[i].ext = ext;
    g_cmd[i].jsid = jsid;
    return i;
}

void uc_cmd_drop_ext(int ext)
{
    int i = 0;
    while (i < g_ncmd) {
        if (g_cmd[i].ext == ext) {
            int k;
            for (k = i; k < g_ncmd - 1; k++) g_cmd[k] = g_cmd[k + 1];
            g_ncmd--;
        } else i++;
    }
    i = 0;
    while (i < g_nkey) {
        if (g_key[i].source == 2 && uc_cmd_find(g_key[i].cmd) < 0) {
            int k;
            for (k = i; k < g_nkey - 1; k++) g_key[k] = g_key[k + 1];
            g_nkey--;
        } else i++;
    }
}

int uc_cmd_run(const char *id)
{
    int i = uc_cmd_find(id);
    /* The "onCommand:" contract, in the one place every caller goes through.
     * A command can be UNKNOWN (an extension that has not been scanned) or
     * DECLARED BUT NOT IMPLEMENTED (a manifest listed it so it appears in the
     * palette, and the extension's JavaScript has not run yet).  Both are
     * resolved the same way - fire the activation event and look again - and
     * the second case is the normal one, so it must not read as an error. */
    if (i < 0 || (!g_cmd[i].fn && g_cmd[i].jsid < 0)) {
        char ev[64];
        uc_scpy(ev, "onCommand:", sizeof ev);
        uc_scat(ev, id, sizeof ev);
        uc_ext_activate_event(ev);
        i = uc_cmd_find(id);
    }
    if (i < 0) {
        char msg[80];
        uc_scpy(msg, "command '", sizeof msg);
        uc_scat(msg, id, sizeof msg);
        uc_scat(msg, "' not found", sizeof msg);
        uc_notify(msg, UC_SEV_ERROR);
        return 0;
    }
    /* An EDIT command acts on the document, so it takes the focus with it.
     * Without this, running "Add Cursor Below" from the palette leaves the
     * keyboard pointed at whatever had it before - usually the side bar - and
     * the next character typed goes nowhere.  The category is already on every
     * command, so this is one rule rather than a flag on thirty of them. */
    if (!strcmp(g_cmd[i].cat, "Edit") || !strcmp(g_cmd[i].cat, "File"))
        uc_focus(UC_F_EDITOR);
    if (g_cmd[i].fn) { g_cmd[i].fn(); return 1; }
    if (g_cmd[i].jsid >= 0) return uc_api_call_cmd(g_cmd[i].jsid);
    {
        char msg[96];
        uc_scpy(msg, "'", sizeof msg);
        uc_scat(msg, id, sizeof msg);
        uc_scat(msg, "' has no handler: its extension did not register one", sizeof msg);
        uc_notify(msg, UC_SEV_WARN);
    }
    return 0;
}

/* ---- context keys --------------------------------------------------------- */
#define CTX_MAX 40
static struct { char key[32]; char val[32]; int on; } g_ctx[CTX_MAX];
static int g_nctx;

static int ctx_slot(const char *key)
{
    int i;
    for (i = 0; i < g_nctx; i++) if (!strcmp(g_ctx[i].key, key)) return i;
    if (g_nctx >= CTX_MAX) return -1;
    i = g_nctx++;
    uc_scpy(g_ctx[i].key, key, sizeof g_ctx[i].key);
    g_ctx[i].val[0] = 0;
    g_ctx[i].on = 0;
    return i;
}

void uc_ctx_set(const char *key, int on)
{
    int i = ctx_slot(key);
    if (i >= 0) g_ctx[i].on = on ? 1 : 0;
}

void uc_ctx_set_str(const char *key, const char *val)
{
    int i = ctx_slot(key);
    if (i >= 0) { uc_scpy(g_ctx[i].val, val, sizeof g_ctx[i].val); g_ctx[i].on = val && val[0]; }
}

static int ctx_get(const char *key, const char **val)
{
    int i;
    for (i = 0; i < g_nctx; i++)
        if (!strcmp(g_ctx[i].key, key)) { if (val) *val = g_ctx[i].val; return g_ctx[i].on; }
    if (val) *val = "";
    return 0;
}

void uc_ctx_refresh(void)
{
    UcDoc *d = uc_doc_active();
    uc_ctx_set("editorFocus", UC.focus == UC_F_EDITOR && d != 0);
    uc_ctx_set("editorTextFocus", UC.focus == UC_F_EDITOR && d != 0);
    uc_ctx_set("editorHasSelection", d && uc_has_selection(d));
    uc_ctx_set("editorHasMultipleSelections", d && d->ncur > 1);
    uc_ctx_set("editorReadonly", d && d->readonly);
    uc_ctx_set("sideBarVisible", UC.sidebar_visible);
    uc_ctx_set("panelVisible", UC.panel_visible);
    uc_ctx_set("terminalFocus", UC.focus == UC_F_PANEL && UC.panel_tab == UC_PANEL_TERMINAL);
    uc_ctx_set("inQuickOpen", uc_quick_active());
    uc_ctx_set("editorTextFocus", UC.focus == UC_F_EDITOR && d != 0);
    if (d) {
        UcLang *L = uc_lang_at(d->lang);
        uc_ctx_set_str("editorLangId", L ? L->id : "plaintext");
    }
}

/* ---- the `when` expression ------------------------------------------------
 * Grammar:  or := and ('||' and)* ; and := not ('&&' not)* ;
 *           not := '!' not | atom ; atom := '(' or ')' | key [op literal]
 * A missing key is false, which is what makes a clause naming a context this
 * build has never heard of harmless. */
typedef struct { const char *p; } WhenP;

static void when_ws(WhenP *w) { while (*w->p == ' ') w->p++; }

static int when_or(WhenP *w);

static int when_atom(WhenP *w)
{
    char key[40];
    int n = 0, v;
    const char *sval;
    when_ws(w);
    if (*w->p == '(') {
        w->p++;
        v = when_or(w);
        when_ws(w);
        if (*w->p == ')') w->p++;
        return v;
    }
    while ((*w->p >= 'a' && *w->p <= 'z') || (*w->p >= 'A' && *w->p <= 'Z') ||
           (*w->p >= '0' && *w->p <= '9') || *w->p == '.' || *w->p == '_') {
        if (n < (int)sizeof key - 1) key[n++] = *w->p;
        w->p++;
    }
    key[n] = 0;
    if (!n) return 0;
    v = ctx_get(key, &sval);
    when_ws(w);
    if ((w->p[0] == '=' && w->p[1] == '=') || (w->p[0] == '!' && w->p[1] == '=')) {
        int neq = w->p[0] == '!';
        char lit[40];
        int ln = 0;
        w->p += 2;
        when_ws(w);
        if (*w->p == '\'' || *w->p == '"') {
            char q = *w->p++;
            while (*w->p && *w->p != q) { if (ln < (int)sizeof lit - 1) lit[ln++] = *w->p; w->p++; }
            if (*w->p) w->p++;
        } else {
            while (*w->p && *w->p != ' ' && *w->p != ')' && *w->p != '&' && *w->p != '|') {
                if (ln < (int)sizeof lit - 1) lit[ln++] = *w->p;
                w->p++;
            }
        }
        lit[ln] = 0;
        v = !strcmp(sval, lit);
        if (neq) v = !v;
    }
    return v;
}

static int when_not(WhenP *w)
{
    when_ws(w);
    if (*w->p == '!') { w->p++; return !when_not(w); }
    return when_atom(w);
}

static int when_and(WhenP *w)
{
    int v = when_not(w);
    for (;;) {
        when_ws(w);
        if (w->p[0] == '&' && w->p[1] == '&') { w->p += 2; v = when_not(w) && v; }
        else return v;
    }
}

static int when_or(WhenP *w)
{
    int v = when_and(w);
    for (;;) {
        when_ws(w);
        if (w->p[0] == '|' && w->p[1] == '|') { w->p += 2; v = when_and(w) || v; }
        else return v;
    }
}

int uc_when(const char *expr)
{
    WhenP w;
    if (!expr || !expr[0]) return 1;
    w.p = expr;
    return when_or(&w);
}

/* ---- key names ------------------------------------------------------------- */
static const struct { const char *name; int key; } kKeyName[] = {
    { "left", UI_KEY_LEFT }, { "right", UI_KEY_RIGHT }, { "up", UI_KEY_UP },
    { "down", UI_KEY_DOWN }, { "home", UI_KEY_HOME }, { "end", UI_KEY_END },
    { "pageup", UI_KEY_PGUP }, { "pagedown", UI_KEY_PGDN },
    { "backspace", UI_KEY_BACKSPACE }, { "delete", UI_KEY_DELETE },
    { "enter", UI_KEY_ENTER }, { "tab", UI_KEY_TAB }, { "escape", UI_KEY_ESC },
    { "space", ' ' }, { "oem_2", '/' }, { "oem_1", ';' },
    { "f1", UC_KEY_F(1) }, { "f2", UC_KEY_F(2) }, { "f3", UC_KEY_F(3) },
    { "f4", UC_KEY_F(4) }, { "f5", UC_KEY_F(5) }, { "f6", UC_KEY_F(6) },
    { "f7", UC_KEY_F(7) }, { "f8", UC_KEY_F(8) }, { "f9", UC_KEY_F(9) },
    { "f10", UC_KEY_F(10) }, { "f11", UC_KEY_F(11) }, { "f12", UC_KEY_F(12) }
};
#define NKEYNAME ((int)(sizeof kKeyName / sizeof kKeyName[0]))

static int one_chord(const char *s, int n, int *key, int *mods)
{
    char part[24];
    int i = 0;
    *key = 0; *mods = 0;
    while (i < n) {
        int k = 0;
        while (i < n && s[i] != '+' && k < (int)sizeof part - 1) part[k++] = s[i++];
        part[k] = 0;
        if (i < n && s[i] == '+') i++;
        if (uc_ieq(part, "ctrl") || uc_ieq(part, "cmd") || uc_ieq(part, "meta")) *mods |= UI_MOD_CTRL;
        else if (uc_ieq(part, "shift")) *mods |= UI_MOD_SHIFT;
        else if (uc_ieq(part, "alt")) *mods |= UI_MOD_ALT;
        else if (uc_ieq(part, "win")) *mods |= UI_MOD_GUI;
        else {
            int j;
            for (j = 0; j < NKEYNAME; j++)
                if (uc_ieq(part, kKeyName[j].name)) { *key = kKeyName[j].key; break; }
            if (j == NKEYNAME) {
                if (k == 1) {
                    int c = (unsigned char)part[0];
                    if (c >= 'A' && c <= 'Z') c += 32;
                    *key = c;
                } else return 0;
            }
        }
    }
    return *key != 0;
}

int uc_key_parse(const char *s, int *key, int *mods, int *key2, int *mods2)
{
    const char *sp;
    *key2 = 0; *mods2 = 0;
    if (!s || !s[0]) return 0;
    sp = strchr(s, ' ');
    if (sp) {
        if (!one_chord(s, (int)(sp - s), key, mods)) return 0;
        return one_chord(sp + 1, (int)strlen(sp + 1), key2, mods2);
    }
    return one_chord(s, (int)strlen(s), key, mods);
}

int uc_key_format(int key, int mods, char *out, int cap)
{
    int j;
    out[0] = 0;
    if (mods & UI_MOD_CTRL) uc_scat(out, "Ctrl+", cap);
    if (mods & UI_MOD_SHIFT) uc_scat(out, "Shift+", cap);
    if (mods & UI_MOD_ALT) uc_scat(out, "Alt+", cap);
    for (j = 0; j < NKEYNAME; j++)
        if (kKeyName[j].key == key) {
            char nm[24];
            uc_scpy(nm, kKeyName[j].name, sizeof nm);
            if (nm[0] >= 'a' && nm[0] <= 'z') nm[0] = (char)(nm[0] - 32);
            uc_scat(out, nm, cap);
            return (int)strlen(out);
        }
    if (key >= 32 && key < 127) {
        char c[2];
        c[0] = (char)((key >= 'a' && key <= 'z') ? key - 32 : key);
        c[1] = 0;
        uc_scat(out, c, cap);
    }
    return (int)strlen(out);
}

int uc_keybind_add(int key, int mods, int key2, int mods2, const char *cmd,
                   const char *when, int source)
{
    UcKeybind *k;
    if (g_nkey >= UC_KEYS_MAX || !key) return 0;
    k = &g_key[g_nkey++];
    memset(k, 0, sizeof *k);
    k->key = key; k->mods = mods; k->key2 = key2; k->mods2 = mods2;
    if (cmd && cmd[0] == '-') { k->removed = 1; cmd++; }
    uc_scpy(k->cmd, cmd ? cmd : "", sizeof k->cmd);
    uc_scpy(k->when, when ? when : "", sizeof k->when);
    k->source = (unsigned char)source;
    return 1;
}

static void bind(const char *chord, const char *cmd, const char *when)
{
    int key, mods, key2, mods2;
    if (!uc_key_parse(chord, &key, &mods, &key2, &mods2)) return;
    uc_keybind_add(key, mods, key2, mods2, cmd, when, 0);
}

const char *uc_keys_label_for(const char *cmd, char *buf, int cap)
{
    int i;
    buf[0] = 0;
    for (i = g_nkey - 1; i >= 0; i--) {
        if (g_key[i].removed || strcmp(g_key[i].cmd, cmd)) continue;
        uc_key_format(g_key[i].key, g_key[i].mods, buf, cap);
        if (g_key[i].key2) {
            char b2[24];
            uc_key_format(g_key[i].key2, g_key[i].mods2, b2, sizeof b2);
            uc_scat(buf, " ", cap);
            uc_scat(buf, b2, cap);
        }
        return buf;
    }
    return buf;
}

/* ---- dispatch, including chords -------------------------------------------- */
static int chord_key, chord_mods;

int uc_chord_pending(void) { return chord_key != 0; }

int uc_keys_dispatch(int key, int mods)
{
    int i;
    uc_ctx_refresh();
    if (chord_key) {
        int ck = chord_key, cm = chord_mods;
        chord_key = 0;
        for (i = g_nkey - 1; i >= 0; i--) {
            UcKeybind *k = &g_key[i];
            if (k->key != ck || k->mods != cm) continue;
            if (k->key2 != key || k->mods2 != mods) continue;
            if (!uc_when(k->when)) continue;
            if (k->removed) return 1;
            return uc_cmd_run(k->cmd);
        }
        uc_status_msg("The key combination is not a command");
        return 1;
    }
    for (i = g_nkey - 1; i >= 0; i--) {
        UcKeybind *k = &g_key[i];
        if (k->key != key || k->mods != mods) continue;
        if (!uc_when(k->when)) continue;
        if (k->removed) return 0;
        if (k->key2) { chord_key = key; chord_mods = mods; uc_status_msg("(chord) waiting for the second key"); return 1; }
        return uc_cmd_run(k->cmd);
    }
    return 0;
}

/* ---- fuzzy matching --------------------------------------------------------- */
int uc_fuzzy(const char *needle, const char *hay, int *pos, int maxpos)
{
    int score = 0, hi = 0, ni = 0, last = -2, npos = 0;
    if (!needle || !needle[0]) return 0;
    for (ni = 0; needle[ni]; ni++) {
        int nc = needle[ni], found = -1;
        if (nc >= 'A' && nc <= 'Z') nc += 32;
        if (nc == ' ') continue;
        while (hay[hi]) {
            int hc = hay[hi];
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (hc == nc) { found = hi; break; }
            hi++;
        }
        if (found < 0) return -1;
        score += 10;
        if (found == last + 1) score += 12;                  /* consecutive   */
        if (found == 0) score += 16;                          /* leading       */
        else if (hay[found-1] == ' ' || hay[found-1] == '.' ||
                 hay[found-1] == ':' || hay[found-1] == '_' ||
                 hay[found-1] == '-') score += 10;            /* word start    */
        score -= found - last - 1;                            /* gaps cost     */
        last = found;
        if (pos && npos < maxpos) pos[npos++] = found;
        hi++;
    }
    if (pos && npos < maxpos) pos[npos] = -1;
    return score;
}

/* ===========================================================================
 * The quick-input overlay: the Command Palette and its cousins.
 * ======================================================================== */
#define QROWS 12
#define QITEM_MAX 200

typedef struct {
    char label[72];
    char detail[52];
    char aux[32];             /* keybinding, or a file's folder             */
    int  ref;                 /* command index / doc index / theme index    */
    int  score;
} QItem;

static int   q_mode;
static char  q_text[120];
static int   q_len, q_sel, q_scroll;
static QItem q_item[QITEM_MAX];
static int   q_n;
static int   q_cb;                        /* extension callback slot        */
static char  q_placeholder[64];
static char  (*q_pick)[64];
static int   q_pickn;

int uc_quick_active(void) { return q_mode != UC_Q_NONE; }

static void q_rebuild(void);

void uc_quick_open(int mode)
{
    q_mode = mode;
    q_len = 0;
    q_text[0] = 0;
    q_sel = 0;
    q_scroll = 0;
    if (mode == UC_Q_COMMAND) uc_scpy(q_placeholder, "Type a command name", sizeof q_placeholder);
    else if (mode == UC_Q_FILE) uc_scpy(q_placeholder, "Search files by name", sizeof q_placeholder);
    else if (mode == UC_Q_LINE) uc_scpy(q_placeholder, "Type a line number", sizeof q_placeholder);
    else if (mode == UC_Q_THEME) uc_scpy(q_placeholder, "Select a colour theme", sizeof q_placeholder);
    else if (mode == UC_Q_LANG) uc_scpy(q_placeholder, "Select a language mode", sizeof q_placeholder);
    else if (mode == UC_Q_KEYS) uc_scpy(q_placeholder, "Search keybindings", sizeof q_placeholder);
    q_rebuild();
    uc_ctx_set("inQuickOpen", 1);
}

void uc_quick_close(void)
{
    q_mode = UC_Q_NONE;
    q_cb = -1;
    uc_ctx_set("inQuickOpen", 0);
}

void uc_quick_pick(char (*items)[64], int n, const char *placeholder, int jscb)
{
    q_pick = items;
    q_pickn = n;
    q_cb = jscb;
    uc_scpy(q_placeholder, placeholder ? placeholder : "Select an item", sizeof q_placeholder);
    q_mode = UC_Q_PICK;
    q_len = 0; q_text[0] = 0; q_sel = 0; q_scroll = 0;
    q_rebuild();
    uc_ctx_set("inQuickOpen", 1);
}

void uc_quick_input(const char *placeholder, const char *value, int jscb)
{
    q_cb = jscb;
    uc_scpy(q_placeholder, placeholder ? placeholder : "", sizeof q_placeholder);
    uc_scpy(q_text, value ? value : "", sizeof q_text);
    q_len = (int)strlen(q_text);
    q_mode = UC_Q_INPUT;
    q_n = 0;
    uc_ctx_set("inQuickOpen", 1);
}

static void q_add(const char *label, const char *detail, const char *aux, int ref)
{
    int s;
    if (q_n >= QITEM_MAX) return;
    s = q_len ? uc_fuzzy(q_text, label, 0, 0) : 0;
    if (q_len && s < 0) {
        if (!detail || uc_fuzzy(q_text, detail, 0, 0) < 0) return;
        s = 1;
    }
    uc_scpy(q_item[q_n].label, label, sizeof q_item[0].label);
    uc_scpy(q_item[q_n].detail, detail ? detail : "", sizeof q_item[0].detail);
    uc_scpy(q_item[q_n].aux, aux ? aux : "", sizeof q_item[0].aux);
    q_item[q_n].ref = ref;
    q_item[q_n].score = s;
    q_n++;
}

static void q_sort(void)
{
    int i, j;
    for (i = 1; i < q_n; i++) {
        QItem t = q_item[i];
        for (j = i - 1; j >= 0 && q_item[j].score < t.score; j--) q_item[j + 1] = q_item[j];
        q_item[j + 1] = t;
    }
}

static void q_rebuild(void)
{
    int i;
    q_n = 0;
    switch (q_mode) {
    case UC_Q_COMMAND:
        for (i = 0; i < g_ncmd; i++) {
            char label[72], keys[28];
            label[0] = 0;
            if (g_cmd[i].cat[0]) { uc_scpy(label, g_cmd[i].cat, sizeof label); uc_scat(label, ": ", sizeof label); }
            uc_scat(label, g_cmd[i].title, sizeof label);
            uc_keys_label_for(g_cmd[i].id, keys, sizeof keys);
            q_add(label, g_cmd[i].id, keys, i);
        }
        break;
    case UC_Q_FILE: {
        static char names[220][16];         /* 3.5 KB - not a stack frame */
        static unsigned char isdir[220];
        int n = uc_list_dir(UC.ws_vol, UC.ws_dir, names, isdir, 220);
        if (n > 220) n = 220;
        for (i = 0; i < n; i++) {
            UcLang *L;
            if (!names[i][0] || isdir[i]) continue;
            L = uc_lang_at(uc_lang_for_file(names[i]));
            q_add(names[i], UC.ws_dir[0] ? UC.ws_dir : uno_fs_volume_name(UC.ws_vol),
                  L ? L->name : "", i);
        }
        /* the open editors come first, as they do in Go to File */
        for (i = 0; i < uc_doc_count(); i++) {
            UcDoc *d = uc_doc_at(i);
            char t[24];
            uc_doc_title(d, t, sizeof t);
            q_add(t, "open editor", "", 1000 + i);
        }
        break;
    }
    case UC_Q_THEME:
        for (i = 0; i < uc_theme_count(); i++) {
            UcTheme *t = uc_theme_at(i);
            q_add(t->name, t->dark ? "dark" : "light", t->builtin ? "" : "extension", i);
        }
        break;
    case UC_Q_LANG:
        for (i = 0; i < uc_lang_count(); i++)
            q_add(uc_lang_at(i)->name, uc_lang_at(i)->id, "", i);
        break;
    case UC_Q_KEYS:
        for (i = 0; i < g_nkey; i++) {
            char keys[28], k2[24];
            uc_key_format(g_key[i].key, g_key[i].mods, keys, sizeof keys);
            if (g_key[i].key2) {
                uc_key_format(g_key[i].key2, g_key[i].mods2, k2, sizeof k2);
                uc_scat(keys, " ", sizeof keys);
                uc_scat(keys, k2, sizeof keys);
            }
            q_add(g_key[i].cmd, g_key[i].when[0] ? g_key[i].when : "always", keys, i);
        }
        break;
    case UC_Q_PICK:
        for (i = 0; i < q_pickn; i++) q_add(q_pick[i], "", "", i);
        break;
    default:
        break;
    }
    if (q_len) q_sort();
    if (q_sel >= q_n) q_sel = q_n ? q_n - 1 : 0;
}

static void q_accept(void)
{
    int ref = (q_sel >= 0 && q_sel < q_n) ? q_item[q_sel].ref : -1;
    int mode = q_mode;
    char text[120];
    uc_scpy(text, q_text, sizeof text);
    uc_quick_close();
    switch (mode) {
    case UC_Q_COMMAND:
        if (ref >= 0 && ref < g_ncmd) uc_cmd_run(g_cmd[ref].id);
        break;
    case UC_Q_FILE:
        if (ref >= 1000) uc_doc_activate(ref - 1000);
        else if (ref >= 0 && q_sel < q_n) uc_doc_open(UC.ws_vol, UC.ws_dir, q_item[q_sel].label);
        uc_focus(UC_F_EDITOR);
        break;
    case UC_Q_LINE: {
        UcDoc *d = uc_doc_active();
        long ln = strtol(text[0] == ':' ? text + 1 : text, 0, 10);
        if (d && ln > 0) uc_move_to(d, uc_line_start(d, (int)ln - 1), 0);
        break;
    }
    case UC_Q_THEME:
        if (ref >= 0 && ref < uc_theme_count()) {
            char json[64];
            UcTheme *t = uc_theme_at(ref);
            uc_theme_select(t->name);
            uc_scpy(json, "\"", sizeof json);
            uc_scat(json, t->name, sizeof json);
            uc_scat(json, "\"", sizeof json);
            uc_cfg_set("workbench.colorTheme", json);
        }
        break;
    case UC_Q_LANG: {
        UcDoc *d = uc_doc_active();
        if (d && ref >= 0) d->lang = ref;
        break;
    }
    case UC_Q_PICK:
        if (q_cb >= 0) uc_api_call_str(q_cb, ref >= 0 && q_sel < q_n ? q_item[q_sel].label : "");
        break;
    case UC_Q_INPUT:
        if (q_cb >= 0) uc_api_call_str(q_cb, text);
        break;
    default: break;
    }
}

int uc_quick_key(int key, int mods, int ch)
{
    if (!q_mode) return 0;
    if (key == UI_KEY_ESC) {
        if (q_mode == UC_Q_PICK || q_mode == UC_Q_INPUT) {
            int cb = q_cb;
            uc_quick_close();
            if (cb >= 0) uc_api_call_str(cb, "");     /* cancelled: undefined  */
        } else uc_quick_close();
        return 1;
    }
    if (key == UI_KEY_ENTER) { q_accept(); return 1; }
    if (key == UI_KEY_UP)   { if (q_sel > 0) q_sel--; return 1; }
    if (key == UI_KEY_DOWN) { if (q_sel < q_n - 1) q_sel++; return 1; }
    if (key == UI_KEY_PGUP) { q_sel -= QROWS; if (q_sel < 0) q_sel = 0; return 1; }
    if (key == UI_KEY_PGDN) { q_sel += QROWS; if (q_sel >= q_n) q_sel = q_n - 1; return 1; }
    if (key == UI_KEY_BACKSPACE) {
        if (q_len > 0) q_text[--q_len] = 0;
        q_sel = 0;
        q_rebuild();
        return 1;
    }
    if (ch >= 32 && ch < 127 && q_len < (int)sizeof q_text - 1) {
        q_text[q_len++] = (char)ch;
        q_text[q_len] = 0;
        q_sel = 0;
        /* "> " switches Go to File into the command palette, and ":" into Go
         * to Line, exactly as the one input box in VS Code does */
        if (q_len == 1 && q_mode == UC_Q_FILE) {
            if (ch == '>') { q_mode = UC_Q_COMMAND; q_len = 0; q_text[0] = 0; }
            else if (ch == ':') { q_mode = UC_Q_LINE; }
        }
        q_rebuild();
        return 1;
    }
    (void)mods;
    return 1;
}

void uc_quick_draw(UcRect wb)
{
    UcRect r;
    int rows, i, ih = uc_ui_h() + 8;
    if (!q_mode) return;
    rows = q_n > QROWS ? QROWS : q_n;
    r.w = 620;
    if (r.w > wb.w - 40) r.w = wb.w - 40;
    r.x = wb.x + (wb.w - r.w) / 2;
    r.y = wb.y + 4;
    r.h = ih + 12 + rows * ih;
    if (q_sel < q_scroll) q_scroll = q_sel;
    if (q_sel >= q_scroll + rows) q_scroll = q_sel - rows + 1;

    fb_blend_rect(r.x + 3, r.y + 3, r.w, r.h, uc_col(UC_C_WIDGET_SHADOW), 70);
    fb_fill_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_QUICK_BG));
    fb_frame_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_FOCUS_BORDER));

    /* the input line */
    fb_fill_rect(r.x + 6, r.y + 6, r.w - 12, ih, uc_col(UC_C_INPUT_BG));
    fb_frame_rect(r.x + 6, r.y + 6, r.w - 12, ih, uc_col(UC_C_INPUT_BORDER));
    if (q_len) {
        int tw;
        uc_ui_text(r.x + 12, r.y + 10, q_text, uc_col(UC_C_INPUT_FG));
        tw = uc_ui_text_w(q_text);
        if ((uno_dbg_uptime_ms() / 530) & 1)
            fb_fill_rect(r.x + 12 + tw + 1, r.y + 9, 2, ih - 6, uc_col(UC_C_CURSOR));
    } else {
        uc_ui_text(r.x + 12, r.y + 10, q_placeholder, uc_col(UC_C_INPUT_PLACEHOLDER));
        if ((uno_dbg_uptime_ms() / 530) & 1)
            fb_fill_rect(r.x + 12, r.y + 9, 2, ih - 6, uc_col(UC_C_CURSOR));
    }

    for (i = 0; i < rows; i++) {
        int k = q_scroll + i;
        int y = r.y + ih + 10 + i * ih;
        QItem *it;
        if (k >= q_n) break;
        it = &q_item[k];
        if (k == q_sel) fb_fill_rect(r.x + 4, y - 2, r.w - 8, ih, uc_col(UC_C_QUICK_SEL_BG));
        uc_ui_text(r.x + 12, y + 1, it->label,
                   k == q_sel ? uc_col(UC_C_LIST_SEL_FG) : uc_col(UC_C_SIDEBAR_FG));
        /* the keybinding is right-aligned and the detail runs towards it, so
         * the detail is clipped at the keybinding's left edge rather than
         * being drawn over it */
        {
            int aw = it->aux[0] ? uc_ui_text_w(it->aux) + 30 : 12;
            if (it->detail[0]) {
                int dx = r.x + 20 + uc_ui_text_w(it->label);
                int avail = (r.x + r.w - aw) - dx;
                if (avail > 8) {
                    fb_set_clip(dx, y - 2, avail, ih);
                    uc_ui_text(dx, y + 1, it->detail, uc_col(UC_C_BREADCRUMB_FG));
                    fb_reset_clip();
                }
            }
            if (it->aux[0])
                uc_ui_text(r.x + r.w - aw + 6, y + 1, it->aux, uc_col(UC_C_LIST_HIGHLIGHT));
        }
    }
    if (!q_n && q_mode != UC_Q_INPUT)
        uc_ui_text(r.x + 12, r.y + ih + 12, "No matching results",
                   uc_col(UC_C_BREADCRUMB_FG));
}

int uc_quick_event(UcRect wb, const unoui_event *e)
{
    UcRect r;
    int rows, ih = uc_ui_h() + 8;
    if (!q_mode || e->kind != UI_EV_MOUSE_DOWN) return 0;
    rows = q_n > QROWS ? QROWS : q_n;
    r.w = 620;
    if (r.w > wb.w - 40) r.w = wb.w - 40;
    r.x = wb.x + (wb.w - r.w) / 2;
    r.y = wb.y + 4;
    r.h = ih + 12 + rows * ih;
    if (e->x < r.x || e->x >= r.x + r.w || e->y < r.y || e->y >= r.y + r.h) {
        uc_quick_close();
        return 1;
    }
    if (e->y > r.y + ih + 8) {
        int k = q_scroll + (e->y - (r.y + ih + 10)) / ih;
        if (k >= 0 && k < q_n) { q_sel = k; q_accept(); }
    }
    return 1;
}

/* ===========================================================================
 * The built-in commands.
 * ======================================================================== */
static UcDoc *D(void) { return uc_doc_active(); }

static void c_new(void)      { uc_doc_new(); }
/* Ask the shell for its native picker first and fall back to quick-open, so
 * the same command is a real Open dialog on a desktop and the in-editor list
 * on pc64, without either platform learning about the other. */
static void c_open(void)
{
    int vol = 0;
    char dir[UC_PATH_MAX], name[32];
    if (pc64_shell_pick(0, &vol, dir, sizeof dir, name, sizeof name))
        uc_doc_open(vol, dir, name);
    else
        uc_quick_open(UC_Q_FILE);
}

/* Open Folder RE-ROOTS the workspace, which only the shell can do - it owns
 * the volume the folder is mounted on - so this asks and then follows. */
static void c_open_folder(void)
{
    int vol = 0;
    char dir[UC_PATH_MAX], name[32];
    if (!pc64_shell_pick(1, &vol, dir, sizeof dir, name, sizeof name)) {
        uc_status_msg("Open Folder needs a desktop file dialog");
        return;
    }
    uc_open_folder(vol, dir);
}
static void c_save(void)     { UcDoc *d = D(); if (d) { if (d->name[0]) uc_doc_save(d); else uc_status_msg("Untitled: use Save As"); } }
static void c_save_all(void) { int i; for (i = 0; i < uc_doc_count(); i++) { UcDoc *d = uc_doc_at(i); if (d->dirty && d->name[0]) uc_doc_save(d); } }
static void c_close(void)    { int i = uc_doc_active_index(); if (i >= 0) uc_doc_close(i); }
static void c_undo(void)     { UcDoc *d = D(); if (d) uc_undo(d); }
static void c_redo(void)     { UcDoc *d = D(); if (d) uc_redo(d); }
/* The clipboard staging buffer is module-static, not a local: this module runs
 * on the kernel stack, where an 8 KB frame is a real risk and there is no
 * guard page to catch it.  Nothing re-enters cut/copy, so one buffer is one
 * buffer. */
static char g_clipstage[8192];

static void c_cut(void)
{
    UcDoc *d = D();
    char *buf = g_clipstage;
    if (!d) return;
    if (!uc_has_selection(d)) uc_select_line(d);
    uc_selection_text(d, buf, sizeof g_clipstage);
    uc_clip_set(buf, -1);
    uc_delete_selection(d);
}
static void c_copy(void)
{
    UcDoc *d = D();
    char *buf = g_clipstage;
    if (!d) return;
    if (!uc_has_selection(d)) {
        /* copy with no selection copies the whole line, and must leave the
         * caret where it was rather than selecting what it just copied */
        uc_select_line(d);
        uc_selection_text(d, buf, sizeof g_clipstage);
        uc_clear_extra_cursors(d);
        d->cur[0].anchor = d->cur[0].caret;
    } else uc_selection_text(d, buf, sizeof g_clipstage);
    uc_clip_set(buf, -1);
}
static void c_paste(void)
{
    UcDoc *d = D();
    int n = 0;
    const char *s = uc_clip_get(&n);
    if (d && n) uc_insert(d, s, n);
}
static void c_select_all(void) { UcDoc *d = D(); if (d) uc_select_all(d); }
static void c_find(void)       { uc_find_open(0); }
static void c_replace(void)    { uc_find_open(1); }
static void c_find_next(void)  { uc_find_next(0); }
static void c_find_prev(void)  { uc_find_next(1); }
static void c_comment(void)    { UcDoc *d = D(); if (d) uc_toggle_comment(d); }
static void c_move_up(void)    { UcDoc *d = D(); if (d) uc_move_lines(d, -1); }
static void c_move_down(void)  { UcDoc *d = D(); if (d) uc_move_lines(d, 1); }
static void c_dup_down(void)   { UcDoc *d = D(); if (d) uc_duplicate_lines(d); }
static void c_del_line(void)   { UcDoc *d = D(); if (d) uc_delete_line(d); }
static void c_cursor_below(void){ UcDoc *d = D(); if (d) uc_add_cursor_line(d, 1); }
static void c_cursor_above(void){ UcDoc *d = D(); if (d) uc_add_cursor_line(d, -1); }
static void c_indent(void)     { UcDoc *d = D(); if (d) uc_indent(d, 0); }
static void c_outdent(void)    { UcDoc *d = D(); if (d) uc_indent(d, 1); }
static void c_suggest(void)    { UcDoc *d = D(); if (d) uc_suggest_open(d, 1); }
static void c_jump_bracket(void)
{
    UcDoc *d = D();
    int m;
    if (!d) return;
    m = uc_bracket_match(d, d->cur[0].caret);
    if (m < 0 && d->cur[0].caret > 0) m = uc_bracket_match(d, d->cur[0].caret - 1);
    if (m >= 0) uc_move_to(d, m, 0);
}
static void c_select_next_match(void)
{
    UcDoc *d = D();
    char sel[120];
    if (!d) return;
    if (!uc_has_selection(d)) { uc_select_word(d); return; }
    uc_selection_text(d, sel, sizeof sel);
    uc_find_set(sel);
    uc_find_next(0);
}
static void c_palette(void)    { uc_quick_open(UC_Q_COMMAND); }
static void c_quickopen(void)  { uc_quick_open(UC_Q_FILE); }
static void c_gotoline(void)   { uc_quick_open(UC_Q_LINE); }
static void c_theme(void)      { uc_quick_open(UC_Q_THEME); }
static void c_langmode(void)   { uc_quick_open(UC_Q_LANG); }
static void c_keys_ui(void)    { uc_quick_open(UC_Q_KEYS); }
static void c_toggle_sidebar(void) { uc_toggle_sidebar(-1); }
static void c_view_explorer(void)  { uc_toggle_sidebar(UC_VIEW_EXPLORER); }
static void c_view_search(void)    { uc_toggle_sidebar(UC_VIEW_SEARCH); }
static void c_view_scm(void)       { uc_toggle_sidebar(UC_VIEW_SCM); }
static void c_view_run(void)       { uc_toggle_sidebar(UC_VIEW_RUN); }
static void c_view_ext(void)       { uc_toggle_sidebar(UC_VIEW_EXTENSIONS); }
static void c_toggle_panel(void)   { uc_toggle_panel(-1); }
/* Idempotent, unlike the three-state toggles beside it: whatever state the
 * workbench is in, this leaves it in exactly one.  That is what a user who has
 * hidden the side bar, opened the terminal and lost the editor wants from a
 * menu entry, and it is the only way a screenshot harness can start a scene
 * from a known layout without rebooting between every figure. */
static void c_reset_layout(void)
{
    UC.sidebar_visible = 1;
    UC.view = UC_VIEW_EXPLORER;
    UC.panel_visible = 0;
    UC.zen = 0;
    UC.sidebar_user = 0;
    uc_focus(UC_F_EDITOR);
    uc_explorer_refresh();
    uc_layout();
    uc_repaint();
}
static void c_terminal(void)       { uc_toggle_panel(UC_PANEL_TERMINAL); }
static void c_problems(void)       { uc_toggle_panel(UC_PANEL_PROBLEMS); }
static void c_output(void)         { uc_toggle_panel(UC_PANEL_OUTPUT); }
static void c_zoom_in(void)        { uc_font_zoom(1); }
static void c_zoom_out(void)       { uc_font_zoom(-1); }
static void c_zoom_reset(void)     { uc_cfg_set("editor.fontSize", "14"); uc_metrics_init(); }
static void c_minimap(void)
{
    int on = !uc_cfg_bool("editor.minimap.enabled");
    uc_cfg_set("editor.minimap.enabled", on ? "true" : "false");
}
static void c_wordwrap(void)
{
    int on = strcmp(uc_cfg_str("editor.wordWrap"), "on") != 0;
    uc_cfg_set("editor.wordWrap", on ? "\"on\"" : "\"off\"");
}
static void c_next_editor(void)
{
    int n = uc_doc_count(), i = uc_doc_active_index();
    if (n > 0) uc_doc_activate((i + 1) % n);
}
static void c_prev_editor(void)
{
    int n = uc_doc_count(), i = uc_doc_active_index();
    if (n > 0) uc_doc_activate((i + n - 1) % n);
}
static void c_settings_json(void)
{
    if (uc_cfg_vol() >= 0) {
        char dir[24];
        uc_cfg_dir(dir, sizeof dir);
        uc_cfg_save();                       /* make sure the file exists     */
        uc_doc_open(uc_cfg_vol(), dir, "SETTINGS.JSN");
    }
}
static void c_keys_json(void)
{
    if (uc_cfg_vol() >= 0) {
        char dir[24];
        uc_cfg_dir(dir, sizeof dir);
        uc_keys_write_reference();
        uc_doc_open(uc_cfg_vol(), dir, "KEYBIND.JSN");
    }
}
static void c_settings_ui(void)  { uc_toggle_sidebar(UC_VIEW_EXPLORER); c_settings_json(); }
static void c_reload_ext(void)   { uc_ext_reload(); }
static void c_run_task(void)     { uc_tasks_run(0); }
static void c_run_start(void)    { uc_launch_run(-1); }
static void c_escape(void)
{
    UcDoc *d = D();
    if (uc_suggest_active()) { uc_suggest_close(); return; }
    if (uc_find_active()) { uc_find_close(); return; }
    if (d && d->ncur > 1) { uc_clear_extra_cursors(d); return; }
}

static void c_welcome(void)
{
    int i = uc_doc_new();
    UcDoc *d = uc_doc_at(i);
    static const char kWelcome[] =
        "# UnoCode\n"
        "\n"
        "A code editor for UnoDOS/pc64, in the shape of Visual Studio Code.\n"
        "\n"
        "## Start\n"
        "\n"
        "- Ctrl+Shift+P   every command, searchable\n"
        "- Ctrl+P         go to a file\n"
        "- Ctrl+B         show or hide the side bar\n"
        "- Ctrl+`         the integrated terminal (type `help`)\n"
        "- Ctrl+,         settings.json\n"
        "\n"
        "## Customise\n"
        "\n"
        "- Colour themes are VS Code theme files. Ctrl+Shift+P, then\n"
        "  `Preferences: Color Theme`.\n"
        "- Keybindings live in UNOCODE\\KEYBIND.JSN and use the same\n"
        "  { \"key\", \"command\", \"when\" } records as VS Code.\n"
        "- Extensions live in EXT\\<ID>\\ with a PACKAGE.JSN manifest and an\n"
        "  optional MAIN.JS. See the Extensions view in the activity bar.\n";
    if (!d) return;
    uc_replace_range(d, 0, 0, kWelcome, (int)(sizeof kWelcome - 1));
    d->lang = uc_lang_by_id("markdown");
    if (d->lang < 0) d->lang = 0;
    d->dirty = 0;
    d->saved_at = d->undo_at;
    uc_scpy(d->name, "WELCOME.MD", sizeof d->name);
    d->vol = -1;
    uc_move_to(d, 0, 0);
}

/* ---- registration + the default keymap -------------------------------------- */
static void reg(const char *id, const char *cat, const char *title, UcCmdFn fn)
{
    uc_cmd_register(id, title, cat, fn, -1, -1);
}

void uc_cmd_init(void)
{
    g_ncmd = 0;
    g_nkey = 0;
    q_cb = -1;

    reg("workbench.action.files.newUntitledFile", "File", "New File", c_new);
    reg("workbench.action.files.openFile", "File", "Open File...", c_open);
    reg("workbench.action.files.openFolder", "File", "Open Folder...", c_open_folder);
    reg("workbench.action.files.save", "File", "Save", c_save);
    reg("workbench.action.files.saveAll", "File", "Save All", c_save_all);
    reg("workbench.action.closeActiveEditor", "View", "Close Editor", c_close);
    reg("workbench.action.nextEditor", "View", "Open Next Editor", c_next_editor);
    reg("workbench.action.previousEditor", "View", "Open Previous Editor", c_prev_editor);

    reg("undo", "Edit", "Undo", c_undo);
    reg("redo", "Edit", "Redo", c_redo);
    reg("editor.action.clipboardCutAction", "Edit", "Cut", c_cut);
    reg("editor.action.clipboardCopyAction", "Edit", "Copy", c_copy);
    reg("editor.action.clipboardPasteAction", "Edit", "Paste", c_paste);
    reg("editor.action.selectAll", "Edit", "Select All", c_select_all);
    reg("actions.find", "Edit", "Find", c_find);
    reg("editor.action.startFindReplaceAction", "Edit", "Replace", c_replace);
    reg("editor.action.nextMatchFindAction", "Edit", "Find Next", c_find_next);
    reg("editor.action.previousMatchFindAction", "Edit", "Find Previous", c_find_prev);
    reg("editor.action.commentLine", "Edit", "Toggle Line Comment", c_comment);
    reg("editor.action.moveLinesUpAction", "Edit", "Move Line Up", c_move_up);
    reg("editor.action.moveLinesDownAction", "Edit", "Move Line Down", c_move_down);
    reg("editor.action.copyLinesDownAction", "Edit", "Copy Line Down", c_dup_down);
    reg("editor.action.deleteLines", "Edit", "Delete Line", c_del_line);
    reg("editor.action.insertCursorBelow", "Edit", "Add Cursor Below", c_cursor_below);
    reg("editor.action.insertCursorAbove", "Edit", "Add Cursor Above", c_cursor_above);
    reg("editor.action.addSelectionToNextFindMatch", "Edit", "Add Selection To Next Find Match", c_select_next_match);
    reg("editor.action.indentLines", "Edit", "Indent Line", c_indent);
    reg("editor.action.outdentLines", "Edit", "Outdent Line", c_outdent);
    reg("editor.action.triggerSuggest", "Edit", "Trigger Suggest", c_suggest);
    reg("editor.action.jumpToBracket", "Edit", "Go to Bracket", c_jump_bracket);
    reg("editor.action.cancelSelectionOrOperation", "Edit", "Cancel", c_escape);

    reg("workbench.action.showCommands", "View", "Show All Commands", c_palette);
    reg("workbench.action.quickOpen", "View", "Go to File...", c_quickopen);
    reg("workbench.action.gotoLine", "View", "Go to Line...", c_gotoline);
    reg("workbench.action.toggleSidebarVisibility", "View", "Toggle Side Bar", c_toggle_sidebar);
    reg("workbench.view.explorer", "View", "Show Explorer", c_view_explorer);
    reg("workbench.view.search", "View", "Show Search", c_view_search);
    reg("workbench.view.scm", "View", "Show Source Control", c_view_scm);
    reg("workbench.view.debug", "View", "Show Run and Debug", c_view_run);
    reg("workbench.view.extensions", "View", "Show Extensions", c_view_ext);
    reg("workbench.action.togglePanel", "View", "Toggle Panel", c_toggle_panel);
    reg("workbench.action.resetLayout", "View", "Reset Layout", c_reset_layout);
    reg("workbench.action.terminal.toggleTerminal", "Terminal", "Toggle Terminal", c_terminal);
    reg("workbench.actions.view.problems", "View", "Show Problems", c_problems);
    reg("workbench.action.output.toggleOutput", "View", "Show Output", c_output);
    reg("workbench.action.zoomIn", "View", "Zoom In", c_zoom_in);
    reg("workbench.action.zoomOut", "View", "Zoom Out", c_zoom_out);
    reg("workbench.action.zoomReset", "View", "Reset Zoom", c_zoom_reset);
    reg("editor.action.toggleMinimap", "View", "Toggle Minimap", c_minimap);
    reg("editor.action.toggleWordWrap", "View", "Toggle Word Wrap", c_wordwrap);

    reg("workbench.action.selectTheme", "Preferences", "Color Theme", c_theme);
    reg("workbench.action.editor.changeLanguageMode", "Preferences", "Change Language Mode", c_langmode);
    reg("workbench.action.openSettings", "Preferences", "Open Settings", c_settings_ui);
    reg("workbench.action.openSettingsJson", "Preferences", "Open Settings (JSON)", c_settings_json);
    reg("workbench.action.openGlobalKeybindings", "Preferences", "Open Keyboard Shortcuts", c_keys_ui);
    reg("workbench.action.openGlobalKeybindingsFile", "Preferences", "Open Keyboard Shortcuts (JSON)", c_keys_json);

    reg("workbench.action.reloadWindow", "Developer", "Reload Extensions", c_reload_ext);
    reg("workbench.action.tasks.runTask", "Tasks", "Run Task", c_run_task);
    reg("workbench.action.debug.start", "Run", "Start Debugging", c_run_start);
    reg("unocode.showWelcome", "Help", "Welcome", c_welcome);

    /* the default keymap.  Ctrl+letter chords are the reliable path on this
     * machine; see uc_main.c's key hook for what the transports deliver. */
    bind("ctrl+n", "workbench.action.files.newUntitledFile", 0);
    bind("ctrl+o", "workbench.action.files.openFile", 0);
    bind("ctrl+k ctrl+o", "workbench.action.files.openFolder", 0);
    bind("ctrl+s", "workbench.action.files.save", 0);
    bind("ctrl+k s", "workbench.action.files.saveAll", 0);
    bind("ctrl+shift+p", "workbench.action.showCommands", 0);
    bind("f1", "workbench.action.showCommands", 0);
    bind("ctrl+p", "workbench.action.quickOpen", 0);
    bind("ctrl+g", "workbench.action.gotoLine", 0);
    bind("ctrl+z", "undo", "editorTextFocus");
    bind("ctrl+y", "redo", "editorTextFocus");
    bind("ctrl+shift+z", "redo", "editorTextFocus");
    bind("ctrl+x", "editor.action.clipboardCutAction", "editorTextFocus");
    bind("ctrl+c", "editor.action.clipboardCopyAction", "editorTextFocus");
    bind("ctrl+v", "editor.action.clipboardPasteAction", "editorTextFocus");
    bind("ctrl+a", "editor.action.selectAll", "editorTextFocus");
    bind("ctrl+f", "actions.find", 0);
    bind("ctrl+h", "editor.action.startFindReplaceAction", 0);
    bind("f3", "editor.action.nextMatchFindAction", 0);
    bind("shift+f3", "editor.action.previousMatchFindAction", 0);
    bind("ctrl+/", "editor.action.commentLine", "editorTextFocus");
    bind("alt+up", "editor.action.moveLinesUpAction", "editorTextFocus");
    bind("alt+down", "editor.action.moveLinesDownAction", "editorTextFocus");
    bind("shift+alt+down", "editor.action.copyLinesDownAction", "editorTextFocus");
    bind("ctrl+shift+k", "editor.action.deleteLines", "editorTextFocus");
    bind("ctrl+alt+down", "editor.action.insertCursorBelow", "editorTextFocus");
    bind("ctrl+alt+up", "editor.action.insertCursorAbove", "editorTextFocus");
    bind("ctrl+d", "editor.action.addSelectionToNextFindMatch", "editorTextFocus");
    bind("ctrl+]", "editor.action.indentLines", "editorTextFocus");
    bind("ctrl+[", "editor.action.outdentLines", "editorTextFocus");
    bind("ctrl+space", "editor.action.triggerSuggest", "editorTextFocus");
    bind("ctrl+b", "workbench.action.toggleSidebarVisibility", 0);
    bind("ctrl+shift+e", "workbench.view.explorer", 0);
    bind("ctrl+shift+f", "workbench.view.search", 0);
    bind("ctrl+shift+g", "workbench.view.scm", 0);
    bind("ctrl+shift+d", "workbench.view.debug", 0);
    bind("ctrl+shift+x", "workbench.view.extensions", 0);
    bind("ctrl+j", "workbench.action.togglePanel", 0);
    bind("ctrl+`", "workbench.action.terminal.toggleTerminal", 0);
    bind("ctrl+shift+m", "workbench.actions.view.problems", 0);
    bind("ctrl+shift+u", "workbench.action.output.toggleOutput", 0);
    bind("ctrl+=", "workbench.action.zoomIn", 0);
    bind("ctrl+-", "workbench.action.zoomOut", 0);
    bind("ctrl+0", "workbench.action.zoomReset", 0);
    bind("ctrl+k ctrl+t", "workbench.action.selectTheme", 0);
    bind("ctrl+k m", "workbench.action.editor.changeLanguageMode", 0);
    bind("ctrl+,", "workbench.action.openSettingsJson", 0);
    bind("ctrl+k ctrl+s", "workbench.action.openGlobalKeybindings", 0);
    bind("ctrl+r", "workbench.action.reloadWindow", 0);
    bind("ctrl+shift+b", "workbench.action.tasks.runTask", 0);
    bind("f5", "workbench.action.debug.start", 0);
    bind("ctrl+pagedown", "workbench.action.nextEditor", 0);
    bind("ctrl+pageup", "workbench.action.previousEditor", 0);
    bind("escape", "editor.action.cancelSelectionOrOperation",
         "suggestWidgetVisible || findWidgetVisible || editorHasMultipleSelections");
}

/* ---- keybindings.json ------------------------------------------------------- */
int uc_keys_load(void)
{
    char path[48], dir[24];
    char *src = 0;
    long len = 0;
    char err[80];
    UcJson *root, *e;
    int n = 0;
    if (uc_cfg_vol() < 0) return 0;
    uc_cfg_dir(dir, sizeof dir);
    uc_path_join(path, sizeof path, dir, "KEYBIND.JSN");
    if (!uc_read_file(uc_cfg_vol(), path, &src, &len)) return 0;
    root = uc_json_parse(src, (int)len, err, sizeof err);
    free(src);
    if (!root || root->type != UJ_ARR) {
        if (root) uc_json_free(root);
        uc_notify("keybindings.json is not a JSON array", UC_SEV_WARN);
        return 0;
    }
    for (e = root->child; e; e = e->next) {
        const char *k = uc_json_str(e, "key", 0);
        const char *c = uc_json_str(e, "command", 0);
        const char *w = uc_json_str(e, "when", "");
        int key, mods, key2, mods2;
        if (!k || !c) continue;
        if (!uc_key_parse(k, &key, &mods, &key2, &mods2)) continue;
        uc_keybind_add(key, mods, key2, mods2, c, w, 1);
        n++;
    }
    uc_json_free(root);
    return n;
}

/* Write the whole default keymap out as a real keybindings.json.  A user who
 * wants to change one binding needs to SEE the defaults in the syntax the file
 * expects; shipping an empty array with a comment is how "I could not work out
 * the key name" happens. */
int uc_keys_write_reference(void)
{
    char path[48], dir[24];
    char *buf;
    int cap = 24576, n = 0, i;
    int ok;
    if (uc_cfg_vol() < 0 || !uno_fs_writable(uc_cfg_vol())) return 0;
    uc_cfg_dir(dir, sizeof dir);
    uc_path_join(path, sizeof path, dir, "KEYBIND.JSN");
    if (uno_fs_size(uc_cfg_vol(), path) > 0) return 1;    /* never clobber */
    buf = (char *)malloc((unsigned long)cap);
    if (!buf) return 0;
    uc_scpy(buf,
        "// UnoCode keybindings.  Each record is { \"key\", \"command\",\n"
        "// \"when\" }; a leading '-' on the command REMOVES a default.\n"
        "// The list below is the shipped default keymap - edit freely.\n[\n", cap);
    n = (int)strlen(buf);
    for (i = 0; i < g_nkey; i++) {
        char line[200], keys[40], k2[24];
        uc_key_format(g_key[i].key, g_key[i].mods, keys, sizeof keys);
        {   /* the file's spelling is lower case with '+' separators */
            int j;
            for (j = 0; keys[j]; j++) if (keys[j] >= 'A' && keys[j] <= 'Z') keys[j] = (char)(keys[j] + 32);
        }
        if (g_key[i].key2) {
            int j;
            uc_key_format(g_key[i].key2, g_key[i].mods2, k2, sizeof k2);
            for (j = 0; k2[j]; j++) if (k2[j] >= 'A' && k2[j] <= 'Z') k2[j] = (char)(k2[j] + 32);
            uc_scat(keys, " ", sizeof keys);
            uc_scat(keys, k2, sizeof keys);
        }
        uc_scpy(line, "    { \"key\": \"", sizeof line);
        uc_scat(line, keys, sizeof line);
        uc_scat(line, "\", \"command\": \"", sizeof line);
        uc_scat(line, g_key[i].cmd, sizeof line);
        uc_scat(line, "\"", sizeof line);
        if (g_key[i].when[0]) {
            uc_scat(line, ", \"when\": \"", sizeof line);
            uc_scat(line, g_key[i].when, sizeof line);
            uc_scat(line, "\"", sizeof line);
        }
        uc_scat(line, i + 1 < g_nkey ? " },\n" : " }\n", sizeof line);
        if (n + (int)strlen(line) + 4 >= cap) break;
        uc_scpy(buf + n, line, cap - n);
        n += (int)strlen(line);
    }
    uc_scpy(buf + n, "]\n", cap - n);
    n += 2;
    ok = uno_fs_write(uc_cfg_vol(), path, (const unsigned char *)buf, n);
    free(buf);
    return ok;
}
