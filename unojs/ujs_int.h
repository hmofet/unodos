/* ===========================================================================
 * unojs internals - shared by the lexer, parser, compiler, VM and library.
 * NOT a public header: embedders see unojs.h only.
 * ======================================================================== */
#ifndef UJS_INT_H
#define UJS_INT_H

#include "unojs.h"
#include <string.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* ---- NaN boxing ----------------------------------------------------------
 * A double is stored as itself. Everything else rides in the payload of a
 * quiet NaN:
 *
 *   double      : (bits & NANISH) != NANISH, or payload == 0 (canonical NaN)
 *   immediate   : NANISH | tag              (sign clear, payload != 0)
 *   heap pointer: NANISH | SIGN | ptr48     (sign set)
 *
 * Every double that arrives NaN is canonicalized to payload 0 on the way in
 * (ujs_number), which is what keeps "payload != 0" a sound box test. */
#define UJS_NANISH   0x7FF8000000000000ULL
#define UJS_SIGN     0x8000000000000000ULL
#define UJS_PAYLOAD  0x0007FFFFFFFFFFFFULL

#define UJS_TAG_UNDEF 1
#define UJS_TAG_NULL  2
#define UJS_TAG_FALSE 3
#define UJS_TAG_TRUE  4

#define VBITS(v)     ((v).bits)
#define IS_BOXED(v)  ((VBITS(v) & UJS_NANISH) == UJS_NANISH && (VBITS(v) & UJS_PAYLOAD) != 0)
#define IS_PTR(v)    (IS_BOXED(v) && (VBITS(v) & UJS_SIGN) != 0)
#define IS_DOUBLE(v) (!IS_BOXED(v))
#define PTR_OF(v)    ((void *)(size_t)(VBITS(v) & UJS_PAYLOAD))

static inline ujs_val ujs_mkbits(u64 b) { ujs_val v; v.bits = b; return v; }
static inline ujs_val ujs_ptrval(void *p)
{ return ujs_mkbits(UJS_NANISH | UJS_SIGN | (u64)(size_t)p); }
static inline double ujs_dbl(ujs_val v) { double d; memcpy(&d, &v.bits, 8); return d; }

/* ---- GC heap objects -----------------------------------------------------
 * Every collectable allocation starts with ujs_hdr and is threaded on one
 * all-objects list; the collector is a non-moving mark-sweep over that list.
 * Non-moving matters: embedder C code holds raw ujs_vals (rooted by handle
 * scopes), and a moving collector would invalidate them. */
enum { H_STR = 1, H_OBJ, H_ENV, H_CODE };

typedef struct ujs_hdr {
    struct ujs_hdr *next;
    u32  size;                 /* bytes, for the sweeper's accounting */
    u8   type;
    u8   mark;
    u16  pad;
} ujs_hdr;

typedef struct {
    ujs_hdr h;
    u32  len;                  /* bytes, excluding the NUL */
    u32  hash;
    char b[1];                 /* NUL-terminated UTF-8 */
} ujs_str;

/* object classes */
enum { C_PLAIN = 0, C_ARRAY, C_FUNC, C_CFUNC, C_HOST, C_ERROR };

/* property flags */
enum { P_ENUM = 1, P_WRITE = 2, P_CONFIG = 4, P_ACCESSOR = 8,
       P_DEFAULT = P_ENUM | P_WRITE | P_CONFIG };

typedef struct {
    u32     atom;              /* 0 = empty slot */
    u32     flags;
    ujs_val v;                 /* value, or the getter when P_ACCESSOR */
    ujs_val setter;            /* only when P_ACCESSOR */
} ujs_prop;

struct ujs_code;

