/* ===========================================================================
 * unoscript - the UnoDOS system scripting & automation surface (Python).
 *
 * The production, always-on story: "every surface of the OS is scriptable from
 * Python."  Where `unoauto` is the DEBUG-only test/automation harness (it
 * compiles away when UNO_DEBUG=0), `unoscript` ships in PRODUCTION and is
 * reachable by any script the user runs.  Because it is always on, its gate is
 * not a compile flag - it is PRIVILEGE.  Think Automator + AppleScript, but the
 * scriptable surface is the whole OS and every deep operation is capability-
 * gated.
 *
 * OWNERSHIP (per the 2026-07-22 re-home discipline): unoscript owns ONLY the
 * Python bindings, the capability/tier model, and the seam into `unosecure`.
 * It does NOT own the surfaces it scripts - those belong to their subsystems
 * (unoui, unofs, unosched, the kernel) and unoscript reaches them through their
 * public APIs.  It does NOT own identity/RBAC/escalation - that is `unosecure`.
 *
 * STATUS: STUB, pending `unosecure`.  The capability model and the whole API
 * shape are here; the privileged surfaces return USC_EDENIED through a weak,
 * fail-closed `unosec_*` fallback (see unoscript.c) until the real unosecure
 * subsystem provides the strong definitions.  Not yet wired into build.sh.
 * See UNOSCRIPT.md (design) and UNOSECURE-SPEC.md (the dependency's contract).
 * ======================================================================== */
#ifndef UNOSCRIPT_H
#define UNOSCRIPT_H

#include <stddef.h>

#define UNOSCRIPT_API 0            /* pre-1: shape may still move before unosecure */

/* ---- Return codes ---------------------------------------------------------
 * Every surface call returns >=0 on success (often a count/length) or one of
 * these negatives.  The Python layer turns USC_EDENIED into a PermissionError
 * and USC_EUNAVAIL into "unsupported" so scripts can branch cleanly. */
#define USC_OK          0
#define USC_EDENIED   (-1)         /* capability not held / escalation refused  */
#define USC_EUNAVAIL  (-2)         /* surface not present in this build/port    */
#define USC_EINVAL    (-3)         /* bad argument                              */
#define USC_ENOSEC    (-4)         /* unosecure absent - cannot adjudicate      */

/* ---- Access tiers ---------------------------------------------------------
 * Coarse bands, purely descriptive: each capability lives in exactly one tier.
 * The tier says how a grant is normally obtained; unosecure has the final say.
 *
 *   0 AMBIENT  interactive-equivalent - a script doing this is no more powerful
 *              than the user sitting at the machine.  No grant needed beyond a
 *              logged-in user; the sandbox tier can still be denied it.
 *   1 USER     scoped to the acting user's account.  Default-granted to an
 *              interactive user, denied to untrusted/sandboxed scripts.
 *   2 ADMIN    system-wide observe/control.  Requires an EXPLICIT escalation
 *              (a consent prompt or an RBAC role) - the sudo/UAC analog.
 *   3 KERNEL   raw memory / port I/O / syscall / code patching.  The strongest
 *              escalation: interactive confirmation and/or a signed script,
 *              always audited.  This is where you can hurt the machine. */
typedef enum {
    USC_TIER_AMBIENT = 0,
    USC_TIER_USER    = 1,
    USC_TIER_ADMIN   = 2,
    USC_TIER_KERNEL  = 3,
} usc_tier_t;

/* ---- Capabilities ---------------------------------------------------------
 * The fine-grained permission each scripted operation requires.  unosecure
 * maps users (via roles) to sets of these, and adjudicates escalation for the
 * ones a user does not statically hold.  Keep this list append-only; it is the
 * vocabulary unosecure is written against. */
