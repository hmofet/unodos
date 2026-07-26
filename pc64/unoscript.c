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
#include "unoscript_path.h"   /* pure fs path helpers (scheme + scope logic)   */
#include <string.h>

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

/* delegation targets now provided (production): synthetic input lives in the
 * platform (uefi_main.c); the screen-tree text + shell clipboard live in the
 * shell (pc64_uui.c).  unoscript only adds the capability gate on top. */
void uno_pc64_inject_key(int scan, int uni, int ctrl);
void uno_pc64_inject_pointer(int x, int y, int btn);
int  pc64_shell_screen_text(char *out, int cap);
int  pc64_shell_clip_set(const char *s);
int  pc64_shell_clip_get(char *out, int cap);

/* -- ui (unoui) --------------------------------------------------------- */
int usc_ui_pointer(int x, int y, int btn)
{
    if (!unoscript_guard(USC_CAP_UI_INPUT, "ui.pointer")) return denied(USC_CAP_UI_INPUT);
    uno_pc64_inject_pointer(x, y, btn);      /* same clamp+click path as real input */
    return USC_OK;
}
int usc_ui_key(int scan, int uni, int mods)
{
    if (!unoscript_guard(USC_CAP_UI_INPUT, "ui.key")) return denied(USC_CAP_UI_INPUT);
    uno_pc64_inject_key(scan, uni, mods);    /* same map_key path as real input     */
    return USC_OK;
}
int usc_ui_screen_text(char *out, int cap)
{
    if (!unoscript_guard(USC_CAP_UI_READ, "ui.screen_text")) return denied(USC_CAP_UI_READ);
    if (!out || cap <= 0) return USC_EINVAL;
    return pc64_shell_screen_text(out, cap);  /* window-tree text, focused marked */
}
int usc_ui_clipboard_get(char *out, int cap)
{
    if (!unoscript_guard(USC_CAP_UI_READ, "ui.clip_get")) return denied(USC_CAP_UI_READ);
    if (!out || cap <= 0) return USC_EINVAL;
    return pc64_shell_clip_get(out, cap);
}
int usc_ui_clipboard_set(const char *s)
{
    if (!unoscript_guard(USC_CAP_CLIPBOARD_WRITE, "ui.clip_set")) return denied(USC_CAP_CLIPBOARD_WRITE);
    return pc64_shell_clip_set(s) ? USC_OK : USC_EINVAL;
}

/* -- app (shell) -------------------------------------------------------- */
/* delegation targets: production shell accessors (pc64_uui.c). */
int  pc64_shell_app_count(void);
int  pc64_shell_launch(int a);
void pc64_shell_close_top(void);
int  pc64_shell_app_message(int idx, const char *msg, char *reply, int cap);
/* proc enumeration primitives (pc64_uui.c): a "process" is an open app slot. */
int  pc64_shell_app_open(int idx);
const char *pc64_shell_app_name(int idx);
int  pc64_shell_app_is_focused(int idx);

int usc_app_count(void)
{
    if (!unoscript_guard(USC_CAP_APP_CTRL, "app.count")) return denied(USC_CAP_APP_CTRL);
    return pc64_shell_app_count();
}
int usc_app_launch(int idx)
{
    if (!unoscript_guard(USC_CAP_APP_CTRL, "app.launch")) return denied(USC_CAP_APP_CTRL);
    return pc64_shell_launch(idx) ? USC_OK : USC_EINVAL;   /* bad idx / refused slot */
}
int usc_app_close_top(void)
{
    if (!unoscript_guard(USC_CAP_APP_CTRL, "app.close_top")) return denied(USC_CAP_APP_CTRL);
    pc64_shell_close_top();
    return USC_OK;
}
int usc_app_message(int idx, const char *json, char *reply, int cap)
{
    if (!unoscript_guard(USC_CAP_APP_MSG, "app.message")) return denied(USC_CAP_APP_MSG);
    if (!reply || cap <= 0) return USC_EINVAL;
    return pc64_shell_app_message(idx, json, reply, cap);  /* reply length >= 0 */
}