typedef struct ujs_obj {
    ujs_hdr h;
    u8      cls;
    ujs_val proto;             /* an object, or null */
    ujs_prop *props;           /* open-addressed; power-of-two capacity */
    u32     nprops, propcap;
    ujs_val *elems;            /* dense indexed storage (arrays, and any
                                * object given small integer keys)          */
    u32     nelems, elemcap;
    union {
        struct { struct ujs_code *code; struct ujs_env *env; } fn;
        struct { ujs_cfunc fn; int nargs; } cfn;
        struct { void *user; ujs_finalizer fin; } host;
    } u;
} ujs_obj;

typedef struct ujs_env {
    ujs_hdr h;
    struct ujs_env *parent;
    u32     n;
    ujs_val slots[1];
} ujs_env;

/* A compiled function body. GC'd because its constant pool holds values. */
typedef struct ujs_code {
    ujs_hdr h;
    u8     *bc;      u32 nbc;
    ujs_val *consts; u32 nconsts;
    u32     nparams;
    u32     nslots;              /* environment slots this body needs */
    u32     name_atom;
    int     is_arrow;            /* arrow: `this` comes from the definition */
    /* line table for diagnostics: parallel to bytecode offsets, sparse */
    u32    *lines; u32 nlines;
} ujs_code;

/* ---- opcodes -------------------------------------------------------------
 * Stack machine. Operands are little-endian; A1 = one byte, A2 = two, A4 =
 * four. The VM's dispatch switch documents each one's stack effect. */
enum {
    OP_NOP = 0,
    OP_CONST,      /* A2 k       -> push consts[k]                          */
    OP_UNDEF, OP_NULL, OP_TRUE, OP_FALSE,
    OP_ZERO, OP_ONE,
    OP_DUP, OP_POP, OP_SWAP,
    OP_ROT3,       /*             a b c -> c a b (rotate the top three)      */
    OP_OVER_DUP,   /*             a b   -> a b a (copy the second down)      */
    OP_GETLOC,     /* A1 depth, A2 idx  -> push env(depth).slots[idx]       */
    OP_SETLOC,     /* A1 depth, A2 idx  : store TOS (leaves it)             */
    OP_GETGLOB,    /* A2 atom-const     -> push global[name] (throws if absent) */
    OP_SETGLOB,    /* A2 atom-const                                          */
    OP_DECLGLOB,   /* A2 atom-const     : define on the global, undefined    */
    OP_GETPROP_KEEP,/* A4 atom          : obj -> obj value (compound assign)  */
    OP_GETIDX_KEEP,/*                     obj key -> obj key value           */
    OP_GETGLOB_SOFT,/* A4 atom          -> value, or undefined if undeclared  */
    OP_SETRES,     /*             pop -> the script's completion value        */
    OP_GETRES,     /*                 -> push the completion value            */
    OP_GETPROP,    /* A2 atom-const     : obj -> value                       */
    OP_SETPROP,    /* A2 atom-const     : obj val -> val                     */
    OP_GETIDX,     /*                     obj key -> value                   */
    OP_SETIDX,     /*                     obj key val -> val                 */
    OP_DELPROP,    /* A4 atom           : obj -> bool                        */
    OP_DELIDX,     /*                     obj key -> bool                    */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_NEG, OP_POS, OP_NOT, OP_BITNOT,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_USHR,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_IN, OP_INSTANCEOF, OP_TYPEOF,
    OP_JMP,        /* A4 target                                              */
    OP_JT,         /* A4 target : pop, jump if truthy                        */
    OP_JF,         /* A4 target : pop, jump if falsy                         */
    OP_JT_KEEP,    /* A4 target : jump if truthy, leaving TOS (for ||)       */
    OP_JF_KEEP,    /* A4 target : jump if falsy, leaving TOS (for &&)        */
    OP_CALL,       /* A1 argc   : fn self arg... -> result                   */
    OP_NEW,        /* A1 argc   : ctor arg...    -> object                   */
    OP_RET,        /*             pop -> return                              */
    OP_CLOSURE,    /* A2 k      -> push a closure over consts[k] (a code obj)*/
    OP_ARRAY,      /* A2 n      : n values -> array                          */
    OP_OBJECT,     /* A2 n      : n (key,value) pairs -> object              */
    OP_THIS,
    OP_THROW,
    OP_TRY,        /* A4 catch-target, A4 finally-target : push a handler    */
    OP_ENDTRY,     /*             pop the handler                            */
    OP_FINALLY_END,/*             resume whatever the finally interrupted    */
    OP_ENV_PUSH,   /* A2 n      : enter a block scope of n slots             */
    OP_ENV_POP,
    OP_ITER,       /*             obj -> iterator state (for-in: keys)       */
    OP_ITER_OF,    /*             obj -> iterator state (for-of: values)      */
    OP_ITER_NEXT,  /* A4 done-target : iter -> iter value, or jump when done */
    OP_LINE,       /* A4 line   : diagnostics only                           */
    OP__COUNT
};

