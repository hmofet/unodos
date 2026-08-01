# An SSH client for UnoDOS, and the toolkit work it needs

Status: **PROPOSAL**, 2026-07-31. This document is the design and its reasoning.
The build order a worker follows is [`SSH-CLIENT-SPEC.md`](SSH-CLIENT-SPEC.md);
where the two disagree, the spec wins.

## 1. What we are building, and why it is four things

The goal is an SSH client for pc64 with key management, a tabbed interface and
saved sessions. Pulling that thread turns up three more pieces of work, and it
is worth being explicit that they are separable rather than one monolith:

1. **`unossh`** - a new subsystem: the SSH protocol, its crypto, and a key
   store. Headless. No UI, no shell dependency.
2. **unoui tabs and MDI** - the toolkit does not have a usable tabbed-document
   control, and has no multiple-document container at all. Both are general
   toolkit gaps that this app happens to expose.
3. **The browser refactor** - the browser carries its own hand-rolled tab strip
   with hard-coded colours. Once the toolkit has a real one, the browser should
   consume it, and stop being the only app in the system whose chrome ignores
   the theme.
4. **The automation surface** - unoautomate gets a verb that drives `unossh`, so
   the harness that already commands this machine can log into other machines
   and command those. This is the piece with the most leverage and the least
   code.

The through-line: **one headless core, two front ends.** The GUI app and the
automation verb are both consumers of `unossh`, and neither is privileged. That
split is not architecture for its own sake - it falls straight out of item 4,
because the harness must be able to open an SSH connection on a box with no
window open and possibly no desktop drawn.

## 2. The single most important finding: we have almost all the crypto

An SSH client needs ECDH, a host-key signature algorithm, a cipher, a MAC, a
hash and a CSPRNG. The vendored BearSSL tree at `pc64/bearssl/` supplies all of
it except one thing, and `pc64/build.sh:251-257` compiles the **whole** portable
source tree into the kernel already (an 8-file skip list drops only the x86-NI
and SSE2 variants). These are ordinary public library symbols, not statics
buried inside TLS - `pc64/unosecure.c` already calls BearSSL directly for a
non-TLS purpose, which is the precedent.

| Need | Status |
|---|---|
| X25519 ECDH | **have** - `br_ec_c25519_*`, `bearssl_ec.h:502-592` |
| SHA-256 / SHA-512 | **have** - `br_sha256_*` / `br_sha512_*`, `bearssl_hash.h` |
| HMAC | **have** - `br_hmac_*`, `bearssl_hmac.h:84-233`, generic over any hash |
| AES-CTR | **have** - `br_aes_ct64_ctr_*`, `bearssl_block.h:1477-1523` |
| ChaCha20 / Poly1305 | **have** - `br_chacha20_ct_run`, `br_poly1305_ctmul_run` |
| RSA sign/verify | **have** - full `bearssl/src/rsa/` |
| ECDSA P-256 | **have** - `br_ecdsa_i31_*` |
| CSPRNG | **have** - `br_hmac_drbg_*`, seeded from `tls_entropy_get()` |
| **Ed25519** | **MISSING. BearSSL has no EdDSA at all.** |

### Ed25519 has to be written, and it is not optional

`ssh-ed25519` has been OpenSSH's default key type since 2014. Every machine this
client is actually meant to reach - `mba`, `devbuntu`, `behemoth`, the ZimaBlade
- is reachable with a lab key that is almost certainly Ed25519. A client that
cannot do Ed25519 is a client that cannot log into our own estate, which is the
entire point of item 4.

BearSSL's curve25519 code does **not** help as much as it looks like it should.
Its implementations are Montgomery-ladder X25519 only, the field arithmetic is
not publicly exported, and Ed25519 needs the twisted-Edwards form with point
addition, a different encoding and SHA-512-based nonce derivation. Treat this as
a from-scratch implementation of RFC 8032.

The good news is that it is the *easiest* kind of from-scratch crypto to get
right: fully specified, with official test vectors, and completely testable on
the host with no OS, no network and no hardware. It gets its own phase and its
own host harness, and it must pass RFC 8032 §7.1 vectors before anything else in
the programme depends on it.

