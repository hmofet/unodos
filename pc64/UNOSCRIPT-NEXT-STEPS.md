# unoscript, next steps (surface wiring roadmap)

**As of 2026-07-23.** `unoscript` (the production Python OS-scripting surface) and
`unosecure` (identity/RBAC/escalation) are both landed, in `build.sh`, and
verified: production build links green, `unosec_present()` → 1, the `unosec_*`
contract matches `unoscript.h` exactly, and the weak fail-closed gate is overridden
by the real adjudicator. See `UNOSCRIPT.md` (design) and `UNOSECURE.md`.

**What that means:** the *privilege gate is live and real*. What's left is
**per-subsystem surface wiring**: each scripted operation delegates to the owning
subsystem, and until that owner exposes its accessor the op returns
`USC_EUNAVAIL` (Python `NotImplementedError`) even when the gate *permits* it.

**One surface is already wired:** `usc_power(0)` (shutdown) consumes the existing
production `uno_pc64_shutdown()`. It's the end-to-end proof the stack works on a
live surface.

This doc is the dispatch list: one section per owning agent, with the exact
accessor `unoscript` will call and the capability it lights up. `unoscript`
implements none of these, it wires its `usc_*` entry point to your accessor once
you land it. When you do, ping back via `UNOAUTOMATE-REQUESTS.md` and the
`unoscript` agent wires the one-line delegation + drops the `USC_EUNAVAIL` stub.

---

## Priority order

1. ~~**unoui, synthetic input + screen read** (tier 0).~~ **DONE** (2026-07-23) -
   `ui.pointer/key/screen_text/clipboard_get/clipboard_set` are wired to real
   platform/shell seams and QEMU-verified over URC. UI automation works. See §1.
2. ~~**shell, app control** (tier 0/1).~~ **DONE** (2026-07-23) -
   `app.count/launch/close_top` (tier 0) + `app.message` (tier 1) wired and
   QEMU-verified over URC. See §2.
3. ~~**process enumeration** (tier 2).~~ **DONE** (2026-07-25), `proc.list`/
   `proc.inspect` enumerate the shell's open-app run-set (pc64 has no preemptive
   scheduler; `unosched` is the concurrency-primitive lib, not a run-queue), via
   production `pc64_shell_app_open`/`_name`/`_is_focused`. QEMU-verified
   (`tools/unoscript_qemu.py`): wired + tier-2 gated. See §3. The thread→session
   binding remains a `unosecure` follow-up for when concurrent scripted tasks exist.
4. ~~**user-scoped file IO** (tier 1).~~ **DONE** (2026-07-25), `fs.read`/
   `fs.write` with a per-uid home (`USERS/<uid>/`, bare relative paths) + absolute
   `/vol/path` (fs.sys), composed in `unoscript.c` from the existing `uno_fs_*`
   primitives. Traversal-safe path logic host-tested (`unoscript_path_test.c`);
   wired + gated over URC (`unoscript_qemu.py`). See §4.
5. ~~**kernel, mem / io / reboot / syscall** (tier 2/3).~~ **DONE** (2026-07-25)
, `mem.read/write` (single-AS peek/poke, pid 0), `io.in_/out` (raw port I/O
   via new `uno_native_port_*`), `sys.power` 1=reboot (`uno_native_reset`);
   2=suspend is `USC_EUNAVAIL` (no ACPI S3); syscall/unsigned-module-load left as
   the tier-3 "later". Wired + gated over URC with inert probes
   (`unoscript_qemu.py`). See §5.
6. ~~**unoauto (self), production HOOK registry** (tier 2).~~ **DONE**
   (2026-07-25), resolved as **debug-only by decision**: production `hook.add`
   is `USC_EUNAVAIL` (a script tap on the `libc.malloc` fire point is a hot-path
   cost + reentrant), while a UNO_DEBUG build wires the real `unoauto_hook`
   registry with a safe LOG-emitting shim over the fixed fire set. See §6.

