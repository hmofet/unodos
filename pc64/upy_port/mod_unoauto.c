/* ===========================================================================
 * The `unoauto` MicroPython module - scripted automation over unoautomate.
 *
 * Binds the unoautomate DRIVE surface - structured logging, the PROBE snapshot,
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
 * PRODUCTION: this module ships too, as of 2026-08-03.  It used to be a set of
 * inert stubs (available() -> False) because unoautomate itself was debug-only.
 * Now the surface is real in every build and PRIVILEGE decides who may use it,
 * exactly as for the URC channel - see unoauto_gate.h.  Each function below
 * asks unosecure for the matching automate.* capability and, when the script
 * does not hold it, returns the same inert value the old production stub did
 * (False / [] / None) rather than raising: an automation script that checks
 * what it may do keeps working unchanged on a machine that grants it nothing.
 *
 * The capability is CHECKED, never requested: a script that wants an escalation
 * prompt should ask through unoscript's guard, the surface designed for it.
 * Here, already holding the grant is the precondition.
 * ======================================================================== */
#include "py/runtime.h"
#include "py/obj.h"
#include "../unoauto.h"

#include "../unoscript.h"       /* usc_cap_t + the unosec_* seam */

unsigned long strlen(const char *);
/* kernel exports (no public header; the kExports table in pc64_modload.c
 * carries the same list) */
void uno_pc64_inject_key(int scan, int uni, int ctrl);
void uno_pc64_inject_pointer(int x, int y, int btn);
int  pc64_shell_app_count(void);
int  pc64_shell_launch(int a);
void pc64_shell_close_top(void);
void uno_pc64_shutdown(void);
unsigned long long uno_dbg_uptime_ms(void);
long unoauto_deadline_left(void);   /* 23-char alias of _left_ms */
/* remote dev-PC channel (unoauto_remote.c) */
int  unoauto_remote_active(void);
int  unoauto_remote_send(const char *type, const char *text);
int  unoauto_remote_recv(char *buf, int cap);
void unoauto_remote_stop(void);

/* THE GATE.  In a debug build the harness is the whole point, so everything is
 * permitted; in production each call must hold the capability its blast radius
 * earns.  unosec_require is the side-effect-free live check (a static role
 * grant OR an escalation already raised) - it never prompts, so a headless
 * script cannot wedge on a consent sheet nobody is there to answer. */
static int ua_may(usc_cap_t cap)
{
#ifdef UNO_DEBUG
    (void)cap; return 1;
#else
    return unosec_require(cap);
#endif
}

/* available() now answers "may I use this?" rather than "was this compiled
 * in?".  Scripts already branch on it, so it keeps carrying the useful one. */
static mp_obj_t ua_available(void)
{ return mp_obj_new_bool(ua_may(USC_CAP_AUTOMATE_OBSERVE)); }

static mp_obj_t ua_log(mp_obj_t s)
{ if (ua_may(USC_CAP_AUTOMATE_OBSERVE))
      unoauto_log(UA_CH_SCRIPT, "%s", mp_obj_str_get_str(s));
  return mp_const_none; }

/* probe() -> [(name, kind, state, v1, v2), ...] - see unoauto.h for kinds */
static mp_obj_t ua_probe(void)
{
    UnoAutoProbeEnt e[64];
    mp_obj_t list = mp_obj_new_list(0, 0);
    int n, i;
    if (!ua_may(USC_CAP_AUTOMATE_OBSERVE)) return list;   /* empty, as before */
    n = unoauto_probe(e, 64);
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
{ if (ua_may(USC_CAP_AUTOMATE_DRIVE))
      uno_pc64_inject_key(mp_obj_get_int(a[0]), mp_obj_get_int(a[1]),
                          n > 2 ? mp_obj_get_int(a[2]) : 0);
  return mp_const_none; }
static mp_obj_t ua_pointer(mp_obj_t x, mp_obj_t y, mp_obj_t btn)
{ if (ua_may(USC_CAP_AUTOMATE_DRIVE))
      uno_pc64_inject_pointer(mp_obj_get_int(x), mp_obj_get_int(y),
                              mp_obj_get_int(btn));
  return mp_const_none; }

static mp_obj_t ua_apps(void)
{ return mp_obj_new_int(ua_may(USC_CAP_AUTOMATE_OBSERVE) ? pc64_shell_app_count() : 0); }
static mp_obj_t ua_launch(mp_obj_t i)
{ return mp_obj_new_bool(ua_may(USC_CAP_AUTOMATE_DRIVE)
                         && pc64_shell_launch(mp_obj_get_int(i))); }
static mp_obj_t ua_close_top(void)
{ if (ua_may(USC_CAP_AUTOMATE_DRIVE)) pc64_shell_close_top(); return mp_const_none; }
/* uptime and the test deadline are clock reads - AMBIENT by any measure, and
 * unoscript already hands every script a clock.  Ungated. */
static mp_obj_t ua_uptime(void)        { return mp_obj_new_int_from_ull(uno_dbg_uptime_ms()); }
static mp_obj_t ua_deadline(void)      { return mp_obj_new_int(unoauto_deadline_left()); }
/* powering the machine off is SYSTEM: the one call here that can lose unsaved
 * work belonging to somebody else. */
static mp_obj_t ua_poweroff(void)
{ if (ua_may(USC_CAP_AUTOMATE_SYSTEM)) uno_pc64_shutdown(); return mp_const_none; }

/* remote dev-PC link: exchange messages with the PC you develop from.  Reading
 * the link's state is OBSERVE; putting bytes on it, or killing somebody else's
 * live remote session, is SYSTEM. */
static mp_obj_t ua_rc_active(void)
{ return mp_obj_new_bool(ua_may(USC_CAP_AUTOMATE_OBSERVE) && unoauto_remote_active()); }
static mp_obj_t ua_rc_send(mp_obj_t s)
{ if (!ua_may(USC_CAP_AUTOMATE_SYSTEM)) return mp_obj_new_int(-1);
  return mp_obj_new_int(unoauto_remote_send("MSG", mp_obj_str_get_str(s))); }
static mp_obj_t ua_rc_recv(void)
{ char b[256]; int n;
  if (!ua_may(USC_CAP_AUTOMATE_SYSTEM)) return mp_const_none;
  n = unoauto_remote_recv(b, sizeof b);
  return n > 0 ? mp_obj_new_str(b, n) : mp_const_none; }
static mp_obj_t ua_rc_stop(void)
{ if (ua_may(USC_CAP_AUTOMATE_SYSTEM)) unoauto_remote_stop(); return mp_const_none; }

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
static MP_DEFINE_CONST_FUN_OBJ_0(ua_rc_active_obj, ua_rc_active);
static MP_DEFINE_CONST_FUN_OBJ_1(ua_rc_send_obj, ua_rc_send);
static MP_DEFINE_CONST_FUN_OBJ_0(ua_rc_recv_obj, ua_rc_recv);
static MP_DEFINE_CONST_FUN_OBJ_0(ua_rc_stop_obj, ua_rc_stop);

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
    { MP_ROM_QSTR(MP_QSTR_remote_active), MP_ROM_PTR(&ua_rc_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_remote_send),   MP_ROM_PTR(&ua_rc_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_remote_recv),   MP_ROM_PTR(&ua_rc_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_remote_stop),   MP_ROM_PTR(&ua_rc_stop_obj) },
};
static MP_DEFINE_CONST_DICT(unoauto_globals, unoauto_globals_table);
const mp_obj_module_t mp_module_unoauto = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&unoauto_globals,
};
MP_REGISTER_MODULE(MP_QSTR_unoauto, mp_module_unoauto);
