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
| `tabs-a` | toolkits | **DONE** - `UI_TABS` becomes a real tabbed-document control + public geometry split |
| `tabs-b` | toolkits | **DONE** - `UI_MDI` container widget |
| `tabs-c` | toolkits | **DONE** - browser refactored onto `tabs-a` |
| `ssh-a` | unossh | **DONE** - Ed25519, host-tested against RFC 8032 vectors |
| `ssh-b` | unossh | **DONE** - transport proven against a real OpenSSH 9.5 server |
| `ssh-c` | unossh | **DONE** - publickey auth + session channel, proven end to end against OpenSSH |
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

**1. The `UI_TF_*` values in §3's table were wrong.** They are written there as
1/2/4/8, which collide with `UI_F_DEFAULT`..`UI_F_HOT` in the very same `flags`
word. They are `1 << 14` .. `1 << 17`, sitting beside `UI_WF_FILL` (1<<12) and
`UI_WF_LIST_REVEAL` (1<<13). The model's `flags` field uses the SAME constants
rather than a shifted-down copy, so there is one representation to reason about
and a widget's flags can be handed to a model with a mask and nothing else.

**2. A widget keeps its scroll position in `value`.** §3 named the model field
`first` but never said where the widget-path equivalent lives. It is `value`,
exactly as a list's `top` is - and `draw_one` re-clamps it every frame the way
the `UI_LIST` case does, so an app that sets it out of range self-corrects.

**3. Four functions the contract table missed**, all of which turned out to be
load-bearing: `unoui_tabs_model_of` (the widget/model bridge, needed in both
`unoui.c` and `unoui_input.c`, so it cannot be static), `unoui_tabs_maxfirst`,
`unoui_tab_close_rect`, and `unoui_tabs_reveal`. **Reveal is not optional.**
Without it the arrow keys walk the selection to a tab that is scrolled out of
sight and nothing brings it back - the identical bug `UI_LIST` already fixed
with `unoui_list_reveal`, arrived at from the identical direction.

**4. "Does the set fit?" must be answered BEFORE the `first` clamp.** If the
overflow reserve is computed from the visible tabs, the `>>` control appears and
vanishes as you scroll, and because it is reserved out of the strip the usable
width changes underneath you at the same moment - the layout oscillates. The
fit test therefore reads only the flags and the total natural/elastic width,
never `first`. `tabs_test.c` asserts the control's width is invariant across
every legal scroll position, which is the cheap way to keep it that way.

**5. Per-tab hover is canvas-only, deliberately.** `unoui_widget` has no field
for a hot tab, and the only free ones are `vmin`/`vmax`, which would be
field-abuse of the kind this toolkit has so far avoided. So
`unoui_tabs_model_of` reports `hot = -1` and the widget path draws no hot tab.
Canvas hosts pass their own hover in, which is what the actual consumer needs -
`pc64_browser.c` already tracks `g_hot_tab`/`g_hot_close` itself. Revisit only
if a widget-path app ever wants it.

**6. The host contact sheet renders EIGHT themes, not ten.** `unoui/build.sh`'s
`THEMES` list omits `themes/theme_aurora.c`, which is the file defining both
Aurora Light and Aurora Dark - the pc64 shell's own defaults. §3's gate wording
("all ten themes") is not achievable as the host build stands. Ten is what the
shell offers (`pc64_uui.c` `kThemes[]`); eight is what the host build covers.
Worth fixing on its own sometime; out of scope here, and noted so the next
phase does not write another unachievable gate.

**7. The contact-sheet demo has no tabs widget at all.** `unoui_demo.c` (the
static write-once window behind `themes.png`) never adds one, so "the Control
Panel's strip unchanged on the contact sheet" was not a checkable claim. The
plain-strip evidence is instead the storyboard's editor window - which does
carry one, and is rendered under the default, Windows 3.1 and Mac Plus themes -
plus the explicit unchanged-behaviour assertions at the top of `tabs_test.c`.

