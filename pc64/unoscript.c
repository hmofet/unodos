/* ===========================================================================
 * unoscript.c - the scripting runtime: capability model, the privilege guard,
 * a weak fail-closed `unosecure` fallback, and the surface stubs.
 *
 * STATUS: STUB, pending `unosecure`.  Everything here is structurally complete
 * but the privileged surfaces are not wired to their subsystems yet, and the
 * guard denies tier>=1 by default because no real adjudicator is linked.  When
 * `unosecure` provides strong `unosec_*` definitions, the weak ones below drop
 * out and the guard starts making real decisions - no change here required.
 *
 * NOT in build.sh yet (see UNOSCRIPT.md "Build wiring - deferred").
 * ======================================================================== */
#include "unoscript.h"

/* unoauto LOG channel for audit routing when unosecure is absent (optional). */
#ifdef UNO_DEBUG
#include "unoauto.h"
#endif

/* ---------------------------------------------------------------------------
 * Capability -> tier + name table.  Single source of truth; keep aligned with
 * the usc_cap_t enum order.
 * ------------------------------------------------------------------------- */
static const struct { usc_tier_t tier; const char *name; } CAPS[USC_CAP__COUNT] = {
    [USC_CAP_NONE]            = { USC_TIER_AMBIENT, "none" },
    [USC_CAP_UI_INPUT]        = { USC_TIER_AMBIENT, "ui.input" },
    [USC_CAP_UI_READ]         = { USC_TIER_AMBIENT, "ui.read" },
    [USC_CAP_APP_CTRL]        = { USC_TIER_AMBIENT, "app.ctrl" },
    [USC_CAP_CLOCK]           = { USC_TIER_AMBIENT, "clock" },
    [USC_CAP_FS_USER]         = { USC_TIER_USER,    "fs.user" },
    [USC_CAP_SETTINGS]        = { USC_TIER_USER,    "settings" },
    [USC_CAP_AUTOMATION]      = { USC_TIER_USER,    "automation" },
    [USC_CAP_APP_MSG]         = { USC_TIER_USER,    "app.msg" },
    [USC_CAP_CLIPBOARD_WRITE] = { USC_TIER_USER,    "clipboard.write" },
    [USC_CAP_PROC_ENUM]       = { USC_TIER_ADMIN,   "proc.enum" },
    [USC_CAP_PROC_INSPECT]    = { USC_TIER_ADMIN,   "proc.inspect" },
    [USC_CAP_HOOK]            = { USC_TIER_ADMIN,   "hook" },
    [USC_CAP_LOG_SYS]         = { USC_TIER_ADMIN,   "log.sys" },
    [USC_CAP_FS_SYS]          = { USC_TIER_ADMIN,   "fs.sys" },
    [USC_CAP_IO_READ]         = { USC_TIER_ADMIN,   "io.read" },
    [USC_CAP_POWER]           = { USC_TIER_ADMIN,   "power" },
    [USC_CAP_MEM_READ]        = { USC_TIER_KERNEL,  "mem.read" },
    [USC_CAP_MEM_WRITE]       = { USC_TIER_KERNEL,  "mem.write" },
    [USC_CAP_IO_WRITE]        = { USC_TIER_KERNEL,  "io.write" },
    [USC_CAP_SYSCALL]         = { USC_TIER_KERNEL,  "syscall" },
    [USC_CAP_MODULE]          = { USC_TIER_KERNEL,  "module" },
};

usc_tier_t  unoscript_cap_tier(usc_cap_t c)
{ return (c >= 0 && c < USC_CAP__COUNT) ? CAPS[c].tier : USC_TIER_KERNEL; }
const char *unoscript_cap_name(usc_cap_t c)
{ return (c >= 0 && c < USC_CAP__COUNT) ? CAPS[c].name : "?"; }

/* ---------------------------------------------------------------------------
 * Weak fail-closed `unosecure` fallback.
 *
 * These `__attribute__((weak))` definitions link ONLY when the real unosecure
 * subsystem has not provided strong ones.  Policy while unsecured:
 *   - tier 0 (AMBIENT) is allowed: a script is no stronger than the user.
 *   - tier >= 1 is DENIED: with no accounts/RBAC/escalation, we cannot safely
 *     grant user- or admin-level authority.  Fail closed.
 * This lets UI/app scripting work on day one while everything deeper stays shut
 * until unosecure adjudicates it.
 * ------------------------------------------------------------------------- */
__attribute__((weak)) usc_uid_t unosec_current_user(void) { return 0; }

__attribute__((weak)) int unosec_check(usc_uid_t u, usc_cap_t cap)
{ (void)u; return unoscript_cap_tier(cap) == USC_TIER_AMBIENT; }

