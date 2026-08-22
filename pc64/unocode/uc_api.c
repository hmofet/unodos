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
 * uc_api.c - the extension host: the `vscode` API projected into unojs.
 *
 * WHY unojs.  UnoDOS already carries two JavaScript engines for the browser
 * (pc64/js.h dispatches between them).  unojs is the one with an EMBEDDING
 * API - host functions, host objects, handle scopes - and, decisively, with
 * FUEL: `fuel_per_slice` and `fuel_total` mean an extension that loops forever
 * returns control instead of freezing the desktop.  On a machine with no
 * preemption and no process boundary, that is not a nicety; it is the only
 * thing standing between a bad extension and a dead machine.  A misbehaving
 * extension here costs a notification and gets disabled.
 *
 * WHAT THE API IS.  A real subset of `vscode`, using its names and shapes:
 * commands, window, workspace, languages, plus Position/Range/Uri.  Two
 * deliberate deviations, both stated in UNOCODE.md rather than hidden:
 *
 *   1. Asynchronous calls return a THENABLE with `.then(cb)`, not a Promise.
 *      There is no event loop and no microtask queue to build a real Promise
 *      on; `.then` is the part extensions actually use and it works.
 *   2. There is no `require` of arbitrary modules.  `require('vscode')`
 *      returns the API object and anything else throws, because there is no
 *      module resolver and pretending otherwise would fail later and less
 *      clearly.
 *
 * EVERY entry point from C into JS goes through call_fn(), which is where the
 * fuel budget, the yield loop and the exception reporting live - so there is
 * one place that can hang and one place that reports why it did not.
 * ======================================================================== */
#include "unocode.h"
#include "unojs.h"
#include "uc_secret.h"
#include "uc_http.h"      /* the uc_buf_* appenders, for vscode.lm's request */

/* ---- state ------------------------------------------------------------------ */
#define HANDLERS_MAX 128
#define PROVIDERS_MAX 16
#define LISTENERS_MAX 24
#define THENABLES_MAX 16

static ujs_vm *g_vm;
static ujs_val g_handlers;                 /* rooted array of JS callbacks   */
static int     g_nh;
static signed char g_hext[HANDLERS_MAX];   /* owning extension, -2 = dropped */
static int     g_cur_ext = -1;             /* whose code is running now      */
static int     g_out_ch = -1;              /* the "Extension Host" channel   */

static struct { int ext, jsid; char lang[16]; } g_prov[PROVIDERS_MAX];
static int g_nprov;

enum { EV_SAVE = 0, EV_OPEN, EV_CHANGE, EV_N };
static struct { int ext, jsid, kind; } g_listen[LISTENERS_MAX];
static int g_nlisten;

static int g_nthen;                        /* next promise id, wrapping   */

int uc_api_alive(void) { return g_vm != 0; }
const char *uc_api_engine(void) { return g_vm ? "unojs 0.1" : "not started"; }
unsigned long uc_api_fuel(void) { return g_vm ? ujs_fuel_used(g_vm) : 0; }

/* ---- argument helpers -------------------------------------------------------- */
static ujs_val argv_at(ujs_args *a, int i)
{
    return (i < a->argc) ? a->argv[i] : ujs_undefined();
}

static const char *arg_str(ujs_args *a, int i, const char *dflt)
{
    ujs_val v = argv_at(a, i);
    const char *s;
    if (!ujs_is_string(v)) return dflt;
    s = ujs_string_bytes(a->vm, v, 0);
    return s ? s : dflt;
}

static int arg_int(ujs_args *a, int i, int dflt)
{
    ujs_val v = argv_at(a, i);
    if (!ujs_is_number(v)) return dflt;
    return (int)ujs_to_number(a->vm, v);
}

static const char *val_str(ujs_vm *vm, ujs_val v, const char *dflt)
{
    const char *s;
    if (!ujs_is_string(v)) return dflt;
    s = ujs_string_bytes(vm, v, 0);
    return s ? s : dflt;
}

/* ---- handler table ------------------------------------------------------------ */
static int handler_add(ujs_vm *vm, ujs_val fn, int ext)
{
    if (g_nh >= HANDLERS_MAX) return -1;
    if (!ujs_is_function(vm, fn)) return -1;
    if (ujs_array_push(vm, g_handlers, fn) != UJS_OK) return -1;
    g_hext[g_nh] = (signed char)ext;
    return g_nh++;
}

static ujs_val handler_get(int jsid)
{
    ujs_val out = ujs_undefined();
    if (jsid < 0 || jsid >= g_nh || g_hext[jsid] == -2) return out;
    if (ujs_get_index(g_vm, g_handlers, (unsigned)jsid, &out) != UJS_OK)
        return ujs_undefined();
    return out;
}

void uc_api_drop_ext(int ext)
{
    int i;
    if (!g_vm) return;
    for (i = 0; i < g_nh; i++)
        if (g_hext[i] == ext) {
            g_hext[i] = -2;
            ujs_set_index(g_vm, g_handlers, (unsigned)i, ujs_undefined());
        }
    i = 0;
    while (i < g_nprov) {
        if (g_prov[i].ext == ext) {
            int k;
            for (k = i; k < g_nprov - 1; k++) g_prov[k] = g_prov[k + 1];
            g_nprov--;
        } else i++;
    }
    i = 0;
    while (i < g_nlisten) {
        if (g_listen[i].ext == ext) {
            int k;
            for (k = i; k < g_nlisten - 1; k++) g_listen[k] = g_listen[k + 1];
            g_nlisten--;
        } else i++;
    }
}

/* ---- the one place C enters JS -------------------------------------------------
 * Fuel is granted a slice at a time and resumed a bounded number of times; a
 * script that has not finished by then is killed and its extension disabled.
 * That is the whole safety story for a host with no preemption, so it is here
 * and nowhere else. */
static void report_exception(const char *what)
{
    char buf[220], desc[160];
    ujs_val ex = ujs_exception(g_vm);
    ujs_describe(g_vm, ex, desc, sizeof desc);
    ujs_clear_exception(g_vm);
    uc_scpy(buf, what, sizeof buf);
    uc_scat(buf, ": ", sizeof buf);
    uc_scat(buf, desc, sizeof buf);
    if (g_out_ch >= 0) { uc_output_write(g_out_ch, buf); uc_output_write(g_out_ch, "\n"); }
    uc_notify(buf, UC_SEV_ERROR);
}

static int call_fn(ujs_val fn, int argc, const ujs_val *argv, ujs_val *out,
                   const char *what)
{
    ujs_result r;
    int slices = 0;
    if (!g_vm || !ujs_is_function(g_vm, fn)) return 0;
    ujs_fuel_reset(g_vm);
    r = ujs_call(g_vm, fn, ujs_undefined(), argc, argv, out);
    while (r == UJS_YIELD && ++slices < 64) r = ujs_resume(g_vm, out);
    if (r == UJS_YIELD) {
        char buf[120];
        uc_scpy(buf, what, sizeof buf);
        uc_scat(buf, ": ran too long and was stopped", sizeof buf);
        uc_notify(buf, UC_SEV_ERROR);
        if (g_cur_ext >= 0) uc_ext_enable(g_cur_ext, 0);
        return 0;
    }
    if (r != UJS_OK) { report_exception(what); return 0; }
    return 1;
}

/* ---- console ------------------------------------------------------------------- */
static void out_line(const char *s)
{
    if (g_out_ch < 0) g_out_ch = uc_output_channel("Extension Host");
    uc_output_write(g_out_ch, s);
    uc_output_write(g_out_ch, "\n");
}

