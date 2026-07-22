/* ===========================================================================
 * UnoDOS/pc64 - UNOAUTOMATE core, Stage 1 (see unoauto.h for the roadmap).
 *
 * What is real today:
 *   LOG   channels + sinks.  Formatting happens once here; the legacy
 *         destinations (kernel ring via uno_dbg_log, NETLOG via the old
 *         net-trace path) are pre-registered sinks, so output is byte-
 *         identical to the pre-seam harness while every new consumer
 *         attaches through unoauto_sink_add instead of a new global.
 *   TEST  the registration table + runner skeleton.  SPECTEST suites
 *         migrate one register call at a time; until then both paths run.
 *
 * PROBE/HOOK/DRIVE are declared (unoauto.h) and stubbed here so callers can
 * wire up against the final API from day one; they grow in Stage 2 files
 * (unoauto_probe.c, unoauto_py.c) - NOT in this one, which stays small.
 * ======================================================================== */
#ifdef UNO_DEBUG

#include "unoauto.h"

int  vsnprintf(char *buf, unsigned long cap, const char *fmt, __builtin_va_list ap);
int  snprintf(char *buf, unsigned long cap, const char *fmt, ...);
void uno_dbg_log(const char *fmt, ...);
void uno_dbg_net_trace(const char *fmt, ...);

#define UA_LINE_MAX   256
#define UA_SINK_MAX   8
#define UA_TEST_MAX   128
#define UA_HOOK_MAX   16

/* ---- LOG ---------------------------------------------------------------- */
typedef struct { unsigned mask; UnoAutoSinkFn fn; void *user; } UaSink;
static UaSink ua_sinks[UA_SINK_MAX];

/* Legacy bridge sinks: keep the old harness outputs exactly as they were.
 * KERNEL/UI/STORAGE/TEST -> the kernel log ring; NET -> the NETLOG trace
 * (which also feeds the watchdog heartbeat - see uno_debug.h on why). */
static void ua_sink_kring(UnoAutoChan ch, const char *line, void *user)
{
    (void)ch; (void)user;
    uno_dbg_log("%s", line);
}
static void ua_sink_netlog(UnoAutoChan ch, const char *line, void *user)
{
    (void)ch; (void)user;
    uno_dbg_net_trace("%s", line);
}

static int ua_inited;
static void ua_init(void)
{
    if (ua_inited) return;
    ua_inited = 1;
    ua_sinks[0].mask = (1u << UA_CH_KERNEL) | (1u << UA_CH_UI) |
                       (1u << UA_CH_STORAGE) | (1u << UA_CH_TEST) |
                       (1u << UA_CH_SCRIPT);
    ua_sinks[0].fn   = ua_sink_kring;
    ua_sinks[1].mask = (1u << UA_CH_NET);
    ua_sinks[1].fn   = ua_sink_netlog;
}

void unoauto_vlog(UnoAutoChan ch, const char *fmt, __builtin_va_list ap)
{
    char line[UA_LINE_MAX];
    int i;
    if ((unsigned)ch >= UA_CH_COUNT) ch = UA_CH_KERNEL;
    ua_init();
    vsnprintf(line, sizeof line, fmt, ap);
    for (i = 0; i < UA_SINK_MAX; i++)
        if (ua_sinks[i].fn && (ua_sinks[i].mask & (1u << ch)))
            ua_sinks[i].fn(ch, line, ua_sinks[i].user);
}

void unoauto_log(UnoAutoChan ch, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    unoauto_vlog(ch, fmt, ap);
    __builtin_va_end(ap);
}

int unoauto_sink_add(unsigned mask, UnoAutoSinkFn fn, void *user)
{
    int i;
    ua_init();
    for (i = 0; i < UA_SINK_MAX; i++)
        if (!ua_sinks[i].fn) {
            ua_sinks[i].mask = mask; ua_sinks[i].fn = fn; ua_sinks[i].user = user;
            return i;
        }
    return -1;
}

void unoauto_sink_remove(int id)
{
    if (id >= 0 && id < UA_SINK_MAX) ua_sinks[id].fn = 0;
}

/* ---- TEST --------------------------------------------------------------- */
typedef struct { const char *suite, *id; UnoAutoTestFn fn; } UaTest;
static UaTest ua_tests[UA_TEST_MAX];
static int    ua_ntests;

static int ua_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int unoauto_test_register(const char *suite, const char *id, UnoAutoTestFn fn)
{
    if (ua_ntests >= UA_TEST_MAX) return -1;
    ua_tests[ua_ntests].suite = suite;
    ua_tests[ua_ntests].id    = id;
    ua_tests[ua_ntests].fn    = fn;
    return ua_ntests++;
}

int unoauto_test_run(const char *suite, char *report, int cap)
{
    int i, pass = 0, fail = 0, off = 0;
    for (i = 0; i < ua_ntests; i++) {
        int rc;
        if (suite && !ua_streq(suite, ua_tests[i].suite)) continue;
        rc = ua_tests[i].fn(0);
        if (rc == 0) pass++; else fail++;
        unoauto_log(UA_CH_TEST, "%s %s.%s (%d)",
                    rc == 0 ? "PASS" : "FAIL",
                    ua_tests[i].suite, ua_tests[i].id, rc);
        if (report && off < cap)
            off += snprintf(report + off, (unsigned long)(cap - off),
                            "%s %s.%s\n", rc == 0 ? "PASS" : "FAIL",
                            ua_tests[i].suite, ua_tests[i].id);
    }
    unoauto_log(UA_CH_TEST, "unoauto: %d pass %d fail", pass, fail);
    return fail;
}

/* ---- PROBE / HOOK / DRIVE: Stage 2 stubs -------------------------------- */
int unoauto_probe(UnoAutoProbeEnt *out, int max)
{
    (void)out; (void)max;
    return 0;                       /* nothing enumerable yet */
}

typedef struct { const char *point; UnoAutoHookFn fn; void *user; } UaHook;
static UaHook ua_hooks[UA_HOOK_MAX];

int unoauto_hook_add(const char *point, UnoAutoHookFn fn, void *user)
{
    int i;
    for (i = 0; i < UA_HOOK_MAX; i++)
        if (!ua_hooks[i].fn) {
            ua_hooks[i].point = point; ua_hooks[i].fn = fn; ua_hooks[i].user = user;
            return i;
        }
    return -1;
}

void unoauto_hook_remove(int id)
{
    if (id >= 0 && id < UA_HOOK_MAX) ua_hooks[id].fn = 0;
}

void unoauto_hook_fire(const char *point, void *arg)
{
    int i;
    for (i = 0; i < UA_HOOK_MAX; i++)
        if (ua_hooks[i].fn && ua_streq(ua_hooks[i].point, point))
            ua_hooks[i].fn(point, arg, ua_hooks[i].user);
}

int unoauto_drive_ready(void)
{
    return 0;                       /* PYRT binding lands in unoauto_py.c */
}

#endif /* UNO_DEBUG */
