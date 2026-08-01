# SSH client + unoui tabs/MDI, implementation spec (worker brief)

Status: **SPEC**, 2026-07-31. No phase started. The design rationale is in
[`SSH-CLIENT-PLAN.md`](SSH-CLIENT-PLAN.md); this document is the build order.
Where the two disagree, THIS file wins.

The claim is filed: `pc64/UNOAUTOMATE-REQUESTS.md`, entry "2026-07-31 - CLAIM
(new lane: unossh) + toolkits lane: tabs and MDI". Do not re-file; do append
your own dated progress notes there as phases land.

## 0. Read first, in this order

1. `/AGENTS.md` - the working agreement. Read it before anything else.
2. `docs/SSH-CLIENT-PLAN.md` - the design and its reasoning, especially §2
   (what crypto we have and the one thing we do not) and §3 (why MDI is a
   widget and not a window tree).
3. For the UI lane: `unoui/unoui.h`, `unoui/unoui_theme.h`, then
   `unoui/unoui.c` (`d_tabs` :775, `draw_one` :949, `render_one_window` :1293,
   `unoui_render_window_chrome` :1281, and the `unoui_list_*` geometry split as
   your model) and `unoui/unoui_input.c` (`press_widget` :398, the `UI_TABS`
   case :467, `hit_widget` :129).
4. For the protocol lane: `pc64/bearssl/inc/*.h` (`bearssl_ec.h`,
   `bearssl_hash.h`, `bearssl_hmac.h`, `bearssl_block.h`, `bearssl_rand.h`),
   `pc64/tls_entropy.h`, `pc64/netsock.h`, and `pc64/tls.c:140-210` as the
   worked example of seeding a DRBG from `tls_entropy_get()`.
5. `pc64/harness.py` - the QEMU gate you will extend. Read its docstrings.

## 1. Ground rules

- **Two lanes.** The **unossh lane** (a NEW subsystem) owns `pc64/unossh*`,
  `pc64/ed25519.*`, `pc64/sshapp_*`. The **toolkits lane** owns `unoui/*` and
  the browser refactor. They share no files until phase `ssh-f`.
- **Register the new subsystem before you write it.** AGENTS §1: adding a new
  subsystem means adding its row to the ownership registry *in the same
  commit*. Phase `ssh-a` adds `| unossh (SSH client + key store) |
  docs/SSH-CLIENT-SPEC.md, unossh.h | unossh*, ed25519*, sshapp_* |`. Rebase
  first - `AGENTS.md` has a stray `=======` on line 52 that may be being
  removed concurrently.
- **Branches.** One worktree + branch per phase, off `origin/master`:
  `git worktree add ../unodos-<phase> -b <phase> origin/master`. Rebase at
  session start. Land each phase, delete branch + worktree the same day. Never
  merge one phase branch into another; if N+1 needs unlanded N, land N first.
- **Merge gate, every phase**: rebased on `origin/master`; `cd pc64 &&
  ./build.sh` green at `UNO_DEBUG=0` **and** `UNO_DEBUG=1`; `cd unoui &&
  ./build.sh` green (the host contact sheet renders all ten themes and catches
  layout breakage); the phase's own gate below passes; no choke-point
  restructure.
- **Choke points are append-only.** `pc64/build.sh`'s file list,
  `pc64_modload.c`'s `KX()` arrays, `pc64_uui.c`'s app tables,
  `uno_pc64_init`'s boot wiring, `REMOTE.md`'s verb table. Each such touch is
  its own `seam:`-prefixed commit, separate from the lane commit.
- **unoui ABI discipline.** `unoui.c` and the themes also build into the PS2
  port, the Dreamcast port and the host demo. Every struct change is
  APPEND-AT-END, and **zero must always mean "feature absent, behaviour
  identical to today"**. Themes use positional initialisers, so an appended
  metrics field reads 0 in every theme you do not edit.
- **Style.** C, `/* */` comments, declarations at block top, 4-space indent,
  `snake_case`, `g_` file-scope globals, ASCII only. Comments state constraints
  and invariants, not narration. No new libc dependencies.
- **Host harnesses are built with the OS's sanitizer flags.** Non-negotiable,
  and the reason is in the plan §2: `-fsanitize=signed-integer-overflow,bounds,
  shift,integer-divide-by-zero,null`, leaving `-fsanitize-undefined-trap-on-error`
  OFF on the host so you get a file and line rather than a SIGILL.

## 2. Phase order