static ujs_val js_console(ujs_args *a)
{
    char line[400], one[200];
    int i;
    line[0] = 0;
    for (i = 0; i < a->argc; i++) {
        ujs_val v = a->argv[i];
        if (ujs_is_string(v)) uc_scpy(one, val_str(a->vm, v, ""), sizeof one);
        else ujs_describe(a->vm, v, one, sizeof one);
        uc_scat(line, one, sizeof line);
        if (i + 1 < a->argc) uc_scat(line, " ", sizeof line);
    }
    out_line(line);
    return ujs_undefined();
}

/* ---- promises (UCD-21) -----------------------------------------------------
 * These used to be THENABLES: an object with a .then that remembered one
 * callback, because unojs had no microtask queue to build a real Promise on.
 * It has one now, so every asynchronous call in this API returns an actual
 * Promise - which means `await vscode.window.showInputBox(...)` works, and
 * the deviation this file used to document is gone.
 *
 * The id indirection stays, because the CALLER of the resolution is a C
 * function in uc_cmd.c holding an int, not a JS value: uc_quick_input() is
 * handed an id and hands it back when the box closes.  The id now names a
 * promise in a rooted array rather than a callback slot. */
static ujs_val g_proms;                    /* rooted: id -> promise         */

static ujs_val promise_new_id(ujs_vm *vm, int *id_out)
{
    ujs_val p = ujs_promise(vm);
    int id = g_nthen % THENABLES_MAX;
    g_nthen++;
    ujs_set_index(vm, g_proms, (unsigned)id, p);
    *id_out = id;
    return p;
}

/* A promise that is ALREADY answered.  Its reactions still run on the next
 * pump rather than here: settling never calls back into JS, which is what
 * keeps the one entry point (call_fn from C context) that the fuel and
 * exception machinery were built around. */
static ujs_val pending_thenable(ujs_vm *vm, const char *val)
{
    ujs_val p = ujs_promise(vm);
    ujs_promise_resolve(vm, p, val ? ujs_string(vm, val, -1) : ujs_undefined());
    return p;
}

static ujs_val pending_thenable_val(ujs_vm *vm, ujs_val v)
{
    ujs_val p = ujs_promise(vm);
    ujs_promise_resolve(vm, p, v);
    return p;
}

/* Settle the promise an async API handed out.  uc_cmd.c holds the id from
 * when the quick pick or input box opened, and hands it back when the box
 * closes - which is why the parameter is an id and not a JS value.
 *
 * Settling does NOT run the extension's continuation: that happens on the
 * next uc_api_pump(), through the microtask queue, like every other promise
 * reaction (UCD-21). */
static int settle_id(int id, ujs_val v)
{
    ujs_val p = ujs_undefined();
    if (!g_vm || id < 0 || id >= THENABLES_MAX) return 0;
    if (ujs_get_index(g_vm, g_proms, (unsigned)id, &p) != UJS_OK) return 0;
    if (!ujs_is_object(p)) return 0;
    ujs_set_index(g_vm, g_proms, (unsigned)id, ujs_undefined());
    ujs_promise_resolve(g_vm, p, v);
    return 1;
}

int uc_api_call_str(int thenable_id, const char *value)
{
    return settle_id(thenable_id,
                     (value && value[0]) ? ujs_string(g_vm, value, -1)
                                         : ujs_undefined());
}

/* Settle one by id with a JS value.  Nothing calls it today - every value-
 * returning API resolves its promise at the point it builds it - but it is
 * the other half of settle_id's contract and the next host promise that
 * answers with an object will want it. */
int uc_api_settle_val(int id, ujs_val v) { return settle_id(id, v); }

/* Call a stored handler with one string argument - deltas and completions
 * from the LM slot come through here, always from C frame context. */
static int call_handler_str(int jsid, const char *s)
{
    ujs_val fn, arg, out;
    ujs_scope sc;
    int prev;
    if (!g_vm || jsid < 0) return 0;
    fn = handler_get(jsid);
    if (!ujs_is_function(g_vm, fn)) return 0;
    prev = g_cur_ext;
    g_cur_ext = (jsid < g_nh) ? g_hext[jsid] : -1;
    ujs_scope_open(g_vm, &sc);
    arg = ujs_string(g_vm, s ? s : "", -1);
    call_fn(fn, 1, &arg, &out, "callback");
    ujs_scope_close(g_vm, &sc, ujs_undefined());
    g_cur_ext = prev;
    return 1;
}

int uc_api_call_num(int jsid, int arg)
{
    ujs_val fn = handler_get(jsid), v = ujs_number(arg), out;
    return call_fn(fn, 1, &v, &out, "callback");
}

int uc_api_call_cmd(int jsid)
{
    ujs_val fn = handler_get(jsid), out;
    int prev, rc;
    if (!g_vm) return 0;
    /* the handler runs AS its extension: a permission checked inside the
     * callback (vscode.lm, UCD-50) must see who is really asking, not the -1
     * of "no extension is loading right now" */
    prev = g_cur_ext;
    g_cur_ext = (jsid >= 0 && jsid < g_nh) ? g_hext[jsid] : -1;
    rc = call_fn(fn, 0, 0, &out, "command");
    g_cur_ext = prev;
    return rc;
}

/* ---- the document object -------------------------------------------------------
 * Built lazily and handed to a provider or a listener.  getText() is a host
 * call rather than a property so a 200 KB file is only copied into the JS heap
 * if the extension actually asks for it. */
static ujs_val js_doc_getText(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    (void)a;
    if (!d) return ujs_string(g_vm, "", 0);
    return ujs_string(g_vm, d->text, d->len);
}

static ujs_val js_doc_lineAt(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    int n = arg_int(a, 0, 0), s, e;
    ujs_val o;
    if (!d) return ujs_undefined();
    if (n < 0 || n >= uc_line_count(d)) return ujs_undefined();
    s = uc_line_start(d, n);
    e = uc_line_end(d, n);
    o = ujs_object_new(a->vm);
    ujs_set(a->vm, o, "lineNumber", ujs_number(n));
    ujs_set(a->vm, o, "text", ujs_string(a->vm, d->text + s, e - s));
    return o;
}

static ujs_val js_doc_save(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    (void)a;
    return ujs_bool(d ? uc_doc_save(d) : 0);
}

static ujs_val document_new(ujs_vm *vm, UcDoc *d)
{
    ujs_val o = ujs_object_new(vm);
    UcLang *L = d ? uc_lang_at(d->lang) : 0;
    char path[UC_FULL_MAX];
    uc_doc_path(d, path, sizeof path);
    ujs_set(vm, o, "fileName", ujs_string(vm, path, -1));
    ujs_set(vm, o, "languageId", ujs_string(vm, L ? L->id : "plaintext", -1));
    ujs_set(vm, o, "lineCount", ujs_number(d ? uc_line_count(d) : 0));
    ujs_set(vm, o, "isDirty", ujs_bool(d ? d->dirty : 0));
    ujs_set_fn(vm, o, "getText", js_doc_getText, 0);
    ujs_set_fn(vm, o, "lineAt", js_doc_lineAt, 1);
    ujs_set_fn(vm, o, "save", js_doc_save, 0);
    return o;
}

static ujs_val position_new(ujs_vm *vm, int line, int ch)
{
    ujs_val p = ujs_object_new(vm);
    ujs_set(vm, p, "line", ujs_number(line));
    ujs_set(vm, p, "character", ujs_number(ch));
    return p;
}

