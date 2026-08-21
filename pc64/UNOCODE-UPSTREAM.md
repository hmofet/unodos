# UnoCode lives in its own repository now

**Canonical home: https://github.com/hmofet/unocode-desktop** (MPL-2.0, same
licence as UnoDOS), in that repository's `core/` directory.

UnoCode started here, as `pc64/unocode/`, the acceptance test that this OS could
carry a VS Code-class editor. It is now a cross-platform editor that ships on
Windows, macOS and Linux, and UnoDOS is one of its targets rather than its home.
This is the same arrangement as [Duum](DUUM-UPSTREAM.md), for the same reason,
and the two documents deliberately have the same shape.

## The rule

**Do not develop UnoCode in this repository.** Everything under `pc64/unocode/`
except the files listed below is a **vendored, generated copy**; every file
carries a banner saying so. An edit made here is lost the next time anyone
syncs, and worse, it silently forks the editor away from the tree that three
desktop platforms build from.

A change to the editor - the workbench, the document model, the terminal, the
themes, the settings, the keybindings, the grammars, the JSONC parser, the
regex engine, the extension host - goes to the unocode-desktop repo and comes
back here as a sync.

What still belongs to UnoDOS, and is edited here normally:

| File | Why it is ours |
|---|---|
| `unoui/`, `unojs/` | the toolkit and the JS engine. UnoCode is one consumer of each; the shell, every app, the Dreamcast port, unoweb and the browser are the others. Upstream consumes these from a pinned submodule of this repo, read-only |
| `pc64/fb.c`, `pc64/pc64_font.c` | software rendering, same reason |
| `pc64/unocode/tools/unocode_urc.py` | the device harness: twelve scenes driven through a booted UnoDOS under unoautomate. Upstream has no OS to boot, so it cannot have this |
| `pc64/build.sh` step 3c2 | packaging the fourteen objects into `UNOCODE.UNO`, a unoui-class module |
| `pc64/docs_shots.py`, `docs/unocode.html` | the manual chapter and its nine figures |
| `pc64/pc64_modload.c` (`kExports`) | which kernel symbols a module may reach. UnoCode consumes this list; it does not own it |

`pc64/unocode/VENDORED.txt` records which upstream commit the current copy came
from. Read it before assuming a bug is ours.

## Syncing a new version

```bash
# from a local checkout of the unocode-desktop repo
python pc64/tools/sync_unocode.py --from ../unocode-desktop

# or straight from a branch or a published tag
python pc64/tools/sync_unocode.py --ref main
python pc64/tools/sync_unocode.py --ref v1.0.0 --dry-run
```

It replaces the `uc_*.c` files, `unocode.h`, `UNOCODE.md`, the core's own tests
and the sample extensions, adds the do-not-edit banner to each, deletes
anything upstream has removed, leaves `unocode_urc.py` alone, and writes
`VENDORED.txt`.

Then prove it, because a sync is a code drop from another repository.

## What a sync has to check that upstream cannot

**Upstream's gate cannot see a pc64 break, and this is not a theoretical
worry.** Its `sources.sh` compiles the editor module and its foundations - not
the kernel - so `pc64_modload.c`, `pc64_fs.c` and everything else kernel-side is
never built there at any warning level. A one-line kernel export added on
2026-08-21 built clean, gated perfectly green in that repo, and did not compile
here at all (unodos `029a4f17` is the fix). A change to a signature used across
the kernel will do it harder.

So the sync, not the upstream gate, is where that class of break is caught:

```bash
cd pc64 && sh tools/gate.sh                  # QUICK=1 for builds only
python3 unocode/tools/unocode_urc.py         # 12 scenes, screenshots
```

`gate.sh` is the one that compiles the kernel. `unocode_urc.py` is the one that
proves the editor still works on the device, and there are three things it
knows that a desktop cannot:

- **`uno_fs_list_dir()` and `uno_fs_list_begin()` report FILES ONLY.** A folder
  of folders reads as empty, which is how `EXT\` once came back "no extensions
  installed" with three extensions plainly on the disk. `uc_list_dir()` wraps
  `uno_fat_list_ex()` for the directory flag. A host filesystem has no such
  split, so upstream's `host_fs.c` cannot reproduce the bug or the fix.
- **`uno_fs_isdir()` answers native FAT only.** It returns 0 on a firmware SFS
  volume, so gating a scan on it makes the scan a silent no-op there.
- **USB HID keyboards deliver no F-keys in this build.** `hid_kbd.c` maps
  usages below 0x39, and F1..F12 are 0x3A..0x45; PS/2 does deliver them. The
  module key hook also carries no Shift flag, so `Ctrl+Shift+P` is recovered
  from the shifted CHARACTER (`'P'` against `'p'`), and Alt is unrecoverable
  there - Alt bindings live on canvas-event keys only. Request filed in
  `pc64/UNOAUTOMATE-REQUESTS.md`.

Two more that bite when photographing it for the manual, and that a sync can
reintroduce: the workbench **remembers** its side bar, panel and theme for the
life of the boot and closing its window does not reset them, so every
`docs_shots.py` scene runs `Reset Layout` first and drives the product through
the command palette **by name** rather than by chord; and the theme is
persisted to settings.json, so a scene that switches it must switch it back or
every later figure is in the wrong colours.

## Why the editor can be shared at all

UnoCode reaches its platform through a narrow, written-down seam and nothing
else: `uno_fs_*` for storage, the `pc64_shell_*` hooks for the desktop it lives
on, unoui for drawing, unojs for extensions. That is what let a hosted SDL2 port
supply eight hooks and a filesystem shim - about 1.5k lines against the editor's
11.5k - and get a byte-identical workbench. The seam is documented in
`pc64/unocode/UNOCODE.md`, and widening it is the only kind of change that
requires both repositories at once.

The one place the seam is currently too narrow is filenames: the listing seam
is `char (*names)[16]`, so the desktop carries a FAT-style alias table
(`VeryLongComponentName.tsx` becomes `VeryLongCo~1.tsx`) to survive it. Widening
it to `UC_NAME_MAX` is upstream's UCD-11, and it lands **here first** because it
touches three kernel files that only this repo compiles.

## Open reports for upstream

Things found on UnoDOS hardware that are **editor** bugs, so they are fixed in
`hmofet/unocode-desktop` and come back here as a sync. Do not patch
`pc64/unocode/`. An entry stays here once it is answered, rewritten to say what
came back rather than what was wanted.

*(None open. The three device traps above are all handled in the core already;
they are recorded as things a sync must not regress, not as outstanding work.)*

## Not in THIRD-PARTY.md, on purpose

`THIRD-PARTY.md` is the manifest of code belonging to **another entity**, and
its keys are checked against the notices UnoDOS ships in
`pc64/docs_esp/LICENSES.MD`. UnoCode is the same author under the same licence
(MPL-2.0) as UnoDOS, so it carries no third-party notice obligation and adding
a key would only add a shipped notice that says nothing. It is a separate
*repository*, not a separate *entity*.
