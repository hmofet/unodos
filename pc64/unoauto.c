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
void uno_dbg_check(const char *tag);
unsigned long long uno_dbg_uptime_ms(void);
/* Transitional (Stage 1): the NETLOG persistence sink still lives in
 * pc64_nettest.c (it owns the CRASH\NETLOG.TXT buffer, the watchdog feed and
 * the on-screen ticker).  ua_init asks it to self-register so a direct
 * unoauto_log(UA_CH_NET, ...) is never dropped before the first legacy
 * uno_dbg_net_trace call.  Stage 3 moves persistence into a generic file
 * sink here and this extern disappears. */
void pc64_netlog_sink_ensure(void);

#define UA_LINE_MAX   256
#define UA_SINK_MAX   8
#define UA_TEST_MAX   128
#define UA_HOOK_MAX   16

/* ---- LOG ---------------------------------------------------------------- */
typedef struct { unsigned mask; UnoAutoSinkFn fn; void *user; } UaSink;
static UaSink ua_sinks[UA_SINK_MAX];

/* The kernel-ring sink: every channel mirrors into the ring, exactly like
 * the old harness (uno_dbg_net_trace's first act was a ring mirror).  NET's
 * extra behaviors - watchdog feed, on-screen ticker, the timestamped
 * CRASH\NETLOG.TXT buffer - are the netlog sink, which pc64_nettest.c
 * registers in slot 1 so sink ORDER matches the old call order. */
static void ua_sink_kring(UnoAutoChan ch, const char *line, void *user)
{
    (void)ch; (void)user;
    uno_dbg_log("%s", line);
}

static int ua_inited;
static void ua_init(void)
{
    if (ua_inited) return;
    ua_inited = 1;                        /* before ensure(): it re-enters us */
    ua_sinks[0].mask = (1u << UA_CH_COUNT) - 1u;   /* ring mirror: all channels */
    ua_sinks[0].fn   = ua_sink_kring;
    pc64_netlog_sink_ensure();            /* transitional - see the extern above */
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

/* wall-clock budget (UNOAUTOMATE-REQUESTS 2026-07-22): per-test deadline
 * armed by the runner, pollable by any cooperative wait loop. */
static unsigned           ua_budget_ms;      /* 0 = off                       */
static unsigned long long ua_deadline_at;    /* uptime at which the running
                                                test is over budget; 0 = none */

void unoauto_test_deadline_ms(unsigned ms) { ua_budget_ms = ms; }

long unoauto_deadline_left_ms(void)
{
    unsigned long long now;
    if (!ua_deadline_at) return -1;
    now = uno_dbg_uptime_ms();
    return now >= ua_deadline_at ? 0 : (long)(ua_deadline_at - now);
}
/* alias: .UNO import names cap at 23 chars and _left_ms is 24 */
long unoauto_deadline_left(void) { return unoauto_deadline_left_ms(); }

int unoauto_test_run(const char *suite, void *ctx, char *report, int cap)
{
    int i, pass = 0, fail = 0, off = 0;
    for (i = 0; i < ua_ntests; i++) {
        int rc, over;
        unsigned long long t0;
        if (suite && !ua_streq(suite, ua_tests[i].suite)) continue;
        uno_dbg_check(ua_tests[i].id);   /* a hang report names the suite */
        t0 = uno_dbg_uptime_ms();
        ua_deadline_at = ua_budget_ms ? t0 + ua_budget_ms : 0;
        rc = ua_tests[i].fn(ctx);
        over = ua_budget_ms &&
               (uno_dbg_uptime_ms() - t0) > (unsigned long long)ua_budget_ms;
        ua_deadline_at = 0;
        if (over) {                      /* slow = failed, run continues */
            if (rc == 0) rc = -1;
            unoauto_log(UA_CH_TEST, "FAIL %s.%s OVERRAN %lu ms (budget %u ms)",
                        ua_tests[i].suite, ua_tests[i].id,
                        (unsigned long)(uno_dbg_uptime_ms() - t0), ua_budget_ms);
        }
        if (rc == 0) { pass++; unoauto_log(UA_CH_TEST, "PASS %s.%s",
                                          ua_tests[i].suite, ua_tests[i].id); }
        else         { fail++; if (!over)
                       unoauto_log(UA_CH_TEST, "FAIL %s.%s (%d checks)",
                                          ua_tests[i].suite, ua_tests[i].id, rc); }
        if (report && off < cap)
            off += snprintf(report + off, (unsigned long)(cap - off),
                            "%s %s.%s\n", rc == 0 ? "PASS" : "FAIL",
                            ua_tests[i].suite, ua_tests[i].id);
    }
    unoauto_log(UA_CH_TEST, "unoauto: suite %s - %d pass %d fail",
                suite ? suite : "(all)", pass, fail);
    return fail;
}

/* ---- HOOK / DRIVE: Stage 2 stubs (PROBE lives in unoauto_probe.c) ------- */
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