**8. A 1-bit palette loses the active underline, and that is survivable.**
Checked by zooming the rendered frames rather than assumed: on Mac Plus the
accent collapses to black and the underline disappears into the baseline, but
the selected tab still reads as selected through its 2 px raise and its merge
with the baseline - the same cue the plain painter has always relied on there.
The rule for phases B and F: **a state signalled only by `accent` does not
survive every theme.** Signal it with geometry too.

**9. `UI_MDI_CLOSE` does not exist.** §4 gave a child both a close and a resize
flag. The close box is drawn by the THEME's title-bar painter from
`unoui_metrics.closebox`, and neither that painter nor the existing title-bar
hit test takes any per-window opt-out. A per-child flag could therefore only
have suppressed the *click*, leaving a drawn control that did nothing - which is
precisely the mistake `WM-MODERN-SPEC.md` correction 7 was written about. So a
child gets a close box exactly when the theme has one, and it is always live.
`UI_MDI_RESIZE` is the only child flag.

**10. The widget needs a pointer to the child set, and `n` is derived.**
§4 described `unoui_mdi` but never said how a `UI_MDI` widget reaches it:
`unoui_widget` gained an appended `struct unoui_mdi *mdi` (0 = absent, per the
ABI rule). And the live-child count is derived from the z-list rather than
stored in an `n` field, so there is one source of truth for "how many children"
and it cannot drift out of step with z.

**11. Nine functions the contract table missed**, none of them optional:
`unoui_mdi_add` / `_close` / `_raise` / `_focused` / `_count` / `_zorder` /
`_at` / `_clamp` / `_content_rect`. An app cannot maintain the z-list from
outside without re-implementing the index+1 invariant, which is the one thing
this design must not ask of it. Also `unoui_mdi_tile` has no `mode` argument -
the grid falls out of the child count (`ceil(sqrt(n))` columns), so a caller
never has to pick one.

**12. `unoui_draw_frame_chrome` was the wrong shape and was not added.** §4
wanted the chrome factored out of `render_window_chrome` into a
rect-plus-title-plus-flags call. It cannot be: the theme painters take a
`unoui_window *`, reading `->r`, `->title`, `->active` and `->flags` and
*writing* `->content_*`, so a rect-based signature has nothing to hand them.
A child is instead rendered by filling in a temporary `unoui_window` and passing
it to `PICK(window)` / `PICK(titlebar)` / `draw_resize_grip` unchanged. The
existing path needed no refactor at all, and children inherit every theme
automatically - which is what §4 actually wanted.

**13. That temporary is one file-scope scratch, not a stack local.**
`unoui_window` embeds `unoui_widget w[UNOUI_MAX_WIDGETS]`, so a local would be
several KB of stack per child per frame. `nw = 0` means the painters never read
the array, and unoui is single-threaded by construction.

**14. Delegating to the theme bought a state cue that survives 1-bit** - the
exact opposite of correction 8, and the more useful half of the pair. Mac Plus
marks an active window with a striped title bar, which is geometry rather than
colour, so MDI focus reads correctly there with nothing written for it. The
generalisation for `tabs-c` and the app phases: **prefer delegating a state to
the theme over hand-painting it**, because the theme has already solved it on
palettes you are not looking at.

**15. A latent clip bug in `UI_CANVAS`, found and deliberately NOT fixed.**
After the canvas draws, `draw_one` restores the clip with the non-BARE content
formula unconditionally. On a `UI_WIN_BARE` window - the shell's desktop and
taskbar - that is the wrong rect for any widget following the canvas. It is
harmless today only because those windows happen to carry their canvas last.
`UI_MDI` restores correctly for both cases; `UI_CANVAS` was left alone because
it is a live shell path and this phase has no gate that would prove a change to
it safe. Whoever touches the shell next should fix it there, with a gate.

**16. `band_tabs` does not disappear.** §5 said it would and that `band_bar` /
`band_body` would re-origin around it. They do not need to: the browser is a
canvas hosting the control inside a rect, so the band simply *is* the control's
rect. `ch_tabh()` became `unoui_tabs_h(TH())` and every other band's arithmetic
was left untouched - one line instead of re-originating the whole chrome.