The two lanes run concurrently. Within a lane, order is strict.

| Phase | Lane | What |
|---|---|---|
| `tabs-a` | toolkits | `UI_TABS` becomes a real tabbed-document control + public geometry split |
| `tabs-b` | toolkits | `UI_MDI` container widget |
| `tabs-c` | toolkits | browser refactored onto `tabs-a` |
| `ssh-a` | unossh | Ed25519, host-tested against RFC 8032 vectors |
| `ssh-b` | unossh | transport: version exchange, `curve25519-sha256` KEX, `aes256-ctr` + `hmac-sha2-256`, rekey |
| `ssh-c` | unossh | userauth (publickey, password) + session channel, exec and shell |
| `ssh-d` | unossh | key store + session store, persistent |
| `ssh-e` | unossh | the unoautomate verb |
| `ssh-f` | both | the GUI app - needs `tabs-a`, `tabs-b`, `ssh-c`, `ssh-d` |

## 3. `tabs-a` - the tabbed-document control

**Do not add a second widget kind.** Extend `UI_TABS` with opt-in flags, all
off by default, so the Control Panel (`pc64_uui.c:530`, the only current
consumer) is byte-identical with no edit.

New contract surface. All names are final; do not rename.

| Addition | Where | Notes |
|---|---|---|
| `UI_TF_CLOSE 1`, `UI_TF_PLUS 2`, `UI_TF_ELASTIC 4`, `UI_TF_OVERFLOW 8` | `unoui.h` | per-widget flags, appended to the `UI_WF_*` space |
| `unoui_tabs_model` | `unoui.h` | `{ const char *const *labels; int n, sel, hot, hot_part, first, flags; }` |
| `unoui_tabs_h(const unoui_theme *)` | `unoui.h` | strip height; today's `ui_tab_h()` made public |
| `unoui_tab_rect(theme, unoui_rect, const unoui_tabs_model *, int i)` | `unoui.h` | ONE source of per-tab geometry |
| `unoui_tabs_draw(theme, unoui_rect, const unoui_tabs_model *)` | `unoui.h` | the painter, callable from a canvas |
| `unoui_tabs_hit(theme, unoui_rect, const unoui_tabs_model *, int x, int y, int *which)` | `unoui.h` | returns `UI_TAB_*`, writes the index |
| `UI_TAB_NONE 0`, `UI_TAB_SEL 1`, `UI_TAB_CLOSE 2`, `UI_TAB_PLUS 3`, `UI_TAB_OVER 4` | `unoui.h` | hit results |
| `UI_ACT_TABCLOSE 9996`, `UI_ACT_TABNEW 9995` | `unoui.h` | same contract as `UI_ACT_CLOSE`: `id` = widget id, `value` = tab index |

Requirements:

- **The geometry split is the point.** `unoui_tabs_draw` and `unoui_tabs_hit`
  must both derive every rect from `unoui_tab_rect`, so a click can only land
  where a tab was drawn. Today's code re-derives `fb_text_w(items[i]) + 16`
  independently in two places (`unoui.c:780` vs `unoui_input.c:471`); that class
  of bug must be impossible when you are done. This is exactly what
  `unoui_titlebtn_rect` did for the WM titlebar buttons.
- **Labels stay app-owned.** The model holds `const char *const *`; unoui never
  copies or frees a label. This is what lets the browser pass pointers into its
  own mutable `char title[48]` storage.
- `UI_TF_ELASTIC` divides the width evenly, clamped to a min and max, instead of
  sizing to text. `UI_TF_OVERFLOW` adds a `>>` control when tabs do not fit and
  scrolls via `model.first`.
- Colours come from the palette only (`face`, `win_bg`, `accent`, `text`,
  `text_dim`). No literal `FB_RGB()` in the painter.
- The theme override slot `unoui_draw.tabs` keeps working; a theme that
  overrides it and knows nothing of the flags must still render correctly for
  the Control Panel's flagless use.

**Gate**: `unoui/build.sh` contact sheet shows the Control Panel's strip
unchanged on all ten themes, plus a new storyboard frame exercising close/+/
overflow. Add a host-side hit-test assertion (the `unoui/host_unoui_input.c`
harness) that walks every tab index and asserts draw and hit agree.

## 4. `tabs-b` - the MDI container

A new widget kind `UI_MDI` whose rect hosts a list of child frames. Read plan
§3 for why this is not a window tree; do not add parent pointers to
`unoui_window`.

