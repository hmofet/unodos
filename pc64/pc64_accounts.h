/* ===========================================================================
 * pc64_accounts - the security UI: a consumer of `unosecure`.
 *
 * unosecure adjudicates identity/permission; it never draws.  This file is the
 * shell-side UI that CALLS it:
 *   - the boot login / first-run gate  (pc64_login_gate)
 *   - the escalation consent sheet      (pc64_consent_register)
 *   - the Accounts manager modal        (pc64_accounts_open)
 * Each runs its own modal unoui_ui over the shell theme, so it needs nothing
 * from pc64_uui.c's statics - just the platform fb/input/present primitives.
 * ======================================================================== */
#ifndef PC64_ACCOUNTS_H
#define PC64_ACCOUNTS_H

/* Boot gate: if any account exists, block until a valid login binds the shell
 * session (so unosec_current_user() reflects the user for the whole session).
 * On a fresh machine (no accounts) it returns immediately - the shell runs in
 * the ambient/guest context until an admin is created.  Call once, after
 * unosec_boot(), before the desktop loop. */
void pc64_login_gate(void);

/* Register the escalation consent provider with unosecure (the UAC-style sheet
 * drawn when a script requests an ADMIN/KERNEL capability it does not hold). */
void pc64_consent_register(void);

/* Open the Accounts manager (a modal): list users, create/delete, set
 * passwords, toggle the admin role.  Self-elevates via a login sheet when the
 * shell session lacks authority; offers first-run admin creation when no
 * account exists yet. */
void pc64_accounts_open(void);

#endif /* PC64_ACCOUNTS_H */