/* -- fs (unofs) --------------------------------------------------------- */
/* Consumed, not owned: the volume fs primitives (pc64_fs.c).  The user-scoped
 * surface is composed here from these + the acting identity, exactly as the
 * proc surface composes from the shell's app primitives.  Path scheme + scope
 * (decided 2026-07-25): a bare relative path is the acting user's home
 * ("USERS/<uid>/..." on the primary writable native-FAT volume, always fs.user);
 * an absolute "/label/rest" names a volume by label and is fs.sys unless it lands
 * back in that same home subtree.  "..", "." and "//" are rejected (see
 * unoscript_path.h), so a relative path can never leave its home. */
int         uno_fs_volumes(void);
const char *uno_fs_volume_name(int vol);
int         uno_fs_kind(int vol);                 /* 1 = native FAT (subdirs)  */
int         uno_fs_writable(int vol);
long        uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
int         uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
int         uno_fs_mkdir(int vol, const char *path);

#define FS_NAME_MAX 256

/* case-insensitive ASCII equality (volume-label match). */
static int fs_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
/* the primary user volume: first writable native-FAT vol (subdirs needed for
 * USERS/<uid>/), or -1 if the machine has none. */
static int fs_user_vol(void)
{
    int i, n = uno_fs_volumes();
    for (i = 0; i < n; i++)
        if (uno_fs_kind(i) == 1 && uno_fs_writable(i)) return i;
    return -1;
}
static int fs_vol_by_label(const char *lab)
{
    int i, n = uno_fs_volumes();
    for (i = 0; i < n; i++) {
        const char *nm = uno_fs_volume_name(i);
        if (nm && fs_ieq(nm, lab)) return i;
    }
    return -1;
}
/* resolve a surface path for the acting user into (vol, name, is_sys).  Pure of
 * side effects (only reads the volume table); returns USC_OK or a USC_E* code. */
static int fs_resolve(const char *path, char *name, int *vol, int *is_sys)
{
    unsigned long uid = unosec_current_user();
    if (!path || !path[0]) return USC_EINVAL;

    if (path[0] == '/') {                          /* absolute /label/rest      */
        char lab[32]; const char *rest; int v;
        if (uscp_split_abs(path, lab, (int)sizeof lab, &rest) != 0) return USC_EINVAL;
        if (uscp_has_traversal(rest)) return USC_EINVAL;
        if ((int)strlen(rest) >= FS_NAME_MAX) return USC_EINVAL;
        v = fs_vol_by_label(lab);
        if (v < 0) return USC_EINVAL;              /* unknown volume label      */
        strcpy(name, rest);
        *vol = v;
        /* fs.user only if this absolute path IS the acting user's own home. */
        *is_sys = !(v == fs_user_vol() && uscp_under_home(uid, rest));
        return USC_OK;
    }

    /* relative -> the acting user's home (always fs.user). */
    {
        int uv = fs_user_vol();
        if (uv < 0) return USC_EUNAVAIL;           /* no writable native-FAT vol */
        if (uscp_has_traversal(path)) return USC_EINVAL;
        if (uscp_home_name(uid, path, name, FS_NAME_MAX) < 0) return USC_EINVAL;
        *vol = uv;
        *is_sys = 0;
        return USC_OK;
    }
}
/* best-effort create every parent directory of root-relative `name`.  mkdir is
 * idempotent (0 when the dir already exists), so walking the prefixes is safe -
 * this is what provisions USERS/, USERS/<uid>/ and any script subdirs on first
 * write.  A no-op on RAM / firmware-SFS volumes (mkdir unsupported there). */
static void fs_mkparents(int vol, const char *name)
{
    char d[FS_NAME_MAX]; int i;
    for (i = 0; name[i] && i < FS_NAME_MAX - 1; i++)
        if (name[i] == '/' && i > 0) { memcpy(d, name, (size_t)i); d[i] = 0;
                                       uno_fs_mkdir(vol, d); }
}