typedef enum {
    USC_CAP_NONE = 0,

    /* -- Tier 0: AMBIENT -- */
    USC_CAP_UI_INPUT,        /* inject pointer/keyboard events                  */
    USC_CAP_UI_READ,         /* read screen, window tree, clipboard             */
    USC_CAP_APP_CTRL,        /* launch / focus / close ordinary apps            */
    USC_CAP_CLOCK,           /* read uptime / wall clock                        */

    /* -- Tier 1: USER -- */
    USC_CAP_FS_USER,         /* read/write files within the user's scope (unofs)*/
    USC_CAP_SETTINGS,        /* read/write the user's settings                  */
    USC_CAP_AUTOMATION,      /* register persistent / background scripts        */
    USC_CAP_APP_MSG,         /* send structured messages to / read app state    */
    USC_CAP_CLIPBOARD_WRITE, /* place data on the clipboard                     */

    /* -- Tier 2: ADMIN (explicit escalation) -- */
    USC_CAP_PROC_ENUM,       /* enumerate ALL processes / threads               */
    USC_CAP_PROC_INSPECT,    /* read another process's state / registers (RO)   */
    USC_CAP_HOOK,            /* attach hooks/taps; intercept a call surface     */
    USC_CAP_LOG_SYS,         /* read system logs / all LOG channels             */
    USC_CAP_FS_SYS,          /* read/write outside the user's scope             */
    USC_CAP_IO_READ,         /* read I/O ports / MMIO / device registers        */
    USC_CAP_POWER,           /* reboot / shutdown / suspend                     */

    /* -- Tier 3: KERNEL (strongest escalation, audited) -- */
    USC_CAP_MEM_READ,        /* peek arbitrary / cross-process memory           */
    USC_CAP_MEM_WRITE,       /* poke arbitrary / cross-process memory           */
    USC_CAP_IO_WRITE,        /* write I/O ports / MMIO / device registers       */
    USC_CAP_SYSCALL,         /* intercept / emulate / trace syscalls            */
    USC_CAP_MODULE,          /* load unsigned modules / patch live code         */

    USC_CAP__COUNT
} usc_cap_t;

/* The static tier of a capability (for UIs and policy display). */
usc_tier_t  unoscript_cap_tier(usc_cap_t cap);
const char *unoscript_cap_name(usc_cap_t cap);   /* stable string, e.g. "mem.read" */

/* =====================================================================
 * The unosecure seam  (IMPLEMENTED BY THE `unosecure` SUBSYSTEM)
 * ---------------------------------------------------------------------
 * unoscript calls exactly these to answer "may this script do X now?".  They
 * are declared here but PROVIDED by unosecure.  Until it lands, unoscript.c
 * carries weak, fail-closed fallbacks (deny anything above tier 0, report
 * USC_ENOSEC).  When unosecure links its strong definitions in, the gate
 * upgrades transparently - the r8169 weak-fallback pattern.  The precise
 * contract these must satisfy is UNOSECURE-SPEC.md.
 * ===================================================================== */
typedef unsigned long usc_uid_t;         /* 0 = system/kernel identity          */

typedef enum {
    USC_SCOPE_ONCE = 0,      /* one operation                                   */
    USC_SCOPE_SESSION,       /* until the script/session ends                   */
    USC_SCOPE_TIMED,         /* ttl_ms milliseconds                             */
} usc_scope_t;

/* Identity of the script currently on the Python call stack. */
usc_uid_t unosec_current_user(void);

/* Static check: does user `u` hold `cap` by role, ignoring live escalations? */
int  unosec_check(usc_uid_t u, usc_cap_t cap);

/* Live check: may the CURRENT script use `cap` right now (static grant OR an
 * active escalation)?  This is what the guard calls on the hot path. */
int  unosec_require(usc_cap_t cap);

/* Raise the current script's authority for `cap` within `scope`.  unosecure
 * decides - interactive consent (drawn via unoui), a standing RBAC role, or a
 * signed-script policy.  Returns a grant handle >0, or 0 if denied. */
int  unosec_request(usc_cap_t cap, usc_scope_t scope, int ttl_ms);

/* Relinquish an escalation early (good scripts drop what they raised). */
void unosec_drop(int grant);

/* Audit sink: every tier>=2 attempt (allowed or not) is reported here so
 * unosecure can log/forward it.  Weak fallback routes to the unoauto LOG
 * channel when present, else no-op. */
void unosec_audit(usc_cap_t cap, const char *detail, int allowed);

/* True once unosecure's strong definitions are linked (weak fallback -> 0). */
int  unosec_present(void);

