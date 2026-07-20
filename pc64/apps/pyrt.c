/* ===========================================================================
 * PYRT.UNO - the UnoDOS Python runtime module.
 *
 * A native .UNO module (flags UNO_MODF_PY) carrying the vendored MicroPython
 * VM + the `uno` bindings.  Its entry returns a PyHost the shell holds; the
 * shell hands it Python programs (from PYAPP containers) and PYRT compiles,
 * runs and hosts them, presenting a UnoUuiApp whose build/draw/key/tick call
 * into the app's Python callbacks under NLR protection (a raising app can
 * never unwind through the kernel).
 *
 * Everything the interpreter needs beyond its own code - malloc, libc, libm,
 * the framebuffer/unoui/unosound/uno3d platform - comes through the kernel
 * export table, resolved by the .UNO loader.  See pyhost.h / mod_uno.c.
 * ======================================================================== */
#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/builtin.h"
#include "unoui.h"
#include "../pyhost.h"

/* shell + fb the module imports (resolved against kExports by the loader) */
unoui_widget *unoui_add_canvas(unoui_window *, int x, int y, int w, int h, unoui_canvas *c);
void unoui_window_init(unoui_window *win, const char *title, int x, int y, int w, int h);
void unoui_widget_fill(unoui_widget *w);
void pc64_shell_dirty(void);
int  pc64_shell_workarea_w(void);
int  pc64_shell_workarea_h(void);

/* mod_uno.c drawing-rect coupling: the canvas' absolute rect is published so
 * uno.fb.* offset + clip to it (Python draws in canvas-relative coords) */
void uno_set_draw_rect(int x, int y, int w, int h);

#ifdef UNO_APP_SYM
#  define uno_app_main UNO_APP_SYM
#endif

/* from pc64_upy_port.c */
void        pyrt_out_reset(void);
const char *pyrt_out_text(void);
void        pyrt_set_stack_top(void *p);

/* the bound callbacks of the running app (mod_uno.c installs these) */
typedef struct {
    mp_obj_t app;        /* the uno.App instance                    */
    mp_obj_t build, draw, action, key, tick, opened, closed;
    const char *name;
} PyApp;
static PyApp gApp;
static UnoUuiApp gVt;                        /* the vtable handed to the shell */
static char gName[32];
static int  gInited;

/* mod_uno.c: pull `app` out of a just-exec'd module and fill gApp */
int  uno_bind_app(PyApp *pa);                /* 1 ok, 0 no app / bad */
void uno_win_set(unoui_window *w);           /* stash the current window for uno.ui */
mp_obj_t uno_canvas_obj(void);               /* the canvas arg passed to draw() */

/* ---- NLR-fenced calls into Python ----------------------------------------- */
static mp_obj_t call0(mp_obj_t fn)
{
    if (fn == MP_OBJ_NULL) return mp_const_none;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t r = mp_call_function_0(fn);
        nlr_pop();
        return r;
    }
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);   /* -> pyrt_out */
    return MP_OBJ_NULL;
}
static mp_obj_t call1(mp_obj_t fn, mp_obj_t a)
{
    if (fn == MP_OBJ_NULL) return mp_const_none;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t r = mp_call_function_1(fn, a);
        nlr_pop();
        return r;
    }
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    return MP_OBJ_NULL;
}
static mp_obj_t call3(mp_obj_t fn, mp_obj_t a, mp_obj_t b, mp_obj_t c)
{
    if (fn == MP_OBJ_NULL) return mp_const_none;
    nlr_buf_t nlr;
    mp_obj_t args[3] = { a, b, c };
    if (nlr_push(&nlr) == 0) {
        mp_obj_t r = mp_call_function_n_kw(fn, 3, 0, args);
        nlr_pop();
        return r;
    }
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    return MP_OBJ_NULL;
}

/* ---- the app's canvas: draws by calling the Python draw() ----------------- */
static void py_canvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    (void)w; (void)ctx;
    uno_set_draw_rect(r.x, r.y, r.w, r.h);
    if (gApp.draw) call1(gApp.draw, uno_canvas_obj());
}
static int py_canvas_event(struct unoui_widget *w, const void *ev, void *ctx)
{ (void)w; (void)ev; (void)ctx; return 0; }
static unoui_canvas gCanvas = { py_canvas_draw, py_canvas_event, 0 };