| Addition | Where | Notes |
|---|---|---|
| `UI_MDI` | `unoui.h` `ui_kind` | **append at the end of the enum** |
| `unoui_mdi_child` | `unoui.h` | `{ unoui_rect r; const char *title; int flags; unoui_canvas *canvas; int used; }` |
| `unoui_mdi` | `unoui.h` | `{ unoui_mdi_child *ch; int n, cap, focus; int z[UNOUI_MDI_MAX]; }` |
| `unoui_add_mdi(win, x, y, w, h, unoui_mdi *m, int id)` | `unoui.h` | app owns the `unoui_mdi` |
| `unoui_mdi_tile(unoui_mdi *, unoui_rect, int mode)` | `unoui.h` | 1/2/4-up and an n>4 grid, same policy as the shell's tiling |
| `unoui_mdi_cascade(unoui_mdi *, unoui_rect)` | `unoui.h` | |
| `unoui_draw_frame_chrome(theme, unoui_rect, const char *title, int active, int flags)` | `unoui.h` | factored OUT of `render_window_chrome`, which then calls it |

Requirements:

- **Reuse the chrome painter.** `unoui_draw_frame_chrome` is a refactor, not new
  artwork: `render_window_chrome` (`unoui.c:1269-1279`) keeps its current
  behaviour by calling it. MDI children then get correct per-theme frames,
  title bars and close boxes on all ten themes for free.
- Children are clipped to the container rect, always. Drag a child by its title
  bar, clamped inside the container; press-to-raise; close box; optional resize
  grip when the child sets the resize flag.
- Child z-order is the `z[]` list inside `unoui_mdi`, entirely local. The
  top-level `ui->win[]` array and its three pin bands are **untouched**.
- Zero-initialised `unoui_mdi` means "no children", and a window with no `UI_MDI`
  widget behaves exactly as today.
- **Learn E's lesson about bss-shaped state**: a `z[]` of app indices
  "terminated by -1" is a trap because 0 is a valid index and a bss array reads
  as all zero. Store index+1 terminated by 0. This cost the WM lane a
  mid-gate reboot and a run of nonsense assertions (correction 21).

**Gate**: `unoui/build.sh` storyboard frames - three children cascaded, tiled
2-up and 4-up, one focused, on at least Aurora Dark and the flat Win 3.1
palette. Host harness asserts raise order and that a drag clamps at the
container edge.

## 5. `tabs-c` - the browser refactor

The browser is one `UI_CANVAS` (`pc64_browser.c:1990`), so it hosts the tab
control through the `tabs-a` geometry functions rather than as a widget.

Delete and replace:

| What | Lines today |
|---|---|
| tab-strip geometry `tab_width` / `tab_rect` / `tab_plus_rect` | `:1246-1272` |
| `draw_tabs` | `:1362-1392` |
| hover hit-test | `:1842-1856` |
| click hit-test | `:1912-1926` |
| `band_tabs` (disappears; `band_bar`/`band_body` re-origin) | `:1198-1208` |

Requirements:

- Build a dense `const char *labels[]` of pointers into the existing
  `g_tab[i].title` each frame. Do not change `btab`'s storage in this phase.
- The `- 18` magic number for the close zone (`:1917`, `:1852`) goes away
  entirely - `unoui_tabs_hit` returns `UI_TAB_CLOSE`.
- Ctrl-T and Ctrl-F4 (`pc64_browser_key` :1979, :1986) keep working unchanged.
- **The chrome palette is a decision, not a discovery.** `CH_FACE`/`CH_EDGE`/
  `CH_TEXT`/`CH_DIM`/`CH_ACTIVE`/`CH_HOT` (`:1296-1301`) are literal colours
  used by the toolbar and panels too. Either move all of the browser chrome onto
  palette roles in this phase, or keep `CH_*` for the toolbar and accept a
  deliberately half-themed browser - and say which you chose, and why, in the
  landing note. Do not leave it accidental.
- `MAXTABS 6` and the sparse `used` array may become dense in this phase if it
  simplifies the diff; if you do, `tab_new`/`tab_close` (`:954-985`) are the
  only lifecycle points.

**Gate**: a `harness.py browser_tabs` scenario, pointer-driven on
`UNO_DETACH=1 UNO_DEBUG=1`: open three tabs with `+`, switch by clicking, close
the middle one by its close box, assert the right tab survived and the right
document is showing. Plus the existing default `harness.py` pass as a
regression.