int usc_fs_read(const char *path, void *buf, int cap)
{
    char name[FS_NAME_MAX]; int vol, is_sys, rc; long got;
    /* floor first: an unauthenticated / tier-0 caller is denied before we even
     * resolve, so path validity never leaks below the FS_USER gate. */
    if (!unoscript_guard(USC_CAP_FS_USER, "fs.read")) return denied(USC_CAP_FS_USER);
    if (!buf || cap <= 0) return USC_EINVAL;
    rc = fs_resolve(path, name, &vol, &is_sys);
    if (rc != USC_OK) return rc;
    if (is_sys && !unoscript_guard(USC_CAP_FS_SYS, "fs.read(sys)"))
        return denied(USC_CAP_FS_SYS);            /* outside home re-guards sys */
    got = uno_fs_read(vol, name, (unsigned char *)buf, cap);
    return got < 0 ? USC_EINVAL : (int)got;        /* bytes read, or missing    */
}
int usc_fs_write(const char *path, const void *buf, int len)
{
    char name[FS_NAME_MAX]; int vol, is_sys, rc;
    if (!unoscript_guard(USC_CAP_FS_USER, "fs.write")) return denied(USC_CAP_FS_USER);
    if (!buf || len < 0) return USC_EINVAL;
    rc = fs_resolve(path, name, &vol, &is_sys);
    if (rc != USC_OK) return rc;
    if (is_sys && !unoscript_guard(USC_CAP_FS_SYS, "fs.write(sys)"))
        return denied(USC_CAP_FS_SYS);
    fs_mkparents(vol, name);                        /* provision home / subdirs  */
    return uno_fs_write(vol, name, (const unsigned char *)buf, len) ? USC_OK : USC_EINVAL;
}

/* -- proc (shell run-set) ----------------------------------------------- */
/* pc64 has no preemptive scheduler (unosched is the concurrency-primitive lib,
 * not a run-queue), so the enumerable run-set is the shell's OPEN app slots -
 * the same set F11 / unoauto PROBE report.  Each open slot is one process:
 * pid = slot index (stable for a boot), tid = 0 (cooperative single thread),
 * state bit0 = focused, name = app title, owner = the acting identity.  v1/v2
 * (cpu-ms / stack) carry 0 here - per-app draw cost lives only in the UNO_DEBUG
 * profiler, so it is not part of the production surface. */
static void proc_fill(usc_proc_ent *e, int slot)
{
    e->owner = unosec_current_user();
    e->pid   = slot;
    e->tid   = 0;
    e->state = pc64_shell_app_is_focused(slot) ? 1 : 0;
    e->name  = pc64_shell_app_name(slot);
    e->v1 = e->v2 = 0;
}
int usc_proc_list(usc_proc_ent *out, int max)
{
    int i, total, n = 0;
    if (!unoscript_guard(USC_CAP_PROC_ENUM, "proc.list")) return denied(USC_CAP_PROC_ENUM);
    if (!out || max <= 0) return USC_EINVAL;
    total = pc64_shell_app_count();
    for (i = 0; i < total && n < max; i++)
        if (pc64_shell_app_open(i)) proc_fill(&out[n++], i);
    return n;   /* number of running processes written */
}
int usc_proc_inspect(int pid, usc_proc_ent *out)
{
    if (!unoscript_guard(USC_CAP_PROC_INSPECT, "proc.inspect")) return denied(USC_CAP_PROC_INSPECT);
    if (!out) return USC_EINVAL;
    if (pid < 0 || pid >= pc64_shell_app_count() || !pc64_shell_app_open(pid))
        return USC_EINVAL;   /* no such running process */
    proc_fill(out, pid);
    return USC_OK;
}

