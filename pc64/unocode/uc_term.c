/* ===========================================================================
 * uc_term.c - the integrated terminal, the task runner and Run/Debug.
 *
 * VS Code's terminal hosts the machine's shell.  UnoDOS/pc64 has no shell
 * process to host - there is no process model to host one IN - so this is a
 * small shell of its own over the calls a module already has: the filesystem,
 * the settings store, the theme list, the extension host and the app loader.
 * That is a real limitation and it is stated rather than papered over: `help`
 * lists exactly what exists, and an unknown word is an error, never a silent
 * no-op that reads like a shell that ran something.
 *
 * WHAT MAKES IT WORTH HAVING: `js` evaluates an expression in the SAME
 * extension-host VM the extensions run in, which is the difference between an
 * extension system you can debug and one you can only stare at.
 *
 * Tasks (TASKS.JSN) and launch configurations (LAUNCH.JSN) are read from the
 * workspace folder in VS Code's shape, and both end up executing through this
 * shell - one execution path, so a task cannot do something the terminal
 * cannot, and cannot fail differently.
 * ======================================================================== */
#include "unocode.h"

#define TERM_LINES 300
#define TERM_COLS  160
#define HIST_MAX   24

static char  g_line[TERM_LINES][TERM_COLS];
static int   g_nline, g_head, g_scroll;
static char  g_input[TERM_COLS];
static int   g_ilen, g_icaret;
static char  g_hist[HIST_MAX][TERM_COLS];
static int   g_nhist, g_hpos;
static char  g_cwd[UC_PATH_MAX];
static int   g_cwvol = -1;
static int   g_ready;

/* ---- output ---------------------------------------------------------------- */
static char *line_at(int i) { return g_line[(g_head + i) % TERM_LINES]; }

static void term_newline(void)
{
    if (g_nline < TERM_LINES) g_nline++;
    else g_head = (g_head + 1) % TERM_LINES;
    line_at(g_nline - 1)[0] = 0;
}

void uc_term_write(const char *s)
{
    if (!s) return;
    if (!g_nline) term_newline();
    while (*s) {
        char *cur = line_at(g_nline - 1);
        int n = (int)strlen(cur);
        if (*s == '\n') { term_newline(); s++; continue; }
        if (*s == '\r') { s++; continue; }
        if (n >= TERM_COLS - 1) { term_newline(); continue; }
        cur[n] = *s++;
        cur[n + 1] = 0;
    }
    g_scroll = 0;
}

void uc_term_writeln(const char *s) { uc_term_write(s); uc_term_write("\n"); }

void uc_term_clear(void) { g_nline = 0; g_head = 0; g_scroll = 0; }

/* ---- the PYAPP container ---------------------------------------------------
 * A Python file is "built" by wrapping it in a UNO_MODF_PYAPP container - the
 * 48-byte UnoModHdr and the raw source - which the shell hands to PYRT.UNO.
 * The format is the app-registry lane's contract (uno_appdesc.h, tools/
 * mkuno.py `pyapp`); this writes it field for field so a container built here
 * is byte-identical to the toolchain's, crc included. */
#define PY_MAGIC 0x314F4E55u
#define PY_ABI   1
#define PY_MODF_PYAPP 0x0004u
#define PY_HDR   48