**17. The chrome-palette question answered itself: ALL of it moved.** §5 offered
two options and asked for a deliberate choice. Converting only the tabs was not
really available - the control paints from the palette, so a themed strip would
have sat directly on top of a hard-coded near-white toolbar, incoherent on any
theme that is not light, and Aurora Light and Dark are the shell's own defaults.
The conversion turned out to be six macro definitions (`CH_FACE` becomes
`TH()->pal.face`, and so on), so the toolbar, the drop-down panels and the
status bar all became themed with no edit at their use sites. The PAGE colours
(`PG_*`) are deliberately NOT themed: a document renders as a document, which is
what every browser does with a page regardless of the desktop.

**18. `pc64_browser.c` needed `unoui_theme.h`.** It had only the forward
declaration, because the existing panel code passes the theme straight to
`unoui_list_draw` without ever dereferencing it. Reading the palette needs the
complete type. `pc64_clock.c` already includes it for the same reason.

**19. Ctrl-T deselects the previous tab, and that is what makes the strip
derivable.** In the gate, the first add-a-tab diff's left edge is tab 0
*repainting* because it lost the selection - not the new tab appearing. So the
strip's left edge is `d1[0]` and the tab pitch is `d2[0] - d1[0]`. Assuming it
the other way round produced a negative strip origin, which the range checks
caught on the first run. The pattern is worth copying: derive geometry from two
measurements, then range-check every derived value, so a bad derivation fails
loudly instead of clicking empty chrome and reporting a pass.

**20. The "+" button moving is a 0.2 signal, not a 1.0 one.** Its box is filled
with `face`, which is also the empty strip's background, so when it moves only
its frame and its cross glyph differ. Measured: 0.216 of the zone changes when
it moves and 0.005 when it does not - two orders of magnitude apart, so the
threshold sits at 0.10 with room either side. An assertion written at the
obvious ">0.5 changed" would have failed on completely correct behaviour, which
is the trap in every pixel-diff gate: take the threshold from the measurement,
not from intuition about how big the change ought to be.

**21. The dark theme is verified by palette values, not by a photograph.** The
gate screenshots Aurora Light, the shell's default. Aurora Dark maps `face` to
0x303643 against `text` 0xE7EBF3, so the contrast is high by construction and is
exactly what every other themed window already gets - but no run has actually
grabbed the browser under a dark palette. Anyone who adds a theme-switching step
to `harness.py` should take that shot while they are in there.

**22. §7 has the MAC ordering wrong: SSH is encrypt-AND-MAC.** The spec says
"encrypt-then-MAC ordering as the protocol specifies". RFC 4253 §6.4 specifies
no such thing: the MAC is computed over the sequence number concatenated with
the *unencrypted* packet, and is sent in the clear after the ciphertext. That
is encrypt-and-MAC. Encrypt-then-MAC in SSH is an OpenSSH extension negotiated
as `hmac-sha2-256-etm@openssh.com`, and a client that assumes it without
negotiating it will fail every packet. Implement RFC 4253's order; the etm
variants are a later, separate opt-in.

**23. BearSSL's X25519 takes the point little-endian and the scalar
BIG-endian.** It byteswaps the point internally (its comment says so) and
clamps the scalar itself, but the scalar arrives in the generic EC API's
big-endian convention while RFC 7748 - and therefore SSH - is little-endian
throughout. Handing it a little-endian scalar produces a perfectly well-formed
shared secret that simply is not the one the peer computed, which then surfaces
as an exchange-hash mismatch several messages later. `ssh_x25519*()` reverses
on the way in so no caller has to know, and `sshwiretest` pins it with RFC
7748's own vectors rather than by reasoning about it.

**24. The wire helpers are worth having as a separate, pure file.** Everything
in `unossh_wire.c` is a pure function of its arguments - no sockets, no
connection state, no allocation - which is what makes the half of SSH that
fails silently (mpint encoding, length units, padding rules, the exchange hash)
testable on the host in seconds. The I/O state machine sits on top rather than
mixing in.