/* -- hook (unoauto tap registry) --------------------------------------- *
 * DECISION (step 6): the tap registry stays DEBUG-ONLY.  In production
 * usc_hook_add reports USC_EUNAVAIL - a deliberate non-goal, not a stub-in-
 * waiting.  The fire points include libc.malloc (pc64_libc.c), so a
 * script-visible tap there is a hot-path cost on EVERY allocation and reentrant
 * (the observer would allocate inside the allocator).  Production already
 * compiles the whole hook machinery away (unoauto.h no-op macros), matching that
 * intent.  In a UNO_DEBUG build we wire the real bounded, allocation-free
 * unoauto_hook registry with a LOG-emitting shim - a SAFE observability tap (no
 * Python callback runs in kernel/driver context): a debug script says
 * hook.add("fs.write") and sees "hook: fs.write" on the SCRIPT LOG channel over
 * URC.  We pass the STABLE literal to the registry (it stores the pointer, so a
 * transient Python string can't be used) - which also validates the point. */
#ifdef UNO_DEBUG
/* exactly the unoauto_hook_fire() sites in the tree; returns the stable literal
 * (registry keeps the pointer) or 0 for an unknown / untappable point. */
static const char *hook_known_point(const char *p)
{
    static const char *const PTS[] = { "fs.read", "fs.write", "libc.malloc",
                                       "mod.load", "mod.unload", "uui.action", 0 };
    int i;
    for (i = 0; PTS[i]; i++) if (!strcmp(PTS[i], p)) return PTS[i];
    return 0;
}
static void usc_hook_log_shim(const char *point, void *arg, void *user)
{ (void)arg; (void)user; unoauto_log(UA_CH_SCRIPT, "hook: %s", point ? point : "?"); }
#endif

int usc_hook_add(const char *point)
{
    if (!unoscript_guard(USC_CAP_HOOK, "hook.add")) return denied(USC_CAP_HOOK);
    if (!point || !point[0]) return USC_EINVAL;
#ifdef UNO_DEBUG
    {
        const char *lit = hook_known_point(point);
        int id;
        if (!lit) return USC_EINVAL;                 /* unknown fire point      */
        id = unoauto_hook_add(lit, usc_hook_log_shim, 0);
        return id >= 0 ? id : USC_EUNAVAIL;          /* table full -> unavail   */
    }
#else
    return USC_EUNAVAIL;   /* deliberate non-goal in production (see above)      */
#endif
}
void usc_hook_remove(int id)
{
#ifdef UNO_DEBUG
    if (id >= 0) unoauto_hook_remove(id);
#else
    (void)id;
#endif
}

/* -- mem (kernel) ------------------------------------------------------- *
 * pc64 is a single-address-space cooperative kernel: there is ONE flat address
 * space (identity-mapped, ring 0), so `pid` 0 = that space and any other pid is
 * USC_EINVAL (no per-task page tables to translate through).  The read/write is
 * a bounded memcpy on the raw address - there is no MMU protection to lean on,
 * which is exactly why this is KERNEL tier and always audited by unosecure.  A
 * NULL address or a non-positive length is refused; an unmapped address will
 * fault the machine, the same as any kernel-mode peek. */
int usc_mem_read(int pid, unsigned long long addr, void *buf, int len)
{
    if (!unoscript_guard(USC_CAP_MEM_READ, "mem.read")) return denied(USC_CAP_MEM_READ);
    if (pid != 0) return USC_EINVAL;            /* only the one address space  */
    if (!buf || len <= 0 || !addr) return USC_EINVAL;
    memcpy(buf, (const void *)(unsigned long)addr, (unsigned)len);
    return len;                                  /* bytes read                 */
}
int usc_mem_write(int pid, unsigned long long addr, const void *buf, int len)
{
    if (!unoscript_guard(USC_CAP_MEM_WRITE, "mem.write")) return denied(USC_CAP_MEM_WRITE);
    if (pid != 0) return USC_EINVAL;
    if (!buf || len <= 0 || !addr) return USC_EINVAL;
    memcpy((void *)(unsigned long)addr, buf, (unsigned)len);
    return len;                                  /* bytes written              */
}

/* -- io (kernel) -------------------------------------------------------- */
/* Raw x86 port I/O via the platform's uno_native_port_* (pc64_native.c).
 * `width` is in BYTES (1/2/4); anything else is USC_EINVAL. */
