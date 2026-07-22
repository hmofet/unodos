/* ===========================================================================
 * The `unoauto` MicroPython module - scripted automation over unoautomate.
 *
 * DEBUG builds: binds the unoautomate DRIVE surface (kernel exports gated
 * UNO_DEBUG in pc64_modload.c) - structured logging, the PROBE snapshot,
 * synthetic input, app launch/close through the real launcher code path,
 * and power-off for unattended runs.  An automation script is an ordinary
 * Python APP (uno.App) whose tick() advances a generator - it interleaves
 * with real shell frames, so injected input and launched apps are processed
 * exactly like a human's:
 *
 *   import uno, unoauto
 *   def steps():
 *       unoauto.launch(0); yield          # let the shell run a frame
 *       wins = [r for r in unoauto.probe() if r[1] == 1]
 *       unoauto.log("open: %r" % [w[0] for w in wins])
 *   class T(uno.App):
 *       def build(self, cv): self.g = steps()
 *       def tick(self):
 *           try: next(self.g)
 *           except StopIteration: pass
 *   app = T()
 *
 * PRODUCTION builds: the module still imports, but every function is a stub
 * (available() -> False) and NO debug kernel symbol is referenced - so the
 * prod PYRT.UNO's import table stays clean.
 * ======================================================================== */
#include "py/runtime.h"
#include "py/obj.h"
#include "../unoauto.h"

#ifdef UNO_DEBUG

unsigned long strlen(const char *);
/* debug-only kernel exports (no public header; mirrors pc64_modload.c) */
void uno_pc64_inject_key(int scan, int uni, int ctrl);
void uno_pc64_inject_pointer(int x, int y, int btn);
int  pc64_shell_app_count(void);
int  pc64_shell_launch(int a);
void pc64_shell_close_top(void);
void uno_pc64_shutdown(void);
unsigned long long uno_dbg_uptime_ms(void);
long unoauto_deadline_left(void);   /* 23-char alias of _left_ms */

static mp_obj_t ua_available(void) { return mp_const_true; }

static mp_obj_t ua_log(mp_obj_t s)
{ unoauto_log(UA_CH_SCRIPT, "%s", mp_obj_str_get_str(s)); return mp_const_none; }

/* probe() -> [(name, kind, state, v1, v2), ...] - see unoauto.h for kinds */
static mp_obj_t ua_probe(void)
{
    UnoAutoProbeEnt e[64];
    int n = unoauto_probe(e, 64), i;
    mp_obj_t list = mp_obj_new_list(0, 0);
    for (i = 0; i < n; i++) {
        mp_obj_t t[5];
        t[0] = mp_obj_new_str(e[i].name, strlen(e[i].name));
        t[1] = mp_obj_new_int(e[i].kind);
        t[2] = mp_obj_new_int(e[i].state);
        t[3] = mp_obj_new_int_from_ull(e[i].v1);
        t[4] = mp_obj_new_int_from_ull(e[i].v2);
        mp_obj_list_append(list, mp_obj_new_tuple(5, t));
    }
    return list;
}

/* key(scan, uni, ctrl=0) / pointer(x, y, btn): synthetic input, processed by
 * the NEXT shell frame - yield before asserting on the result. */
static mp_obj_t ua_key(size_t n, const mp_obj_t *a)
{ uno_pc64_inject_key(mp_obj_get_int(a[0]), mp_obj_get_int(a[1]),
                      n > 2 ? mp_obj_get_int(a[2]) : 0); return mp_const_none; }
static mp_obj_t ua_pointer(mp_obj_t x, mp_obj_t y, mp_obj_t btn)
{ uno_pc64_inject_pointer(mp_obj_get_int(x), mp_obj_get_int(y),
                          mp_obj_get_int(btn)); return mp_const_none; }

static mp_obj_t ua_apps(void)          { return mp_obj_new_int(pc64_shell_app_count()); }
static mp_obj_t ua_launch(mp_obj_t i)  { return mp_obj_new_bool(pc64_shell_launch(mp_obj_get_int(i))); }
static mp_obj_t ua_close_top(void)     { pc64_shell_close_top(); return mp_const_none; }
static mp_obj_t ua_uptime(void)        { return mp_obj_new_int_from_ull(uno_dbg_uptime_ms()); }
static mp_obj_t ua_deadline(void)      { return mp_obj_new_int(unoauto_deadline_left()); }
static mp_obj_t ua_poweroff(void)      { uno_pc64_shutdown(); return mp_const_none; }

