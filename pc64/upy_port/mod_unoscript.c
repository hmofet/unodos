/* ===========================================================================
 * The `unoscript` MicroPython module - the production, capability-gated OS
 * scripting surface.  Unlike `unoauto` (debug-only, compiles away), this ships
 * in PRODUCTION and is the Python face of "script every surface of the OS".
 *
 *   import unoscript as u
 *   u.ui.click(400, 300)            # tier 0 - works for any logged-in user
 *   u.ui.type("hello")
 *   for p in u.proc.list():         # tier 2 - raises OSError(EPERM) until you
 *       print(p)                    #          escalate (or hold the role)
 *   u.request("mem.read")           # ask unosecure to raise authority
 *   data = u.mem.read(pid, 0x1000, 64)
 *
 * Namespaces (u.ui / u.app / u.fs / u.proc / u.mem / u.io / u.sys) map to the
 * capability domains in unoscript.h.  Every call funnels through the C guard,
 * which consults `unosecure`.  Denied ops raise OSError(EPERM); ops whose
 * subsystem seam is not yet present raise NotImplementedError.
 *
 * STATUS: STUB, pending `unosecure`.  Tier-0 calls are permitted by the guard
 * but their surface seams (unoui, shell, ...) are stubbed USC_EUNAVAIL for now,
 * so today they raise NotImplementedError; tier>=1 raises OSError(EPERM)
 * (fail-closed) until unosecure adjudicates.  Not yet in build.sh.
 * ======================================================================== */
#include "py/runtime.h"
#include "py/obj.h"
#include "../unoscript.h"

unsigned long strlen(const char *);

/* Turn a usc return code into either a value or the right Python exception. */
static mp_obj_t usc_ret(int rc)
{
    /* Denied -> OSError.  (This port is at ROM_LEVEL_CORE_FEATURES, where
     * PermissionError is not compiled in; OSError is the available base.  If the
     * port is ever raised to BASIC_FEATURES, switch these to PermissionError.) */
    if (rc == USC_EDENIED || rc == USC_ENOSEC)
        mp_raise_msg(&mp_type_OSError,
                     rc == USC_ENOSEC ? MP_ERROR_TEXT("EPERM: unosecure not present")
                                      : MP_ERROR_TEXT("EPERM: capability denied"));
    if (rc == USC_EUNAVAIL)
        mp_raise_msg(&mp_type_NotImplementedError, MP_ERROR_TEXT("surface not wired"));
    if (rc == USC_EINVAL)
        mp_raise_ValueError(MP_ERROR_TEXT("bad argument"));
    return mp_obj_new_int(rc);
}

/* ---- top-level control ------------------------------------------------ */
static mp_obj_t u_available(void) { return mp_obj_new_bool(unoscript_available()); }

static mp_obj_t u_whoami(void) { return mp_obj_new_int(unosec_current_user()); }

static mp_obj_t u_secured(void) { return mp_obj_new_bool(unosec_present()); }

/* cap_tier("mem.read") -> 3 ; unknown -> raises */
static mp_obj_t u_cap_tier(mp_obj_t name)
{
    const char *s = mp_obj_str_get_str(name);
    for (int c = 0; c < USC_CAP__COUNT; c++)
        if (!__builtin_strcmp(s, unoscript_cap_name(c)))
            return mp_obj_new_int(unoscript_cap_tier(c));
    mp_raise_ValueError(MP_ERROR_TEXT("unknown capability"));
}

/* request("mem.read", scope=0, ttl_ms=0) -> True if unosecure granted it.
 * The interactive escalation entry: prompts / checks roles via unosecure. */
static mp_obj_t u_request(size_t n, const mp_obj_t *a)
{
    const char *s = mp_obj_str_get_str(a[0]);
    usc_scope_t scope = n > 1 ? mp_obj_get_int(a[1]) : USC_SCOPE_SESSION;
    int ttl = n > 2 ? mp_obj_get_int(a[2]) : 0;
    for (int c = 0; c < USC_CAP__COUNT; c++)
        if (!__builtin_strcmp(s, unoscript_cap_name(c)))
            return mp_obj_new_bool(unosec_request(c, scope, ttl) > 0);
    mp_raise_ValueError(MP_ERROR_TEXT("unknown capability"));
}

static MP_DEFINE_CONST_FUN_OBJ_0(u_available_obj, u_available);
static MP_DEFINE_CONST_FUN_OBJ_0(u_whoami_obj, u_whoami);
static MP_DEFINE_CONST_FUN_OBJ_0(u_secured_obj, u_secured);
static MP_DEFINE_CONST_FUN_OBJ_1(u_cap_tier_obj, u_cap_tier);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(u_request_obj, 1, 3, u_request);

/* ======================= namespace: ui (tier 0) ======================== */
static mp_obj_t ui_click(size_t n, const mp_obj_t *a)
{ return usc_ret(usc_ui_pointer(mp_obj_get_int(a[0]), mp_obj_get_int(a[1]),
                                n > 2 ? mp_obj_get_int(a[2]) : 1)); }
