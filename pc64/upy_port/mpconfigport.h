/* ===========================================================================
 * MicroPython port config for UnoDOS pc64 (PYRT.UNO).
 *
 * Freestanding x86-64 / mingw (LLP64: long is 32-bit, pointers 64-bit).
 * Single-precision floats (matches pc64_math + uno3d, no double libm).
 * The x64 native emitter is on so @micropython.viper compiles hot loops
 * (Duum's renderer) to machine code.  No filesystem/os/socket/REPL; the
 * host embeds the VM and drives apps through the `uno` module.
 *
 * Two build faces share this file:
 *   - HOST test (glibc): -DUPY_HOST — real libc/libm, for the unit battery.
 *   - TARGET (PYRT.UNO): freestanding, backed by pc64_upy_port.c + the
 *     kernel libc/libm shims.
 * ======================================================================== */
#include <stdint.h>

#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

/* compiler on-device (v1 ships source), GC on */
#define MICROPY_ENABLE_COMPILER         (1)
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_GC_ALLOC_THRESHOLD      (1)
#define MICROPY_ALLOC_PATH_MAX          (128)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT  (32)

/* no REPL, no frozen modules, single-file apps (v1) */
#define MICROPY_HELPER_REPL             (0)
#define MICROPY_MODULE_FROZEN_MPY       (0)
#define MICROPY_MODULE_FROZEN_STR       (0)
#define MICROPY_ENABLE_EXTERNAL_IMPORT  (0)
#define MICROPY_PY_BUILTINS_INPUT       (0)

/* the x64 native emitter (@micropython.native / @micropython.viper).
 * mpconfig.h derives MICROPY_EMIT_NATIVE from this — do not set it directly. */
#define MICROPY_EMIT_X64                (1)

/* floats: single precision (pc64_math is single; no double libm on target) */
#define MICROPY_FLOAT_IMPL              (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_PY_BUILTINS_COMPLEX     (0)
#define MICROPY_PY_MATH                 (1)
#define MICROPY_PY_CMATH                (0)

/* real big integers (pure C, cheap) */
#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_MPZ)

/* let the host inject KeyboardInterrupt (cooperative shell) */
#define MICROPY_KBD_EXCEPTION           (1)

/* error reporting: terse to keep PYRT small; still gives file:line */
#define MICROPY_ERROR_REPORTING         (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_ENABLE_SOURCE_LINE      (1)

/* no OS/filesystem/stdio-file layer - uno.fs is the explicit door */
#define MICROPY_PY_SYS_STDFILES         (0)
#define MICROPY_PY_SYS_PLATFORM         "unodos"
#define MICROPY_PY_IO                   (0)
#define MICROPY_VFS                     (0)
#define MICROPY_READER_VFS              (0)
#define MICROPY_READER_POSIX            (0)
#define MICROPY_PY_OS                   (0)
#define MICROPY_PY_TIME                 (0)

/* finaliser off (no external resources needing __del__ in v1) */
#define MICROPY_ENABLE_FINALISER        (0)

/* NLR: native x86-64 unwinding (nlrx64.c) by default; the port can force
 * MICROPY_NLR_SETJMP=1 (backed by __builtin_setjmp, see pc64/include/setjmp.h)
 * if native NLR misbehaves on the PE/LLP64 stack. */
#ifndef MICROPY_NLR_SETJMP
#define MICROPY_NLR_SETJMP              (0)
#endif

/* ---- machine types: LLP64 (mingw x86-64) ------------------------------- */
typedef intptr_t  mp_int_t;             /* 64-bit, pointer sized */
typedef uintptr_t mp_uint_t;            /* 64-bit */
typedef intptr_t  mp_off_t;             /* NOT long (which is 32-bit here) */

#define MP_SSIZE_MAX                    (INTPTR_MAX)
#define MICROPY_OBJ_REPR               (MICROPY_OBJ_REPR_A)

/* printf integer formats must use long long, since long is 32-bit here */
#define INT_FMT                         "%lld"
#define UINT_FMT                        "%llu"
#define HEX2_FMT                        "%02x"

/* alloca is a compiler builtin */
#include <alloca.h>

#define MICROPY_HW_BOARD_NAME           "UnoDOS pc64"
#define MICROPY_HW_MCU_NAME             "x86-64"

#define MP_STATE_PORT                   MP_STATE_VM

/* the `uno` C module is registered from mod_uno.c */
extern const struct _mp_obj_module_t mp_module_uno;
#define MICROPY_PORT_BUILTIN_MODULES \
    { MP_ROM_QSTR(MP_QSTR_uno), MP_ROM_PTR(&mp_module_uno) },
