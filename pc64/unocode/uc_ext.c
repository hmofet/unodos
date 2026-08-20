/* ===========================================================================
 * uc_ext.c - extension discovery, manifests and contributions.
 *
 * AN EXTENSION IS A DIRECTORY on any mounted volume:
 *
 *     EXT\<ID>\PACKAGE.JSN     the manifest - VS Code's package.json
 *     EXT\<ID>\MAIN.JS         the entry point, if it has JavaScript
 *     EXT\<ID>\THEMES\*.JSN    colour themes
 *     EXT\<ID>\SYNTAX\*.JSN    TextMate grammars
 *     EXT\<ID>\SNIPPET\*.JSN   snippets
 *
 * The names are 8.3 because FAT is; the CONTENT is VS Code's, key for key.
 * A manifest that declares `contributes.themes` gets its themes into the theme
 * picker without any JavaScript at all, which is the point of splitting
 * declarative contributions from the extension host: the common cases (a
 * theme, a language, a grammar, snippets) cost no interpreter and cannot hang.
 *
 * ACTIVATION is lazy and event-driven, as it is in VS Code.  A manifest
 * DECLARES its commands so they appear in the palette immediately; the JS that
 * implements them is not run until an activation event fires - typically
 * `onCommand:<id>` when the user actually picks one.  Twenty installed
 * extensions therefore cost twenty manifest reads at startup and no
 * interpreter time.
 * ======================================================================== */
#include "unocode.h"

static UcExt g_ext[UC_EXT_MAX];
static int   g_next;

int   uc_ext_count(void) { return g_next; }
UcExt *uc_ext_at(int i) { return (i >= 0 && i < g_next) ? &g_ext[i] : 0; }

int uc_ext_find(const char *id)
{
    int i;
    for (i = 0; i < g_next; i++) if (uc_ieq(g_ext[i].id, id)) return i;
    return -1;
}

int uc_ext_dir(int i, char *out, int cap)
{
    UcExt *e = uc_ext_at(i);
    if (!e) { uc_scpy(out, "", cap); return 0; }
    uc_scpy(out, "EXT\\", cap);
    uc_scat(out, e->id, cap);
    return 1;
}

/* ---- snippets ---------------------------------------------------------------- */
#define SNIP_MAX 96
typedef struct {
    char lang[16];
    char prefix[24];
    char body[160];
    char desc[48];
    int  ext;
} UcSnippet;
static UcSnippet g_snip[SNIP_MAX];
static int       g_nsnip;

/* VS Code snippet bodies are an array of lines with ${1:placeholder} tab
 * stops.  There is no tab-stop machinery here, so the placeholders are
 * flattened to their default text - which is what actually helps: the snippet
 * expands to correct code you then edit, instead of to a template full of
 * dollar signs. */
static void snippet_body(UcJson *body, char *out, int cap)
{
    out[0] = 0;
    if (!body) return;
    if (body->type == UJ_STR) uc_scpy(out, body->str, cap);
    else if (body->type == UJ_ARR) {
        UcJson *e;
        for (e = body->child; e; e = e->next) {
            if (e->type != UJ_STR) continue;
            uc_scat(out, e->str, cap);
            if (e->next) uc_scat(out, "\n", cap);
        }
    }
    /* strip the tab stops */
    {
        int r = 0, w = 0;
        while (out[r]) {
            if (out[r] == '$' && out[r + 1] == '{') {
                int k = r + 2;
                while (out[k] && out[k] != ':' && out[k] != '}') k++;
                if (out[k] == ':') {
                    k++;
                    while (out[k] && out[k] != '}') out[w++] = out[k++];
                }
                if (out[k] == '}') k++;
                r = k;
                continue;
            }
            if (out[r] == '$' && out[r + 1] >= '0' && out[r + 1] <= '9') { r += 2; continue; }
            out[w++] = out[r++];
        }
        out[w] = 0;
    }
}

static void load_snippets(int ext, int vol, const char *path, const char *lang)
{
    char *src = 0;
    long len = 0;
    char err[80];
    UcJson *root, *m;
    if (!uc_read_file(vol, path, &src, &len)) return;
    root = uc_json_parse(src, (int)len, err, sizeof err);
    free(src);
    if (!root || root->type != UJ_OBJ) { if (root) uc_json_free(root); return; }
    for (m = root->child; m && g_nsnip < SNIP_MAX; m = m->next) {
        const char *pre = uc_json_str(m, "prefix", 0);
        if (!pre) continue;
        uc_scpy(g_snip[g_nsnip].lang, lang, sizeof g_snip[0].lang);
        uc_scpy(g_snip[g_nsnip].prefix, pre, sizeof g_snip[0].prefix);
        uc_scpy(g_snip[g_nsnip].desc, uc_json_str(m, "description", m->key ? m->key : ""),
                sizeof g_snip[0].desc);
        snippet_body(uc_json_member(m, "body"), g_snip[g_nsnip].body,
                     sizeof g_snip[0].body);
        g_snip[g_nsnip].ext = ext;
        if (g_snip[g_nsnip].body[0]) g_nsnip++;
    }
    uc_json_free(root);
}