static mp_obj_t ui_move(mp_obj_t x, mp_obj_t y)
{ return usc_ret(usc_ui_pointer(mp_obj_get_int(x), mp_obj_get_int(y), 0)); }
static mp_obj_t ui_key(size_t n, const mp_obj_t *a)
{ return usc_ret(usc_ui_key(mp_obj_get_int(a[0]), n > 1 ? mp_obj_get_int(a[1]) : 0,
                            n > 2 ? mp_obj_get_int(a[2]) : 0)); }
static mp_obj_t ui_screen(void)
{ char b[512]; int rc = usc_ui_screen_text(b, sizeof b);
  return rc >= 0 ? mp_obj_new_str(b, strlen(b)) : usc_ret(rc); }
static mp_obj_t ui_clip_get(void)
{ char b[512]; int rc = usc_ui_clipboard_get(b, sizeof b);
  return rc >= 0 ? mp_obj_new_str(b, strlen(b)) : usc_ret(rc); }
static mp_obj_t ui_clip_set(mp_obj_t s)
{ return usc_ret(usc_ui_clipboard_set(mp_obj_str_get_str(s))); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_click_obj, 2, 3, ui_click);
static MP_DEFINE_CONST_FUN_OBJ_2(ui_move_obj, ui_move);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_key_obj, 1, 3, ui_key);
static MP_DEFINE_CONST_FUN_OBJ_0(ui_screen_obj, ui_screen);
static MP_DEFINE_CONST_FUN_OBJ_0(ui_clip_get_obj, ui_clip_get);
static MP_DEFINE_CONST_FUN_OBJ_1(ui_clip_set_obj, ui_clip_set);
static const mp_rom_map_elem_t ui_tab[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_ui) },
    { MP_ROM_QSTR(MP_QSTR_click),     MP_ROM_PTR(&ui_click_obj) },
    { MP_ROM_QSTR(MP_QSTR_move),      MP_ROM_PTR(&ui_move_obj) },
    { MP_ROM_QSTR(MP_QSTR_key),       MP_ROM_PTR(&ui_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_screen),    MP_ROM_PTR(&ui_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_clip_get),  MP_ROM_PTR(&ui_clip_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_clip_set),  MP_ROM_PTR(&ui_clip_set_obj) },
};
static MP_DEFINE_CONST_DICT(ui_dict, ui_tab);
static const mp_obj_module_t u_ui = { { &mp_type_module }, (mp_obj_dict_t *)&ui_dict };

/* ======================= namespace: app (tier 0/1) ===================== */
static mp_obj_t app_count(void)         { return usc_ret(usc_app_count()); }
static mp_obj_t app_launch(mp_obj_t i)  { return usc_ret(usc_app_launch(mp_obj_get_int(i))); }
static mp_obj_t app_close_top(void)     { return usc_ret(usc_app_close_top()); }
static MP_DEFINE_CONST_FUN_OBJ_0(app_count_obj, app_count);
static MP_DEFINE_CONST_FUN_OBJ_1(app_launch_obj, app_launch);
static MP_DEFINE_CONST_FUN_OBJ_0(app_close_top_obj, app_close_top);
static const mp_rom_map_elem_t app_tab[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_app) },
    { MP_ROM_QSTR(MP_QSTR_count),     MP_ROM_PTR(&app_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_launch),    MP_ROM_PTR(&app_launch_obj) },
    { MP_ROM_QSTR(MP_QSTR_close_top), MP_ROM_PTR(&app_close_top_obj) },
};
static MP_DEFINE_CONST_DICT(app_dict, app_tab);
static const mp_obj_module_t u_app = { { &mp_type_module }, (mp_obj_dict_t *)&app_dict };

/* ======================= namespace: proc (tier 2) ====================== */
/* list() -> [(pid, tid, state, name, owner), ...] */
static mp_obj_t proc_list(void)
{
    usc_proc_ent e[64];
    int n = usc_proc_list(e, 64);
    if (n < 0) return usc_ret(n);
    mp_obj_t lst = mp_obj_new_list(0, 0);
    for (int i = 0; i < n; i++) {
        mp_obj_t t[5] = {
            mp_obj_new_int(e[i].pid), mp_obj_new_int(e[i].tid),
            mp_obj_new_int(e[i].state),
            mp_obj_new_str(e[i].name, strlen(e[i].name)),
            mp_obj_new_int(e[i].owner),
        };
        mp_obj_list_append(lst, mp_obj_new_tuple(5, t));
    }
    return lst;
}
static MP_DEFINE_CONST_FUN_OBJ_0(proc_list_obj, proc_list);
static const mp_rom_map_elem_t proc_tab[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_proc) },
    { MP_ROM_QSTR(MP_QSTR_list),     MP_ROM_PTR(&proc_list_obj) },
};
static MP_DEFINE_CONST_DICT(proc_dict, proc_tab);
static const mp_obj_module_t u_proc = { { &mp_type_module }, (mp_obj_dict_t *)&proc_dict };