## 6. `ssh-a` - Ed25519

New files `pc64/ed25519.c` / `.h`. Pure computation: no allocation, no OS calls,
no network. Public surface:

```c
void ed25519_pubkey(unsigned char pk[32], const unsigned char sk[32]);
void ed25519_sign(unsigned char sig[64], const unsigned char *m, int mlen,
                  const unsigned char pk[32], const unsigned char sk[32]);
int  ed25519_verify(const unsigned char sig[64], const unsigned char *m,
                    int mlen, const unsigned char pk[32]);   /* 1 = good */
```

SHA-512 comes from BearSSL (`br_sha512_*`). Everything else is yours.

**Gate**: `pc64/tools/ed25519test.c`, built on the host with the sanitizer flags
above, passing every RFC 8032 §7.1 test vector plus a sign/verify round trip
over random messages and a corrupted-signature rejection test. **This phase does
not land until those vectors pass.** Nothing downstream may start against an
unverified implementation.

## 7. `ssh-b` - the transport

New files `pc64/unossh.c`, `unossh_kex.c`, `unossh.h`. Sits on `netsock.h`
(`net_socket`/`net_connect`), **not** on `pc64/tls.*` - see plan §2.

- Version exchange, binary packet protocol, packet padding, `SSH_MSG_*` framing.
- `curve25519-sha256` key exchange via `br_ec_c25519_*`, exchange hash H over
  SHA-256, session ID, key derivation per RFC 4253 §7.2.
- `ssh-ed25519` host key verification via `ssh-a`.
- `aes256-ctr` (`br_aes_ct64_ctr_*`) + `hmac-sha2-256` (`br_hmac_*`),
  encrypt-then-MAC ordering as the protocol specifies.
- Randomness: seed a `br_hmac_drbg` from `tls_entropy_get()`, exactly as
  `tls.c:149,205` does. **`tls_entropy_get()` returns 0 when there is no
  source, and the caller must refuse to continue** - never fall back to
  anything else, and specifically never to `Random()` in `mac_compat.c:285`,
  which is a 32-bit LCG.
- Rekey on the usual thresholds.
- Write it so the exchange-hash inputs can be dumped under `UNO_DEBUG` - a
  wrong H is otherwise an opaque disconnect, and this is the single most likely
  place to lose a day.

**Gate**: connect to a real `sshd` and complete the key exchange. Run
`sshd -ddd -p 2222` on the host (WSL is fine); QEMU user-mode networking reaches
the host at `10.0.2.2`, so this needs no LAN and no other machine, and the
server's verbose log states precisely what it disliked. Assert over URC that the
session ID and negotiated algorithms are what we expect.

## 8. `ssh-c` - auth and channels

`unossh_auth.c`, `unossh_chan.c`.

- `ssh-userauth` service request; `publickey` (Ed25519 first, RSA via
  `br_rsa_pkcs1_*` after) and `password`.
- Session channel: open, window adjust, `exec` and `shell` requests, `pty-req`,
  data and extended-data, EOF and close.
- Public API in `unossh.h`, handle-based, `SSH_MAXCONN 4`:
  `ssh_connect`, `ssh_auth_key`, `ssh_auth_password`, `ssh_exec`, `ssh_shell`,
  `ssh_read`, `ssh_write`, `ssh_poll`, `ssh_exit_status`, `ssh_close`,
  `ssh_last_error`. **Non-blocking**, driven from the shell's frame loop - the
  GUI must never stall the desktop waiting on the network.

**Gate**: `ssh_exec` a command on the same test `sshd` and assert both its
output and its exit status, over URC.

## 9. `ssh-d` - key and session stores

`unossh_keys.c`, `unossh_sess.c`.

- Generate, import, export, list, delete. Public keys read/write the standard
  one-line OpenSSH format for interop.
- Private keys in our own container: PBKDF2-HMAC-SHA256 + AES-256-CTR, calling
  BearSSL directly. **Do not reuse `unosecure.c`'s PBKDF2 - it is `static`
  (`unosecure.c:166`).**
- Import unencrypted `openssh-key-v1` files. Encrypted ones need `bcrypt_pbkdf`,
  which we do not have: detect that case and say so plainly in the error rather
  than failing obscurely.
- Saved sessions - name, host, port, user, key reference, last-used - in a
  plain-text `SSH.CFG`.