#else /* !UNO_DEBUG: same surface, inert - no debug kernel imports */

static mp_obj_t ua_available(void) { return mp_const_false; }
static mp_obj_t ua_log(mp_obj_t s) { (void)s; return mp_const_none; }
static mp_obj_t ua_probe(void)     { return mp_obj_new_list(0, 0); }
static mp_obj_t ua_key(size_t n, const mp_obj_t *a) { (void)n; (void)a; return mp_const_none; }
static mp_obj_t ua_pointer(mp_obj_t x, mp_obj_t y, mp_obj_t b)
{ (void)x; (void)y; (void)b; return mp_const_none; }
static mp_obj_t ua_apps(void)      { return mp_obj_new_int(0); }
static mp_obj_t ua_launch(mp_obj_t i) { (void)i; return mp_const_false; }
static mp_obj_t ua_close_top(void) { return mp_const_none; }
static mp_obj_t ua_uptime(void)    { return mp_obj_new_int(0); }
static mp_obj_t ua_deadline(void)  { return mp_obj_new_int(-1); }
static mp_obj_t ua_poweroff(void)  { return mp_const_none; }

#endif /* UNO_DEBUG */

static MP_DEFINE_CONST_FUN_OBJ_0(ua_available_obj, ua_available);
static MP_DEFINE_CONST_FUN_OBJ_1(ua_log_obj, ua_log);
static MP_DEFINE_CONST_FUN_OBJ_0(ua_probe_obj, ua_probe);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ua_key_obj, 2, 3, ua_key);
static MP_DEFINE_CONST_FUN_OBJ_3(ua_pointer_obj, ua_pointer);
static MP_DEFINE_CONST_FUN_OBJ_0(ua_apps_obj, ua_apps);
static MP_DEFINE_CONST_FUN_OBJ_1(ua_launch_obj, ua_launch);
static MP_DEFINE_CONST_FUN_OBJ_0(ua_close_top_obj, ua_close_top);
static MP_DEFINE_CONST_FUN_OBJ_0(ua_uptime_obj, ua_uptime);
static MP_DEFINE_CONST_FUN_OBJ_0(ua_deadline_obj, ua_deadline);
static MP_DEFINE_CONST_FUN_OBJ_0(ua_poweroff_obj, ua_poweroff);

static const mp_rom_map_elem_t unoauto_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),      MP_ROM_QSTR(MP_QSTR_unoauto) },
    { MP_ROM_QSTR(MP_QSTR_available),     MP_ROM_PTR(&ua_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_log),           MP_ROM_PTR(&ua_log_obj) },
    { MP_ROM_QSTR(MP_QSTR_probe),         MP_ROM_PTR(&ua_probe_obj) },
    { MP_ROM_QSTR(MP_QSTR_key),           MP_ROM_PTR(&ua_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_pointer),       MP_ROM_PTR(&ua_pointer_obj) },
    { MP_ROM_QSTR(MP_QSTR_apps),          MP_ROM_PTR(&ua_apps_obj) },
    { MP_ROM_QSTR(MP_QSTR_launch),        MP_ROM_PTR(&ua_launch_obj) },
    { MP_ROM_QSTR(MP_QSTR_close_top),     MP_ROM_PTR(&ua_close_top_obj) },
    { MP_ROM_QSTR(MP_QSTR_uptime),        MP_ROM_PTR(&ua_uptime_obj) },
    { MP_ROM_QSTR(MP_QSTR_deadline_left), MP_ROM_PTR(&ua_deadline_obj) },
    { MP_ROM_QSTR(MP_QSTR_poweroff),      MP_ROM_PTR(&ua_poweroff_obj) },
};
static MP_DEFINE_CONST_DICT(unoauto_globals, unoauto_globals_table);
const mp_obj_module_t mp_module_unoauto = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&unoauto_globals,
};
MP_REGISTER_MODULE(MP_QSTR_unoauto, mp_module_unoauto);
