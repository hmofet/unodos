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
3. ~~**process enumeration** (tier 2).~~ **DONE** (2026-07-25) — `proc.list`/
   `proc.inspect` enumerate the shell's open-app run-set (pc64 has no preemptive
   scheduler; `unosched` is the concurrency-primitive lib, not a run-queue), via
   production `pc64_shell_app_open`/`_name`/`_is_focused`. QEMU-verified
   (`tools/unoscript_qemu.py`): wired + tier-2 gated. See §3. The thread→session
   binding remains a `unosecure` follow-up for when concurrent scripted tasks exist.
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

## 3. process enumeration + inspect  ·  tier 2  ·  `proc.enum` / `proc.inspect`  ·  DONE (2026-07-25)

**Premise correction.** §3 originally named `unosched` as the owner of an
`unosched_enumerate(usc_proc_ent*, int)` seam. `unosched/` is in fact the tiered
**concurrency-primitive library** (COOP floor + SMP sync/`uno_job` offload) — it
has no PID/TID run-queue. pc64 is a single-address-space cooperative OS with no
preemptive processes, so there was nothing there to enumerate.

**Shipped.** The enumerable run-set is the shell's OPEN app slots — the same set
the F11 profiler / `unoauto_probe` report — promoted from the `UNO_DEBUG`-only
PROBE accessors to production, exactly as §1/§2 un-gated the ui/app accessors:
- **Shell (`pc64_uui.c`):** `pc64_shell_app_open(idx)` / `pc64_shell_app_name(idx)`
  / `pc64_shell_app_is_focused(idx)` (production), over the existing
  `g_open`/`app_name`/`focused_app` state the launcher & taskbar already use.
- **unoscript (`unoscript.c`):** `usc_proc_list` walks `0..pc64_shell_app_count()`,
  emitting a `usc_proc_ent` per OPEN slot — `pid` = slot index (stable for a
  boot), `tid` = 0 (single cooperative thread), `state` bit0 = focused,
  `name` = app title, `owner` = `unosec_current_user()`. `usc_proc_inspect(pid)`
  returns the one row (or `USC_EINVAL` for a closed/out-of-range pid). `v1/v2`
  (cpu-ms/stack) are 0 — per-app draw cost lives only in the UNO_DEBUG profiler,
  so it is not part of the production surface.

The `u.proc.list()` Python binding (`mod_unoscript.c`) and the kExport already
landed with §1/§2 — no seam edit was needed.

**Verified** (`tools/unoscript_qemu.py`, QEMU over URC): `u.cap_tier('proc.enum')`
== 2; unescalated `u.proc.list()` raises `OSError: EPERM: capability denied` (the
guard, no longer the `NotImplementedError` "surface not wired" stub); `app.count`
unregressed; launching an app makes it visible to a fresh PROBE (the enumeration
source is live). prod + debug link green.

**Deferred (not part of this surface):** the thread→session binding `unosecure`
requested (`unosec_enter_session(task->sec_session)` on context switch) only
matters once **concurrent** scripted tasks exist; today one script runs at a
time and `unosec`'s enter/leave around the script body covers it. The
authenticated "returns the right rows once escalated" end-to-end assertion is the
cross-cutting deferred gate below; the C-level PROC_ENUM escalation flip is
already proven by `unosec_selftest` (`-DUNO_SECTEST`).

**Unlocked:** `u.proc.list()` and per-process inspection.

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
