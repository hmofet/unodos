/* ===========================================================================
 * unosecure - the UnoDOS security subsystem: identity, authentication,
 * authorization (RBAC), and privilege escalation.
 *
 * A first-class OS subsystem (peer of unofs / unonet / unosched): PRODUCTION,
 * always-on, fail-closed.  It owns *who* an actor is and *what* they may do; it
 * never owns the surfaces being protected.  unoscript (and, in time, login, the
 * installer, fs ACLs and the remote link) call in to ask "may this actor do X?"
 * and perform X themselves.
 *
 * This header carries two things:
 *   1. The unosec_* SEAM (declared in unoscript.h, the frozen contract) - we
 *      provide the STRONG definitions that transparently replace unoscript.c's
 *      weak fail-closed fallbacks (the r8169 weak-fallback pattern).
 *   2. unosecure's OWN surface (the unosec_* account/role/session/policy API)
 *      that login / the installer / a settings UI consume.
 *
 * Design notes for the seam and every policy decision live in UNOSECURE-SPEC.md
 * (the contract) and UNOSECURE.md (this implementation's design + storage/audit
 * formats + the thread->session binding).
 * ======================================================================== */
#ifndef PC64_UNOSECURE_H
#define PC64_UNOSECURE_H

#include "unoscript.h"   /* usc_uid_t, usc_cap_t, usc_scope_t, usc_tier_t, the
                            unosec_* seam prototypes we implement */

#define UNOSECURE_API 1

/* ---- reserved identities --------------------------------------------------
 * 0 is the system/kernel (all authority; never handed out as a login - the
 * first admin is a distinct account).  UID_NONE is the unauthenticated context
 * (no session bound): it holds nothing above AMBIENT, so a script with no login
 * is exactly as weak as the fail-closed floor unoscript already ships. */
#define UNOSEC_UID_SYSTEM  ((usc_uid_t)0)
#define UNOSEC_UID_NONE    ((usc_uid_t)~0UL)

/* ---- trust classes (see UNOSECURE-SPEC.md §5) -----------------------------
 * A script is "user U running code of trust class T"; policy keys off the
 * triple (uid, class, requested cap).  Higher-numbered = less trusted. */
typedef enum {
    UNOSEC_TRUST_INTERACTIVE = 0,  /* typed at a live prompt / shell            */
    UNOSEC_TRUST_INSTALLED   = 1,  /* a stored .py/app the user launched        */
    UNOSEC_TRUST_REMOTE      = 2,  /* arrived over unoauto_remote / the URC link*/
    UNOSEC_TRUST_SANDBOX     = 3,  /* untrusted / downloaded - most restricted  */
} usc_trust_t;

/* ---- sessions -------------------------------------------------------------
 * A session binds a uid + trust class + lifetime, and holds the live escalation
 * grants raised within it.  Handles are small opaque ints (index+generation, so
 * a stale handle never aliases a reused slot); 0 is invalid. */
typedef int usec_session_t;

/* ---- escalation-consent result (what a consent provider returns) ---------- */
typedef enum {
    UNOSEC_CONSENT_DENY    = 0,
    UNOSEC_CONSENT_ONCE    = 1,    /* allow this single operation               */
    UNOSEC_CONSENT_SESSION = 2,    /* allow until the session ends              */
} usc_consent_t;

/* A consent provider draws the escalation sheet (unosecure never touches UI -
 * it adjudicates, then asks a registered provider to prompt).  unoui/login
 * registers one; absent a provider, interactive consent fails closed (DENY).
 * The provider is handed everything the sheet in §6 needs to name. */
typedef usc_consent_t (*unosec_consent_fn)(void *ctx,
        usc_uid_t uid, usc_trust_t trust,
        usc_cap_t cap, const char *cap_name, usc_tier_t tier,
        const char *detail);

/* ---- escalation policy mode (see UNOSECURE-SPEC.md §6) ---------------------
 * The system-wide stance on prompting.  DENY = kiosk (refuse all escalation);
 * PROMPT = the normal machine (roles auto-grant, else draw a consent sheet);
 * AUTOGRANT = a developer/test machine (grant tier<=ADMIN escalations without a
 * sheet; KERNEL still needs consent or a signed manifest).  The QEMU gate flips
 * to AUTOGRANT (or registers an always-allow provider) as its "test policy". */
