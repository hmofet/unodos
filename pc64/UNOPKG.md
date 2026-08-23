# unopkg, the subsystem contract

Foreign packages that install as ordinary UnoDOS apps. Double-click an `.APK`
in Files and a desktop icon appears; open it and a native window opens. There
is no launcher, no guest desktop, and nothing on screen that says "Android".

The programme and its phases are `docs/ANDROID-APPLIANCE-PLAN.md`; the runtime
that will eventually host these apps is `pc64/UNOVIRT.md` and the channel is
Track B (`unoguest`). This file is the API surface, its changelog, and the
things a consumer has to know.

**Status: P2 [implemented]** - a package is understood, installed, and appears
as an app. **The runtime backend is a stub**: `uno_pkg_launch` always reports
that nothing is connected yet, because the channel it would speak over is
phase P3. What the stub reports is not itself a stub - see "The runtime probe"
below.

## API (`pc64_pkg.h`, `UNO_PKG_API 1`)

| Call | Answers |
|---|---|
| `uno_pkg_probe(vol, path, &info)` | is this a package, what is in it, and can this machine run it |
| `uno_pkg_install(vol, path, &info, progress, err, errmax)` | make it an app |
| `uno_pkg_remove(id)` | unmake it |
| `uno_pkg_installed(id)` | is `id` one of ours |
| `uno_pkg_launch(target, msg, max)` | **exported to the shim**: run it, or say why not |
| `uno_pkg_runtime_str(target, buf, max)` | **exported to the shim**: one line on the runtime |

Exactly two entries are in `kExports`, and deliberately: a shim is a file the
INSTALLER wrote, so anything it can reach is something an installed foreign app
can reach. It may ask to run its own target and ask how the runtime is. It may
not install, remove or enumerate.

## An install is three files and a rescan

```
<apps>\FIREFOX.UNO      the shim: a copy of the template, two fields rewritten
<pkg>\FIREFOX.PKG       the record: id, name, target, version, where the package is
(the package itself)    LEFT WHERE THE USER PUT IT - see below
```

`<apps>` is `EFI\UNODOS\APPS` on an installed system and `APPS` on a dev stick,
chosen by the same rule every other consumer of the two-layout convention uses
(`uno_mod_find`, `pc64_font.c`). `<pkg>` is `EFI\UNODOS\PKG` or `PKG` beside it.
The rescan is `pc64_shell_apps_rescan()`, so the icon appears without a reboot.

**The record is written before the shim, on purpose.** If the shim lands and
the record does not, there is an app icon with nothing behind it. In the other
order the worst case is an orphan record, which the next install overwrites and
which nothing reads without a shim beside it.

## THE PACKAGE IS NOT COPIED, and that is a constraint, not a choice

The layer below writes **whole files only** - `fat.h`: "create/overwrite a file
with exactly len bytes" - so copying a 100 MB APK would mean holding 100 MB of
kernel heap at once. So the record stores `srcvol:` and `srcpath:` and the
package stays where the user put it.

The consequence is real and belongs in the UI rather than in a footnote: delete
or unplug the package and the app reports that its package is missing. This is
the consumer that motivates the write-at-offset request filed with the unofs
lane; when that lands, an install copies and this section goes away.