/* ======================= namespace: mem (tier 3) ======================= */
/* read(pid, addr, len) -> bytes ; write(pid, addr, bytes) -> n */
static mp_obj_t mem_read(mp_obj_t pid, mp_obj_t addr, mp_obj_t len)
{
    int n = mp_obj_get_int(len);
    if (n <= 0 || n > 4096) mp_raise_ValueError(MP_ERROR_TEXT("len 1..4096"));
    char buf[4096];
    int rc = usc_mem_read(mp_obj_get_int(pid),
                          (unsigned long long)mp_obj_get_int(addr), buf, n);
    return rc >= 0 ? mp_obj_new_bytes((const byte *)buf, rc) : usc_ret(rc);
}
static mp_obj_t mem_write(mp_obj_t pid, mp_obj_t addr, mp_obj_t data)
{
    mp_buffer_info_t bi;
    mp_get_buffer_raise(data, &bi, MP_BUFFER_READ);
    return usc_ret(usc_mem_write(mp_obj_get_int(pid),
                                 (unsigned long long)mp_obj_get_int(addr),
                                 bi.buf, bi.len));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mem_read_obj, mem_read);
static MP_DEFINE_CONST_FUN_OBJ_3(mem_write_obj, mem_write);
static const mp_rom_map_elem_t mem_tab[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mem) },
    { MP_ROM_QSTR(MP_QSTR_read),     MP_ROM_PTR(&mem_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),    MP_ROM_PTR(&mem_write_obj) },
};
static MP_DEFINE_CONST_DICT(mem_dict, mem_tab);
static const mp_obj_module_t u_mem = { { &mp_type_module }, (mp_obj_dict_t *)&mem_dict };

/* ======================= namespace: io (tier 2/3) ====================== */
static mp_obj_t io_in(mp_obj_t port, mp_obj_t width)
{ unsigned v = 0; int rc = usc_io_in(mp_obj_get_int(port), mp_obj_get_int(width), &v);
  return rc >= 0 ? mp_obj_new_int_from_uint(v) : usc_ret(rc); }
static mp_obj_t io_out(mp_obj_t port, mp_obj_t width, mp_obj_t val)
{ return usc_ret(usc_io_out(mp_obj_get_int(port), mp_obj_get_int(width),
                            mp_obj_get_int(val))); }
static MP_DEFINE_CONST_FUN_OBJ_2(io_in_obj, io_in);
static MP_DEFINE_CONST_FUN_OBJ_3(io_out_obj, io_out);
static const mp_rom_map_elem_t io_tab[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_io) },
    { MP_ROM_QSTR(MP_QSTR_in_),      MP_ROM_PTR(&io_in_obj) },
    { MP_ROM_QSTR(MP_QSTR_out),      MP_ROM_PTR(&io_out_obj) },
};
static MP_DEFINE_CONST_DICT(io_dict, io_tab);
static const mp_obj_module_t u_io = { { &mp_type_module }, (mp_obj_dict_t *)&io_dict };

/* ======================= namespace: sys (tier 2) ======================= */
static mp_obj_t sys_power(mp_obj_t action) { return usc_ret(usc_power(mp_obj_get_int(action))); }
static MP_DEFINE_CONST_FUN_OBJ_1(sys_power_obj, sys_power);
static const mp_rom_map_elem_t sys_tab[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sys) },
    { MP_ROM_QSTR(MP_QSTR_power),    MP_ROM_PTR(&sys_power_obj) },
};
static MP_DEFINE_CONST_DICT(sys_dict, sys_tab);
static const mp_obj_module_t u_sys = { { &mp_type_module }, (mp_obj_dict_t *)&sys_dict };

/* ======================= the parent module ============================= */
static const mp_rom_map_elem_t unoscript_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_unoscript) },
    /* control */
    { MP_ROM_QSTR(MP_QSTR_available), MP_ROM_PTR(&u_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_whoami),    MP_ROM_PTR(&u_whoami_obj) },
    { MP_ROM_QSTR(MP_QSTR_secured),   MP_ROM_PTR(&u_secured_obj) },
    { MP_ROM_QSTR(MP_QSTR_cap_tier),  MP_ROM_PTR(&u_cap_tier_obj) },
    { MP_ROM_QSTR(MP_QSTR_request),   MP_ROM_PTR(&u_request_obj) },
    /* namespaces */
    { MP_ROM_QSTR(MP_QSTR_ui),        MP_ROM_PTR(&u_ui) },
    { MP_ROM_QSTR(MP_QSTR_app),       MP_ROM_PTR(&u_app) },
    { MP_ROM_QSTR(MP_QSTR_proc),      MP_ROM_PTR(&u_proc) },
    { MP_ROM_QSTR(MP_QSTR_mem),       MP_ROM_PTR(&u_mem) },
    { MP_ROM_QSTR(MP_QSTR_io),        MP_ROM_PTR(&u_io) },
    { MP_ROM_QSTR(MP_QSTR_sys),       MP_ROM_PTR(&u_sys) },
};
static MP_DEFINE_CONST_DICT(unoscript_globals, unoscript_globals_table);
const mp_obj_module_t mp_module_unoscript = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&unoscript_globals,
};
MP_REGISTER_MODULE(MP_QSTR_unoscript, mp_module_unoscript);