/* ---- the editor object ----------------------------------------------------------- */
static ujs_val js_ed_insert(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    const char *s = arg_str(a, 0, "");
    if (d) uc_insert(d, s, (int)strlen(s));
    uc_repaint();
    return ujs_undefined();
}

static ujs_val js_ed_replaceSelection(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    const char *s = arg_str(a, 0, "");
    if (!d) return ujs_undefined();
    uc_insert(d, s, (int)strlen(s));       /* uc_insert replaces a selection */
    uc_repaint();
    return ujs_undefined();
}

static ujs_val js_ed_getSelectedText(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    char buf[4096];
    (void)a;
    if (!d) return ujs_string(g_vm, "", 0);
    uc_selection_text(d, buf, sizeof buf);
    return ujs_string(g_vm, buf, -1);
}

static ujs_val js_ed_setCursor(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    if (d) uc_move_to(d, uc_offset_of(d, arg_int(a, 0, 0), arg_int(a, 1, 0)), 0);
    uc_repaint();
    return ujs_undefined();
}

/* editor.edit(fn) - the builder gets insert/replace/delete over line/character
 * positions.  The edits apply immediately rather than being batched, which is
 * the one thing VS Code's builder does that a single-threaded host does not
 * need: there is nothing else that can touch the document in between. */
static ujs_val js_edit_insert(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    ujs_val pos = argv_at(a, 0), lv = ujs_undefined(), cv = ujs_undefined();
    const char *s = arg_str(a, 1, "");
    int off;
    if (!d) return ujs_undefined();
    ujs_get(a->vm, pos, "line", &lv);
    ujs_get(a->vm, pos, "character", &cv);
    off = uc_offset_of(d, (int)ujs_to_number(a->vm, lv), (int)ujs_to_number(a->vm, cv));
    uc_replace_range(d, off, off, s, (int)strlen(s));
    return ujs_undefined();
}

static ujs_val js_edit_replace(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    ujs_val rg = argv_at(a, 0), sv = ujs_undefined(), ev = ujs_undefined();
    ujs_val l1 = ujs_undefined(), c1 = ujs_undefined(), l2 = ujs_undefined(), c2 = ujs_undefined();
    const char *s = arg_str(a, 1, "");
    int from, to;
    if (!d) return ujs_undefined();
    ujs_get(a->vm, rg, "start", &sv);
    ujs_get(a->vm, rg, "end", &ev);
    ujs_get(a->vm, sv, "line", &l1); ujs_get(a->vm, sv, "character", &c1);
    ujs_get(a->vm, ev, "line", &l2); ujs_get(a->vm, ev, "character", &c2);
    from = uc_offset_of(d, (int)ujs_to_number(a->vm, l1), (int)ujs_to_number(a->vm, c1));
    to   = uc_offset_of(d, (int)ujs_to_number(a->vm, l2), (int)ujs_to_number(a->vm, c2));
    uc_replace_range(d, from, to, s, (int)strlen(s));
    return ujs_undefined();
}

static ujs_val js_ed_edit(ujs_args *a)
{
    ujs_val builder, out, fn = argv_at(a, 0);
    ujs_scope sc;
    UcDoc *d = uc_doc_active();
    if (!ujs_is_function(a->vm, fn)) return ujs_bool(0);
    ujs_scope_open(a->vm, &sc);
    builder = ujs_object_new(a->vm);
    ujs_set_fn(a->vm, builder, "insert", js_edit_insert, 2);
    ujs_set_fn(a->vm, builder, "replace", js_edit_replace, 2);
    if (d) uc_begin_group(d);
    call_fn(fn, 1, &builder, &out, "editor.edit");
    if (d) uc_end_group(d);
    ujs_scope_close(a->vm, &sc, ujs_undefined());
    uc_repaint();
    return ujs_bool(1);
}

static ujs_val editor_new(ujs_vm *vm, UcDoc *d)
{
    ujs_val o = ujs_object_new(vm), sel;
    int a, b;
    o = ujs_object_new(vm);
    ujs_set(vm, o, "document", document_new(vm, d));
    a = d ? (d->cur[0].anchor < d->cur[0].caret ? d->cur[0].anchor : d->cur[0].caret) : 0;
    b = d ? (d->cur[0].anchor > d->cur[0].caret ? d->cur[0].anchor : d->cur[0].caret) : 0;
    sel = ujs_object_new(vm);
    ujs_set(vm, sel, "start", position_new(vm, d ? uc_line_of(d, a) : 0, d ? uc_col_of(d, a) : 0));
    ujs_set(vm, sel, "end",   position_new(vm, d ? uc_line_of(d, b) : 0, d ? uc_col_of(d, b) : 0));
    ujs_set(vm, sel, "active", position_new(vm, d ? uc_line_of(d, d->cur[0].caret) : 0,
                                                d ? uc_col_of(d, d->cur[0].caret) : 0));
    ujs_set(vm, sel, "isEmpty", ujs_bool(a == b));
    ujs_set(vm, o, "selection", sel);
    ujs_set_fn(vm, o, "insert", js_ed_insert, 1);
    ujs_set_fn(vm, o, "replaceSelection", js_ed_replaceSelection, 1);
    ujs_set_fn(vm, o, "getSelectedText", js_ed_getSelectedText, 0);
    ujs_set_fn(vm, o, "setCursor", js_ed_setCursor, 2);
    ujs_set_fn(vm, o, "edit", js_ed_edit, 1);
    return o;
}

static ujs_val js_active_editor(ujs_args *a)
{
    UcDoc *d = uc_doc_active();
    if (!d) return ujs_undefined();
    return editor_new(a->vm, d);
}

/* ---- vscode.window ---------------------------------------------------------------- */
static ujs_val js_show_info(ujs_args *a)  { uc_notify(arg_str(a, 0, ""), UC_SEV_INFO);  return ujs_undefined(); }
static ujs_val js_show_warn(ujs_args *a)  { uc_notify(arg_str(a, 0, ""), UC_SEV_WARN);  return ujs_undefined(); }
static ujs_val js_show_error(ujs_args *a) { uc_notify(arg_str(a, 0, ""), UC_SEV_ERROR); return ujs_undefined(); }
static ujs_val js_status_msg(ujs_args *a) { uc_status_msg(arg_str(a, 0, "")); return ujs_undefined(); }

static ujs_val js_quick_pick(ujs_args *a)
{
    static char items[64][64];
    ujs_val arr = argv_at(a, 0);
    unsigned n, i;
    int id;
    ujs_val t;
    if (!ujs_is_array(a->vm, arr)) return ujs_throw_error(a->vm, "TypeError",
                                        "showQuickPick expects an array");
    n = ujs_array_length(a->vm, arr);
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) {
        ujs_val v = ujs_undefined();
        ujs_get_index(a->vm, arr, i, &v);
        uc_scpy(items[i], val_str(a->vm, v, ""), 64);
    }
    t = promise_new_id(a->vm, &id);
    uc_quick_pick(items, (int)n, arg_str(a, 1, "Select an item"), id);
    return t;
}

static ujs_val js_input_box(ujs_args *a)
{
    int id;
    ujs_val t = promise_new_id(a->vm, &id);
    uc_quick_input(arg_str(a, 0, ""), arg_str(a, 1, ""), id);
    return t;
}

