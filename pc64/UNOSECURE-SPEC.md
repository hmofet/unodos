# unosecure — specification (for the implementing agent)

**You are building `unosecure`, the UnoDOS security subsystem: user accounts,
RBAC, and privilege escalation.** It is a first-class OS subsystem (peer of
`unofs` / `unonet` / `unosched`), production, always-on, fail-closed. Its first
consumer is `unoscript` (the Python OS-scripting surface), which is already
stubbed against your contract and blocked on you — but you are NOT scriptable's
appendage; you are the system's authority on identity and permission, and other
subsystems (login, installer, fs ACLs, remote link) will consume you too.

This spec is a contract, not an implementation. Where it says "you decide,"
design it; where it gives a C signature, match it exactly — `unoscript` calls it.

---

## 1. Scope & ownership

**You own:** identity (accounts), authentication (login/sessions), authorization
(RBAC: roles → capabilities), and privilege escalation (raising a session's
authority, with consent/policy). Plus the audit trail of security-relevant
events.

**You do NOT own:** the surfaces being protected. `unoscript` (and others) call
you to ask "may this actor do X?"; they perform X themselves. You never touch
UI/fs/memory — you adjudicate.

**Files (suggested):** `pc64/unosecure.h` (the contract below), `pc64/unosecure.c`
(engine), storage via `unofs`, consent UI via `unoui`. Add to `build.sh` in the
same change that wires `unoscript` in. Bump nothing in `unoscript.h` — you
*provide* the `unosec_*` symbols it already declares.

## 2. The capability vocabulary you adjudicate

`unoscript.h` defines `usc_cap_t` and `usc_tier_t` (tiers 0..3: AMBIENT / USER /
ADMIN / KERNEL) and the capability list (`ui.input`, `fs.user`, `proc.enum`,
`mem.read`, `syscall`, …). **Treat capabilities as opaque integers keyed by their
stable string name** (`unoscript_cap_name()`), so your role tables and policy
files survive additions to the enum. Do not hardcode enum values.

Design your own account/role capabilities too (e.g. `sec.user.create`,
`sec.role.assign`) in your own namespace — those are yours, not `unoscript`'s.

## 3. The seam you MUST implement (exact signatures)

`unoscript` links these weakly today (fail-closed). When you provide strong
definitions, they take over transparently — no change on the `unoscript` side.
Signatures are in `unoscript.h`; repeated here with the required semantics:

```c
usc_uid_t unosec_current_user(void);
```
The identity of the script/thread currently executing. Must be cheap (hot path).
`0` is reserved for the system/kernel identity. Derive from the current session/
thread context you maintain.

```c
int unosec_check(usc_uid_t u, usc_cap_t cap);   /* 1 if u STATICALLY holds cap */
```
Pure RBAC lookup: does user `u`, via any assigned role, hold `cap`? Ignores live
escalations. Side-effect free (used by UIs to grey out actions).

```c
int unosec_require(usc_cap_t cap);              /* 1 if CURRENT ctx may use cap NOW */
```
The hot-path decision: `unosec_check(current_user, cap)` **OR** an active,
unexpired escalation grant for `cap` in the current session. No prompting here —
this must be non-blocking and fast. (Prompting happens in `unosec_request`.)

```c
int unosec_request(usc_cap_t cap, usc_scope_t scope, int ttl_ms);  /* handle>0, or 0 denied */
```
Raise the current session's authority for `cap`. This is where policy and consent
live. Decision inputs (you design the policy engine):
- the caller's static roles (an admin may auto-grant);
- the script's **trust class** and **signed manifest** (§5);
- **interactive consent** — draw a sheet via `unoui` ("Script X wants `mem.read`.
  Allow once / for this session / deny"). Consent is required for KERNEL tier and
  for ADMIN tier absent a granting role; you decide finer policy.
- Scope: `ONCE` (single op), `SESSION` (until the session ends), `TIMED`
  (`ttl_ms`). Return a handle you can revoke; `0` = denied.
This call MAY block (it can prompt). `unoscript`'s guard only calls it off the
fast path (explicit `u.request(...)`, or a one-shot for an ADMIN op).

```c
void unosec_drop(int grant);           /* relinquish an escalation early */
void unosec_audit(usc_cap_t cap, const char *detail, int allowed);
int  unosec_present(void);             /* return 1 — you exist */
```
`unosec_audit` receives every tier≥2 attempt (allowed or denied) — persist it
(append to a tamper-evident log via `unofs`) and optionally forward to the
`unoauto` LOG channel. `unosec_present()` returning 1 is how `unoscript` (and its
`u.secured()` Python call) knows a real adjudicator is linked.

## 4. Accounts, auth, sessions (your own surface)

Design the API; suggested shape:

- **Accounts:** create / delete / list users; each has a `usc_uid_t`, a name, a
  password **hash** (never plaintext — you choose the KDF; salt per user), and a
  set of roles. Persist via `unofs` in a root-only store. A bootstrap path
  creates the first admin on a fresh install (installer consumes this).
- **Authentication:** verify a credential → establish a **session** bound to a
  uid and a trust class. This is the login story; a login UI or the shell calls
  it. Sessions can expire / lock.
- **Session context:** you must be able to answer `unosec_current_user()` from
  whatever executes a script. Coordinate the "which session owns this Python
  thread" binding with `unosched` (thread→session) and `unoscript` (it tags a
  script with the launching session). Define that binding call, e.g.
  `unosec_enter_session(handle)` / `unosec_leave()`, or a thread-local the
  scheduler sets on context switch — **your design, but document it**, because
  `unoscript` needs to set identity when it starts a script.