static unsigned int py_crc32(const unsigned char *p, int n)
{
    unsigned int c = 0xFFFFFFFFu;
    int i, k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
static void put_u32(unsigned char *p, unsigned int v)
{ p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }
static void put_u16(unsigned char *p, unsigned short v)
{ p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }

static int py_pack(const unsigned char *src, int len, unsigned char *out, int cap)
{
    int i;
    if (len < 0 || PY_HDR + len > cap) return -1;
    for (i = 0; i < PY_HDR; i++) out[i] = 0;
    put_u32(out + 0,  PY_MAGIC);
    put_u16(out + 4,  PY_ABI);
    put_u16(out + 6,  (unsigned short)PY_MODF_PYAPP);
    put_u32(out + 12, (unsigned)len);
    put_u32(out + 16, (unsigned)len);
    put_u32(out + 40, py_crc32(src, len));
    for (i = 0; i < len; i++) out[PY_HDR + i] = src[i];
    return PY_HDR + len;
}

/* ---- running a file --------------------------------------------------------- */
static int run_file(int vol, const char *dir, const char *name)
{
    char path[UC_PATH_MAX + 20];
    uc_path_join(path, sizeof path, dir, name);
    if (uc_ends_icase(name, ".UNO")) {
        if (pc64_shell_run_user(vol, path) < 0) {
            uc_term_writeln("could not launch the module");
            return 0;
        }
        return 1;
    }
    if (uc_ends_icase(name, ".PY")) {
        char *src = 0;
        long len = 0;
        unsigned char *cont;
        char out[UC_PATH_MAX + 20];
        int n, ok = 0, i;
        if (!uc_read_file(vol, path, &src, &len)) { uc_term_writeln("no such file"); return 0; }
        cont = (unsigned char *)malloc((unsigned long)len + PY_HDR + 8);
        if (!cont) { free(src); return 0; }
        n = py_pack((const unsigned char *)src, (int)len, cont, (int)len + PY_HDR + 8);
        free(src);
        /* the container needs a name on disk for the loader to read it back;
         * it goes beside the source with a .UNO extension */
        uc_scpy(out, name, sizeof out);
        for (i = (int)strlen(out) - 1; i > 0; i--) if (out[i] == '.') { out[i] = 0; break; }
        uc_scat(out, ".UNO", sizeof out);
        {
            char full[UC_PATH_MAX + 24];
            uc_path_join(full, sizeof full, dir, out);
            if (n > 0 && uno_fs_write(vol, full, cont, n))
                ok = pc64_shell_run_user(vol, full) >= 0;
        }
        free(cont);
        if (!ok) {
            const char *err = pc64_shell_py_error();
            uc_term_writeln(err && err[0] ? err : "the Python app did not start");
        }
        return ok;
    }
    uc_term_writeln("nothing to run: expected a .PY or a .UNO");
    return 0;
}

/* ---- argument splitting ------------------------------------------------------ */
static int split(char *s, char **argv, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '"') {
            s++;
            argv[n++] = s;
            while (*s && *s != '"') s++;
            if (*s) *s++ = 0;
        } else {
            argv[n++] = s;
            while (*s && *s != ' ') s++;
            if (*s) *s++ = 0;
        }
    }
    return n;
}

/* ---- the commands ------------------------------------------------------------- */
static void cmd_help(void)
{
    uc_term_writeln("UnoCode shell - the commands this build has:");
    uc_term_writeln("  ls [dir]        list a directory      cd <dir>     change directory");
    uc_term_writeln("  cat <file>      print a file          pwd          where you are");
    uc_term_writeln("  vol             list volumes          vol <n>      switch volume");
    uc_term_writeln("  mkdir <dir>     make a directory      rm <file>    delete a file");
    uc_term_writeln("  cp <a> <b>      copy a file           echo <text>  print text");
    uc_term_writeln("  find <text>     search the folder     wc <file>    count lines/bytes");
    uc_term_writeln("  open <file>     open in the editor    run <file>   run a .PY or .UNO");
    uc_term_writeln("  set <key> <val> change a setting      get <key>    read a setting");
    uc_term_writeln("  theme [name]    list or set the theme ext          list extensions");
    uc_term_writeln("  cmd <id>        run any UnoCode command");
    uc_term_writeln("  js <expr>       evaluate in the extension host");
    uc_term_writeln("  task [label]    run a task            clear        clear the terminal");
}

static void cmd_ls(const char *arg)
{
    static char names[160][16];
    static unsigned char isdir[160];
    char dir[UC_PATH_MAX];
    int n, i;
    if (arg && arg[0]) uc_path_join(dir, sizeof dir, g_cwd, arg);
    else uc_scpy(dir, g_cwd, sizeof dir);
    n = uc_list_dir(g_cwvol, dir, names, isdir, 160);
    if (n <= 0) { uc_term_writeln("(empty)"); return; }
    if (n > 160) n = 160;
    for (i = 0; i < n; i++) {
        char full[UC_PATH_MAX], row[64], num[16];
        if (!names[i][0]) continue;
        uc_path_join(full, sizeof full, dir, names[i]);
        uc_scpy(row, names[i], sizeof row);
        while (strlen(row) < 14) uc_scat(row, " ", sizeof row);
        if (isdir[i]) uc_scat(row, "<dir>", sizeof row);
        else {
            uc_itoa(num, uno_fs_size(g_cwvol, full));
            uc_scat(row, num, sizeof row);
        }
        uc_term_writeln(row);
    }
}