static ujs_val js_out_append(ujs_args *a)
{
    ujs_val chv = ujs_undefined();
    ujs_get(a->vm, a->self, "__ch", &chv);
    uc_output_write((int)ujs_to_number(a->vm, chv), arg_str(a, 0, ""));
    return ujs_undefined();
}
static ujs_val js_out_appendLine(ujs_args *a)
{
    ujs_val chv = ujs_undefined();
    int ch;
    ujs_get(a->vm, a->self, "__ch", &chv);
    ch = (int)ujs_to_number(a->vm, chv);
    uc_output_write(ch, arg_str(a, 0, ""));
    uc_output_write(ch, "\n");
    return ujs_undefined();
}
static ujs_val js_out_show(ujs_args *a)
{
    ujs_val chv = ujs_undefined();
    ujs_get(a->vm, a->self, "__ch", &chv);
    uc_output_show((int)ujs_to_number(a->vm, chv));
    return ujs_undefined();
}

static ujs_val js_create_output(ujs_args *a)
{
    ujs_val o = ujs_object_new(a->vm);
    int ch = uc_output_channel(arg_str(a, 0, "Extension"));
    ujs_set(a->vm, o, "__ch", ujs_number(ch));
    ujs_set(a->vm, o, "name", ujs_string(a->vm, arg_str(a, 0, "Extension"), -1));
    ujs_set_fn(a->vm, o, "append", js_out_append, 1);
    ujs_set_fn(a->vm, o, "appendLine", js_out_appendLine, 1);
    ujs_set_fn(a->vm, o, "show", js_out_show, 0);
    return o;
}

/* ---- vscode.commands --------------------------------------------------------------- */
static ujs_val js_register_command(ujs_args *a)
{
    const char *id = arg_str(a, 0, 0);
    int jsid;
    if (!id) return ujs_throw_error(a->vm, "TypeError", "registerCommand needs an id");
    jsid = handler_add(a->vm, argv_at(a, 1), g_cur_ext);
    if (jsid < 0) return ujs_throw_error(a->vm, "Error", "too many registered callbacks");
    {
        /* keep the manifest's title if there is one; a command registered only
         * from code gets its id as its palette entry, which is at least honest */
        int ci = uc_cmd_find(id);
        UcCommand *c = uc_cmd_at(ci);
        uc_cmd_register(id, c && c->title[0] ? c->title : id,
                        c && c->cat[0] ? c->cat : "Extension", 0, g_cur_ext, jsid);
    }
    return ujs_object_new(a->vm);        /* a Disposable-shaped placeholder */
}

static ujs_val js_execute_command(ujs_args *a)
{
    const char *id = arg_str(a, 0, 0);
    if (id) uc_cmd_run(id);
    return ujs_undefined();
}

static ujs_val js_get_commands(ujs_args *a)
{
    ujs_val arr = ujs_array_new(a->vm);
    int i;
    for (i = 0; i < uc_cmd_count(); i++)
        ujs_array_push(a->vm, arr, ujs_string(a->vm, uc_cmd_at(i)->id, -1));
    return arr;
}

/* ---- vscode.workspace ---------------------------------------------------------------- */
static void section_key(ujs_vm *vm, ujs_val self, const char *key, char *out, int cap)
{
    ujs_val sv = ujs_undefined();
    const char *sec;
    ujs_get(vm, self, "__section", &sv);
    sec = val_str(vm, sv, "");
    out[0] = 0;
    if (sec[0]) { uc_scpy(out, sec, cap); uc_scat(out, ".", cap); }
    uc_scat(out, key, cap);
}

static ujs_val js_cfg_get(ujs_args *a)
{
    char key[80];
    const UcSettingDef *def;
    section_key(a->vm, a->self, arg_str(a, 0, ""), key, sizeof key);
    def = uc_cfg_find(key);
    if (!def && !uc_cfg_is_user(key)) return argv_at(a, 1);      /* the default */
    if (def && def->type == UC_T_BOOL) return ujs_bool(uc_cfg_bool(key));
    if (def && def->type == UC_T_INT)  return ujs_number(uc_cfg_int(key));
    return ujs_string(a->vm, uc_cfg_str(key), -1);
}

static ujs_val js_cfg_update(ujs_args *a)
{
    char key[80], json[200];
    ujs_val v = argv_at(a, 1);
    section_key(a->vm, a->self, arg_str(a, 0, ""), key, sizeof key);
    if (ujs_is_string(v)) {
        char esc[160];
        uc_json_esc(esc, sizeof esc, val_str(a->vm, v, ""));
        uc_scpy(json, "\"", sizeof json);
        uc_scat(json, esc, sizeof json);
        uc_scat(json, "\"", sizeof json);
    } else if (ujs_typeof(v) == UJS_TYPE_BOOL) {
        uc_scpy(json, ujs_to_bool(v) ? "true" : "false", sizeof json);
    } else {
        char num[24];
        uc_itoa(num, (long)ujs_to_number(a->vm, v));
        uc_scpy(json, num, sizeof json);
    }
    uc_cfg_set(key, json);
    if (!strcmp(key, "workbench.colorTheme")) uc_theme_select(uc_cfg_str(key));
    uc_metrics_init();
    uc_repaint();
    return ujs_bool(1);
}

static ujs_val js_get_configuration(ujs_args *a)
{
    ujs_val o = ujs_object_new(a->vm);
    ujs_set(a->vm, o, "__section", ujs_string(a->vm, arg_str(a, 0, ""), -1));
    ujs_set_fn(a->vm, o, "get", js_cfg_get, 2);
    ujs_set_fn(a->vm, o, "update", js_cfg_update, 2);
    ujs_set_fn(a->vm, o, "has", js_cfg_get, 1);
    return o;
}

static ujs_val js_fs_read(ujs_args *a)
{
    const char *p = arg_str(a, 0, "");
    char *src = 0;
    long len = 0;
    ujs_val v;
    if (!uc_read_file(UC.ws_vol, p, &src, &len))
        return ujs_throw_error(a->vm, "Error", "no such file");
    v = ujs_string(a->vm, src, (int)len);
    free(src);
    return v;
}

static ujs_val js_fs_write(ujs_args *a)
{
    const char *p = arg_str(a, 0, "");
    ujs_val cv = argv_at(a, 1);
    size_t n = 0;
    const char *body = ujs_is_string(cv) ? ujs_string_bytes(a->vm, cv, &n) : 0;
    if (!body) return ujs_throw_error(a->vm, "TypeError", "writeFile needs a string");
    return ujs_bool(uno_fs_write(UC.ws_vol, p, (const unsigned char *)body, (long)n));
}

/* readDirectory: VS Code's [name, FileType] pairs (1 = file, 2 = directory),
 * over the workspace volume like the rest of workspace.fs.  UCD-51's list_dir
 * tool is the first caller. */
static ujs_val js_fs_readdir(ujs_args *a)
{
    char (*names)[UC_NAME_MAX];
    static unsigned char isdir[220];
    const char *p = arg_str(a, 0, "");
    int n, i;
    ujs_val arr;
    names = (char (*)[UC_NAME_MAX])malloc(220UL * UC_NAME_MAX);
    if (!names) return ujs_throw_error(a->vm, "Error", "out of memory");
    n = uc_list_dir(UC.ws_vol, p, names, isdir, 220);
    arr = ujs_array_new(a->vm);
    if (n > 220) n = 220;
    for (i = 0; i < n; i++) {
        ujs_val pair;
        if (!names[i][0]) continue;
        pair = ujs_array_new(a->vm);
        ujs_array_push(a->vm, pair, ujs_string(a->vm, names[i], -1));
        ujs_array_push(a->vm, pair, ujs_number(isdir[i] ? 2 : 1));
        ujs_array_push(a->vm, arr, pair);
    }
    free(names);
    return arr;
}

