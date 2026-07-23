# unoscript — next steps (surface wiring roadmap)

**As of 2026-07-23.** `unoscript` (the production Python OS-scripting surface) and
`unosecure` (identity/RBAC/escalation) are both landed, in `build.sh`, and
verified: production build links green, `unosec_present()` → 1, the `unosec_*`
contract matches `unoscript.h` exactly, and the weak fail-closed gate is overridden
by the real adjudicator. See `UNOSCRIPT.md` (design) and `UNOSECURE.md`.

**What that means:** the *privilege gate is live and real*. What's left is
**per-subsystem surface wiring** — each scripted operation delegates to the owning
subsystem, and until that owner exposes its accessor the op returns
`USC_EUNAVAIL` (Python `NotImplementedError`) even when the gate *permits* it.

**One surface is already wired:** `usc_power(0)` (shutdown) consumes the existing
production `uno_pc64_shutdown()`. It's the end-to-end proof the stack works on a
live surface.

This doc is the dispatch list: one section per owning agent, with the exact
accessor `unoscript` will call and the capability it lights up. `unoscript`
implements none of these — it wires its `usc_*` entry point to your accessor once
you land it. When you do, ping back via `UNOAUTOMATE-REQUESTS.md` and the
`unoscript` agent wires the one-line delegation + drops the `USC_EUNAVAIL` stub.

---

## Priority order

1. **unoui — synthetic input + screen read** (tier 0). This is the "generally
   available" layer and the biggest single unlock: it makes UI automation
   (Automator/AppleScript-style) actually work for any logged-in user. Do this first.
2. **shell — app control** (tier 0/1). Launch/close/enumerate + app messaging.
3. **unosched — process enumeration** (tier 2). The "what's running" surface;
   also lands the thread→session binding `unosecure` already requested.
4. **unofs — user-scoped file IO** (tier 1).
5. **kernel — mem / io / reboot / syscall** (tier 2/3). The deep, dangerous
   surfaces; land last, most carefully, all audited.
6. **unoauto (self) — production HOOK registry** (tier 2). The `hook` surface.

Tiers 0–1 give a genuinely useful scripting OS (UI + apps + files). Tiers 2–3 are
the power-user/debugging surfaces and can trail.

---

## 1. unoui — synthetic input, screen read, clipboard  ·  tier 0  ·  `ui.input` / `ui.read` / `clipboard.write`

**Problem:** `uno_pc64_inject_key/_pointer` exist but are `#ifdef UNO_DEBUG`.
`unoscript` is production, so it cannot call them. Provide production equivalents.

**Accessors to expose (production, not debug-gated):**
```c
/* synthetic input, delivered to the next shell frame like a real device event */
int  unoui_input_pointer(int x, int y, int btn);      /* -> usc_ui_pointer      */
int  unoui_input_key(int scan, int uni, int mods);    /* -> usc_ui_key          */
/* read side */
int  unoui_screen_text(char *out, int cap);           /* -> usc_ui_screen_text  */
int  unoui_clipboard_get(char *out, int cap);         /* -> usc_ui_clipboard_get*/
int  unoui_clipboard_set(const char *s);              /* -> usc_ui_clipboard_set*/
```
`unoui_screen_text` should render the focused window's accessibility/label text
(the tree, not a pixel grab) so scripts can find controls. Keep injection on the
same path the debug injector used, minus the `UNO_DEBUG` gate.

**Unlocks:** `u.ui.click/move/key/screen/clip_get/clip_set`.

## 2. shell — app enumeration, launch/close, messaging  ·  tier 0/1  ·  `app.ctrl` / `app.msg`

**Problem:** `pc64_shell_app_count/launch/close_top` are `#ifdef UNO_DEBUG`.

**Accessors (production):**
```c
int  pc64_shell_app_count_pub(void);                       /* -> usc_app_count     */
int  pc64_shell_launch_pub(int idx);                       /* -> usc_app_launch    */
int  pc64_shell_close_top_pub(void);                       /* -> usc_app_close_top */
/* structured message to an app; JSON in, JSON reply out (tier 1) */
int  pc64_shell_app_message(int idx, const char *json,
                            char *reply, int cap);          /* -> usc_app_message   */
```
(Names illustrative — if the debug ones can simply lose their gate, do that and
say so.) App messaging is the AppleScript-"tell application" analog; define a
minimal verb set per built-in app.

**Unlocks:** `u.app.count/launch/close_top` and structured app control.

## 3. unosched — process/thread enumeration + inspect  ·  tier 2  ·  `proc.enum` / `proc.inspect`