## 5. Trust classes & signed manifests

A script is not just "user U" — it is "user U running code of trust class T."
Define at least: `INTERACTIVE` (typed at a live prompt), `INSTALLED` (a stored
`.py`/app the user launched), `SANDBOX` (untrusted/downloaded), `REMOTE` (arrived
over the `unoauto_remote`/URC link). Policy keys off (uid, class, requested cap):
- Tier 0 → allow for all classes (still ≤ the user).
- Tier 1 → allow INTERACTIVE/INSTALLED for the owning user; deny SANDBOX by default.
- Tier ≥ 2 → require a granting role or interactive consent; you may *require* a
  valid signature for KERNEL tier and refuse SANDBOX outright.

**Signed manifest (optional but recommended):** a script may ship a manifest
declaring the capabilities it intends to use, signed by a key the system trusts.
A valid signature lets you grant without prompting on every op (grant the manifest
set at launch, audited). Define the manifest format and the trust-store for keys.

## 6. Escalation UX (consent)

Via `unoui`: a modal sheet naming the script, the capability (use
`unoscript_cap_name`), a human description, and Allow-once / Allow-session / Deny.
KERNEL-tier prompts should be visually distinct and default to Deny. Respect a
system policy that can disable prompting entirely (kiosk → deny all escalation) or
auto-grant for a role (developer machine). Every prompt outcome is audited.

## 7. Non-negotiables

- **Fail closed.** Any ambiguity, missing session, storage error, or policy gap →
  deny. Match the weak fallback `unoscript` already ships (tier 0 allow, rest deny)
  as the floor, then add real grants above it.
- **Production, not `UNO_DEBUG`.** This compiles into every build. No debug gate.
- **No plaintext secrets.** Hash+salt passwords; protect the store as root-only.
- **Root/uid-0 is the system**, not a login you hand out casually; the first admin
  is a distinct account.
- **Audit is append-only** and covers auth events + every tier≥2 escalation.

## 8. Definition of done

1. `pc64/unosecure.{h,c}` implementing every `unosec_*` symbol in §3 with real
   semantics; `unosec_present()` → 1.
2. Accounts + auth + sessions (§4) with `unofs`-backed, hashed persistence and a
   bootstrap-admin path.
3. RBAC: roles, role→capability (by name), user→role assignment; `unosec_check`
   and `unosec_require` honour them.
4. Escalation (§5/§6): `unosec_request` with scopes, policy, `unoui` consent, and
   signed-manifest fast-path; grants revocable via `unosec_drop`.
5. Audit trail (§3/§7).
6. Wired into `build.sh` **together with** `unoscript` (remove its "deferred"
   note), so `unoscript`'s tier≥1 surfaces light up. Verify: a QEMU gate where an
   unprivileged script's `u.proc.list()` raises OSError(EPERM), then succeeds
   after `u.request("proc.enum")` grants via a test policy.

## 9. Coordinate with

- **unoscript** (this repo, `unoscript.h`) — your first consumer; contract frozen
  above. File anything you need from it back through UNOAUTOMATE-REQUESTS.md.
- **unosched** — thread→session binding for `unosec_current_user`.
- **unofs** — the account/audit store and (later) file ACLs keyed on your uids.
- **unoui** — the consent sheet.
- **installer / login** — bootstrap admin + the authenticate entry point.