/* Launch a user program (UCD-51's run tool).  Resolves the shell's answer:
 * "" is a clean launch, anything else is the reason it did not run - on pc64
 * that is the Python traceback, which is exactly what an assistant needs to
 * read to fix the program.  The desktop shell refuses (see canRunPrograms);
 * the refusal text lands here too, so even a caller that ignored the
 * capability flag gets the truth instead of a hang. */
static ujs_val js_run_user(ujs_args *a)
{
    const char *p = arg_str(a, 0, "");
    int rc = pc64_shell_run_user(UC.ws_vol, p);
    return ujs_string(a->vm, rc < 0 ? pc64_shell_py_error() : "", -1);
}

static ujs_val js_open_document(ujs_args *a)
{
    const char *p = arg_str(a, 0, "");
    int i = uc_doc_open(UC.ws_vol, UC.ws_dir, p);
    if (i < 0) return ujs_throw_error(a->vm, "Error", "no such file");
    uc_repaint();
    return document_new(a->vm, uc_doc_at(i));
}

static ujs_val listen_add(ujs_args *a, int kind)
{
    int jsid = handler_add(a->vm, argv_at(a, 0), g_cur_ext);
    if (jsid >= 0 && g_nlisten < LISTENERS_MAX) {
        g_listen[g_nlisten].ext = g_cur_ext;
        g_listen[g_nlisten].jsid = jsid;
        g_listen[g_nlisten].kind = kind;
        g_nlisten++;
    }
    return ujs_object_new(a->vm);
}
static ujs_val js_on_save(ujs_args *a)   { return listen_add(a, EV_SAVE); }
static ujs_val js_on_open(ujs_args *a)   { return listen_add(a, EV_OPEN); }
static ujs_val js_on_change(ujs_args *a) { return listen_add(a, EV_CHANGE); }

/* ---- vscode.languages ------------------------------------------------------------------ */
static ujs_val js_register_completion(ujs_args *a)
{
    ujs_val sel = argv_at(a, 0), prov = argv_at(a, 1), fn = ujs_undefined();
    char lang[16];
    int jsid;
    uc_scpy(lang, "", sizeof lang);
    if (ujs_is_string(sel)) uc_scpy(lang, val_str(a->vm, sel, ""), sizeof lang);
    else if (ujs_is_object(sel)) {
        ujs_val lv = ujs_undefined();
        ujs_get(a->vm, sel, "language", &lv);
        uc_scpy(lang, val_str(a->vm, lv, ""), sizeof lang);
    }
    if (ujs_is_function(a->vm, prov)) fn = prov;
    else ujs_get(a->vm, prov, "provideCompletionItems", &fn);
    jsid = handler_add(a->vm, fn, g_cur_ext);
    if (jsid < 0) return ujs_throw_error(a->vm, "Error", "too many registered callbacks");
    if (g_nprov < PROVIDERS_MAX) {
        g_prov[g_nprov].ext = g_cur_ext;
        g_prov[g_nprov].jsid = jsid;
        uc_scpy(g_prov[g_nprov].lang, lang, sizeof g_prov[0].lang);
        g_nprov++;
    }
    return ujs_object_new(a->vm);
}

int uc_api_completions(UcDoc *d, int offset)
{
    UcLang *L;
    int i, added = 0;
    if (!g_vm || !d) return 0;
    L = uc_lang_at(d->lang);
    for (i = 0; i < g_nprov; i++) {
        ujs_val fn, args[2], out = ujs_undefined();
        ujs_scope sc;
        if (g_prov[i].lang[0] && L && strcmp(g_prov[i].lang, L->id)) continue;
        fn = handler_get(g_prov[i].jsid);
        if (!ujs_is_function(g_vm, fn)) continue;
        ujs_scope_open(g_vm, &sc);
        args[0] = document_new(g_vm, d);
        args[1] = position_new(g_vm, uc_line_of(d, offset), uc_col_of(d, offset));
        g_cur_ext = g_prov[i].ext;
        if (call_fn(fn, 2, args, &out, "provideCompletionItems") &&
            ujs_is_array(g_vm, out)) {
            unsigned n = ujs_array_length(g_vm, out), k;
            for (k = 0; k < n && k < 40; k++) {
                ujs_val it = ujs_undefined(), lv = ujs_undefined(), dv = ujs_undefined(), iv = ujs_undefined();
                char label[48], detail[40], insert[80];
                ujs_get_index(g_vm, out, k, &it);
                if (ujs_is_string(it)) {
                    uc_scpy(label, val_str(g_vm, it, ""), sizeof label);
                    uc_scpy(detail, "", sizeof detail);
                    uc_scpy(insert, label, sizeof insert);
                } else {
                    ujs_get(g_vm, it, "label", &lv);
                    ujs_get(g_vm, it, "detail", &dv);
                    ujs_get(g_vm, it, "insertText", &iv);
                    uc_scpy(label, val_str(g_vm, lv, ""), sizeof label);
                    uc_scpy(detail, val_str(g_vm, dv, ""), sizeof detail);
                    uc_scpy(insert, val_str(g_vm, iv, label), sizeof insert);
                }
                if (label[0]) added += uc_suggest_add(label, detail, insert, UC_CI_FUNCTION);
            }
        }
        g_cur_ext = -1;
        ujs_scope_close(g_vm, &sc, ujs_undefined());
    }
    return added;
}

static void fire(int kind, UcDoc *d)
{
    int i;
    if (!g_vm || !g_nlisten) return;
    for (i = 0; i < g_nlisten; i++) {
        ujs_val fn, arg, out;
        ujs_scope sc;
        if (g_listen[i].kind != kind) continue;
        fn = handler_get(g_listen[i].jsid);
        if (!ujs_is_function(g_vm, fn)) continue;
        ujs_scope_open(g_vm, &sc);
        arg = document_new(g_vm, d);
        g_cur_ext = g_listen[i].ext;
        call_fn(fn, 1, &arg, &out, "event listener");
        g_cur_ext = -1;
        ujs_scope_close(g_vm, &sc, ujs_undefined());
    }
}

void uc_api_fire_save(UcDoc *d)   { fire(EV_SAVE, d); }
void uc_api_fire_open(UcDoc *d)   { fire(EV_OPEN, d); }
void uc_api_fire_change(UcDoc *d) { fire(EV_CHANGE, d); }

int uc_api_hover(UcDoc *d, int offset, char *out, int cap)
{
    (void)d; (void)offset;
    if (cap > 0) out[0] = 0;
    return 0;              /* no hover UI in this build; see UNOCODE.md */
}

/* ---- context.secrets (UCD-48) ------------------------------------------------------
 * VS Code's SecretStorage shape: store / get / delete, each returning a
 * thenable.  Names are PREFIXED "EXT.<ID>." from the object itself, not from
 * the caller's say-so, so an extension cannot read another's secrets - or the
 * assistant's key - by guessing a string.  There is no onDidChange and no
 * enumeration; both are stated in UNOCODE.md. */
static int secret_key(ujs_args *a, char *out, int cap)
{
    ujs_val pv = ujs_undefined();
    const char *name = arg_str(a, 0, "");
    if (!name[0]) return 0;
    ujs_get(a->vm, a->self, "__pfx", &pv);
    uc_scpy(out, val_str(a->vm, pv, "EXT.."), cap);
    uc_scat(out, name, cap);
    return (int)strlen(out) < cap - 1;
}