typedef enum {
    UNOSEC_POLICY_DENY      = 0,
    UNOSEC_POLICY_PROMPT    = 1,   /* default                                   */
    UNOSEC_POLICY_AUTOGRANT = 2,
} usc_policy_t;

/* =====================================================================
 * Lifecycle
 * ===================================================================== */

/* Bring the subsystem up: open the unofs-backed store, load accounts / roles /
 * policy / trust keys / the audit chain, seed the built-in roles, and (on a
 * fresh install with no accounts) leave the bootstrap path armed.  Idempotent;
 * safe to call before storage exists (degrades to an in-RAM store, still
 * fail-closed).  Returns 1 up, 0 on a fatal internal error (still fail-closed).
 * Called from kernel init, before unoscript_boot(). */
int  unosec_boot(void);

/* =====================================================================
 * Accounts  (SPEC §4).  Mutators require the caller's session to hold the
 * matching sec.* capability (sec.user.create / sec.user.delete / ...); they
 * fail closed (return <0 / 0) when it does not.
 * ===================================================================== */

/* Create an account: name + password (hashed, never stored plaintext) + an
 * initial role.  Returns the new uid (>=1), or 0 on failure (dup name / no
 * authority / store full). */
usc_uid_t unosec_account_create(const char *name, const char *password,
                                const char *role);
int  unosec_account_delete(usc_uid_t u);
int  unosec_account_list(usc_uid_t *out, int max);      /* count, or <0        */
const char *unosec_account_name(usc_uid_t u);           /* 0 if unknown        */
usc_uid_t   unosec_account_by_name(const char *name);   /* UID_NONE if unknown */
int  unosec_account_set_password(usc_uid_t u, const char *password);

/* First-run bootstrap: create the first admin ONLY when no account exists yet
 * (the installer calls this).  Refuses once any account is present.  Returns
 * the admin uid, or 0. */
usc_uid_t unosec_bootstrap_admin(const char *name, const char *password);

/* =====================================================================
 * RBAC  (SPEC §4 / §3).  Roles are named sets of capability *names* (opaque
 * strings, so tables survive additions to usc_cap_t and can hold unosecure's
 * own sec.* caps too).  Built-in roles seeded at boot: "admin", "user",
 * "guest".  Assignment requires sec.role.assign; role edits sec.role.edit.
 * ===================================================================== */
int  unosec_role_grant (usc_uid_t u, const char *role);
int  unosec_role_revoke(usc_uid_t u, const char *role);
int  unosec_role_add_cap(const char *role, const char *cap_name); /* define/extend */
int  unosec_role_exists(const char *role);

/* =====================================================================
 * Authentication & sessions  (SPEC §4)
 * ===================================================================== */

/* Verify (name,password) and, on success, open a session bound to that uid and
 * trust class.  Returns a session handle >0, or 0 on bad credentials / locked /
 * store error.  Every attempt (pass or fail) is audited. */
usec_session_t unosec_login(const char *name, const char *password,
                            usc_trust_t trust);

/* Open a session for an already-established identity WITHOUT a password - the
 * scheduler/unoscript path when a parent session spawns a child script under a
 * derived (possibly lower) trust class.  Requires the caller's current session
 * to already be `uid` (or system).  Returns a handle >0 or 0. */
usec_session_t unosec_session_open(usc_uid_t uid, usc_trust_t trust);

void      unosec_logout(usec_session_t s);       /* end session, drop its grants */
usc_uid_t unosec_session_user(usec_session_t s); /* UID_NONE if invalid          */
int       unosec_session_valid(usec_session_t s);/* 1 if live & unexpired         */
void      unosec_session_lock(usec_session_t s); /* require re-auth to use again  */