__attribute__((weak)) int unosec_require(usc_cap_t cap)
{ return unoscript_cap_tier(cap) == USC_TIER_AMBIENT; }

__attribute__((weak)) int unosec_request(usc_cap_t cap, usc_scope_t scope, int ttl_ms)
{ (void)cap; (void)scope; (void)ttl_ms; return 0; }   /* no adjudicator -> deny */

__attribute__((weak)) void unosec_drop(int grant) { (void)grant; }

__attribute__((weak)) void unosec_audit(usc_cap_t cap, const char *detail, int allowed)
{
#ifdef UNO_DEBUG
    unoauto_log(UA_CH_SCRIPT, "audit %s %s -> %s",
                unoscript_cap_name(cap), detail ? detail : "",
                allowed ? "ALLOW" : "DENY");
#else
    (void)cap; (void)detail; (void)allowed;
#endif
}

__attribute__((weak)) int unosec_present(void) { return 0; }

/* ---------------------------------------------------------------------------
 * The guard.  Every surface op calls this before touching anything.
 * ------------------------------------------------------------------------- */
int unoscript_guard(usc_cap_t cap, const char *what)
{
    int ok = unosec_require(cap);

    /* tier>=2 caps that are not statically held may prompt for a one-shot
     * escalation - but only when a real adjudicator is present.  Absent
     * unosecure, unosec_request() denies, so this is a no-op until it lands. */
    if (!ok && unoscript_cap_tier(cap) >= USC_TIER_ADMIN) {
        int g = unosec_request(cap, USC_SCOPE_ONCE, 0);
        if (g > 0) { ok = 1; unosec_drop(g); }
    }

    if (unoscript_cap_tier(cap) >= USC_TIER_ADMIN || !ok)
        unosec_audit(cap, what, ok);
    return ok;
}

/* Guard helper: deny with the right code depending on why. */
static int denied(usc_cap_t cap)
{ return unosec_present() ? USC_EDENIED : USC_ENOSEC; (void)cap; }

/* ===========================================================================
 * Surface stubs.
 *
 * Pattern for every op: guard the capability, then delegate to the owning
 * subsystem.  The delegation targets marked TODO(<subsystem>) are accessors
 * unoscript has REQUESTED from that subsystem's owner (see UNOAUTOMATE-REQUESTS
 * / the subsystem's own request file) - unoscript does not implement them.
 * ======================================================================== */

/* -- ui (unoui) --------------------------------------------------------- */
int usc_ui_pointer(int x, int y, int btn)
{
    if (!unoscript_guard(USC_CAP_UI_INPUT, "ui.pointer")) return denied(USC_CAP_UI_INPUT);
    /* TODO(unoui): route to the shared synthetic-input entry (today the debug
     * uno_pc64_inject_pointer; a production seam is requested from unoui). */
    (void)x; (void)y; (void)btn;
    return USC_EUNAVAIL;
}
int usc_ui_key(int scan, int uni, int mods)
{
    if (!unoscript_guard(USC_CAP_UI_INPUT, "ui.key")) return denied(USC_CAP_UI_INPUT);
    (void)scan; (void)uni; (void)mods;
    return USC_EUNAVAIL;   /* TODO(unoui) */
}
int usc_ui_screen_text(char *out, int cap)
{
    if (!unoscript_guard(USC_CAP_UI_READ, "ui.screen_text")) return denied(USC_CAP_UI_READ);
    if (out && cap > 0) out[0] = 0;
    return USC_EUNAVAIL;   /* TODO(unoui): accessibility text of the window tree */
}
int usc_ui_clipboard_get(char *out, int cap)
{
    if (!unoscript_guard(USC_CAP_UI_READ, "ui.clip_get")) return denied(USC_CAP_UI_READ);
    if (out && cap > 0) out[0] = 0;
    return USC_EUNAVAIL;   /* TODO(unoui) */
}
int usc_ui_clipboard_set(const char *s)
{
    if (!unoscript_guard(USC_CAP_CLIPBOARD_WRITE, "ui.clip_set")) return denied(USC_CAP_CLIPBOARD_WRITE);
    (void)s;
    return USC_EUNAVAIL;   /* TODO(unoui) */
}