static ujs_val js_secrets_get(ujs_args *a)
{
    char key[64], val[UC_SECRET_MAX];
    if (!secret_key(a, key, sizeof key))
        return pending_thenable(a->vm, 0);
    if (!uc_secret_get(key, val, sizeof val))
        return pending_thenable(a->vm, 0);
    return pending_thenable(a->vm, val);
}

static ujs_val js_secrets_store(ujs_args *a)
{
    char key[64];
    const char *val = arg_str(a, 1, 0);
    if (!secret_key(a, key, sizeof key) || !val || !uc_secret_set(key, val))
        return ujs_throw_error(a->vm, "Error", "the secret store refused");
    return pending_thenable(a->vm, 0);
}

static ujs_val js_secrets_delete(ujs_args *a)
{
    char key[64];
    if (secret_key(a, key, sizeof key)) uc_secret_del(key);
    return pending_thenable(a->vm, 0);
}

static ujs_val secrets_new(ujs_vm *vm, int ext)
{
    ujs_val o = ujs_object_new(vm);
    char pfx[28];
    UcExt *e = uc_ext_at(ext);
    uc_scpy(pfx, "EXT.", sizeof pfx);
    uc_scat(pfx, e ? e->id : "ANON", sizeof pfx);
    uc_scat(pfx, ".", sizeof pfx);
    ujs_set(vm, o, "__pfx", ujs_string(vm, pfx, -1));
    ujs_set_fn(vm, o, "get", js_secrets_get, 1);
    ujs_set_fn(vm, o, "store", js_secrets_store, 2);
    ujs_set_fn(vm, o, "delete", js_secrets_delete, 1);
    return o;
}

/* ---- vscode.lm (UCD-50) ------------------------------------------------------------
 * The model, offered to extensions in vscode.lm's shape - and GATED: reaching
 * a model is a declared privilege ("languageModels" in PACKAGE.JSN
 * "permissions"), because an extension host that can reach the network can
 * exfiltrate a workspace, and EXT\ is a folder anyone can drop a file into.
 * The refusal names the missing declaration, so the fix is in the message.
 *
 * Deviations from vscode.lm, all documented in UNOCODE.md: one model (the
 * ai.model setting), one request at a time, and the response streams through
 * onText/onDone/onError callbacks rather than an async iterator - there is
 * no event loop to build one on. */
static struct {
    int gen;                  /* stale response objects compare against this */
    int active;
    int cb_text, cb_done, cb_err;
    char *full; int flen, fcap;
} g_lmjs = { 0, 0, -1, -1, -1, 0, 0, 0 };

static int lm_allowed(int ext)
{
    UcExt *x = uc_ext_at(ext);
    return x && x->perm_lm;
}

static ujs_val lm_refuse(ujs_vm *vm, int ext)
{
    char msg[160];
    UcExt *x = uc_ext_at(ext);
    uc_scpy(msg, "extension ", sizeof msg);
    uc_scat(msg, x ? x->id : "?", sizeof msg);
    uc_scat(msg, " does not declare \"languageModels\" in PACKAGE.JSN "
                 "\"permissions\", so it cannot reach the model", sizeof msg);
    return ujs_throw_error(vm, "Error", msg);
}

static void lm_delta(void *user, const char *s, int n)
{
    (void)user;
    if (g_lmjs.flen + n + 1 > g_lmjs.fcap) {
        int want = g_lmjs.fcap ? g_lmjs.fcap * 2 : 4096;
        char *p;
        while (want < g_lmjs.flen + n + 1) want *= 2;
        if (want > 256 * 1024) return;              /* capped, not crashed   */
        p = (char *)realloc(g_lmjs.full, (unsigned long)want);
        if (!p) return;
        g_lmjs.full = p;
        g_lmjs.fcap = want;
    }
    memcpy(g_lmjs.full + g_lmjs.flen, s, (unsigned long)n);
    g_lmjs.flen += n;
    g_lmjs.full[g_lmjs.flen] = 0;
    if (g_lmjs.cb_text >= 0) call_handler_str(g_lmjs.cb_text, s);
}

static void lm_done(void *user, int status, const char *err)
{
    (void)user; (void)status;
    g_lmjs.active = 0;
    g_lmjs.gen++;                    /* the response object is now stale     */
    if (err) {
        if (g_lmjs.cb_err >= 0) call_handler_str(g_lmjs.cb_err, err);
    } else if (g_lmjs.cb_done >= 0)
        call_handler_str(g_lmjs.cb_done, g_lmjs.full ? g_lmjs.full : "");
    free(g_lmjs.full);
    g_lmjs.full = 0;
    g_lmjs.flen = g_lmjs.fcap = 0;
    g_lmjs.cb_text = g_lmjs.cb_done = g_lmjs.cb_err = -1;
}

/* a response object's generation, or -1 when it is stale */
static int lm_gen_of(ujs_args *a)
{
    ujs_val gv = ujs_undefined();
    int gen;
    ujs_get(a->vm, a->self, "__gen", &gv);
    gen = (int)ujs_to_number(a->vm, gv);
    return (g_lmjs.active && gen == g_lmjs.gen) ? gen : -1;
}

static ujs_val js_lm_onText(ujs_args *a)
{
    if (lm_gen_of(a) >= 0)
        g_lmjs.cb_text = handler_add(a->vm, argv_at(a, 0), g_cur_ext);
    return a->self;
}
static ujs_val js_lm_onDone(ujs_args *a)
{
    if (lm_gen_of(a) >= 0)
        g_lmjs.cb_done = handler_add(a->vm, argv_at(a, 0), g_cur_ext);
    return a->self;
}
static ujs_val js_lm_onError(ujs_args *a)
{
    if (lm_gen_of(a) >= 0)
        g_lmjs.cb_err = handler_add(a->vm, argv_at(a, 0), g_cur_ext);
    return a->self;
}
static ujs_val js_lm_cancel(ujs_args *a)
{
    if (lm_gen_of(a) >= 0) uc_lm_cancel();
    return ujs_undefined();
}

static ujs_val js_lm_sendRequest(ujs_args *a)
{
    ujs_val arr = argv_at(a, 0), ev = ujs_undefined(), R;
    char *b;
    int cap = 64 * 1024, p = 0, ext;
    unsigned n, i;
    const char *why = 0;

    ujs_get(a->vm, a->self, "__ext", &ev);
    ext = (int)ujs_to_number(a->vm, ev);
    if (!lm_allowed(ext)) return lm_refuse(a->vm, ext);
    if (g_lmjs.active)
        return ujs_throw_error(a->vm, "Error",
            "one model request at a time - the running one has not finished");
    if (!ujs_is_array(a->vm, arr))
        return ujs_throw_error(a->vm, "TypeError",
                               "sendRequest expects an array of messages");

    b = (char *)malloc((unsigned long)cap);
    if (!b) return ujs_throw_error(a->vm, "Error", "out of memory");
    uc_buf_raw(b, &p, cap, "[");
    n = ujs_array_length(a->vm, arr);
    for (i = 0; i < n; i++) {
        ujs_val m = ujs_undefined();
        const char *role = "user", *content = "";
        ujs_get_index(a->vm, arr, i, &m);
        if (ujs_is_string(m))
            content = val_str(a->vm, m, "");
        else {
            ujs_val rv = ujs_undefined(), cv = ujs_undefined();
            ujs_get(a->vm, m, "role", &rv);
            ujs_get(a->vm, m, "content", &cv);
            role = val_str(a->vm, rv, "user");
            content = val_str(a->vm, cv, "");
        }
        if (i) uc_buf_raw(b, &p, cap, ",");
        uc_buf_raw(b, &p, cap, "{\"role\":");
        uc_buf_json(b, &p, cap, strcmp(role, "assistant") ? "user" : "assistant");
        uc_buf_raw(b, &p, cap, ",\"content\":");
        uc_buf_json(b, &p, cap, content);
        uc_buf_raw(b, &p, cap, "}");
    }
    uc_buf_raw(b, &p, cap, "]");
    if (p >= cap) {
        free(b);
        return ujs_throw_error(a->vm, "Error", "the messages are too large");
    }
    b[p] = 0;
    if (!uc_lm_begin(b, lm_delta, lm_done, 0, &why)) {
        free(b);
        return ujs_throw_error(a->vm, "Error", why);
    }
    free(b);
    g_lmjs.gen++;
    g_lmjs.active = 1;
    g_lmjs.cb_text = g_lmjs.cb_done = g_lmjs.cb_err = -1;
    g_lmjs.flen = 0;

    R = ujs_object_new(a->vm);
    ujs_set(a->vm, R, "__gen", ujs_number(g_lmjs.gen));
    ujs_set_fn(a->vm, R, "onText", js_lm_onText, 1);
    ujs_set_fn(a->vm, R, "onDone", js_lm_onDone, 1);
    ujs_set_fn(a->vm, R, "onError", js_lm_onError, 1);
    ujs_set_fn(a->vm, R, "cancel", js_lm_cancel, 0);
    return R;
}