/* ---- the UnoUuiApp trampolines -------------------------------------------- */
static void tr_build(unoui_window *w)
{
    int aw = pc64_shell_workarea_w() - 120, ah = pc64_shell_workarea_h() - 120;
    if (aw > 520) aw = 520; if (aw < 200) aw = 200;
    if (ah > 380) ah = 380; if (ah < 140) ah = 140;
    uno_win_set(w);
    /* a full-window canvas the app draws into; build() may add widgets too */
    unoui_window_init(w, gName, 40, 24, aw + 24, ah + 40);
    unoui_widget_fill(unoui_add_canvas(w, 0, 0, aw, ah, &gCanvas));
    if (gApp.build) call1(gApp.build, uno_canvas_obj());
    gc_collect();
}
static int tr_action(const unoui_action *a)
{
    (void)a;
    if (gApp.action == MP_OBJ_NULL) return 0;
    { mp_obj_t r = call1(gApp.action, mp_obj_new_int(a->id));
      return (r != MP_OBJ_NULL && mp_obj_is_true(r)) ? 1 : 0; }
}
static int tr_key(int uni, int scan, int ctrl)
{
    if (gApp.key == MP_OBJ_NULL) return 0;
    { mp_obj_t r = call3(gApp.key, mp_obj_new_int(uni), mp_obj_new_int(scan), mp_obj_new_int(ctrl));
      return (r != MP_OBJ_NULL && mp_obj_is_true(r)) ? 1 : 0; }
}
static void tr_frame(void)
{
    if (gApp.tick) { call0(gApp.tick); pc64_shell_dirty(); }   /* draw fires in the canvas */
}
static void tr_opened(void) { call0(gApp.opened); }
static void tr_closed(void) { call0(gApp.closed); }
static int  tr_canvas_index(void) { return 0; }

/* ---- PyHost implementation ------------------------------------------------ */
static int py_init(void *heap, unsigned long size)
{
    int stack_marker;
    if (gInited) return 0;
    pyrt_set_stack_top(&stack_marker);
    mp_stack_ctrl_init();
    mp_stack_set_limit(64 * 1024);
    gc_init(heap, (char *)heap + size);
    mp_init();
    gInited = 1;
    return 0;
}

static const UnoUuiApp *py_load(const unsigned char *src, int len, const char *name)
{
    nlr_buf_t nlr;
    int i;
    const char *base = name;
    for (i = 0; name && name[i]; i++) if (name[i] == '\\') base = name + i + 1;
    { int j = 0; while (base[j] && j < 31) { gName[j] = base[j]; j++; } gName[j] = 0; }

    pyrt_out_reset();
    gApp.app = MP_OBJ_NULL;
    gApp.build = gApp.draw = gApp.action = gApp.key = gApp.tick =
        gApp.opened = gApp.closed = MP_OBJ_NULL;

    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
                              (const char *)src, (size_t)len, 0);
        qstr sn = lex->source_name;
        mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t mf = mp_compile(&pt, sn, false);
        mp_call_function_0(mf);              /* exec the module -> defines `app` */
        nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        return 0;                            /* compile/exec error -> last_error */
    }

    if (!uno_bind_app(&gApp)) {              /* find the module-global `app` */
        mp_hal_stdout_tx_str("No `app` object. Define: app = MyApp()\n");
        return 0;
    }

    gVt.abi = UNO_UUIAPP_ABI;
    gVt.name = gName;
    gVt.build  = tr_build;
    gVt.action = tr_action;
    gVt.key    = tr_key;
    gVt.frame  = tr_frame;
    gVt.opened = gApp.opened ? tr_opened : 0;
    gVt.closed = gApp.closed ? tr_closed : 0;
    gVt.canvas_index = tr_canvas_index;
    return &gVt;
}

static void py_unload(void)
{
    gApp.app = MP_OBJ_NULL;
    gApp.build = gApp.draw = gApp.action = gApp.key = gApp.tick =
        gApp.opened = gApp.closed = MP_OBJ_NULL;
    if (gInited) gc_collect();
}

static const char *py_last_error(void) { return pyrt_out_text(); }

static const PyHost kHost = {
    UNO_PYHOST_ABI, py_init, py_load, py_unload, py_last_error
};
const PyHost *uno_app_main(void *reserved) { (void)reserved; return &kHost; }