/* Total instruction length in bytes, indexed by opcode. ONE table, shared by
 * the compiler (which walks emitted code to find an assignment target) and the
 * disassembler, so the two can never drift out of step with each other. */
extern const u8 ujs_op_len[OP__COUNT];

/* ---- VM ------------------------------------------------------------------ */
#define UJS_MAX_FRAMES  256
#define UJS_STACK_SIZE  4096
#define UJS_MAX_ROOTS   512

typedef struct {
    ujs_code *code;
    ujs_env  *env;
    ujs_obj  *fn;
    ujs_val   self;
    u32       ip;
    int       sp_base;
    int       h_base;          /* handler stack depth when the frame began */
    int       is_ctor;
} ujs_frame;

typedef struct {
    int frame;
    u32 catch_ip, finally_ip;
    int sp;
    int env_depth;
} ujs_handler;

typedef struct { u32 atom; ujs_str *s; } ujs_atom_ent;

struct ujs_vm {
    ujs_config cfg;

    /* heap */
    ujs_hdr *objects;
    size_t   heap_used, heap_max;
    size_t   gc_threshold;
    int      gc_disabled;

    /* atoms: interned property-name strings. NOTE (v1 limitation, see
     * UNOJS.md): atoms are never collected, so a script that manufactures
     * unbounded distinct string keys grows the table until heap_max stops it.
     * Integer-like keys bypass atoms entirely (they go to dense elements),
     * which is what keeps the common array-ish patterns bounded. */
    ujs_str **atoms;
    u32      natoms, atomcap;
    u32     *atom_hash;        /* open-addressed index into atoms, +1 biased */
    u32      atom_hashcap;

    /* execution state - all of it lives here so UJS_YIELD can return to the
     * host mid-run and ujs_resume() can pick up exactly where it stopped. */
    ujs_val   *stack;
    int        sp;
    ujs_frame *frames;
    int        nframes;
    ujs_handler *handlers;
    int        nhandlers, handlercap;
    int        running;         /* a suspended run is waiting for resume */
    int        c_depth;         /* host C frames on the stack: cannot yield */

    /* roots */
    ujs_val roots[UJS_MAX_ROOTS];
    int     nroots;
    ujs_val *scope_vals;        /* handle-scope stack */
    int      nscope, scopecap;

    /* well-known objects */
    ujs_obj *global;
    ujs_obj *obj_proto, *fun_proto, *arr_proto, *str_proto, *num_proto,
            *bool_proto, *err_proto;

    /* exception + fuel */
    ujs_val exception;
    int     has_exception;
    /* A `finally` entered because an exception was unwinding: the value is
     * parked here and re-thrown by OP_FINALLY_END. v1 keeps ONE slot, so a
     * finally nested inside another finally's exception path loses the outer
     * exception - noted in UNOJS.md's known gaps. */
    ujs_val finally_exc;
    int     finally_rethrow;
    /* A `return` that must run a `finally` first: the value waits here and
     * OP_FINALLY_END completes the return. One slot, like finally_exc. */
    ujs_val pending_ret;
    int     ret_pending;
    /* completion value of the running script - what ujs_eval hands back */
    ujs_val completion;
    unsigned long fuel, fuel_used;
    unsigned long fuel_slice, fuel_total;