static ujs_val js_lm_select(ujs_args *a)
{
    ujs_val arr, m;
    if (!lm_allowed(g_cur_ext)) return lm_refuse(a->vm, g_cur_ext);
    arr = ujs_array_new(a->vm);
    m = ujs_object_new(a->vm);
    ujs_set(a->vm, m, "id", ujs_string(a->vm, uc_cfg_str("ai.model"), -1));
    ujs_set(a->vm, m, "vendor", ujs_string(a->vm, "anthropic", -1));
    ujs_set(a->vm, m, "family", ujs_string(a->vm, uc_cfg_str("ai.model"), -1));
    ujs_set(a->vm, m, "name", ujs_string(a->vm, uc_cfg_str("ai.model"), -1));
    ujs_set(a->vm, m, "__ext", ujs_number(g_cur_ext));
    ujs_set_fn(a->vm, m, "sendRequest", js_lm_sendRequest, 2);
    ujs_array_push(a->vm, arr, m);
    return pending_thenable_val(a->vm, arr);
}

static ujs_val js_lmmsg_user(ujs_args *a)
{
    ujs_val o = ujs_object_new(a->vm);
    ujs_set(a->vm, o, "role", ujs_string(a->vm, "user", -1));
    ujs_set(a->vm, o, "content", ujs_string(a->vm, arg_str(a, 0, ""), -1));
    return o;
}
static ujs_val js_lmmsg_assistant(ujs_args *a)
{
    ujs_val o = ujs_object_new(a->vm);
    ujs_set(a->vm, o, "role", ujs_string(a->vm, "assistant", -1));
    ujs_set(a->vm, o, "content", ujs_string(a->vm, arg_str(a, 0, ""), -1));
    return o;
}

/* ---- building the API object -------------------------------------------------------- */
static ujs_val g_api;              /* the `vscode` namespace, rooted        */

static ujs_val js_require(ujs_args *a)
{
    const char *m = arg_str(a, 0, "");
    if (!strcmp(m, "vscode") || !strcmp(m, "unocode")) return g_api;
    return ujs_throw_error(a->vm, "Error",
        "only require('vscode') is available in this host");
}

static ujs_val js_position_ctor(ujs_args *a)
{ return position_new(a->vm, arg_int(a, 0, 0), arg_int(a, 1, 0)); }

static ujs_val js_range_ctor(ujs_args *a)
{
    ujs_val r = ujs_object_new(a->vm);
    ujs_set(a->vm, r, "start", position_new(a->vm, arg_int(a, 0, 0), arg_int(a, 1, 0)));
    ujs_set(a->vm, r, "end",   position_new(a->vm, arg_int(a, 2, 0), arg_int(a, 3, 0)));
    return r;
}

static void build_api(ujs_vm *vm)
{
    ujs_val g = ujs_global(vm), win, cmds, ws, langs, fs, con;

    g_api = ujs_object_new(vm);
    ujs_root(vm, g_api);

    ujs_set(vm, g_api, "version", ujs_string(vm, "1.0.0-unocode", -1));

    win = ujs_object_new(vm);
    ujs_set_fn(vm, win, "showInformationMessage", js_show_info, 1);
    ujs_set_fn(vm, win, "showWarningMessage", js_show_warn, 1);
    ujs_set_fn(vm, win, "showErrorMessage", js_show_error, 1);
    ujs_set_fn(vm, win, "setStatusBarMessage", js_status_msg, 1);
    ujs_set_fn(vm, win, "showQuickPick", js_quick_pick, 2);
    ujs_set_fn(vm, win, "showInputBox", js_input_box, 2);
    ujs_set_fn(vm, win, "createOutputChannel", js_create_output, 1);
    ujs_set_accessor(vm, win, "activeTextEditor", js_active_editor, 0);
    ujs_set(vm, g_api, "window", win);

    cmds = ujs_object_new(vm);
    ujs_set_fn(vm, cmds, "registerCommand", js_register_command, 2);
    ujs_set_fn(vm, cmds, "registerTextEditorCommand", js_register_command, 2);
    ujs_set_fn(vm, cmds, "executeCommand", js_execute_command, 1);
    ujs_set_fn(vm, cmds, "getCommands", js_get_commands, 0);
    ujs_set(vm, g_api, "commands", cmds);

    ws = ujs_object_new(vm);
    ujs_set_fn(vm, ws, "getConfiguration", js_get_configuration, 1);
    ujs_set_fn(vm, ws, "openTextDocument", js_open_document, 1);
    ujs_set_fn(vm, ws, "onDidSaveTextDocument", js_on_save, 1);
    ujs_set_fn(vm, ws, "onDidOpenTextDocument", js_on_open, 1);
    ujs_set_fn(vm, ws, "onDidChangeTextDocument", js_on_change, 1);
    ujs_set(vm, ws, "name", ujs_string(vm, UC.ws_dir[0] ? UC.ws_dir
                                             : uno_fs_volume_name(UC.ws_vol), -1));
    ujs_set(vm, ws, "rootPath", ujs_string(vm, UC.ws_dir, -1));
    fs = ujs_object_new(vm);
    ujs_set_fn(vm, fs, "readFile", js_fs_read, 1);
    ujs_set_fn(vm, fs, "writeFile", js_fs_write, 2);
    ujs_set_fn(vm, fs, "readDirectory", js_fs_readdir, 1);
    ujs_set(vm, ws, "fs", fs);
    /* whether this platform can launch a user program at all - the shell's
     * answer, not a guess, so an assistant offers only what exists (UCD-51) */
    ujs_set(vm, ws, "canRunPrograms", ujs_bool(pc64_shell_can_run()));
    ujs_set_fn(vm, ws, "runUserProgram", js_run_user, 1);
    ujs_set(vm, g_api, "workspace", ws);

    langs = ujs_object_new(vm);
    ujs_set_fn(vm, langs, "registerCompletionItemProvider", js_register_completion, 3);
    ujs_set(vm, g_api, "languages", langs);

    {   /* vscode.lm (UCD-50) - present for everyone, GATED at the call */
        ujs_val lm = ujs_object_new(vm), lmm = ujs_object_new(vm);
        ujs_set_fn(vm, lm, "selectChatModels", js_lm_select, 1);
        ujs_set(vm, g_api, "lm", lm);
        ujs_set_fn(vm, lmm, "User", js_lmmsg_user, 1);
        ujs_set_fn(vm, lmm, "Assistant", js_lmmsg_assistant, 1);
        ujs_set(vm, g_api, "LanguageModelChatMessage", lmm);
    }

    ujs_set_fn(vm, g_api, "Position", js_position_ctor, 2);
    ujs_set_fn(vm, g_api, "Range", js_range_ctor, 4);

    /* globals: console, require, and `vscode` itself, so an extension written
     * as a plain script (no module wrapper) also works */
    con = ujs_object_new(vm);
    ujs_set_fn(vm, con, "log", js_console, 1);
    ujs_set_fn(vm, con, "info", js_console, 1);
    ujs_set_fn(vm, con, "warn", js_console, 1);
    ujs_set_fn(vm, con, "error", js_console, 1);
    ujs_set(vm, g, "console", con);
    ujs_set_fn(vm, g, "require", js_require, 1);
    ujs_set(vm, g, "vscode", g_api);
}