unsigned uno_native_port_in(unsigned port, int width);
void     uno_native_port_out(unsigned port, int width, unsigned val);

static int io_width_ok(int w) { return w == 1 || w == 2 || w == 4; }

int usc_io_in(unsigned port, int width, unsigned *val)
{
    if (!unoscript_guard(USC_CAP_IO_READ, "io.in")) return denied(USC_CAP_IO_READ);
    if (!val || !io_width_ok(width) || port > 0xFFFF) return USC_EINVAL;
    *val = uno_native_port_in(port, width);
    return USC_OK;
}
int usc_io_out(unsigned port, int width, unsigned val)
{
    if (!unoscript_guard(USC_CAP_IO_WRITE, "io.out")) return denied(USC_CAP_IO_WRITE);
    if (!io_width_ok(width) || port > 0xFFFF) return USC_EINVAL;
    uno_native_port_out(port, width, val);
    return USC_OK;
}

/* -- power -------------------------------------------------------------- */
/* Shutdown + reboot are production platform primitives; suspend (ACPI S3) is
 * not implemented on pc64, so it reports USC_EUNAVAIL honestly. */
void uno_pc64_shutdown(void);
void uno_native_reset(void);                     /* CF9 hard reset (no return) */

int usc_power(int action)
{
    if (!unoscript_guard(USC_CAP_POWER, "power")) return denied(USC_CAP_POWER);
    switch (action) {
    case 0:  uno_pc64_shutdown(); return USC_OK;   /* shutdown                 */
    case 1:  uno_native_reset();  return USC_OK;   /* reboot (does not return) */
    case 2:  return USC_EUNAVAIL;                  /* suspend: no S3 on pc64   */
    default: return USC_EINVAL;
    }
}

/* ===========================================================================
 * End-to-end AUTHENTICATED self-test (debug-only).
 * ---------------------------------------------------------------------------
 * The wired+gated QEMU gate proves every surface DENIES without a session.
 * This proves the POSITIVE path: with a REAL unosecure login + authority, the
 * surfaces return real data.  It runs entirely in C under a genuine session (no
 * eval-context binding tricks), driving the usc_* surfaces directly, so it is
 * the Python-layer counterpart to unosecure's -DUNO_SECTEST C gate:
 *
 *   - fs (tier 1): as a GUEST (no static fs.user) a read is DENIED; the guard
 *       never auto-requests a tier-1 cap, so it takes an explicit request, which
 *       an interactive user is granted -> then write + read-back ROUND-TRIP.
 *   - proc (tier 2/ADMIN): under a dev AUTOGRANT policy the guard's auto-request
 *       is granted -> proc.list returns the running apps (real rows).
 *   - io.read (tier 2/ADMIN): same, a non-destructive POST-port read returns a value.
 *   - mem.read (tier 3/KERNEL): stays DENIED even under AUTOGRANT - autogrant
 *       covers <=ADMIN only, so the tier boundary is enforced, not bypassed.
 *
 * It needs a FRESH unosecure store (bootstrap_admin must succeed) - which the
 * QEMU gate's throwaway disk provides; on a provisioned system it returns <0
 * (skip).  Best-effort cleanup: delete the tester, restore the policy, leave
 * every session.  Returns 0 on a full pass, else a bitmask of the failures. */
#ifdef UNO_DEBUG
#include "unosecure.h"

int  pc64_shell_launch(int a);
void pc64_consent_register(void);     /* restore the UI's consent provider */

/* A headless consent provider that always DENIES - installed for the duration of
 * the test so the KERNEL mem.read escalation is refused WITHOUT drawing the
 * interactive consent sheet (which would block a headless run forever).  Under
 * AUTOGRANT the ADMIN surfaces short-circuit before consent, so only the KERNEL
 * check reaches this. */
static usc_consent_t e2e_deny_consent(void *ctx, usc_uid_t uid, usc_trust_t trust,
                                      usc_cap_t cap, const char *cap_name,
                                      usc_tier_t tier, const char *detail)
{ (void)ctx; (void)uid; (void)trust; (void)cap; (void)cap_name; (void)tier;
  (void)detail; return UNOSEC_CONSENT_DENY; }