static void cmd_cat(const char *arg)
{
    char path[UC_PATH_MAX + 20];
    char *src = 0;
    long len = 0;
    if (!arg || !arg[0]) { uc_term_writeln("cat: which file?"); return; }
    uc_path_join(path, sizeof path, g_cwd, arg);
    if (!uc_read_file(g_cwvol, path, &src, &len)) { uc_term_writeln("cat: no such file"); return; }
    if (len > 32768) { len = 32768; }
    src[len] = 0;
    uc_term_write(src);
    uc_term_write("\n");
    free(src);
}

static void cmd_vol(const char *arg)
{
    int n = uno_fs_volumes(), i;
    if (arg && arg[0]) {
        int v = (int)strtol(arg, 0, 10);
        if (v < 0 || v >= n) { uc_term_writeln("vol: no such volume"); return; }
        g_cwvol = v;
        g_cwd[0] = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        char row[64];
        uc_scpy(row, i == g_cwvol ? "* " : "  ", sizeof row);
        { char num[8]; uc_itoa(num, i); uc_scat(row, num, sizeof row); }
        uc_scat(row, "  ", sizeof row);
        uc_scat(row, uno_fs_volume_name(i), sizeof row);
        uc_scat(row, uno_fs_writable(i) ? "  rw" : "  ro", sizeof row);
        uc_term_writeln(row);
    }
}

static void cmd_find(const char *arg)
{
    if (!arg || !arg[0]) { uc_term_writeln("find: what text?"); return; }
    uc_search_run(arg);
    uc_toggle_sidebar(UC_VIEW_SEARCH);
    uc_term_writeln("results are in the Search view");
}

static void cmd_theme(const char *arg)
{
    int i;
    if (!arg || !arg[0]) {
        for (i = 0; i < uc_theme_count(); i++) {
            UcTheme *t = uc_theme_at(i);
            char row[64];
            uc_scpy(row, t == uc_theme_active() ? "* " : "  ", sizeof row);
            uc_scat(row, t->name, sizeof row);
            uc_term_writeln(row);
        }
        return;
    }
    if (uc_theme_select(arg)) {
        char json[64];
        uc_scpy(json, "\"", sizeof json);
        uc_scat(json, uc_theme_active()->name, sizeof json);
        uc_scat(json, "\"", sizeof json);
        uc_cfg_set("workbench.colorTheme", json);
    } else uc_term_writeln("theme: no such theme");
}

static void cmd_ext(void)
{
    int i;
    if (!uc_ext_count()) { uc_term_writeln("no extensions installed"); return; }
    for (i = 0; i < uc_ext_count(); i++) {
        UcExt *e = uc_ext_at(i);
        char row[110];
        uc_scpy(row, e->enabled ? "[x] " : "[ ] ", sizeof row);
        uc_scat(row, e->id, sizeof row);
        while (strlen(row) < 16) uc_scat(row, " ", sizeof row);
        uc_scat(row, e->version, sizeof row);
        uc_scat(row, "  ", sizeof row);
        uc_scat(row, e->broken ? e->err : (e->activated ? "active" : "idle"), sizeof row);
        uc_term_writeln(row);
    }
}

static void run_command(char *cmdline);

/* ---- tasks and launch configurations ------------------------------------------ */
#define TASK_MAX 12
static struct { char label[32]; char command[110]; char group[16]; } g_task[TASK_MAX];
static int g_ntask;
static struct { char name[40]; char program[64]; } g_launch[8];
static int g_nlaunch;