    /* scratch for ujs_describe and error formatting */
    char msgbuf[256];
};

/* ---- internal API -------------------------------------------------------- */
/* memory */
void *ujs_alloc_raw(ujs_vm *vm, size_t n);
void  ujs_free_raw(ujs_vm *vm, void *p, size_t n);
void *ujs_gc_alloc(ujs_vm *vm, size_t n, int type);

/* atoms + strings */
u32       ujs_atom(ujs_vm *vm, const char *s, int len);
ujs_str  *ujs_atom_str(ujs_vm *vm, u32 atom);
const char *ujs_atom_cstr(ujs_vm *vm, u32 atom);
ujs_str  *ujs_str_new(ujs_vm *vm, const char *s, int len);
ujs_str  *ujs_str_cat(ujs_vm *vm, ujs_str *a, ujs_str *b);
int       ujs_str_eq(ujs_str *a, ujs_str *b);
ujs_val   ujs_str_val(ujs_str *s);
ujs_str  *ujs_val_str(ujs_val v);

/* objects */
ujs_obj *ujs_obj_new(ujs_vm *vm, int cls, ujs_obj *proto);
ujs_obj *ujs_val_obj(ujs_val v);
ujs_val  ujs_obj_val(ujs_obj *o);
int      ujs_obj_get(ujs_vm *vm, ujs_obj *o, u32 atom, ujs_val *out);   /* 1 = found */
int      ujs_obj_put(ujs_vm *vm, ujs_obj *o, u32 atom, ujs_val v, u32 flags);
int      ujs_obj_del(ujs_vm *vm, ujs_obj *o, u32 atom);
ujs_prop *ujs_obj_find(ujs_obj *o, u32 atom);
void     ujs_arr_set(ujs_vm *vm, ujs_obj *a, u32 i, ujs_val v);
ujs_val  ujs_arr_get(ujs_obj *a, u32 i);

/* environments */
ujs_env *ujs_env_new(ujs_vm *vm, ujs_env *parent, u32 n);

/* conversions (internal; the public ones wrap these) */
double   ujs_num_of(ujs_vm *vm, ujs_val v);
int      ujs_truthy(ujs_val v);
ujs_str *ujs_tostr(ujs_vm *vm, ujs_val v);          /* NULL on throw */
int      ujs_strict_eq(ujs_val a, ujs_val b);
int      ujs_loose_eq(ujs_vm *vm, ujs_val a, ujs_val b);
const char *ujs_typeof_str(ujs_vm *vm, ujs_val v);
void     ujs_num_to_str(double d, char *buf, size_t n);

/* compile + run */
ujs_code *ujs_compile(ujs_vm *vm, const char *src, int len, char *err, size_t errn);
ujs_result ujs_run_frame(ujs_vm *vm, ujs_val *out);
ujs_result ujs_call_value(ujs_vm *vm, ujs_val fn, ujs_val self,
                          int argc, const ujs_val *argv, ujs_val *out, int is_new);

/* library setup */
void ujs_lib_init(ujs_vm *vm);

/* GC */
void ujs_gc_mark_val(ujs_vm *vm, ujs_val v);
void ujs_gc_maybe(ujs_vm *vm);

/* ---- math ----------------------------------------------------------------
 * unojs carries its own double-precision math (ujs_math.c) rather than linking
 * a host libm: pc64's is float-only, and the number formatter needs double
 * exactness. Same code on the host tests and on metal - see ujs_math.c.
 *
 * The declarations moved to `ujs_math.h`, which is PUBLIC and [STABLE]: the
 * quickjs port needs real doubles too and was declaring them by hand against
 * this internal header. Keep the surface there, not here. */
#include "ujs_math.h"

/* the exception helper the VM and library share */
ujs_val ujs_throwf(ujs_vm *vm, const char *kind, const char *fmt, ...);

#endif /* UJS_INT_H */