int unoscript_e2e_selftest(void)
{
    usc_uid_t admin, tester;
    usec_session_t as, ts;
    usc_policy_t saved_policy;
    int fails = 0, g = 0, i, n;
    unsigned char wr[32], rd[32];
    usc_proc_ent rows[8];

    for (i = 0; i < 32; i++) wr[i] = (unsigned char)(i * 5 + 1);

    /* 1. bootstrap a throwaway admin on a FRESH store (skip otherwise). */
    admin = unosec_bootstrap_admin("e2eadm", "e2eadmpw");
    if (!admin) return -1;                              /* provisioned store: skip   */
    as = unosec_login("e2eadm", "e2eadmpw", UNOSEC_TRUST_INTERACTIVE);
    if (!as || !unosec_enter_session(as)) return -2;
    saved_policy = unosec_policy_get();
    tester = unosec_account_create("e2eusr", "e2euserpw", "guest");
    unosec_policy_set(UNOSEC_POLICY_AUTOGRANT);         /* admin holds policy.edit   */
    unosec_leave();
    if (!tester) return -3;

    /* headless: no interactive consent sheet may block the run. */
    unosec_set_consent_provider(e2e_deny_consent, 0);

    /* 2. act as the GUEST tester (interactive trust). */
    ts = unosec_login("e2eusr", "e2euserpw", UNOSEC_TRUST_INTERACTIVE);
    if (!ts || !unosec_enter_session(ts)) { pc64_consent_register(); return -4; }

    /* --- fs: DENY (guest lacks fs.user; tier-1 is never auto-requested) --- */
    if (usc_fs_read("E2E.TXT", rd, (int)sizeof rd) != USC_EDENIED) fails |= 1 << 0;
    /* --- fs: an interactive user may request its own USER-tier authority --- */
    g = unosec_request(USC_CAP_FS_USER, USC_SCOPE_SESSION, 0);
    if (g <= 0) fails |= 1 << 1;
    /* --- fs: write then read-back ROUND-TRIP through the granted surface --- */
    if (usc_fs_write("E2E.TXT", wr, 32) != USC_OK) fails |= 1 << 2;
    n = usc_fs_read("E2E.TXT", rd, (int)sizeof rd);
    if (n != 32) fails |= 1 << 3;
    else for (i = 0; i < 32; i++) if (rd[i] != wr[i]) { fails |= 1 << 3; break; }

    /* --- proc: authorised (AUTOGRANT) -> real rows for the running apps --- */
    pc64_shell_launch(0);                              /* ensure >=1 app is open    */
    n = usc_proc_list(rows, 8);
    if (n < 1) fails |= 1 << 4;
    else if (!rows[0].name) fails |= 1 << 5;

    /* --- io.read: authorised (ADMIN) -> a non-destructive port read works --- */
    { unsigned v = 0; if (usc_io_in(0x80, 1, &v) != USC_OK) fails |= 1 << 6; }

    /* --- mem.read: KERNEL > AUTOGRANT's ADMIN ceiling -> STAYS denied --- *
     * (escalation reaches the consent provider, which denies headlessly). */
    if (usc_mem_read(0, 0x100000, rd, 4) != USC_EDENIED) fails |= 1 << 7;

    if (g > 0) unosec_drop(g);
    unosec_leave();                                     /* leave the tester          */

    /* 3. cleanup as admin: delete the tester, restore policy + the UI consent. */
    if (unosec_enter_session(as)) {
        unosec_account_delete(tester);
        unosec_policy_set(saved_policy);
        unosec_leave();
    }
    pc64_consent_register();                            /* restore interactive consent */
    return fails;                                       /* 0 == every check passed   */
}
#endif /* UNO_DEBUG */

/* ---- Lifecycle -------------------------------------------------------- */
void unoscript_boot(void) { /* nothing to bring up in the stub */ }
int  unoscript_available(void) { return 1; }