int uc_tasks_count(void) { return g_ntask; }
const char *uc_task_label(int i) { return (i >= 0 && i < g_ntask) ? g_task[i].label : ""; }
int uc_launch_count(void) { return g_nlaunch; }
const char *uc_launch_name(int i) { return (i >= 0 && i < g_nlaunch) ? g_launch[i].name : ""; }

void uc_tasks_reload(void)
{
    char path[UC_PATH_MAX + 20];
    char *src = 0;
    long len = 0;
    char err[80];
    UcJson *root, *arr, *e;
    g_ntask = 0;
    g_nlaunch = 0;

    uc_path_join(path, sizeof path, UC.ws_dir, "TASKS.JSN");
    if (uc_read_file(UC.ws_vol, path, &src, &len)) {
        root = uc_json_parse(src, (int)len, err, sizeof err);
        free(src);
        if (root) {
            arr = uc_json_member(root, "tasks");
            if (arr && arr->type == UJ_ARR)
                for (e = arr->child; e && g_ntask < TASK_MAX; e = e->next) {
                    uc_scpy(g_task[g_ntask].label, uc_json_str(e, "label", "task"),
                            sizeof g_task[0].label);
                    uc_scpy(g_task[g_ntask].command, uc_json_str(e, "command", ""),
                            sizeof g_task[0].command);
                    uc_scpy(g_task[g_ntask].group, uc_json_str(e, "group", ""),
                            sizeof g_task[0].group);
                    if (g_task[g_ntask].command[0]) g_ntask++;
                }
            uc_json_free(root);
        }
    }
    src = 0; len = 0;
    uc_path_join(path, sizeof path, UC.ws_dir, "LAUNCH.JSN");
    if (uc_read_file(UC.ws_vol, path, &src, &len)) {
        root = uc_json_parse(src, (int)len, err, sizeof err);
        free(src);
        if (root) {
            arr = uc_json_member(root, "configurations");
            if (arr && arr->type == UJ_ARR)
                for (e = arr->child; e && g_nlaunch < 8; e = e->next) {
                    uc_scpy(g_launch[g_nlaunch].name, uc_json_str(e, "name", "Launch"),
                            sizeof g_launch[0].name);
                    uc_scpy(g_launch[g_nlaunch].program, uc_json_str(e, "program", "${file}"),
                            sizeof g_launch[0].program);
                    g_nlaunch++;
                }
            uc_json_free(root);
        }
    }
}

void uc_tasks_run(const char *label)
{
    int i = 0;
    if (!g_ntask) {
        uc_notify("No tasks.json in this folder", UC_SEV_WARN);
        return;
    }
    if (label) {
        for (i = 0; i < g_ntask; i++) if (!strcmp(g_task[i].label, label)) break;
        if (i >= g_ntask) { uc_notify("No such task", UC_SEV_WARN); return; }
    } else {
        for (i = 0; i < g_ntask; i++) if (!strcmp(g_task[i].group, "build")) break;
        if (i >= g_ntask) i = 0;
    }
    uc_toggle_panel(UC_PANEL_TERMINAL);
    {
        char cmd[120];
        uc_term_write("> task: ");
        uc_term_writeln(g_task[i].label);
        uc_scpy(cmd, g_task[i].command, sizeof cmd);
        run_command(cmd);
    }
}

void uc_launch_run(int i)
{
    UcDoc *d = uc_doc_active();
    if (i < 0) i = 0;
    if (g_nlaunch && i < g_nlaunch && strcmp(g_launch[i].program, "${file}")) {
        char cmd[120];
        uc_scpy(cmd, "run ", sizeof cmd);
        uc_scat(cmd, g_launch[i].program, sizeof cmd);
        uc_toggle_panel(UC_PANEL_TERMINAL);
        run_command(cmd);
        return;
    }
    if (!d || !d->name[0]) { uc_notify("Nothing to run: save the file first", UC_SEV_WARN); return; }
    if (d->dirty) uc_doc_save(d);
    uc_toggle_panel(UC_PANEL_TERMINAL);
    uc_term_write("> run ");
    uc_term_writeln(d->name);
    run_file(d->vol, d->dir, d->name);
}

