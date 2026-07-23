# unosecure — the UnoDOS security subsystem (design)

**Status: LIVE.** `unosecure.{c,h}` implement the `unosec_*` seam from
`UNOSECURE-SPEC.md` (the contract) plus the accounts / RBAC / session / policy /
manifest / audit surface. Production, always-on, fail-closed. It is the system's
authority on identity and permission; `unoscript` is its first consumer, and
login / the installer / fs ACLs / the remote link consume it too.

`unosecure` **adjudicates**; it never touches the surfaces it protects. It owns
*who* an actor is and *what* they may do, and answers "may this actor do X?".

## The seam (what `unoscript` links)

`unoscript.c` ships weak, fail-closed fallbacks for the `unosec_*` symbols (tier
0 allow, everything else deny, `unosec_present()`→0). `unosecure.c` provides the
**strong** definitions; at link they win and the gate upgrades transparently —
the r8169 weak-fallback pattern. The seven seam functions and their exact
semantics are frozen in `UNOSECURE-SPEC.md §3`:

| symbol | role |
|---|---|
| `unosec_current_user()` | uid of the current thread's bound session (hot path) |
| `unosec_check(u,cap)` | pure RBAC: does `u` hold `cap` by role? (side-effect free) |
| `unosec_require(cap)` | live: current ctx holds `cap` statically **or** via an active grant |
| `unosec_request(cap,scope,ttl)` | raise authority — policy + consent live here |
| `unosec_drop(grant)` | relinquish an escalation early |
| `unosec_audit(cap,detail,allowed)` | sink for every tier≥2 attempt |
| `unosec_present()` | returns 1 — a real adjudicator is linked |

## Identities

- **uid 0 (`UNOSEC_UID_SYSTEM`)** — the kernel; holds every capability. Never a
  login you hand out; the first admin is a *distinct* account (uid ≥ 1).
- **`UNOSEC_UID_NONE`** — the unauthenticated context (no session bound). Holds
  nothing above AMBIENT, so a script with no login is exactly the fail-closed
  floor `unoscript` already shipped.
- **accounts** — uid ≥ 1, a name, a salted password hash, and a set of roles.

## RBAC

Roles are named sets of capability **names** (opaque strings via
`unoscript_cap_name()`), so the tables survive additions to `usc_cap_t` and can
also hold `unosecure`'s own `sec.*` caps. Built-ins seeded on a fresh store:

- **admin** — every `usc` cap tier 0–2 + all `sec.*` management caps.
- **user** — tier 0–1 (`ui.*`, `app.ctrl`, `fs.user`, `settings`, …).
- **guest** — tier 0 only.

`unosec_check(u,cap)` = uid 0 → yes; AMBIENT → yes; else any assigned role holds
the cap name. `unosec_require(cap)` adds the trust-class cap (SANDBOX gets only
AMBIENT statically) and the live-grant check.

## Trust classes

A script is "user U running code of trust class T": `INTERACTIVE`, `INSTALLED`,
`REMOTE`, `SANDBOX` (least trusted). Policy keys off (uid, class, cap):

- tier 0 → all classes (still ≤ the user).
- tier 1 → INTERACTIVE/INSTALLED/REMOTE for the owning user; SANDBOX denied.
- tier ≥ 2 → a granting role **or** interactive consent; SANDBOX refused
  outright; KERNEL always needs consent or a valid signed manifest.

## Escalation & policy

`unosec_request()` is the only blocking entry (it may prompt). `decide_escalation`
resolves, in order: kiosk `DENY` → no session (closed) → SANDBOX refuse → a
developer **autogrant role** (covers every tier) → `AUTOGRANT` policy (≤ ADMIN) →
a granting role the user holds (≤ ADMIN) → the owning user's own tier-1 authority
→ the registered **consent provider** (absent one, fail closed; KERNEL defaults
to Deny). Grants are `ONCE` / `SESSION` / `TIMED(ttl_ms)`, revocable via the
returned handle (`unosec_drop`) and auto-reaped on expiry / logout.

Consent UI is **not** owned here: `unosec_set_consent_provider()` registers a
callback (`unoui`/login draws the sheet in `UNOSECURE-SPEC.md §6`); `unosecure`
adjudicates and asks the provider to prompt. The QEMU gate flips policy to
`AUTOGRANT` (or registers an always-allow provider) as its "test policy".

