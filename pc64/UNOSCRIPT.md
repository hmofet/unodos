# unoscript — the OS scripting & automation surface (design)

**Status: LIVE — `unosecure` landed.** Files: `unoscript.h` (contract),
`unoscript.c` (runtime + capability guard), `upy_port/mod_unoscript.c` (the
`unoscript` Python module), all now in `build.sh`. `unosecure` (`unosecure.{c,h}`)
provides the strong `unosec_*` adjudicator that replaces the weak fail-closed
gate — so the privilege decision is real. The remaining work is per-subsystem
surface wiring (see "Subsystem seams"): a permitted tier≥1 op returns
NotImplementedError until its owner lands the accessor. Dependency spec:
`UNOSECURE-SPEC.md`; the adjudicator's design is `UNOSECURE.md`.

## What it is

"Every surface of the OS is scriptable from Python." Automator + AppleScript in
spirit, but the scriptable surface is the whole system — synthetic input, window
and app control, the filesystem, process/thread inspection, memory peek/poke,
port I/O, syscall interception — and every deep operation is **capability-gated**.

The line that makes this safe to ship: `unoscript` is **production and always
on**, so its gate cannot be a compile flag. It is **privilege**. Contrast:

| | `unoauto` (existing) | `unoscript` (this) |
|---|---|---|
| Purpose | dev/test harness | user-facing OS automation |
| Presence | `UNO_DEBUG` only — compiles away in prod | ships in production |
| Gate | the compile flag | per-op capability via `unosecure` |
| Audience | the developer | any script any user runs |

They share plumbing (input injection, PROBE, the HOOK registry) but are distinct
subsystems. `unoscript` may *reuse* `unoauto`'s primitives where they exist; it
never depends on `UNO_DEBUG` being on.

## Ownership (per the 2026-07-22 re-home discipline)

`unoscript` owns a deliberately thin slice, so it does not repeat unoautomate's
mistake of absorbing everything it touches:

- **Owns:** the Python bindings, the capability/tier model (`usc_cap_t`), the
  guard, and the seam into `unosecure`.
- **Does NOT own the surfaces.** UI is `unoui`'s, files are `unofs`'s, processes/
  threads are `unosched`'s, memory/IO/syscall are the kernel's. `unoscript`
  reaches each through that subsystem's **public API**, and where an accessor
  doesn't exist yet it files a request against the owner (see "Subsystem seams"
  — `unoscript` implements none of them itself).
- **Does NOT own privilege.** Identity, accounts, RBAC, and escalation are
  entirely `unosecure`'s. `unoscript` only *calls* `unosec_*`.

## Access tiers

Coarse bands; each capability sits in exactly one. The tier says how a grant is
normally obtained — `unosecure` has the final say.

| Tier | Name | Meaning | How granted |
|---|---|---|---|
| 0 | AMBIENT | interactive-equivalent — no more powerful than the user at the keyboard | any logged-in user; sandbox can still be denied |
| 1 | USER | scoped to the acting user's account | default for interactive users; denied to untrusted scripts |
| 2 | ADMIN | system-wide observe/control | **explicit escalation** (consent prompt or RBAC role) — the sudo/UAC analog |
| 3 | KERNEL | raw memory / port I/O / syscall / code patch | strongest escalation: interactive confirm and/or signed script, always audited |

**The design rule you asked for:** UI manipulation is generally available (tier
0). Syscall/memory inspection is not — it requires an explicit privilege
escalation (tier 2/3) that `unosecure` adjudicates.

## Capability catalog

The vocabulary `unosecure` is written against (`usc_cap_t` in `unoscript.h`).
Append-only.

| Capability | Tier | Domain | Grants |
|---|---|---|---|
| `ui.input` | 0 | ui | inject pointer/keyboard |
| `ui.read` | 0 | ui | read screen text, window tree, clipboard |
| `app.ctrl` | 0 | app | launch / focus / close ordinary apps |
| `clock` | 0 | sys | read uptime / wall clock |
| `fs.user` | 1 | fs | read/write within the user's scope |
| `settings` | 1 | sys | read/write the user's settings |
| `automation` | 1 | — | register persistent/background scripts |
| `app.msg` | 1 | app | structured message to / read app state |
| `clipboard.write` | 1 | ui | place data on the clipboard |
| `proc.enum` | 2 | proc | enumerate ALL processes/threads |
| `proc.inspect` | 2 | proc | read another process's state/regs (RO) |
| `hook` | 2 | hook | attach taps / intercept a call surface |
| `log.sys` | 2 | — | read system logs / all LOG channels |
| `fs.sys` | 2 | fs | read/write outside the user's scope |
| `io.read` | 2 | io | read I/O ports / MMIO |
| `power` | 2 | sys | reboot / shutdown / suspend |
| `mem.read` | 3 | mem | peek arbitrary / cross-process memory |
| `mem.write` | 3 | mem | poke arbitrary / cross-process memory |
| `io.write` | 3 | io | write I/O ports / MMIO / device regs |
| `syscall` | 3 | — | intercept / emulate / trace syscalls |
| `module` | 3 | — | load unsigned modules / patch live code |

## The guard & escalation flow