**All six surface-wiring steps are DONE.** Every `usc_*` surface is wired or a
documented non-goal, none is a bare stub. What remains is the cross-cutting
end-to-end authenticated gate (below), not surface wiring.

---

## 1. unoui, synthetic input, screen read, clipboard  ·  tier 0  ·  DONE (2026-07-23)

**Was:** `uno_pc64_inject_key/_pointer` existed but were `#ifdef UNO_DEBUG`, and
`unoscript` is production, so it could not call them; there was no screen-tree
accessor and no system clipboard.

**Shipped** (production, gated only at the unoscript/unosecure layer):
- **Input:** the debug injectors lost their `UNO_DEBUG` gate, `uno_pc64_inject_key`
  / `uno_pc64_inject_pointer` (uefi_main.c) are now production, still on the exact
  `map_key` / clamp+`pointer_moved_clicked` path real device input uses.
  `usc_ui_pointer`/`usc_ui_key` call them (unoscript.c).
- **Screen read:** `pc64_shell_screen_text(out, cap)` (pc64_uui.c) renders the
  window-tree text, one line per open window, the focused one marked `*`: not a
  pixel grab. Wired to `usc_ui_screen_text`.
- **Clipboard:** a small shell-owned text buffer with `pc64_shell_clip_set/get`
  (there was no system clipboard; apps keep their own). Wired to
  `usc_ui_clipboard_set` (tier-1 `clipboard.write`) / `_get` (tier-0 `ui.read`).
  Apps don't consume it yet, integrating WordPad/others' copy-paste is a
  follow-up.

**Verified** (QEMU, ui_automation_test over URC): launch an app → `ui.screen()`
shows its live title (focused-marked); `ui.click`/`ui.key` return `USC_OK` on the
real input path; `ui.clip_get()` reads (tier-0); `ui.clip_set()` is refused with
no session (tier-1 gate). Unlocked `unoscript.ui.click/move/key/screen/clip_get/clip_set`.

**Follow-ups (not blocking):** `ui.key`'s modifier arg currently maps any nonzero
`mods` to the single Cmd/Ctrl modifier (`map_key` takes one modifier); a full
modifier bitmap is a later refinement. App-integrated clipboard as above.

## 2. shell, app enumeration, launch/close, messaging  ·  tier 0/1  ·  DONE (2026-07-23)

**Was:** `pc64_shell_app_count/launch/close_top` were `#ifdef UNO_DEBUG`, and
there was no app-message seam.