**Accessors:** fill `usc_proc_ent` rows (defined in `unoscript.h`:
`{owner, pid, tid, state, name, v1, v2}`).
```c
int  unosched_enumerate(usc_proc_ent *out, int max);   /* -> usc_proc_list       */
int  unosched_inspect(int pid, usc_proc_ent *out);     /* -> usc_proc_inspect    */
```
`v1`/`v2` carry sched detail (cpu-ms / stack use). **Also here:** the thread→
session binding `unosecure` requested — on each context switch call
`unosec_enter_session(task->sec_session)` / `unosec_leave()` so
`unosec_current_user()` follows the running task once concurrent scripted tasks
exist (contract in `UNOSECURE.md`; stopgap is `unoscript`'s enter/leave around a
script body). Do both in the same pass.

**Unlocks:** `u.proc.list()` and per-process inspection.

## 4. unofs — user-scoped file IO  ·  tier 1/2  ·  `fs.user` / `fs.sys`

**Accessors** that honour the acting identity (`unosec_current_user()`), so a
tier-1 script is confined to its user's scope and a path outside it re-guards
`fs.sys`:
```c
int  unofs_read_as(usc_uid_t who, const char *path, void *buf, int cap);  /* -> usc_fs_read  */
int  unofs_write_as(usc_uid_t who, const char *path, const void *buf, int len); /* -> usc_fs_write */
```
Path→scope policy (what "the user's scope" means) is yours; coordinate the ACL
model with `unosecure` (it can key ACLs on its uids). Until then `usc_fs_*`
guards `fs.user` as the floor.

**Unlocks:** `u.fs.read/write`.

## 5. kernel — memory, port IO, reboot/suspend, syscall  ·  tier 2/3  ·  `mem.*` / `io.*` / `power` / `syscall`

The deep surfaces. Each must be a *guarded* accessor — the kernel enforces the
real memory/IO safety; `unoscript`'s gate is the privilege check on top.
```c
int  kmem_read(int pid, unsigned long long addr, void *buf, int len);   /* -> usc_mem_read  */
int  kmem_write(int pid, unsigned long long addr, const void *b, int len);/* -> usc_mem_write */
int  kio_in(unsigned port, int width, unsigned *val);                   /* -> usc_io_in     */
int  kio_out(unsigned port, int width, unsigned val);                   /* -> usc_io_out    */
int  kpower_reboot(void);   int kpower_suspend(void);                    /* -> usc_power 1/2 */
/* tier 3, later: a syscall-tap hook and an unsigned-module load path */
```
`pid` 0 = kernel/physical view; else another process's address space (translate
via that task's page tables). Shutdown (`usc_power(0)`) is already wired to the
existing `uno_pc64_shutdown()` — mirror it for reboot/suspend. Everything here is
KERNEL tier: strongest escalation, always audited by `unosecure`.

**Unlocks:** `u.mem.read/write`, `u.io.in_/out`, `u.sys.power(1|2)`.

## 6. unoauto (self) — production HOOK registry  ·  tier 2  ·  `hook`

`usc_hook_add/remove` wants a *production*-visible tap registry. Today
`unoauto_hook_*` is `UNO_DEBUG`-only. Decide: expose a slim production hook
facility (a bounded, allocation-free tap table) or keep `hook` debug-only and have
`usc_hook_add` return `USC_EUNAVAIL` in production. This one is the `unoscript`/
`unoauto` agent's own call, not a cross-team dependency.

**Unlocks:** `u.hook.*` (if pursued).

---

## Cross-cutting follow-ups (unoscript agent)

- **End-to-end Python gate.** Once a *non-destructive* surface is wired (proc.enum
  is ideal), add a QEMU gate that drives `mod_unoscript` through real `unosecure`:
  assert `u.secured()`, a tier-2 op raises `OSError(EPERM)` unescalated, then
  succeeds after `u.request("proc.enum")` under a dev-autogrant policy. This is the
  Python-layer counterpart to `unosecure`'s `-DUNO_SECTEST` C gate, and it belongs
  to the `unoscript` agent (`tools/automate_qemu.py` family).
- **Manifest-declared caps.** Wire an app's signed manifest (unosecure supports
  HMAC-SHA256 today) to a launch-time grant so trusted automation apps don't prompt
  per op — needs the app-launch path to pass the manifest to `unosec_manifest_apply`.
- **Docs.** As each surface lands, add a worked example to `UNOSCRIPT.md` and the
  user manual (the manual is a standing update rule for pc64 features).

## How to pick this up

Each numbered section is self-contained and dispatchable to that subsystem's
agent. Hand it the section; the accessor signature is the contract. When it lands,
note it in `UNOAUTOMATE-REQUESTS.md` and the `unoscript` agent wires the delegation
(a one-line change per entry) and removes the `USC_EUNAVAIL` stub. No `unoscript`
or `unosecure` change is needed to *prepare* for your accessor — the gate and the
Python bindings are already in place waiting for it.