**Build the host harness with `build.sh`'s sanitizer flags.** This is the UnoAmp
lesson, recorded in `pc64/UNOAUTOMATE-REQUESTS.md` (2026-07-31, "the EQ defect is
CLOSED"): a host harness compiled without
`-fsanitize=signed-integer-overflow,bounds,shift,integer-divide-by-zero,null`
passed cleanly on code that was resetting the box on every run, because correct
arithmetic and *defined* arithmetic are not the same property. Field arithmetic
on 64-bit limbs is exactly the shape that trips this.

### Two other structural facts that decide the architecture

- **`br_*` symbols are not in `kExports[]`** (`pc64/pc64_modload.c:112-192`), so
  a `.UNO` module cannot reach the crypto.
- **The multi-connection socket API is not exported either.** `kExports[]` has
  the single-connection `net_tcp_*`; the real API is `netsock.h`
  (`net_socket`/`net_connect`/`net_accept`, `NSOCK 12`).

So `unossh` is **native, compiled into the kernel**, like UnoAmp. Exporting
BearSSL and netsock through `KX()` just to host an app out-of-tree would be a
much larger and more invasive change than the app is worth, and the automation
verb needs the core resident anyway.

Do **not** build on `pc64/tls.*`. Its state is four file-scope statics
(`tls.c:106-109`) and its API takes no handle - it is a single global
connection. `unossh` sits directly on BearSSL primitives plus `netsock`.

## 3. The toolkit gaps

### Tabs: the widget exists but is a tab *strip*, not a tabbed document control

`UI_TABS` (`unoui.h:37`, painter `unoui.c:775-790`, hit-test
`unoui_input.c:467-478`) draws labels and reports which one was clicked. It has
no close buttons, no "+" affordance, no overflow, no reordering, no per-tab
content association, and its labels are a `const char **` the app must keep
alive. Its only in-tree consumer is the Control Panel, which implements "pages"
by tearing down and rebuilding the entire window on every switch
(`pc64_uui.c:728-733, 3760-3761`).

It also has **no public geometry helpers**. `d_tabs` and the hit-test each
independently re-derive `fb_text_w(items[i]) + 16`. `UI_LIST` already solved
this exact problem - it exposes `unoui_list_draw` / `unoui_list_index_at` /
`unoui_list_bar` (`unoui.h:245-256`) precisely so a canvas app can host one.
Tabs need the same treatment, and they need it *because* of the browser: the
browser is a single `UI_CANVAS`, so unless the tab control can be driven from
inside a canvas rect, the browser cannot adopt it at all.

That is the ordering constraint for the whole UI half: **the geometry split is
not a nicety, it is what makes the browser refactor possible.**

### MDI: does not exist, and should not be built into `unoui_window`

There is no parent/child relation, no nested clipping, and no sub-window
z-order. `unoui_window` (`unoui.h:154-168`) is a flat entry in a 24-slot array
with three pin bands, and `render_one_window` sets exactly one clip rect per
window.

The tempting design - add a parent pointer to `unoui_window` and let the
renderer recurse - is the wrong one here. `unoui.c` and the theme files also
build into the PS2 port, the Dreamcast port and the host demo, and the toolkit's
ABI rule is append-at-end with zero meaning "feature absent, behaviour
identical". A recursive window tree cannot honour that; it changes the meaning
of the window array itself.

**Build MDI as a container widget instead.** A new `UI_MDI` widget kind owns a
list of child frames drawn inside its own rect. The blast radius is one new
widget kind, one painter and one hit-test path; the flat top-level window array
is untouched, and every other port keeps building. Child frames reuse
`unoui_render_window_chrome()` - which the WM programme's phase C2 split out only
days ago for the drag cache - so MDI children get correct per-theme chrome for
free, on all ten themes, with no new artwork.

The cost of this choice, stated honestly: MDI children are not real windows.
They do not appear in the taskbar, they cannot be dragged out to the desktop,
and they do not participate in Alt-Tab or virtual desktops. For an SSH client's
session panes that is the right trade. If a future app genuinely needs
detachable children, that is a different and much larger piece of work.

## 4. What the SSH client actually is

**Algorithm suite**, chosen as the smallest modern set that talks to current
OpenSSH:

| Role | First implementation | Later |
|---|---|---|
| Key exchange | `curve25519-sha256` | - |
| Host key | `ssh-ed25519` | `rsa-sha2-512`, `rsa-sha2-256` |
| Cipher | `aes256-ctr` | `chacha20-poly1305@openssh.com` |
| MAC | `hmac-sha2-256` | (implicit in the AEAD) |
| User auth | `publickey` (Ed25519), `password` | RSA keys, keyboard-interactive |

`aes256-ctr` + `hmac-sha2-256` goes first because both primitives are drop-in
BearSSL calls. `chacha20-poly1305@openssh.com` is deliberately second: it uses
two independent keys and a separate cipher instance for the packet length, which
means reaching past BearSSL's combined `br_poly1305_*_run` convenience wrapper
and driving the ChaCha20 and Poly1305 cores separately. That is a fine thing to
do, but not while also debugging a first key exchange.

**Key management.** Generate, import, export, list, delete. Public keys read and
write the standard one-line OpenSSH format (`ssh-ed25519 AAAA... comment`) for
interop. Private keys are stored in our own container encrypted with
PBKDF2-HMAC-SHA256 + AES-256-CTR, called through BearSSL directly - do not
reuse `unosecure.c`'s PBKDF2, it is `static` (`unosecure.c:166`).

One honest limitation to plan around: importing an *encrypted* OpenSSH private
key needs `bcrypt_pbkdf`, which we do not have and which is a meaningful chunk of
work (Blowfish plus a modified bcrypt). **Unencrypted `openssh-key-v1` files
parse without it**, so import works for keys exported with no passphrase. Say so
in the UI rather than failing obscurely.

**Session management.** Saved connections - name, host, port, user, key
reference, and last-used - in a plain-text `SSH.CFG` alongside `SHELL.CFG`.
Heed the bug the WM work found: `session_save()` had been writing to "the first
writable volume", which is the RAM disk, so no session had ever actually
survived a power cycle. Use the same volume-selection order `unosecure.c`'s
`pick_vol` uses, and gate the phase on a persistence test over a real reboot on
a real FAT image (vvfat cannot carry one).

**The automation verb.** One entry point of the shape
`int ssh_dbg_cmd(const char *line, char *out, int cap)`, reached through
unoautomate's weak-symbol pass-through (the pattern at
`unoauto_remote.c:712-730`, the `eth` dispatch clause at `:1008-1013`).
`unossh` owns the sub-verb grammar and the output format entirely; unoautomate
lands one weak stub, one four-line clause, one `REMOTE.md` table row and a
changelog entry, and never needs to know anything else.