/* ---- the dispatcher ------------------------------------------------------------ */
static void run_command(char *cmdline)
{
    char *argv[12];
    int argc = split(cmdline, argv, 12);
    const char *a1 = argc > 1 ? argv[1] : "";
    if (!argc) return;

    if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) { cmd_help(); return; }
    if (!strcmp(argv[0], "clear") || !strcmp(argv[0], "cls")) { uc_term_clear(); return; }
    if (!strcmp(argv[0], "ls") || !strcmp(argv[0], "dir")) { cmd_ls(a1); return; }
    if (!strcmp(argv[0], "pwd")) {
        char row[UC_PATH_MAX + 24];
        uc_scpy(row, uno_fs_volume_name(g_cwvol), sizeof row);
        uc_scat(row, "\\", sizeof row);
        uc_scat(row, g_cwd, sizeof row);
        uc_term_writeln(row);
        return;
    }
    if (!strcmp(argv[0], "cd")) {
        char next[UC_PATH_MAX];
        if (!a1[0] || !strcmp(a1, "\\")) { g_cwd[0] = 0; return; }
        if (!strcmp(a1, "..")) {
            int i;
            for (i = (int)strlen(g_cwd) - 1; i >= 0; i--)
                if (g_cwd[i] == '\\') { g_cwd[i] = 0; return; }
            g_cwd[0] = 0;
            return;
        }
        uc_path_join(next, sizeof next, g_cwd, a1);
        if (!uno_fs_isdir(g_cwvol, next)) { uc_term_writeln("cd: no such directory"); return; }
        uc_scpy(g_cwd, next, sizeof g_cwd);
        return;
    }
    if (!strcmp(argv[0], "cat") || !strcmp(argv[0], "type")) { cmd_cat(a1); return; }
    if (!strcmp(argv[0], "vol")) { cmd_vol(a1); return; }
    if (!strcmp(argv[0], "echo")) {
        int i;
        for (i = 1; i < argc; i++) { uc_term_write(argv[i]); if (i + 1 < argc) uc_term_write(" "); }
        uc_term_write("\n");
        return;
    }
    if (!strcmp(argv[0], "mkdir")) {
        char p[UC_PATH_MAX];
        uc_path_join(p, sizeof p, g_cwd, a1);
        uc_term_writeln(uno_fs_mkdir(g_cwvol, p) ? "created" : "mkdir failed");
        return;
    }
    if (!strcmp(argv[0], "rm") || !strcmp(argv[0], "del")) {
        char p[UC_PATH_MAX];
        int fi = uno_fs_fat_index(g_cwvol);
        uc_path_join(p, sizeof p, g_cwd, a1);
        if (fi < 0) { uc_term_writeln("rm: this volume cannot delete"); return; }
        uc_term_writeln(uno_fat_delete(fi, p) ? "deleted" : "rm failed");
        return;
    }
    if (!strcmp(argv[0], "cp") || !strcmp(argv[0], "copy")) {
        char s[UC_PATH_MAX], dst[UC_PATH_MAX];
        char *src = 0;
        long len = 0;
        if (argc < 3) { uc_term_writeln("cp: cp <from> <to>"); return; }
        uc_path_join(s, sizeof s, g_cwd, argv[1]);
        uc_path_join(dst, sizeof dst, g_cwd, argv[2]);
        if (!uc_read_file(g_cwvol, s, &src, &len)) { uc_term_writeln("cp: no such file"); return; }
        uc_term_writeln(uno_fs_write(g_cwvol, dst, (const unsigned char *)src, len)
                        ? "copied" : "cp failed");
        free(src);
        return;
    }
    if (!strcmp(argv[0], "wc")) {
        char p[UC_PATH_MAX], row[64], num[16];
        char *src = 0;
        long len = 0, i, lines = 1;
        uc_path_join(p, sizeof p, g_cwd, a1);
        if (!uc_read_file(g_cwvol, p, &src, &len)) { uc_term_writeln("wc: no such file"); return; }
        for (i = 0; i < len; i++) if (src[i] == '\n') lines++;
        free(src);
        uc_itoa(num, lines);
        uc_scpy(row, num, sizeof row);
        uc_scat(row, " lines, ", sizeof row);
        uc_itoa(num, len);
        uc_scat(row, num, sizeof row);
        uc_scat(row, " bytes", sizeof row);
        uc_term_writeln(row);
        return;
    }
    if (!strcmp(argv[0], "find") || !strcmp(argv[0], "grep")) { cmd_find(a1); return; }
    if (!strcmp(argv[0], "open")) {
        if (uc_doc_open(g_cwvol, g_cwd, a1) < 0) uc_term_writeln("open: no such file");
        else uc_focus(UC_F_EDITOR);
        return;
    }
    if (!strcmp(argv[0], "run")) {
        if (!a1[0]) { uc_launch_run(-1); return; }
        run_file(g_cwvol, g_cwd, a1);
        return;
    }
    if (!strcmp(argv[0], "set")) {
        if (argc < 3) { uc_term_writeln("set: set <key> <json value>"); return; }
        uc_term_writeln(uc_cfg_set(argv[1], argv[2]) ? "saved" : "could not write settings");
        if (!strcmp(argv[1], "workbench.colorTheme")) uc_theme_select(uc_cfg_str(argv[1]));
        uc_metrics_init();
        return;
    }
    if (!strcmp(argv[0], "get")) {
        const UcSettingDef *d = uc_cfg_find(a1);
        char row[160];
        if (!d) { uc_term_writeln("get: unknown setting"); return; }
        uc_scpy(row, a1, sizeof row);
        uc_scat(row, " = ", sizeof row);
        uc_scat(row, uc_cfg_str(a1), sizeof row);
        uc_scat(row, uc_cfg_is_user(a1) ? "   (user)" : "   (default)", sizeof row);
        uc_term_writeln(row);
        return;
    }
    if (!strcmp(argv[0], "theme")) { cmd_theme(a1); return; }
    if (!strcmp(argv[0], "ext")) { cmd_ext(); return; }
    if (!strcmp(argv[0], "cmd")) {
        if (!a1[0]) { uc_term_writeln("cmd: which command id?"); return; }
        if (!uc_cmd_run(a1)) uc_term_writeln("cmd: no such command");
        return;
    }
    if (!strcmp(argv[0], "task")) { uc_tasks_run(a1[0] ? a1 : 0); return; }
    if (!strcmp(argv[0], "js")) {
        /* rejoin: split() has already NUL-separated the words */
        char expr[TERM_COLS];
        int i;
        expr[0] = 0;
        for (i = 1; i < argc; i++) {
            uc_scat(expr, argv[i], sizeof expr);
            if (i + 1 < argc) uc_scat(expr, " ", sizeof expr);
        }
        if (!expr[0]) { uc_term_writeln("js: what expression?"); return; }
        uc_api_eval_print(expr);
        return;
    }
    if (!strcmp(argv[0], "uptime")) {
        char row[40], num[20];
        uc_itoa(num, (long)(uno_dbg_uptime_ms() / 1000));
        uc_scpy(row, "up ", sizeof row);
        uc_scat(row, num, sizeof row);
        uc_scat(row, " s", sizeof row);
        uc_term_writeln(row);
        return;
    }
    if (!strcmp(argv[0], "ver")) {
        uc_term_writeln("UnoCode 1.0 on UnoDOS/pc64");
        uc_term_write("extension host: ");
        uc_term_writeln(uc_api_engine());
        return;
    }
    {
        char row[80];
        uc_scpy(row, argv[0], sizeof row);
        uc_scat(row, ": not a command (try `help`)", sizeof row);
        uc_term_writeln(row);
    }
}