- **Persistence trap, already paid for once.** The WM lane found that
  `session_save()` had been writing to "the first writable volume", which is the
  RAM disk, so no session had ever survived a power cycle. Choose the volume the
  way `unosecure.c`'s `pick_vol` does.

**Gate**: a key generated and a session saved both survive a power cycle, tested
on a real FAT image built with `tools/mkuefi.py` - **vvfat cannot carry a
persistence test**, it returned 50 bytes of garbage when the WM lane tried
(correction 13.x). Round-trip a real OpenSSH public key in and out and diff it
byte for byte.

## 10. `ssh-e` - the unoautomate verb

One entry point, per the weak-symbol pass-through pattern:

```c
int ssh_dbg_cmd(const char *line, char *out, int cap);
```

Read `unoauto_remote.c:712-730` (the pattern and its rationale) and the `eth`
dispatch clause at `:1008-1013` (four lines). `unossh` owns the sub-verb grammar
and output format completely; unoautomate lands one weak stub, one clause, one
`REMOTE.md` row and a `HARNESS-POLICY.md` changelog entry. **That is a request
to the unoautomate lane, not your edit** - file it in
`pc64/UNOAUTOMATE-REQUESTS.md` and ship the strong `ssh_dbg_cmd` on your side so
the tree links green either way.

Sub-verbs: `ssh key list|gen|import|export|rm`, `ssh sess list|add|rm`,
`ssh connect <sess|user@host>`, `ssh run <target> <cmd>`, `ssh get <id> <off>`,
`ssh close <id>`.

**The 8 KB constraint is a design input, not an afterthought.** `g_tx` is 8192
bytes and `tx_putn` **drops silently** past it (`unoauto_remote.c:109,129`).
Remote command output is unbounded, so `ssh run` returns an id and `ssh get`
retrieves offset slices, the way `readsec` and `screen read` already do
(`:774-781`).

**Gate**: over URC from `harness.py`, run a command on the test `sshd` and
retrieve more than 8 KB of output correctly - the slicing is the thing under
test, so make the payload deliberately large.

## 11. `ssh-f` - the GUI app

A native app, like UnoAmp: `pc64/sshapp_ui.c`, registered in `pc64_uui.c`'s app
tables (append: an enum slot, a `kAppNames` entry, a `g_build[]` builder). It is
**not** a `.UNO` module - plan §2 explains why (`br_*` and `netsock` are not in
`kExports[]`).

- Tabs from `tabs-a`, one per connection.
- MDI from `tabs-b` for split panes within a tab.
- A session manager pane over `ssh-d`: saved connections, connect, edit, delete.
- A key manager pane: list, generate, import, export, show fingerprint.
- Terminal output rendered with the existing monospace path
  (`pc64_shell_font_mono`).
- Driven from the frame loop via `ssh_poll` - never block.

**Gate**: `harness.py ssh_app`, pointer-driven on `UNO_DETACH=1 UNO_DEBUG=1`:
open the app, connect to the test `sshd` from a saved session, run a command,
see its output, open a second tab, close the first.

## 12. Harness notes that will otherwise cost you a day

All of these are recorded corrections from the WM programme. Read
`pc64/harness.py`'s docstrings before writing a scenario.

1. **`UNO_DEBUG=1` arms a fuzz driver.** `build.sh` stages a `DEBUG.CFG`, and
   its mere presence starts `pc64_stress.c` opening and closing apps from the
   shell's own main loop. It launched Studio in the middle of a drag.
   `quiet_debug_cfg()` rewrites it with `nostress`.
2. **vvfat cannot carry a persistence test** - it returned 50 bytes of garbage.
   Build a real FAT image with `tools/mkuefi.py` and boot that.
3. **QMP `screendump` is asynchronous.** Wait for the file to settle rather than
   sleeping; this retired a class of intermittent all-black frames.
4. **QEMU has no usable USB pointer** for this OVMF. Pointer-driven scenarios
   run `UNO_DETACH=1` with no USB pointer device, driving the machine's PS/2
   mouse over QMP. A held USB button is also currently lost - that is a filed
   defect against the usb lane.
5. **Popover geometry should be derived, not measured**, and every popover is
   opened, Esc'd away and opened again before the diff that measures it, because
   opening one repaints the losing window's title bar.

## 13. Corrections learned in flight

Append here as phases land, numbered, the way `WM-MODERN-SPEC.md` §13 does. A
correction is something this spec got wrong or did not anticipate - record it
even when the fix was obvious, because the next lane reads this section first.

*(none yet)*
