/* ===========================================================================
 * MicroPython port support for UnoDOS pc64 (PYRT.UNO): the HAL bits mphal.h
 * declares (timing, stdout sink) plus a couple of runtime glue functions.
 * Everything else (libc/libm) is the kernel's, reached through the module
 * export table.  Built freestanding into PYRT.UNO; on the host test these are
 * provided by upy_host_test.c instead (so this file is target-only).
 * ======================================================================== */
#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/stackctrl.h"
#include "py/lexer.h"
#include "py/builtin.h"

/* kernel exports the module imports (resolved by the .UNO loader) */
long TickCount(void);                 /* ~60 Hz */
void uno_pc64_delay_ms(int ms);

long uno_upy_ticks(void) { return TickCount(); }
void mp_hal_delay_ms(mp_uint_t ms) { uno_pc64_delay_ms((int)ms); }
void mp_hal_delay_us(mp_uint_t us) { uno_pc64_delay_ms((int)((us + 999) / 1000)); }

/* ---- stdout ring: print()/traceback text the host (Studio) reads --------- */
#define PYRT_OUT_CAP 8192
static char pyrt_out[PYRT_OUT_CAP];
static int  pyrt_out_n;

void pyrt_out_reset(void) { pyrt_out_n = 0; pyrt_out[0] = 0; }
const char *pyrt_out_text(void) { pyrt_out[pyrt_out_n < PYRT_OUT_CAP ? pyrt_out_n : PYRT_OUT_CAP - 1] = 0; return pyrt_out; }

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (pyrt_out_n < PYRT_OUT_CAP - 1) pyrt_out[pyrt_out_n++] = str[i];
        else {                                   /* ring: drop the oldest half */
            int keep = PYRT_OUT_CAP / 2;
            for (int k = 0; k < keep; k++) pyrt_out[k] = pyrt_out[PYRT_OUT_CAP - keep + k];
            pyrt_out_n = keep;
            pyrt_out[pyrt_out_n++] = str[i];
        }
    }
    return len;
}
void mp_hal_stdout_tx_str(const char *str) { mp_hal_stdout_tx_strn(str, strlen(str)); }
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) { mp_hal_stdout_tx_strn(str, len); }

/* ---- GC root scan of the C stack ------------------------------------------
 * PYRT records the stack top at init; gc_collect walks from the current frame
 * up to it.  Registers are flushed by gc_helper_get_regs_for_collect on x64. */
extern void *gc_helper_get_regs_for_collect(void *regs);  /* upy/shared/runtime/gchelper */
static void *g_stack_top;
void pyrt_set_stack_top(void *p) { g_stack_top = p; }

void gc_collect(void)
{
    void *regs[18];
    void *sp = gc_helper_get_regs_for_collect(regs);
    gc_collect_start();
    gc_collect_root(regs, ((uintptr_t)regs - (uintptr_t)sp) / sizeof(uintptr_t) + sizeof(regs)/sizeof(void*));
    gc_collect_root((void **)sp, ((uintptr_t)g_stack_top - (uintptr_t)sp) / sizeof(uintptr_t));
    gc_collect_end();
}

/* no on-disk import in v1: uno.fs is the explicit door */
mp_import_stat_t mp_import_stat(const char *path) { (void)path; return MP_IMPORT_STAT_NO_EXIST; }
mp_lexer_t *mp_lexer_new_from_file(qstr filename) { (void)filename; mp_raise_OSError(2 /*ENOENT*/); }

/* unrecoverable NLR (should never fire: the host fences every entry) */
void nlr_jump_fail(void *val)
{
    (void)val;
    mp_hal_stdout_tx_str("\n[pyrt] fatal: nlr_jump_fail\n");
    for (;;) { }
}