Every surface op calls `unoscript_guard(cap, what)` before doing anything:

```
script calls u.mem.read(pid, addr, n)
      │
      ▼
usc_mem_read()  ── unoscript_guard(USC_CAP_MEM_READ, "mem.read pid=…")
      │                     │
      │              unosec_require(cap)?  ── yes ─► proceed
      │                     │ no
      │              tier ≥ ADMIN?  ── unosec_request(cap, ONCE) ──► unosecure
      │                     │                                         decides:
      │                     │                              role? prompt? signed?
      │              grant>0 ─► proceed & drop         deny ─► audit + return 0
      ▼
delegate to the owning subsystem (kernel guarded peek) OR USC_EUNAVAIL if unwired
```

- **Static grants** (held via an RBAC role) pass `unosec_require` with no prompt.
- **Escalation** (`u.request("mem.read")` or an auto-`ONCE` on first touch) asks
  `unosecure`, which may draw an interactive consent sheet (via `unoui`), consult
  policy for a signed script, or grant from a role. Grants are scoped ONCE /
  SESSION / TIMED and can be dropped early.
- **Audit:** every tier≥2 attempt (allow or deny) is reported to `unosec_audit`,
  which `unosecure` persists/forwards.

**Python surface of the flow:**

```python
import unoscript as u
u.secured()             # False until unosecure links in
u.whoami()              # current user id
u.cap_tier("mem.read")  # 3
u.request("proc.enum")  # -> True if granted (prompt or role), else False
# denied ops raise OSError(EPERM); unwired seams raise NotImplementedError
```

## Namespaces (Python)

`unoscript.ui` · `.app` · `.fs` · `.proc` · `.mem` · `.io` · `.sys` — each maps
to a capability domain. `mod_unoscript.c` wires representative methods; the C
surface in `unoscript.h` is the full 1:1 list.

## Script identity & trust

`unosecure` answers "who is this script and how much do we trust it?" `unoscript`
just carries the question. Expected inputs (see the spec): the launching user, an
optional signed manifest declaring the caps the script wants, and a trust class
(interactive / installed / sandbox / remote). Tier-0 is available to all classes;
higher tiers key off identity + manifest + policy.

## Fail-closed default (pre-unosecure)

Until `unosecure` links its strong `unosec_*` symbols, `unoscript.c` provides
**weak** fallbacks: tier 0 allowed (a script ≤ the user), tier ≥ 1 denied,
`unosec_present()` → 0. So UI scripting is reachable the day the surfaces are
wired, and nothing deeper can be reached without the real adjudicator. This is
the r8169 weak-fallback pattern — strong defs transparently take over.

## Subsystem seams (requests `unoscript` owes to owners)

`unoscript` implements none of these; each is an accessor requested from the
owning subsystem (tracked in that subsystem's request channel):

- **unoui** — a production synthetic-input entry (today only the debug
  `uno_pc64_inject_*`), plus window-tree / accessibility text and clipboard get/set.
- **shell** — production app enumeration/launch/close + a structured app-message IPC.
- **unofs** — a user-scoped read/write seam that honours the acting identity.
- **unosched** — task/thread enumeration + single-task inspect (state, regs, cpu).
- **kernel** — guarded cross-address-space `mem_read/write`, port `io_in/out`,
  reboot + suspend (shutdown is **wired** — `usc_power(0)` consumes the existing
  production `uno_pc64_shutdown()`), and (tier 3) a syscall-tap hook and unsigned-
  module load path.
- **unosecure** — the whole `unosec_*` seam. **DONE** — landed and verified
  (build links green, `unosec_present()`→1, contract matches `unoscript.h`).

## Build wiring — landed with `unosecure`

`unoscript.{c,h}` and `upy_port/mod_unoscript.c` are now in `build.sh`, wired in
the same change that landed `unosecure` (`unosecure.{c,h}`). The core object
list compiles `unosecure unoscript`; `unosecure`'s strong `unosec_*` definitions
replace `unoscript.c`'s weak fail-closed fallbacks at link (the r8169 pattern),
so `unosec_present()` is now 1 and tier≥1 decisions are real. `pc64_modload.c`
exports the `usc_*` surface + the `unosec_*`/`unoscript_*` entry points PYRT
imports, and `mod_unoscript.c` is added to the PYRT source set so `import
unoscript` resolves. The kernel brings the subsystem up at the end of
`uno_pc64_init()` (`unosec_boot()` then `unoscript_boot()`), after storage/detach
so the security store lands on a writable volume.

**First wired surface:** `usc_power(0)` (shutdown) — it consumes the existing
production `uno_pc64_shutdown()`, so `unoscript.sys.power(0)` is a real end-to-end
path (Python → guard → `unosec` adjudication → OS shutdown), gated behind the
tier-2 `power` capability. It proves the whole stack works on a live surface.

The remaining **surface seams** (unoui synthetic input, shell app control, unofs
user-scoped IO, unosched enumeration, kernel mem/io + reboot/suspend) are still
`USC_EUNAVAIL` until each owner wires its accessor — so a *permitted* tier≥1 op
returns NotImplementedError rather than OSError(EPERM). The privilege gate is
live; the plumbing behind it lights up per-subsystem.
