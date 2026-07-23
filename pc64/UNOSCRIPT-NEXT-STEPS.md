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

1. ~~**unoui — synthetic input + screen read** (tier 0).~~ **DONE** (2026-07-23) —
   `ui.pointer/key/screen_text/clipboard_get/clipboard_set` are wired to real
   platform/shell seams and QEMU-verified over URC. UI automation works. See §1.
2. ~~**shell — app control** (tier 0/1).~~ **DONE** (2026-07-23) —
   `app.count/launch/close_top` (tier 0) + `app.message` (tier 1) wired and
   QEMU-verified over URC. See §2.
3. **unosched — process enumeration** (tier 2). The "what's running" surface;
   also lands the thread→session binding `unosecure` already requested.
4. **unofs — user-scoped file IO** (tier 1).
5. **kernel — mem / io / reboot / syscall** (tier 2/3). The deep, dangerous
   surfaces; land last, most carefully, all audited.
6. **unoauto (self) — production HOOK registry** (tier 2). The `hook` surface.

Tiers 0–1 give a genuinely useful scripting OS (UI + apps + files). Tiers 2–3 are
the power-user/debugging surfaces and can trail.

---

## 1. unoui — synthetic input, screen read, clipboard  ·  tier 0  ·  DONE (2026-07-23)

**Was:** `uno_pc64_inject_key/_pointer` existed but were `#ifdef UNO_DEBUG`, and
`unoscript` is production, so it could not call them; there was no screen-tree
accessor and no system clipboard.

**Shipped** (production, gated only at the unoscript/unosecure layer):
- **Input:** the debug injectors lost their `UNO_DEBUG` gate — `uno_pc64_inject_key`
  / `uno_pc64_inject_pointer` (uefi_main.c) are now production, still on the exact
  `map_key` / clamp+`pointer_moved_clicked` path real device input uses.
  `usc_ui_pointer`/`usc_ui_key` call them (unoscript.c).
- **Screen read:** `pc64_shell_screen_text(out, cap)` (pc64_uui.c) renders the
  window-tree text — one line per open window, the focused one marked `*` — not a
  pixel grab. Wired to `usc_ui_screen_text`.
- **Clipboard:** a small shell-owned text buffer with `pc64_shell_clip_set/get`
  (there was no system clipboard; apps keep their own). Wired to
  `usc_ui_clipboard_set` (tier-1 `clipboard.write`) / `_get` (tier-0 `ui.read`).
  Apps don't consume it yet — integrating WordPad/others' copy-paste is a
  follow-up.

**Verified** (QEMU, ui_automation_test over URC): launch an app → `ui.screen()`
shows its live title (focused-marked); `ui.click`/`ui.key` return `USC_OK` on the
real input path; `ui.clip_get()` reads (tier-0); `ui.clip_set()` is refused with
no session (tier-1 gate). Unlocked `unoscript.ui.click/move/key/screen/clip_get/clip_set`.

**Follow-ups (not blocking):** `ui.key`'s modifier arg currently maps any nonzero
`mods` to the single Cmd/Ctrl modifier (`map_key` takes one modifier); a full
modifier bitmap is a later refinement. App-integrated clipboard as above.

## 2. shell — app enumeration, launch/close, messaging  ·  tier 0/1  ·  DONE (2026-07-23)

**Was:** `pc64_shell_app_count/launch/close_top` were `#ifdef UNO_DEBUG`, and
there was no app-message seam.

**Shipped** (production, gated at the unoscript/unosecure layer):
- The debug DRIVE accessors lost their gate — `pc64_shell_app_count` /
  `pc64_shell_launch` / `pc64_shell_close_top` (pc64_uui.c) are now production,
  still the exact launcher-click / title-bar-close paths (EX_PYAPP/EX_USERAPP
  refused to launch so the running script isn't displaced). Wired to
  `usc_app_count`/`usc_app_launch`/`usc_app_close_top` (tier 0, `app.ctrl`).
- `pc64_shell_app_message(idx, msg, reply, cap)` (pc64_uui.c) — a minimal v1 verb
  set by app index: `info` (name/open/focused), `focus`, `close`, reusing
  `open_app`/`raise_win`/`close_focused`. Wired to `usc_app_message` (tier 1,
  `app.msg`) + a new `unoscript.app.message(idx, verb)` Python binding
  (mod_unoscript.c, exported in pc64_modload.c). The message is a plain verb
  string, not JSON yet, and the verbs are generic (shell-level) — a per-app
  message handler for app-specific verbs (AppleScript "tell Notepad to insert…")
  is the natural follow-up.

**Verified** (QEMU, app_control_test over URC): `app.count()`=19; `app.launch(1)`
opened Editor (seen in `ui.screen()`); `app.close_top()` returned USC_OK and the
window left the tree; `app.message()` was refused with no session (tier-1 gate).

**Unlocked:** `unoscript.app.count/launch/close_top/message`.

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
