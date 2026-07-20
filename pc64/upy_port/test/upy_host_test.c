/* Host validation for the UnoDOS MicroPython port (M1).
 *
 * Builds the vendored py/ core with the port's mpconfigport.h on the host
 * (glibc) and runs a few snippets, asserting output.  Proves the vendored
 * tree + codegen + config are sound before the freestanding target build.
 * See build_host.sh. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/objmodule.h"

/* ---- HAL the port header declares (host versions) ------------------------- */
static long g_ticks;
long uno_upy_ticks(void) { return g_ticks++; }
void mp_hal_delay_ms(mp_uint_t ms) { (void)ms; }
void mp_hal_delay_us(mp_uint_t us) { (void)us; }

static char g_out[1 << 16];
static int  g_outn;
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    fwrite(str, 1, len, stdout);
    for (size_t i = 0; i < len && g_outn < (int)sizeof g_out - 1; i++) g_out[g_outn++] = str[i];
    g_out[g_outn] = 0;
    return len;
}
void mp_hal_stdout_tx_str(const char *str) { mp_hal_stdout_tx_strn(str, strlen(str)); }
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) { mp_hal_stdout_tx_strn(str, len); }

/* ---- a minimal `uno` module so the port's module registration links ------- */
static mp_obj_t uno_rgb(mp_obj_t r, mp_obj_t g, mp_obj_t b) {
    mp_int_t rr = mp_obj_get_int(r), gg = mp_obj_get_int(g), bb = mp_obj_get_int(b);
    return mp_obj_new_int(0xFF000000u | (bb << 16) | (gg << 8) | rr);
}
static MP_DEFINE_CONST_FUN_OBJ_3(uno_rgb_obj, uno_rgb);

static const mp_rom_map_elem_t uno_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_uno) },
    { MP_ROM_QSTR(MP_QSTR_rgb),      MP_ROM_PTR(&uno_rgb_obj) },
};
static MP_DEFINE_CONST_DICT(uno_globals, uno_globals_table);
const mp_obj_module_t mp_module_uno = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&uno_globals,
};
MP_REGISTER_MODULE(MP_QSTR_uno, mp_module_uno);

/* ---- VM plumbing ---------------------------------------------------------- */
static char *stack_top;
static char heap[256 * 1024];

void gc_collect(void) {
    void *dummy;
    gc_collect_start();
    gc_collect_root(&dummy, ((mp_uint_t)stack_top - (mp_uint_t)&dummy) / sizeof(mp_uint_t));
    gc_collect_end();
}
mp_lexer_t *mp_lexer_new_from_file(qstr filename) { (void)filename; mp_raise_OSError(MP_ENOENT); }
mp_import_stat_t mp_import_stat(const char *path) { (void)path; return MP_IMPORT_STAT_NO_EXIST; }
void nlr_jump_fail(void *val) { (void)val; printf("FATAL: nlr_jump_fail\n"); exit(2); }

static int run(const char *src) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
        qstr sn = lex->source_name;
        mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t mf = mp_compile(&pt, sn, false);
        mp_call_function_0(mf);
        nlr_pop();
        return 0;
    }
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    return -1;
}

static int fails;
static void expect(const char *tag, const char *want) {
    if (!strstr(g_out, want)) { printf("FAIL %s: output lacked '%s'\n", tag, want); fails++; }
    g_outn = 0; g_out[0] = 0;
}

int main(void) {
    int d; stack_top = (char *)&d;
    gc_init(heap, heap + sizeof heap);
    mp_init();

    run("print(1+1)");                                   expect("arith", "2");
    run("print(2**100)");                                expect("bigint", "1267650600228229401496703205376");
    run("print('hi', [x*x for x in range(4)])");         expect("listcomp", "[0, 1, 4, 9]");
    run("import math\nprint(round(math.sin(0),3), round(math.sqrt(2),3))"); expect("math", "1.414");
    run("class A:\n  def __init__(s,v): s.v=v\n  def d(s): return s.v*3\nprint(A(7).d())"); expect("class", "21");
    run("import uno\nprint(hex(uno.rgb(0x11,0x22,0x33)))"); expect("uno.rgb", "0xff332211");
    run("try:\n  1/0\nexcept ZeroDivisionError as e:\n  print('caught', e)"); expect("except", "caught");

    mp_deinit();
    printf("\nupy_host_test: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