The one hard constraint to design against: **the URC transmit buffer is 8 KB and
drops silently past it** (`unoauto_remote.c:109, 129`). Remote command output is
unbounded, so it must be retrievable in offset slices, the way `readsec` and
`screen read` already are - not returned whole.

## 5. Two lanes, not one

The programme splits cleanly, and the two halves share nothing until the app
itself:

- **Protocol lane**: Ed25519, the SSH transport, key management, the automation
  verb. Host-testable end to end; needs no toolkit work and no display.
- **UI lane**: the tabs widget, MDI, the browser refactor. Needs no crypto and
  no network.

They join only at the GUI app, which is the last phase. Two agents can run these
concurrently with no shared files, and the browser refactor delivers value on
its own even if the SSH work slips.

## 6. Risks, in the order they are likely to bite

1. **Ed25519 correctness.** Mitigated by RFC 8032 test vectors and a sanitizer-
   built host harness, before anything depends on it.
2. **First key exchange against a real server.** Byte-exact framing, MPINT
   encoding and the exchange hash are unforgiving, and a wrong answer is an
   opaque disconnect. Mitigate by testing against a real `sshd` on the LAN with
   `-ddd` on the server side, which will say precisely what it disliked, and by
   writing the transport so the exchange hash inputs can be dumped.
3. **The browser's hard-coded chrome palette.** `CH_FACE`/`CH_EDGE`/... at
   `pc64_browser.c:1296-1301` are literal colours used by the toolbar and panels
   as well as the tabs, so moving tabs onto the theme either leaves the browser
   half-themed or turns into a wider chrome change. Decide deliberately; do not
   discover it halfway.
4. **MDI scope creep.** The container-widget decision above is what keeps this
   bounded. If children start needing to be real windows, stop and re-plan
   rather than growing the widget.

## 7. What this does not do

No SSH *server* (no inbound sessions), no SFTP or SCP in the first programme, no
port forwarding, no agent forwarding, no `known_hosts` TOFU beyond storing and
comparing a host key fingerprint, and no terminal emulation beyond what the
existing text rendering supports. Each of those is a reasonable follow-on; none
belongs in the first landing.
