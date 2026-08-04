/* ===========================================================================
 * UnoDOS/pc64 - the unoautomate PRIVILEGE GATE.
 *
 * unoautomate and its URC remote channel ship in PRODUCTION (2026-08-03).  They
 * used to be `#ifdef UNO_DEBUG`: absent from a shippable image entirely.  They
 * are product features - remote support, automation, a scriptable machine - so
 * the gate moved from a compile flag to PRIVILEGE, the same move unoscript
 * already made (UNOSCRIPT.md: "its gate is not a compile flag - it is
 * privilege").
 *
 * THE MODEL, in one paragraph.  On a production boot the channel is DISARMED:
 * no listener, no dial-out, no verb dispatch, nothing on the wire.  A user AT
 * THE CONSOLE arms it, which runs a real `unosec_request` escalation for each
 * power they want to hand out (see the three automate.* capabilities in
 * unoscript.h) and draws the normal consent sheet for anything their roles do
 * not already cover.  Arming mints a random TOKEN and shows it on screen.  Every
 * inbound connection must send `auth <token>` before any other verb; nothing
 * else is dispatched until it does.  The authenticated link runs as the arming
 * user under UNOSEC_TRUST_REMOTE - the trust class UNOSECURE-SPEC.md §5 reserved
 * for exactly this - and each verb is checked against the powers granted at arm
 * time.  Log out, disarm, or reboot and the token is dead.
 *
 * WHY THE REMOTE PATH NEVER PROMPTS.  A consent sheet needs someone at the
 * machine; a remote link by definition has nobody there.  So the escalation
 * happens ONCE, at the console, when the user arms the channel - and the remote
 * side only ever CHECKS what that escalation already granted.  unoauto_gate_verb
 * is side-effect free and can never draw UI.  A verb whose power was not granted
 * at arm time is refused, not escalated.
 *
 * THE DEBUG BUILD IS UNCHANGED.  With -DUNO_DEBUG the gate is transparent: armed
 * from DEBUG.CFG as it always was, no token, every verb allowed.  Every QEMU
 * gate under tools/ and the WinForms client keep working exactly as before.
 * Set the DEBUG.CFG key `urc-auth` to run the PRODUCTION semantics in a debug
 * build - that is how the gate itself is regression-tested (tools/urcauth-qemu.py)
 * without needing a production image and a human at a screen.
 * ======================================================================== */
#ifndef UNOAUTO_GATE_H
#define UNOAUTO_GATE_H

#include "unoscript.h"          /* usc_cap_t / usc_uid_t */

/* Token: a 6-DIGIT PIN + NUL.  It used to be 16 hex characters, which is a
 * fine credential and a bad thing to ask a person to do: it is read off one
 * screen and typed on another machine, by hand, and "was that a b or a 6" is
 * the failure everybody actually hit.
 *
 * WHY SIX DIGITS IS ENOUGH HERE, and would not be anywhere else.  A million
 * combinations is nothing against an oracle you can hammer.  This is not one:
 * unoauto_gate_auth counts failures and the THIRD one disarms the channel
 * outright (BADAUTH_MAX), the count survives a reconnect, and re-arming needs a
 * user physically at the console.  So an attacker on the LAN gets three guesses
 * in a million per arming, and cannot buy a fourth from the wire.  The security
 * comes from the lockout, not from the length - which is exactly why the
 * lockout must not be relaxed to make the PIN more forgiving. */
#define UNOAUTO_TOKEN_CHARS  6
#define UNOAUTO_TOKEN_BUF    (UNOAUTO_TOKEN_CHARS + 1)

/* The three powers, as a mask - which of the automate.* capabilities the
 * console user actually granted when they armed the channel.  A link can hold
 * OBSERVE without ever being able to author a disk. */
#define UNOAUTO_P_OBSERVE   0x1     /* automate.observe - ADMIN                */
#define UNOAUTO_P_DRIVE     0x2     /* automate.drive   - ADMIN                */
#define UNOAUTO_P_SYSTEM    0x4     /* automate.system  - KERNEL               */
#define UNOAUTO_P_ALL       (UNOAUTO_P_OBSERVE | UNOAUTO_P_DRIVE | UNOAUTO_P_SYSTEM)

/* ---- arming (the console side) -------------------------------------------
 * Escalate for each power in `want` and, if at least one is granted, arm the
 * channel and mint a token.  Requires a bound session (someone signed in);
 * fails closed with no session, under a DENY/kiosk policy, or if the user
 * refuses every consent sheet.  `how` is a short string for the audit line
 * ("ui:remote-panel").  Returns the granted mask, 0 = refused/failed.
 * Idempotent-ish: arming while armed re-escalates only the powers not already
 * held and keeps the existing token. */
unsigned unoauto_gate_arm(unsigned want, const char *how);

/* Stand the channel down: drop the grants, kill the token, close any live link
 * and the listener.  `why` goes in the audit line.  Safe when not armed. */
void unoauto_gate_disarm(const char *why);

int      unoauto_gate_armed(void);      /* 1 once armed                        */
unsigned unoauto_gate_powers(void);     /* the granted UNOAUTO_P_* mask        */
const char *unoauto_gate_token(void);   /* the token to type on the dev PC,
                                           "" when disarmed                    */
usc_uid_t   unoauto_gate_owner(void);   /* uid the link runs as                */
const char *unoauto_gate_owner_name(void); /* "" when disarmed                 */

/* Called every shell frame from the same place unoauto_remote_tick is pumped:
 * revalidates the arming session and disarms if the owner logged out or their
 * session expired, so a token can never outlive the login that made it. */
void unoauto_gate_tick(void);

/* ---- the link side --------------------------------------------------------
 * unoauto_remote.c calls these; nothing else should. */

/* Called once from unoauto_remote_boot, before anything else.  Production: a
 * no-op.  Debug with `urc-auth=<token>` in DEBUG.CFG: arms the production gate
 * with that token so a headless QEMU run can exercise the auth and per-verb
 * paths - see the note in unoauto_gate.c for why that hook has to exist. */
void unoauto_gate_boot(void);

/* May the channel run AT ALL this boot?  0 in production until armed - the
 * remote boot path returns immediately, so a production image with nobody
 * signed in has no listener and no dial-out.  Always 1 in a debug build. */
int  unoauto_gate_open(void);

/* Does this build/state require `auth <token>` before dispatching verbs?
 * 0 in a debug build (unless DEBUG.CFG says `urc-auth`), 1 in production. */
int  unoauto_gate_needs_auth(void);

/* Check a token from the wire.  Constant-time compare; a mismatch is audited
 * and counted (three strikes disarms the channel, so a plaintext LAN protocol
 * is not a brute-force oracle).  1 = the link is now authenticated. */
int  unoauto_gate_auth(const char *token);

int  unoauto_gate_authed(void);   /* 1 when the current link has authenticated */
void unoauto_gate_link_reset(void); /* the link dropped: deauthenticate        */

/* May the authenticated link run `verb`?  Side-effect free apart from the audit
 * line; NEVER prompts.  On refusal *why (if non-NULL) is set to a short reason
 * for the `err` response.  Unknown verbs are refused - a verb added without a
 * row in the table is denied rather than silently ambient. */
int  unoauto_gate_verb(const char *verb, const char **why);

/* The capability a verb needs, for `caps` (what may I do on this link?) and for
 * the audit trail.  USC_CAP_NONE = the verb is not gated (auth/help). */
usc_cap_t unoauto_gate_verb_cap(const char *verb);

#endif /* UNOAUTO_GATE_H */