int uc_ext_snippets(UcDoc *d, const char *prefix)
{
    UcLang *L;
    int i, n = 0;
    if (!d) return 0;
    L = uc_lang_at(d->lang);
    for (i = 0; i < g_nsnip; i++) {
        if (L && g_snip[i].lang[0] && strcmp(g_snip[i].lang, L->id)) continue;
        if (prefix && prefix[0] && uc_fuzzy(prefix, g_snip[i].prefix, 0, 0) < 0) continue;
        n += uc_suggest_add(g_snip[i].prefix, g_snip[i].desc, g_snip[i].body, UC_CI_SNIPPET);
    }
    return n;
}

/* ---- enable / disable state ---------------------------------------------------
 * Persisted as one settings key holding a comma-separated list of ids, which
 * means a user can turn an extension off by editing settings.json on another
 * machine, and it means the state survives a rescan that renumbers the table. */
static int is_disabled(const char *id)
{
    const char *list = uc_cfg_str("extensions.disabled");
    const char *p = list;
    int n = (int)strlen(id);
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!strncmp(p, id, (unsigned long)n) && (p[n] == 0 || p[n] == ',' || p[n] == ' '))
            return 1;
        while (*p && *p != ',') p++;
    }
    return 0;
}

static void write_disabled(void)
{
    char list[200], json[220];
    int i;
    list[0] = 0;
    for (i = 0; i < g_next; i++) {
        if (g_ext[i].enabled) continue;
        if (list[0]) uc_scat(list, ",", sizeof list);
        uc_scat(list, g_ext[i].id, sizeof list);
    }
    uc_scpy(json, "\"", sizeof json);
    uc_scat(json, list, sizeof json);
    uc_scat(json, "\"", sizeof json);
    uc_cfg_set("extensions.disabled", json);
}