## Signed manifests & the trust store

A script may ship a manifest declaring the caps it intends; a valid signature
grants that set SESSION-scoped at launch (audited) instead of prompting per op.
Canonical format (LF-terminated):

```
UNOSEC-MANIFEST v1
name: <script name>
caps: proc.enum,fs.sys,mem.read
key:  <trusted key id>
sig:  <hex HMAC-SHA256 over the body above, up to and including the '\n' before this line>
```

The trust store maps key id → a 32-byte secret (`unosec_trust_add_key`, persisted
root-only). Verify = recompute HMAC-SHA256 and constant-time compare. **HMAC**
keeps the crypto self-contained; an **ECDSA** trust store (BearSSL `br_ecdsa_*`,
already vendored) is a drop-in for the verify step and the on-disk key if
asymmetric trust is wanted — swap `hmac256`/`fromhex` for a `br_ecdsa_vrfy` call
and store a public key.

## Thread → session binding

`unosec_current_user()` must answer from whatever executes a script. The model:

```c
usec_session_t s = unosec_login(name, pw, UNOSEC_TRUST_INSTALLED); /* or _open */
unosec_enter_session(s);   /* this thread now runs AS s */
    ... run the script ...
unosec_leave();            /* pop to the previous binding */
```

`enter`/`leave` nest (a script that launches a helper) as a small bounded stack.
The cooperative scheduler (`unosched`) re-asserts the binding on context switch
by calling `unosec_enter_session` for the task it resumes and `unosec_leave` when
it suspends — so identity follows the running task. `unoscript` sets identity
this way when it launches a script (tagging it with the launching session).
**Open coordination with `unosched`** (filed via `UNOAUTOMATE-REQUESTS.md`): the
scheduler must call the binding on switch; until it does, the binding follows the
enter/leave calls `unoscript` makes around a script body, which is correct for
the single-run-at-a-time cooperative model today.

## Storage (unofs, root-only)

One POD blob `UNOSEC.DB` at the root of the first writable persistent volume
(native FAT preferred; RAM disk is the non-persistent fallback). It holds the
account table, role table, trust keys, policy mode, and the autogrant role,
versioned by an 8-byte magic (`UNSCDB\x01\0`). Passwords are **never** stored
plaintext: PBKDF2-HMAC-SHA256, 4096 rounds, a 16-byte per-user salt from RDRAND
(TSC-whitened fallback). "Root-only" is enforced at the `unosecure` API layer
today (mutators require the caller's session to hold the matching `sec.*` cap);
`unofs` file ACLs keyed on these uids will enforce it at the filesystem layer as
they land (`UNOSECURE-SPEC.md §9`).

## Audit (append-only, tamper-evident)

`UNOSEC.LOG` is a hash-chained log: each line is `SHA256(prev_head || line)`,
prefixed with the new head's short hex so the file carries its own chain. Covers
auth events (login/logout/bootstrap) and every tier≥2 escalation attempt (allow
or deny). `unosec_audit_head()` exposes the 32-byte head for an external verifier
to pin (detects truncation/rewrite). Debug builds also forward each line to the
`unoauto` SCRIPT LOG channel. The in-RAM tail is bounded (16 KiB) and flushed to
`unofs` on each event; on overflow the oldest whole lines are dropped, never the
chain head.

## Bootstrap

On a fresh install (no accounts), `unosec_bootstrap_admin(name, pw)` creates the
first admin (and refuses once any account exists) — the installer's entry point.
Thereafter the admin logs in and provisions users; kernel/system code that needs
to provision runs inside a `UNOSEC_UID_SYSTEM` session.

## Definition-of-done map (`UNOSECURE-SPEC.md §8`)

1. `unosecure.{h,c}`, every `unosec_*` with real semantics, `unosec_present()`→1. ✓
2. Accounts + auth + sessions, `unofs`-backed hashed persistence, bootstrap admin. ✓
3. RBAC: roles, role→cap by name, user→role; honoured by check/require. ✓
4. Escalation: scopes, policy, consent seam, signed-manifest fast-path, drop. ✓
5. Audit trail (hash-chained, append-only). ✓
6. Wired into `build.sh` with `unoscript`; `-DUNO_SECTEST` gates the C-level
   escalation proof (`unosec_selftest`): unprivileged `require(proc.enum)` denies,
   then permits after `request` grants under the test policy. ✓