**Shipped** (production, gated at the unoscript/unosecure layer):
- The debug DRIVE accessors lost their gate, `pc64_shell_app_count` /
  `pc64_shell_launch` / `pc64_shell_close_top` (pc64_uui.c) are now production,
  still the exact launcher-click / title-bar-close paths (EX_PYAPP/EX_USERAPP
  refused to launch so the running script isn't displaced). Wired to
  `usc_app_count`/`usc_app_launch`/`usc_app_close_top` (tier 0, `app.ctrl`).
- `pc64_shell_app_message(idx, msg, reply, cap)` (pc64_uui.c), a minimal v1 verb
  set by app index: `info` (name/open/focused), `focus`, `close`, reusing
  `open_app`/`raise_win`/`close_focused`. Wired to `usc_app_message` (tier 1,
  `app.msg`) + a new `unoscript.app.message(idx, verb)` Python binding
  (mod_unoscript.c, exported in pc64_modload.c). The message is a plain verb
  string, not JSON yet, and the verbs are generic (shell-level), a per-app
  message handler for app-specific verbs (AppleScript "tell Notepad to insert…")
  is the natural follow-up.

**Verified** (QEMU, app_control_test over URC): `app.count()`=19; `app.launch(1)`
opened Editor (seen in `ui.screen()`); `app.close_top()` returned USC_OK and the
window left the tree; `app.message()` was refused with no session (tier-1 gate).

**Unlocked:** `unoscript.app.count/launch/close_top/message`.

## 3. process enumeration + inspect  ·  tier 2  ·  `proc.enum` / `proc.inspect`  ·  DONE (2026-07-25)

**Premise correction.** §3 originally named `unosched` as the owner of an
`unosched_enumerate(usc_proc_ent*, int)` seam. `unosched/` is in fact the tiered
**concurrency-primitive library** (COOP floor + SMP sync/`uno_job` offload), it
has no PID/TID run-queue. pc64 is a single-address-space cooperative OS with no
preemptive processes, so there was nothing there to enumerate.

**Shipped.** The enumerable run-set is the shell's OPEN app slots, the same set
the F11 profiler / `unoauto_probe` report, promoted from the `UNO_DEBUG`-only
PROBE accessors to production, exactly as §1/§2 un-gated the ui/app accessors:
- **Shell (`pc64_uui.c`):** `pc64_shell_app_open(idx)` / `pc64_shell_app_name(idx)`
  / `pc64_shell_app_is_focused(idx)` (production), over the existing
  `g_open`/`app_name`/`focused_app` state the launcher & taskbar already use.
- **unoscript (`unoscript.c`):** `usc_proc_list` walks `0..pc64_shell_app_count()`,
  emitting a `usc_proc_ent` per OPEN slot, `pid` = slot index (stable for a
  boot), `tid` = 0 (single cooperative thread), `state` bit0 = focused,
  `name` = app title, `owner` = `unosec_current_user()`. `usc_proc_inspect(pid)`
  returns the one row (or `USC_EINVAL` for a closed/out-of-range pid). `v1/v2`
  (cpu-ms/stack) are 0, per-app draw cost lives only in the UNO_DEBUG profiler,
  so it is not part of the production surface.

The `u.proc.list()` Python binding (`mod_unoscript.c`) and the kExport already
landed with §1/§2, no seam edit was needed.

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

## 4. user-scoped file IO  ·  tier 1/2  ·  `fs.user` / `fs.sys`  ·  DONE (2026-07-25)

**Approach correction.** §4 originally sketched a new `unofs_read_as(uid, path,
…)` seam in the unofs owner. In the event no new unofs API was needed: pc64's
existing `uno_fs_*` volume primitives (`uno_fs_read`/`_write`/`_mkdir`/`_kind`/
`_writable`/`_volume_name`) already carry everything, so, exactly as §3 composed
the proc surface from the shell's app primitives, `usc_fs_read`/`usc_fs_write`
compose identity + scope on top of them in `unoscript.c`. In-lane, no cross-team
request.

**Path scheme + scope (decided 2026-07-25):**
- **Bare relative path** (`notes/todo.txt`) → the acting user's home,
  `USERS/<uid>/notes/todo.txt` on the **primary writable native-FAT volume**
  (`fs_user_vol()`), always `fs.user` (tier 1). Parent dirs (`USERS/`,
  `USERS/<uid>/`, script subdirs) are auto-provisioned on write via idempotent
  `uno_fs_mkdir`.
- **Absolute `/label/rest`** → the volume whose `uno_fs_volume_name` matches
  `label` (case-insensitive), name = `rest`. `fs.sys` (tier 2) **unless** it is
  the acting user's own home path on the user volume.
- `..`, `.`, `//` are **rejected** (`USC_EINVAL`), a relative path structurally
  cannot leave its home; the absolute form is the only escape hatch, and it is
  `fs.sys`. This traversal/scope logic is the pure `unoscript_path.c`.

**Guarding:** `usc_fs_*` guards `fs.user` (the floor) first, so an
unauthenticated/tier-0 caller is denied before the path even resolves, then a
resolved sys path **re-guards** `fs.sys`. Mirrors the model this section
originally described.

**Verified:**
- **Host** (`tools/unoscript_path_test.c`, seconds, no QEMU): the traversal
  rejection, exact `USERS/<uid>/` construction, home-membership test (uid 1 must
  not match uid 12's home), and the `/label/rest` split. This is the
  security-critical layer.
- **QEMU** (`tools/unoscript_qemu.py`, over URC): `fs.user`==tier 1,
  `fs.sys`==tier 2; `fs.read`/`fs.write` on both a home and a `/vol` path raise
  `OSError: EPERM: capability denied` unescalated (the guard, not the old
  "surface not wired" stub). prod + debug link green.

**Bounds / deferred:** `uid` is a FAT 8.3 dir component (fine for realistic
uids); the read binding caps at 4 KB (streaming is a follow-up); an ACL model
keyed on `unosecure` uids beyond the home subtree, and the authenticated
read/write **round-trip** through the guarded surface (needs a logged-in
session), are the deferred end-to-end gate. The FAT subdir write path itself is
pre-existing/proven.

**Unlocked:** `u.fs.read/write`.

## 5. kernel, memory, port IO, reboot/suspend, syscall  ·  tier 2/3  ·  `mem.*` / `io.*` / `power`  ·  DONE (2026-07-25)

**Reality on pc64.** The original sketch assumed a multi-address-space kernel
with per-task page tables and a distinct "kernel accessor" layer. pc64 is a
single-address-space cooperative kernel (identity-mapped, ring 0), so the
accessors collapse to their essence and, like §3/§4, compose in `unoscript.c`
onto existing/tiny platform primitives, no separate kernel-agent seam:

- **`mem.read/write(pid, addr, buf, len)`**: a bounded `memcpy` on the one flat
  address space. `pid` 0 = that space; any other pid is `USC_EINVAL` (there are
  no page tables to translate through). NULL addr / non-positive len refused.
  There is no MMU protection to enforce safety, an unmapped addr faults the
  machine, exactly as a kernel-mode peek would, which is precisely why this is
  KERNEL tier and always audited.
- **`io.in_/out(port, width, …)`**: raw x86 port I/O. Added exported
  `uno_native_port_in`/`uno_native_port_out` (widths 1/2/4 bytes) to
  `pc64_native.c` (the platform lane), since the existing `n_inb`/`n_outb` were
  file-local `static inline`. `port>0xFFFF` or a bad width is `USC_EINVAL`.
- **`sys.power`**: `0` shutdown (already wired) + `1` reboot
  (`uno_native_reset`, CF9 hard reset) are live; `2` suspend is `USC_EUNAVAIL`
  (pc64 implements no ACPI S3). The **syscall-tap + unsigned-module-load** paths
  named "tier 3, later" remain out of scope, there is no `usc_syscall` entry
  point, and an unsigned module loader is a deliberate non-goal for now.

**Verified** (`tools/unoscript_qemu.py`, over URC): the caps carry the right
tiers (mem/io.write = KERNEL 3, io.read/power = ADMIN 2), and every op raises
`OSError: EPERM: capability denied` unescalated (the guard, not the old "surface
not wired" stub). The denial probes are deliberately **inert even if the gate
failed**: reads, a POST-port `0x80` write, an addr-0 write (rejected by
validation), `power(2)` (a no-op), so the gate can never poke live memory or
reboot the VM. prod + debug link green.

**No positive round-trip, by design.** mem-poke, port-out and reboot are
destructive/irreversible; there is nothing safe to assert once *executed*, so
authority-gated execution is confirmed only through a real logged-in session,
never the automated gate.

**Unlocked:** `u.mem.read/write`, `u.io.in_/out`, `u.sys.power(0|1)` (2 = EUNAVAIL).

## 6. HOOK registry  ·  tier 2  ·  `hook`  ·  DONE (2026-07-25), debug-only by decision

The call to make (the section framed it as the `unoscript`/`unoauto` agent's own):
expose a slim production tap facility, **or** keep `hook` debug-only with
`usc_hook_add` → `USC_EUNAVAIL` in production.

**Decision: keep it debug-only.** A production, script-visible tap on the existing
fire points is the wrong design, the points include `libc.malloc` (pc64_libc.c),
so a tap is (a) a hot-path cost on *every* allocation and (b) reentrant (the
observer allocates inside the allocator). Production already compiles the whole
`unoauto_hook_*` machinery away (unoauto.h no-op macros); `usc_hook_add` matching
that with `USC_EUNAVAIL` is honest, not a deferral.

**But debug-only ≠ dead.** In a `UNO_DEBUG` build `usc_hook_add(point)` wires the
real bounded, allocation-free `unoauto_hook` registry with a **safe LOG-emitting
shim**: no Python callback runs in kernel/driver context; the shim just emits
`hook: <point>` on the SCRIPT LOG channel. So a debug script does
`hook.add("fs.write")` and watches file writes stream over URC. The tappable set
is exactly the fire sites (`fs.read`/`fs.write`, `libc.malloc`,
`mod.load`/`mod.unload`, `uui.action`); the **stable literal** is handed to the
registry (it stores the pointer, so a transient Python string can't be used),
which doubles as validation, an unknown point is `USC_EINVAL`.

**Verified** (`tools/unoscript_qemu.py`, over URC): `hook` is tier 2 and
`hook.add` is denied unescalated (the guard, not the old "surface not wired"
stub). prod + debug link green.

**Unlocked:** `u.hook.add/remove` (debug builds; production reports unsupported).

---

## Cross-cutting follow-ups (unoscript agent)

- ~~**End-to-end Python gate.**~~ **DONE** (2026-07-25), `u.e2e()`
  (`unoscript_e2e_selftest`, debug-only) drives the surfaces through a REAL
  `unosecure` login and proves the POSITIVE path the wired+gated gate can't reach
  unauthenticated: as a **guest** `fs.read` is DENIED, `unosec_request` grants
  `fs.user` (interactive USER-tier), then `fs.write`+`fs.read` **round-trip** 32
  bytes exactly; under a dev **AUTOGRANT** policy `proc.list` returns the running
  apps and `io.in` reads a port; and `mem.read` (KERNEL) **stays denied**: autogrant covers ≤ADMIN only. Runs entirely in C under a genuine session (no
  eval-context tricks), on a throwaway account it deletes afterward, with a
  headless deny-consent provider swapped in for the run (the interactive sheet
  would block a headless run) and the UI provider restored. Driven over URC by
  `tools/unoscript_qemu.py` (asserts `u.e2e()==0`). This is the Python-layer
  counterpart to `unosecure`'s `-DUNO_SECTEST` C gate, and it retroactively gives
  every surface its positive round-trip proof.
- ~~**Manifest-declared caps.**~~ **DONE** (2026-07-26), a launched Python app
  now runs under an isolated `unosecure` session (acting user, INSTALLED trust),
  opened by `unoscript_app_caps_begin(vol, path)` and logged out by
  `unoscript_app_caps_end()` (bracketed in `pc64_shell_run_python` / the pyapp
  close path). If the app ships a `<base>.MFT` sidecar signed by a trusted key,
  `unosec_manifest_apply` grants its declared caps SESSION-scoped at launch, so a
  trusted automation app never prompts per op; on close the session (and its
  grants) is destroyed. Signing keys are enrolled from a `TRUST.MFK` file at boot
  (`unoscript_trust_boot`) or live via `unosec_trust_add_key`. Host tool:
  `tools/uno_manifest.py` (keygen + sign). Gate: `u.mtest()`
  (`unoscript_mtest`), a signed manifest grants a guest `proc.enum` at launch,
  the grant is dropped on close, and a forged signature grants nothing;
  `tools/unoscript_qemu.py` asserts `u.mtest()==0`.
- **Docs.** As each surface lands, add a worked example to `UNOSCRIPT.md` and the
  user manual (the manual is a standing update rule for pc64 features).

## How to pick this up

Each numbered section is self-contained and dispatchable to that subsystem's
agent. Hand it the section; the accessor signature is the contract. When it lands,
note it in `UNOAUTOMATE-REQUESTS.md` and the `unoscript` agent wires the delegation
(a one-line change per entry) and removes the `USC_EUNAVAIL` stub. No `unoscript`
or `unosecure` change is needed to *prepare* for your accessor, the gate and the
Python bindings are already in place waiting for it.