/* ---- current-thread identity binding --------------------------------------
 * unosec_current_user() answers from whatever the executing thread most
 * recently entered.  unoscript sets identity when it starts a script:
 *
 *     usec_session_t s = <the launching session>;
 *     unosec_enter_session(s);   // this thread now runs *as* s
 *     ... run the script ...
 *     unosec_leave();            // pop back to the previous binding
 *
 * enter/leave nest (a script that launches a helper).  The binding is
 * per-executing-context; the cooperative scheduler (unosched) re-asserts it on
 * context switch by calling unosec_enter_session for the task it resumes.  See
 * UNOSECURE.md "thread->session binding". */
int  unosec_enter_session(usec_session_t s);     /* 1 ok, 0 invalid handle       */
void unosec_leave(void);
usec_session_t unosec_current_session(void);     /* 0 if none bound              */

/* =====================================================================
 * Policy & consent  (SPEC §6)
 * ===================================================================== */
void unosec_policy_set(usc_policy_t mode);
usc_policy_t unosec_policy_get(void);
/* Auto-grant every escalation (up to and including KERNEL) that the given role
 * would cover, without a prompt - the "developer machine" affordance.  Empty /
 * NULL name clears it. */
void unosec_policy_autogrant_role(const char *role);
void unosec_set_consent_provider(unosec_consent_fn fn, void *ctx);

/* =====================================================================
 * Signed manifests & the trust store  (SPEC §5)
 * ---------------------------------------------------------------------
 * A script may ship a manifest declaring the caps it intends to use, signed by
 * a key the system trusts.  A valid signature lets us grant the declared set at
 * launch (session-scoped, audited) instead of prompting per op.
 *
 * Manifest format (canonical, LF-terminated lines):
 *     UNOSEC-MANIFEST v1\n
 *     name: <script name>\n
 *     caps: <comma-separated cap names>\n
 *     key:  <trusted key id>\n
 *     sig:  <hex HMAC-SHA256 over the canonical body above, sans the sig line>\n
 * The trust store maps key id -> a 32-byte secret.  (HMAC keeps the crypto self
 * -contained; an ECDSA trust store - bearssl br_ecdsa_* - is a drop-in for the
 * verify step and the on-disk key, see UNOSECURE.md.) */
int  unosec_trust_add_key(const char *key_id, const unsigned char key[32]);

/* Verify `manifest` against the trust store and, if valid AND the caller's
 * session's (uid,trust) is allowed the declared caps by policy, grant that set
 * SESSION-scoped in the current session.  Returns the number of caps granted,
 * 0 if the manifest is unsigned/untrusted (caller falls back to per-op
 * prompts), or <0 on a malformed manifest. */
int  unosec_manifest_apply(const char *manifest);

/* =====================================================================
 * Audit  (SPEC §3 / §7).  Append-only, tamper-evident (a SHA-256 hash chain);
 * covers auth events + every tier>=2 escalation attempt (allowed or denied).
 * ===================================================================== */

/* Copy the human-readable audit tail into `out` (NUL-terminated).  Requires the
 * current session to hold sec.audit.read.  Returns bytes written, or <0. */
int  unosec_audit_read(char *out, int max);

/* The current head of the audit hash chain (32 bytes) - an external verifier
 * can pin it to detect truncation/rewrite.  Returns 1 if written. */
int  unosec_audit_head(unsigned char out[32]);

#ifdef UNO_SECTEST
/* Build-time escalation gate (SPEC §8.6) at the C level: an unprivileged
 * session is denied PROC_ENUM, then permitted after request() grants under a
 * test policy.  1 = pass.  Runs on a scratch in-RAM store and leaves no state. */
int  unosec_selftest(void);
#endif

/* unosecure's OWN capability names (its namespace, not unoscript's). */
#define UNOSEC_CAP_USER_CREATE  "sec.user.create"
#define UNOSEC_CAP_USER_DELETE  "sec.user.delete"
#define UNOSEC_CAP_ROLE_ASSIGN  "sec.role.assign"
#define UNOSEC_CAP_ROLE_EDIT    "sec.role.edit"
#define UNOSEC_CAP_POLICY_EDIT  "sec.policy.edit"
#define UNOSEC_CAP_AUDIT_READ   "sec.audit.read"

#endif /* PC64_UNOSECURE_H */