Note that a **streamed** read needs nothing new: `uno_fs_read_at` already seeks,
so pushing the package into a guest at appliance boot (phase P4's `PKG_PUT`)
never needs the whole file in memory at either end. It is only the *copy* that
is blocked.

## The shim, and why it is a patched copy

`pc64/apps/foreign_shim.c` builds once into `PKG\FSHIM.UNO`, which is
**deliberately not in `APPS\`** - a template is not an app, and a scanner that
found it there would give it a desktop icon. An install copies it and rewrites
two things:

- **the descriptor** (`.unodesc`), found by its own `UAPP` magic rather than
  through the module header. `mkuno.py` guarantees at build time that a module
  carries exactly one - it refuses a second - so the search cannot be
  ambiguous, and `pc64_pkg.c` then needs to know nothing about `UnoModHdr`'s
  layout. The template pads its block with blank lines to reserve room; the
  installer writes a shorter block into the same bytes and shrinks the length
  field. Shrinking is safe by construction: every reader takes exactly `len`
  bytes.
- **the target blob**, a 320-byte array whose first 16 bytes are the marker
  `UNOPKG-TARGET-v1`, carrying `target \0 display-name \0`.

**Why a patched copy rather than one shim that reads a config file.** A module
has no idea what path it was loaded from - `uno_mod_load_uui` takes a bare
filename and the shell resolves it across volumes - so a shared shim could not
tell which of the installed apps it had been opened as. Carrying the answer
inside the copy removes the question, and it makes an installed app exactly one
file, which is what makes uninstall a delete.

**`build.sh` verifies both after linking**, and this is not ceremony. The blob
is an initialised array whose tail is zero, and a compiler is entitled to place
such a thing in `.bss` - where it would not be in the file at all. That failure
is silent, and it would land far away as "the template carries no target slot"
on a user's machine.

## The target string is opaque to everything except its runtime

`android:org.mozilla.firefox/.App`. The shim does not parse it; `pc64_pkg.c`
only reads the prefix to pick a runtime. That is what lets a `.deb` reuse the
whole path with `linux:` and a different install command in the guest agent,
and it is why the shim is one file rather than one per format.

## Reading an APK: two readers, and one of them is not obvious

**The zip reader is local rather than `unodoc/ud_zip.c`.** ud_zip is a better
zip reader and it is **not linked into the kernel** - it lives inside the Office
modules. Pulling unodoc into the kernel to read a central directory would cost
far more than the eighty lines here. What *is* taken from it is the lesson, and
it is the one a naive reader gets wrong: **read the central directory, never
the local headers.** A local header may carry zeroed sizes with the truth in a
trailing data descriptor, which is exactly what a streaming zip writer emits -
and every APK is built by one. The local header is still read, but only for its
two length fields, because the payload starts after them and the central copy
of the name may be shorter.

**`AndroidManifest.xml` is binary XML (AXML), not text**: a string pool followed
by a chunk tree. It is where the package name and the launcher activity live and
there is no other copy of either. `um_inflate` (already in the kernel, for
UnoAmp's `.wsz` skins) does the deflate.

Three things about that reader worth keeping:

- **Namespaces are ignored on purpose.** Every attribute wanted here is either
  bare (`package`) or in the android namespace (`name`, `label`,
  `versionName`), and no manifest element carries two attributes with the same
  local name in different namespaces. Resolving properly would mean matching a
  URI on every attribute of every element for an answer already known.
- **The label is usually a reference, not a string.** `android:label` normally
  points into `resources.arsc`, which this does not parse. So the ordinary
  answer is the package's last component, title-cased - "Firefox" for
  `org.mozilla.firefox`. The guest knows the true label and can correct it once
  there is a channel to ask over (P4).
- **An unknown chunk is skipped by its own length**, which is what makes a
  manifest from a newer build tool harmless rather than fatal.

## Architecture is checked before install, not discovered at run time

A JNI library for the wrong architecture is not a slow app, it is a crash at
the first call into it. So `uno_pkg_probe` returns **0 rather than 1** for a
package whose `lib/` has no `x86_64/`, and names what it does carry so the
refusal is actionable. A package with no `lib/` at all is pure Java/Kotlin and
runs anywhere, so it passes.

## The runtime probe: a stub backend that is not a stub answer

`uno_pkg_launch` cannot start anything yet. But `uno_pkg_runtime_str` reports
the real state of this machine, and on many machines that state is permanent:

| What it finds | What it says |
|---|---|
| `uno_vmm_eligible` false | "Appliances unavailable: " + the blocker's own sentence |
| carve < 1536 MB | this machine's appliance memory, and what Android needs |
| no `EFI\UNODOS\VM\ANDROID.IMG` | the runtime image is not installed |
| all present | available but not yet connected |

Each is a different problem with a different fix. A shim that flattened all
four into "cannot start" would cost somebody an afternoon, and the L1 version
must not lose them. **The 1536 MB floor is not a preference**: `uno_vmm_carve_mb`
gives a 2 GiB machine 768 MB, and Alpine plus a container plus a browser is
about 1 GB. The failure mode of not checking is the OOM killer inside the
guest, which presents as "the app just did not open".

## Files gained one extension, and one trap came with it

`pane_enter` knew exactly one extension, `.UNO`, tested inline. It now goes
through `fm_ext_is` and knows `.APK` too, behind the same arm-twice confirm the
Delete button uses: installing is not undone by pressing Enter again.

**The trap, which was live for the length of one edit:** the mouse handler
clears `fm_arm_del` *after* calling `pane_enter`, which is harmless for Delete
because Delete is not reached through `pane_enter`. Clearing the package arm in
the same place disarms, one line later, exactly what the re-click just armed -
so an install would need three clicks, or never happen at all. The package arm
is cleared only where the SELECTION MOVED.

## Changelog

- **2026-08-23, API 1. P2 - a package installs and becomes an app.**
  `pc64_pkg.h`/`pc64_pkg.c` (zip + AXML readers, install/remove, the runtime
  probe), `pc64/apps/foreign_shim.c` -> `PKG\FSHIM.UNO`, the `.APK` branch in
  Files, two `kExports` entries, and the `build.sh` block that builds and
  verifies the template. The runtime backend is a stub; the state it reports
  is real. Nothing in `pc64_uui.c` changed: an installed foreign app reaches
  the desktop through the ordinary app registry, which is what
  `docs/APP-REGISTRY-PLAN.md` promised and this is the first outside proof of.