/* ---- the manifest --------------------------------------------------------------- */
static void contribute(int ei, UcJson *root)
{
    UcExt *x = &g_ext[ei];
    UcJson *c = uc_json_member(root, "contributes"), *arr, *e;
    char dir[32];
    uc_ext_dir(ei, dir, sizeof dir);
    if (!c) return;

    arr = uc_json_member(c, "commands");
    if (arr && arr->type == UJ_ARR)
        for (e = arr->child; e; e = e->next) {
            const char *id = uc_json_str(e, "command", 0);
            const char *title = uc_json_str(e, "title", id);
            const char *cat = uc_json_str(e, "category", x->name);
            if (!id) continue;
            /* declared, not implemented: the handler slot stays -1 until the
             * extension's JS registers it.  That is what makes the command
             * visible in the palette before the extension has ever run. */
            uc_cmd_register(id, title, cat, 0, ei, -1);
            x->ncmd++;
        }

    arr = uc_json_member(c, "keybindings");
    if (arr && arr->type == UJ_ARR)
        for (e = arr->child; e; e = e->next) {
            const char *k = uc_json_str(e, "key", 0);
            const char *cmd = uc_json_str(e, "command", 0);
            int key, mods, key2, mods2;
            if (!k || !cmd) continue;
            if (!uc_key_parse(k, &key, &mods, &key2, &mods2)) continue;
            uc_keybind_add(key, mods, key2, mods2, cmd, uc_json_str(e, "when", ""), 2);
        }

    arr = uc_json_member(c, "themes");
    if (arr && arr->type == UJ_ARR)
        for (e = arr->child; e; e = e->next) {
            const char *label = uc_json_str(e, "label", 0);
            const char *path = uc_json_str(e, "path", 0);
            const char *ui = uc_json_str(e, "uiTheme", "vs-dark");
            char full[UC_PATH_MAX];
            if (!label || !path) continue;
            while (*path == '.' || *path == '/' || *path == '\\') path++;
            uc_path_join(full, sizeof full, dir, path);
            {   /* the manifest may use '/' - FAT wants '\' */
                int i;
                for (i = 0; full[i]; i++) if (full[i] == '/') full[i] = '\\';
            }
            uc_theme_register(label, x->vol, full, strcmp(ui, "vs") != 0, ei);
            x->ntheme++;
        }

    arr = uc_json_member(c, "languages");
    if (arr && arr->type == UJ_ARR)
        for (e = arr->child; e; e = e->next) {
            UcLang proto;
            UcJson *exts = uc_json_member(e, "extensions"), *x2;
            const char *id = uc_json_str(e, "id", 0);
            UcJson *cfg;
            if (!id) continue;
            memset(&proto, 0, sizeof proto);
            uc_scpy(proto.id, id, sizeof proto.id);
            uc_scpy(proto.name, uc_json_str(e, "aliases", id), sizeof proto.name);
            if (exts && exts->type == UJ_ARR)
                for (x2 = exts->child; x2 && proto.next < UC_LANG_EXTS; x2 = x2->next)
                    if (x2->type == UJ_STR) {
                        uc_scpy(proto.ext[proto.next], x2->str, sizeof proto.ext[0]);
                        uc_upper(proto.ext[proto.next]);
                        proto.next++;
                    }
            cfg = uc_json_member(e, "configuration");
            if (cfg) {
                UcJson *cmt = uc_json_member(cfg, "comments");
                if (cmt) {
                    uc_scpy(proto.line_comment, uc_json_str(cmt, "lineComment", ""),
                            sizeof proto.line_comment);
                }
            }
            proto.tabsize = 4;
            proto.ext_index = ei;
            uc_lang_register(&proto);
        }

    arr = uc_json_member(c, "grammars");
    if (arr && arr->type == UJ_ARR)
        for (e = arr->child; e; e = e->next) {
            const char *lang = uc_json_str(e, "language", 0);
            const char *path = uc_json_str(e, "path", 0);
            char full[UC_PATH_MAX];
            int li;
            if (!lang || !path) continue;
            li = uc_lang_by_id(lang);
            if (li < 0) continue;
            while (*path == '.' || *path == '/' || *path == '\\') path++;
            uc_path_join(full, sizeof full, dir, path);
            { int i; for (i = 0; full[i]; i++) if (full[i] == '/') full[i] = '\\'; }
            if (uc_lang_load_grammar(li, x->vol, full)) x->ngram++;
        }

    arr = uc_json_member(c, "snippets");
    if (arr && arr->type == UJ_ARR)
        for (e = arr->child; e; e = e->next) {
            const char *lang = uc_json_str(e, "language", "");
            const char *path = uc_json_str(e, "path", 0);
            char full[UC_PATH_MAX];
            int before = g_nsnip;
            if (!path) continue;
            while (*path == '.' || *path == '/' || *path == '\\') path++;
            uc_path_join(full, sizeof full, dir, path);
            { int i; for (i = 0; full[i]; i++) if (full[i] == '/') full[i] = '\\'; }
            load_snippets(ei, x->vol, full, lang);
            x->nsnip += g_nsnip - before;
        }
}

/* ---- activation ------------------------------------------------------------------ */
#define ACTEV_MAX 8
static char g_actev[UC_EXT_MAX][ACTEV_MAX][48];
static int  g_nactev[UC_EXT_MAX];

static void activate(int i)
{
    UcExt *x = &g_ext[i];
    char dir[32], path[UC_PATH_MAX];
    unsigned long t0;
    if (x->activated || !x->enabled || x->broken) return;
    if (!x->main[0]) { x->activated = 1; return; }     /* declarative only */
    if (!uc_api_init()) {
        x->broken = 1;
        uc_scpy(x->err, "the extension host could not start", sizeof x->err);
        return;
    }
    uc_ext_dir(i, dir, sizeof dir);
    uc_path_join(path, sizeof path, dir, x->main);
    t0 = uno_dbg_uptime_ms();
    if (!uc_api_run_file(i, x->vol, path, x->err, sizeof x->err)) {
        x->broken = 1;
        uc_notify(x->err, UC_SEV_ERROR);
        return;
    }
    x->act_ms = uno_dbg_uptime_ms() - t0;
    x->activated = 1;
}

void uc_ext_activate_event(const char *event)
{
    int i, k;
    if (!event || !uc_cfg_bool("extensions.autoActivate")) return;
    for (i = 0; i < g_next; i++) {
        if (g_ext[i].activated || !g_ext[i].enabled) continue;
        for (k = 0; k < g_nactev[i]; k++) {
            if (!strcmp(g_actev[i][k], "*") || !strcmp(g_actev[i][k], event)) {
                activate(i);
                break;
            }
        }
    }
}

int uc_ext_activate_for_command(const char *id)
{
    char ev[64];
    uc_scpy(ev, "onCommand:", sizeof ev);
    uc_scat(ev, id, sizeof ev);
    uc_ext_activate_event(ev);
    return 1;
}

void uc_ext_activate_startup(void)
{
    uc_ext_activate_event("*");
    uc_ext_activate_event("onStartupFinished");
}