void uc_term_run(const char *cmdline)
{
    char buf[TERM_COLS];
    uc_scpy(buf, cmdline, sizeof buf);
    run_command(buf);
}

/* ---- the prompt ------------------------------------------------------------------ */
static void prompt_text(char *out, int cap)
{
    uc_scpy(out, uno_fs_volume_name(g_cwvol), cap);
    uc_scat(out, "\\", cap);
    uc_scat(out, g_cwd, cap);
    uc_scat(out, "> ", cap);
}

void uc_term_init(void)
{
    if (g_ready) return;
    g_ready = 1;
    g_cwvol = UC.ws_vol;
    uc_scpy(g_cwd, UC.ws_dir, sizeof g_cwd);
    uc_term_writeln("UnoCode terminal.  Type `help` for the commands this build has.");
    uc_tasks_reload();
}

void uc_term_draw(UcRect r, int focused)
{
    int rh = uc_line_h(), rows = r.h / rh, i, first;
    char prompt[UC_PATH_MAX + 24];
    fb_fill_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_TERM_BG));
    prompt_text(prompt, sizeof prompt);
    first = g_nline - (rows - 1) - g_scroll;
    if (first < 0) first = 0;
    for (i = 0; i < rows - 1 && first + i < g_nline; i++)
        uc_mono(r.x + 8, r.y + i * rh, line_at(first + i), uc_col(UC_C_TERM_FG), 0);
    /* the input line always sits at the bottom */
    {
        int y = r.y + (rows - 1) * rh, x;
        x = uc_mono(r.x + 8, y, prompt, uc_col(UC_C_TERM_GREEN), 0);
        uc_mono_n(x, y, g_input, g_ilen, uc_col(UC_C_TERM_FG), 0);
        if (focused && ((uno_dbg_uptime_ms() / 530) & 1))
            fb_fill_rect(x + g_icaret * uc_char_w(), y, 2, rh, uc_col(UC_C_CURSOR));
    }
}