/* =====================================================================
 * The guard  (unoscript-owned)
 * ---------------------------------------------------------------------
 * Every surface function below funnels through this.  It resolves the cap to a
 * live decision via unosec_require(), auto-requesting a ONCE escalation for
 * tier>=2 caps if the policy allows a prompt, and audits the outcome.  Returns
 * 1 (proceed) or 0 (caller must return USC_EDENIED).  `what` is a short
 * human string for the audit line / consent prompt (e.g. "mem.read pid=7"). */
int  unoscript_guard(usc_cap_t cap, const char *what);

/* =====================================================================
 * Surface API  (unoscript-owned bindings -> subsystem public APIs)
 * ---------------------------------------------------------------------
 * Representative surface; the full catalog lives in UNOSCRIPT.md.  Each fn
 * guards its capability, then delegates to the owning subsystem.  These are the
 * C entry points the Python module (mod_unoscript.c) wraps 1:1.
 * ===================================================================== */

/* -- ui: AMBIENT (unoui) ------------------------------------------------ */
int  usc_ui_pointer(int x, int y, int btn);            /* USC_CAP_UI_INPUT   */
int  usc_ui_key(int scan, int uni, int mods);          /* USC_CAP_UI_INPUT   */
int  usc_ui_screen_text(char *out, int cap);           /* USC_CAP_UI_READ    */
int  usc_ui_clipboard_get(char *out, int cap);         /* USC_CAP_UI_READ    */
int  usc_ui_clipboard_set(const char *s);              /* USC_CAP_CLIPBOARD_WRITE */

/* -- app: AMBIENT/USER (shell) ------------------------------------------ */
int  usc_app_count(void);                              /* USC_CAP_APP_CTRL   */
int  usc_app_launch(int idx);                          /* USC_CAP_APP_CTRL   */
int  usc_app_close_top(void);                          /* USC_CAP_APP_CTRL   */
int  usc_app_message(int idx, const char *json,
                     char *reply, int cap);            /* USC_CAP_APP_MSG    */

/* -- fs: USER/ADMIN (unofs) --------------------------------------------- */
int  usc_fs_read(const char *path, void *buf, int cap);/* USC_CAP_FS_USER/SYS*/
int  usc_fs_write(const char *path, const void *buf, int len); /* ditto      */

/* -- proc: ADMIN (unosched) --------------------------------------------- */
typedef struct {
    usc_uid_t owner;
    int       pid, tid, state;
    const char *name;
    unsigned long long v1, v2;   /* sched detail (cpu ms / stack use)         */
} usc_proc_ent;
int  usc_proc_list(usc_proc_ent *out, int max);        /* USC_CAP_PROC_ENUM  */
int  usc_proc_inspect(int pid, usc_proc_ent *out);     /* USC_CAP_PROC_INSPECT */

/* -- hook: ADMIN (unoauto HOOK / subsystem taps) ------------------------ */
int  usc_hook_add(const char *point);                  /* USC_CAP_HOOK       */
void usc_hook_remove(int id);

/* -- mem: KERNEL (kernel) ----------------------------------------------- */
/* target uid 0 = kernel/physical view; else another process's address space. */
int  usc_mem_read(int pid, unsigned long long addr,
                  void *buf, int len);                 /* USC_CAP_MEM_READ   */
int  usc_mem_write(int pid, unsigned long long addr,
                   const void *buf, int len);          /* USC_CAP_MEM_WRITE  */

/* -- io: ADMIN/KERNEL (kernel) ------------------------------------------ */
int  usc_io_in(unsigned port, int width, unsigned *val);   /* USC_CAP_IO_READ  */
int  usc_io_out(unsigned port, int width, unsigned val);   /* USC_CAP_IO_WRITE */

/* -- power: ADMIN ------------------------------------------------------- */
int  usc_power(int action);   /* 0 shutdown 1 reboot 2 suspend  USC_CAP_POWER */

/* ---- Lifecycle -------------------------------------------------------- */
void unoscript_boot(void);    /* bring the runtime up (called from kernel init) */
int  unoscript_available(void); /* 1 when the runtime is up (always in prod)   */

#endif /* UNOSCRIPT_H */