/* -- app (shell) -------------------------------------------------------- */
int usc_app_count(void)
{
    if (!unoscript_guard(USC_CAP_APP_CTRL, "app.count")) return denied(USC_CAP_APP_CTRL);
    return USC_EUNAVAIL;   /* TODO(shell): production app enumeration */
}
int usc_app_launch(int idx)
{
    if (!unoscript_guard(USC_CAP_APP_CTRL, "app.launch")) return denied(USC_CAP_APP_CTRL);
    (void)idx; return USC_EUNAVAIL;   /* TODO(shell) */
}
int usc_app_close_top(void)
{
    if (!unoscript_guard(USC_CAP_APP_CTRL, "app.close_top")) return denied(USC_CAP_APP_CTRL);
    return USC_EUNAVAIL;   /* TODO(shell) */
}
int usc_app_message(int idx, const char *json, char *reply, int cap)
{
    if (!unoscript_guard(USC_CAP_APP_MSG, "app.message")) return denied(USC_CAP_APP_MSG);
    (void)idx; (void)json; if (reply && cap > 0) reply[0] = 0;
    return USC_EUNAVAIL;   /* TODO(shell): structured app IPC */
}

/* -- fs (unofs) --------------------------------------------------------- */
int usc_fs_read(const char *path, void *buf, int cap)
{
    /* USER scope vs SYS scope is decided by unosecure from the path; the guard
     * here is the floor (FS_USER).  A SYS-scoped path re-guards FS_SYS inside
     * the unofs seam once that accessor exists. */
    if (!unoscript_guard(USC_CAP_FS_USER, "fs.read")) return denied(USC_CAP_FS_USER);
    (void)path; (void)buf; (void)cap;
    return USC_EUNAVAIL;   /* TODO(unofs): user-scoped read seam */
}
int usc_fs_write(const char *path, const void *buf, int len)
{
    if (!unoscript_guard(USC_CAP_FS_USER, "fs.write")) return denied(USC_CAP_FS_USER);
    (void)path; (void)buf; (void)len;
    return USC_EUNAVAIL;   /* TODO(unofs) */
}

/* -- proc (unosched) ---------------------------------------------------- */
int usc_proc_list(usc_proc_ent *out, int max)
{
    if (!unoscript_guard(USC_CAP_PROC_ENUM, "proc.list")) return denied(USC_CAP_PROC_ENUM);
    (void)out; (void)max;
    return USC_EUNAVAIL;   /* TODO(unosched): task/thread enumeration seam */
}
int usc_proc_inspect(int pid, usc_proc_ent *out)
{
    if (!unoscript_guard(USC_CAP_PROC_INSPECT, "proc.inspect")) return denied(USC_CAP_PROC_INSPECT);
    (void)pid; (void)out;
    return USC_EUNAVAIL;   /* TODO(unosched) */
}

/* -- hook (unoauto HOOK / subsystem taps) ------------------------------- */
int usc_hook_add(const char *point)
{
    if (!unoscript_guard(USC_CAP_HOOK, "hook.add")) return denied(USC_CAP_HOOK);
    (void)point;
    return USC_EUNAVAIL;   /* TODO: production-visible tap registry over unoauto_hook_* */
}
void usc_hook_remove(int id) { (void)id; }

/* -- mem (kernel) ------------------------------------------------------- */
int usc_mem_read(int pid, unsigned long long addr, void *buf, int len)
{
    if (!unoscript_guard(USC_CAP_MEM_READ, "mem.read")) return denied(USC_CAP_MEM_READ);
    (void)pid; (void)addr; (void)buf; (void)len;
    return USC_EUNAVAIL;   /* TODO(kernel): guarded cross-AS peek */
}
int usc_mem_write(int pid, unsigned long long addr, const void *buf, int len)
{
    if (!unoscript_guard(USC_CAP_MEM_WRITE, "mem.write")) return denied(USC_CAP_MEM_WRITE);
    (void)pid; (void)addr; (void)buf; (void)len;
    return USC_EUNAVAIL;   /* TODO(kernel): guarded cross-AS poke */
}

/* -- io (kernel) -------------------------------------------------------- */
int usc_io_in(unsigned port, int width, unsigned *val)
{
    if (!unoscript_guard(USC_CAP_IO_READ, "io.in")) return denied(USC_CAP_IO_READ);
    (void)port; (void)width; if (val) *val = 0;
    return USC_EUNAVAIL;   /* TODO(kernel) */
}
int usc_io_out(unsigned port, int width, unsigned val)
{
    if (!unoscript_guard(USC_CAP_IO_WRITE, "io.out")) return denied(USC_CAP_IO_WRITE);
    (void)port; (void)width; (void)val;
    return USC_EUNAVAIL;   /* TODO(kernel) */
}

/* -- power -------------------------------------------------------------- */
int usc_power(int action)
{
    if (!unoscript_guard(USC_CAP_POWER, "power")) return denied(USC_CAP_POWER);
    (void)action;
    return USC_EUNAVAIL;   /* TODO(kernel): reboot/shutdown/suspend seam */
}

/* ---- Lifecycle -------------------------------------------------------- */
void unoscript_boot(void) { /* nothing to bring up in the stub */ }
int  unoscript_available(void) { return 1; }