int uc_term_key(int key, int mods, int ch)
{
    if (key == UI_KEY_ENTER) {
        char prompt[UC_PATH_MAX + 24];
        prompt_text(prompt, sizeof prompt);
        uc_term_write(prompt);
        uc_term_writeln(g_input);
        if (g_ilen) {
            if (g_nhist >= HIST_MAX) {
                int i;
                for (i = 0; i < HIST_MAX - 1; i++) uc_scpy(g_hist[i], g_hist[i+1], TERM_COLS);
                g_nhist--;
            }
            uc_scpy(g_hist[g_nhist++], g_input, TERM_COLS);
            g_hpos = g_nhist;
            uc_term_run(g_input);
        }
        g_ilen = 0;
        g_icaret = 0;
        g_input[0] = 0;
        return 1;
    }
    if (key == UI_KEY_BACKSPACE) {
        if (g_icaret > 0) {
            memmove(g_input + g_icaret - 1, g_input + g_icaret,
                    (unsigned long)(g_ilen - g_icaret + 1));
            g_ilen--;
            g_icaret--;
        }
        return 1;
    }
    if (key == UI_KEY_LEFT)  { if (g_icaret > 0) g_icaret--; return 1; }
    if (key == UI_KEY_RIGHT) { if (g_icaret < g_ilen) g_icaret++; return 1; }
    if (key == UI_KEY_HOME)  { g_icaret = 0; return 1; }
    if (key == UI_KEY_END)   { g_icaret = g_ilen; return 1; }
    if (key == UI_KEY_UP) {
        if (g_hpos > 0) {
            g_hpos--;
            uc_scpy(g_input, g_hist[g_hpos], sizeof g_input);
            g_ilen = g_icaret = (int)strlen(g_input);
        }
        return 1;
    }
    if (key == UI_KEY_DOWN) {
        if (g_hpos < g_nhist - 1) {
            g_hpos++;
            uc_scpy(g_input, g_hist[g_hpos], sizeof g_input);
        } else { g_hpos = g_nhist; g_input[0] = 0; }
        g_ilen = g_icaret = (int)strlen(g_input);
        return 1;
    }
    if (key == UI_KEY_PGUP) { g_scroll += 5; return 1; }
    if (key == UI_KEY_PGDN) { g_scroll -= 5; if (g_scroll < 0) g_scroll = 0; return 1; }
    if (ch >= 32 && ch < 127 && g_ilen < TERM_COLS - 2) {
        memmove(g_input + g_icaret + 1, g_input + g_icaret,
                (unsigned long)(g_ilen - g_icaret + 1));
        g_input[g_icaret++] = (char)ch;
        g_ilen++;
        return 1;
    }
    (void)mods;
    return 0;
}