int uc_api_init(void)
{
    ujs_config cfg;
    if (g_vm) return 1;
    memset(&cfg, 0, sizeof cfg);
    cfg.heap_max = (unsigned long)uc_cfg_int("extensions.heapMB") * 1024u * 1024u;
    cfg.fuel_per_slice = (unsigned long)uc_cfg_int("extensions.fuelPerSlice");
    cfg.fuel_total = cfg.fuel_per_slice * 64u;
    g_vm = ujs_new(&cfg);
    if (!g_vm) return 0;
    g_out_ch = uc_output_channel("Extension Host");
    g_handlers = ujs_array_new(g_vm);
    ujs_root(g_vm, g_handlers);
    g_proms = ujs_array_new(g_vm);
    ujs_root(g_vm, g_proms);
    g_nh = 0;
    g_nprov = 0;
    g_nlisten = 0;
    g_nthen = 0;
    build_api(g_vm);
    return 1;
}

void uc_api_shutdown(void)
{
    if (!g_vm) return;
    ujs_free(g_vm);
    g_vm = 0;
    g_nh = g_nprov = g_nlisten = g_nthen = 0;
}

/* ---- running an extension's entry point ------------------------------------------ */
int uc_api_run_file(int ext, int vol, const char *path, char *err, int cap)
{
    char *src = 0;
    long len = 0;
    char *wrapped;
    ujs_result r;
    ujs_val fn = ujs_undefined(), out = ujs_undefined(), module, exports, req = ujs_undefined();
    ujs_val args[3], ctx, act = ujs_undefined();
    ujs_scope sc;
    int ok = 0;
    if (!uc_api_init()) { uc_scpy(err, "the extension host is not running", cap); return 0; }
    if (!uc_read_file(vol, path, &src, &len)) { uc_scpy(err, "main script not found", cap); return 0; }

    /* The CommonJS wrapper.  An extension's file is a module body, not a
     * script: it says `const vscode = require('vscode')` and assigns
     * `exports.activate`.  Wrapping it in a function expression makes the
     * completion value of the eval the function itself, which is all the
     * module machinery this host needs. */
    {
        static const char head[] = "(function(module, exports, require){\n";
        static const char tail[] = "\n})";
        unsigned long hn = sizeof head - 1, tn = sizeof tail - 1;
        wrapped = (char *)malloc(hn + (unsigned long)len + tn + 1);
        if (!wrapped) { free(src); uc_scpy(err, "out of memory", cap); return 0; }
        memcpy(wrapped, head, hn);
        memcpy(wrapped + hn, src, (unsigned long)len);
        memcpy(wrapped + hn + (unsigned long)len, tail, tn);
        wrapped[hn + (unsigned long)len + tn] = 0;
    }
    free(src);

    g_cur_ext = ext;
    ujs_scope_open(g_vm, &sc);
    ujs_fuel_reset(g_vm);
    r = ujs_eval(g_vm, wrapped, -1, &fn);
    {
        int slices = 0;
        while (r == UJS_YIELD && ++slices < 64) r = ujs_resume(g_vm, &fn);
    }
    free(wrapped);
    if (r != UJS_OK || !ujs_is_function(g_vm, fn)) {
        char desc[140];
        ujs_describe(g_vm, ujs_exception(g_vm), desc, sizeof desc);
        ujs_clear_exception(g_vm);
        uc_scpy(err, "MAIN.JS: ", cap);
        uc_scat(err, desc[0] ? desc : "did not compile", cap);
        ujs_scope_close(g_vm, &sc, ujs_undefined());
        g_cur_ext = -1;
        return 0;
    }
    module = ujs_object_new(g_vm);
    exports = ujs_object_new(g_vm);
    ujs_set(g_vm, module, "exports", exports);
    ujs_get(g_vm, ujs_global(g_vm), "require", &req);
    args[0] = module; args[1] = exports; args[2] = req;
    if (!call_fn(fn, 3, args, &out, "extension body")) {
        uc_scpy(err, "MAIN.JS threw while loading", cap);
        ujs_scope_close(g_vm, &sc, ujs_undefined());
        g_cur_ext = -1;
        return 0;
    }
    /* exports may have been REPLACED (module.exports = {...}), so read it back
     * from the module rather than trusting the object we passed in */
    ujs_get(g_vm, module, "exports", &exports);
    ujs_get(g_vm, exports, "activate", &act);
    if (ujs_is_function(g_vm, act)) {
        ctx = ujs_object_new(g_vm);
        ujs_set(g_vm, ctx, "subscriptions", ujs_array_new(g_vm));
        ujs_set(g_vm, ctx, "extensionPath", ujs_string(g_vm, path, -1));
        ujs_set(g_vm, ctx, "secrets", secrets_new(g_vm, ext));
        ok = call_fn(act, 1, &ctx, &out, "activate");
        if (!ok) uc_scpy(err, "activate() threw", cap);
    } else ok = 1;                       /* a module with no activate is legal */
    ujs_scope_close(g_vm, &sc, ujs_undefined());
    g_cur_ext = -1;
    return ok;
}

void uc_api_pump(void)
{
    /* One call, and it is the engine's own queue (UCD-21).  Every .then, every
     * resumed `await` and every settled host promise runs here - which is why
     * this is called once per frame rather than only when the host thinks
     * something is outstanding. */
    if (g_vm) ujs_run_jobs(g_vm);
}

void uc_api_eval_print(const char *expr)
{
    ujs_val out = ujs_undefined();
    ujs_result r;
    char desc[200];
    int slices = 0;
    if (!uc_api_init()) { uc_term_writeln("js: the extension host could not start"); return; }
    ujs_fuel_reset(g_vm);
    r = ujs_eval(g_vm, expr, -1, &out);
    while (r == UJS_YIELD && ++slices < 64) r = ujs_resume(g_vm, &out);
    if (r == UJS_YIELD) { uc_term_writeln("js: ran too long and was stopped"); return; }
    if (r != UJS_OK) {
        ujs_describe(g_vm, ujs_exception(g_vm), desc, sizeof desc);
        ujs_clear_exception(g_vm);
        uc_term_write("js: ");
        uc_term_writeln(desc);
        return;
    }
    ujs_describe(g_vm, out, desc, sizeof desc);
    uc_term_writeln(desc);
}