int uc_ext_enable(int i, int on)
{
    UcExt *x = uc_ext_at(i);
    if (!x) return 0;
    x->enabled = on ? 1 : 0;
    write_disabled();
    if (!on) {
        uc_cmd_drop_ext(i);
        uc_api_drop_ext(i);
        x->activated = 0;
    } else {
        uc_ext_reload();
    }
    return 1;
}

/* ---- the scan ---------------------------------------------------------------------- */
static void read_manifest(int vol, const char *id)
{
    char dir[32], path[UC_PATH_MAX];
    char *src = 0;
    long len = 0;
    char err[80];
    UcJson *root, *acts;
    UcExt *x;
    int i;
    if (g_next >= UC_EXT_MAX) return;
    if (uc_ext_find(id) >= 0) return;              /* first volume wins */
    uc_scpy(dir, "EXT\\", sizeof dir);
    uc_scat(dir, id, sizeof dir);
    uc_path_join(path, sizeof path, dir, "PACKAGE.JSN");
    if (!uc_read_file(vol, path, &src, &len)) {
        uc_path_join(path, sizeof path, dir, "PACKAGE.JSON");
        if (!uc_read_file(vol, path, &src, &len)) return;
    }
    x = &g_ext[g_next];
    memset(x, 0, sizeof *x);
    uc_scpy(x->id, id, sizeof x->id);
    x->vol = vol;
    x->enabled = !is_disabled(id);
    root = uc_json_parse(src, (int)len, err, sizeof err);
    free(src);
    if (!root) {
        x->broken = 1;
        uc_scpy(x->name, id, sizeof x->name);
        uc_scpy(x->err, "PACKAGE.JSN: ", sizeof x->err);
        uc_scat(x->err, err, sizeof x->err);
        g_next++;
        return;
    }
    uc_scpy(x->name, uc_json_str(root, "displayName", uc_json_str(root, "name", id)),
            sizeof x->name);
    uc_scpy(x->publisher, uc_json_str(root, "publisher", ""), sizeof x->publisher);
    uc_scpy(x->version, uc_json_str(root, "version", "0.0.0"), sizeof x->version);
    uc_scpy(x->desc, uc_json_str(root, "description", ""), sizeof x->desc);
    {
        const char *m = uc_json_str(root, "main", 0);
        if (m) {
            const char *base = m;
            const char *p;
            for (p = m; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
            uc_scpy(x->main, base, sizeof x->main);
            uc_upper(x->main);
        }
    }
    acts = uc_json_member(root, "activationEvents");
    g_nactev[g_next] = 0;
    if (acts && acts->type == UJ_ARR) {
        UcJson *e;
        for (e = acts->child; e && g_nactev[g_next] < ACTEV_MAX; e = e->next)
            if (e->type == UJ_STR)
                uc_scpy(g_actev[g_next][g_nactev[g_next]++], e->str, 48);
    }
    if (!g_nactev[g_next] && x->main[0]) {
        /* no activationEvents and a main: VS Code's modern default is
         * "activate when something the manifest contributes is used", which
         * for us is close enough to onStartupFinished */
        uc_scpy(g_actev[g_next][g_nactev[g_next]++], "onStartupFinished", 48);
    }
    i = g_next++;
    if (x->enabled) contribute(i, root);
    uc_json_free(root);
}

static void scan_volume(int vol)
{
    static char names[UC_EXT_MAX * 2][16];
    static unsigned char isdir[UC_EXT_MAX * 2];
    int n, i;
    /* No uno_fs_isdir() gate on EXT itself: that call answers only for native
     * FAT, and gating on it made the whole scan a no-op on any other backing.
     * A listing that comes back empty is the same answer, one call later. */
    n = uc_list_dir(vol, "EXT", names, isdir, UC_EXT_MAX * 2);
    if (n > UC_EXT_MAX * 2) n = UC_EXT_MAX * 2;
    for (i = 0; i < n; i++) {
        if (!names[i][0] || names[i][0] == '.') continue;
        /* The manifest read is the real test of "is this an extension", so a
         * backing that cannot report the dir flag still works. */
        read_manifest(vol, names[i]);
    }
}

void uc_ext_init(void)
{
    int nv = uno_fs_volumes(), v;
    g_next = 0;
    g_nsnip = 0;
    for (v = 0; v < nv; v++) scan_volume(v);
}

void uc_ext_reload(void)
{
    int i;
    for (i = 0; i < g_next; i++) { uc_cmd_drop_ext(i); uc_api_drop_ext(i); }
    uc_api_shutdown();
    uc_ext_init();
    uc_ext_activate_startup();
    {
        char msg[64], num[12];
        uc_itoa(num, g_next);
        uc_scpy(msg, num, sizeof msg);
        uc_scat(msg, " extensions loaded", sizeof msg);
        uc_notify(msg, UC_SEV_INFO);
    }
}
