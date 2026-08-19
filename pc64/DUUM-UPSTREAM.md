# Duum lives in its own repository now

**Canonical home: https://github.com/hmofet/duum** (MPL-2.0, same licence as
UnoDOS).

Duum started here, as `pc64/apps/DUUM.PY`, the acceptance test that Python is
a first-class app language on this OS. It is now a standalone cross-platform
Doom engine that runs anywhere CPython does, and UnoDOS is one of its ports
rather than its home.

## The rule

**Do not develop Duum in this repository.** `pc64/apps/DUUM.PY` is a
**vendored, generated file**; its header says so. An edit made here is lost
the next time anyone syncs, and worse, it silently forks the engine away from
the upstream that every other platform builds on.

A change to the engine (a rendering fix, an optimisation, a gameplay
behaviour) goes to the duum repo, and comes back here as a sync.

What still belongs to UnoDOS, and is edited here normally:

| File | Why it is ours |
|---|---|
| `pc64/upy_port/mod_uno.c` | the C canvas (`cv_wall_col`, `cv_flat_col`, …). This is the span-writer contract Duum draws through, and it is a pc64 implementation detail. |
| `pc64/tools/duum_host.py` | the host **mirror of that C canvas**, so the gates below test the pixels the *device* would produce, not upstream's reference rasteriser. |
| `pc64/tools/duum_golden.py`, `duum_verify.py`, `duum_collide.py` | the gates, run against the mirror above. Upstream carries its own copies pointed at its own rasteriser; both are wanted, and they check different implementations of the same contract. |
| `pc64/tools/demo/duum_demo.py`, `duum_ab.py`, `duum_sound.py`, `duum_vs_doom.py` | device recording, on-device A/B, audio and oracle tooling. UnoDOS-specific. |
| `pc64/build.sh` step 3e, `pc64/docs_esp/DUUM.MD`, `pc64/wads/` | packaging `DUUM.UNO`, the shipped user doc, and where a WAD goes. |

## Syncing a new version

```bash
# from a local checkout of the duum repo
python pc64/tools/sync_duum.py --from ../duum

# or straight from a published tag
python pc64/tools/sync_duum.py --ref v0.1.0
```

It drops upstream's `dist/unodos/DUUM.PY`, the engine only (the desktop
rasteriser and tkinter frontend are not in that build), onto
`pc64/apps/DUUM.PY`, and prints the version it landed.

Then prove it, because a sync is a code drop from another repository:

```bash
cd pc64
python tools/duum_verify.py --wad wads/DOOM1.WAD       # 0 failing views
python tools/duum_golden.py check --wad wads/DOOM1.WAD # 54/54 identical
python tools/duum_collide.py --wad wads/DOOM1.WAD      # 5/5 checks
```

`duum_golden` compares against a saved baseline; if a sync is *meant* to
change pixels, look at the diff first and then re-`save`. `duum_verify` is an
independent oracle and must be 0 failing views regardless.

`duum_collide` is neither of those, and it is the one a sync most needs. Both
of the others take the player's POSITION as an input, so they render a
perfectly good frame from inside a wall and report success, which is how a Duum
with no wall collision at all shipped past both of them for months. It asserts
about movement instead: a scripted walk into a known wall, randomised moves
that may not cross a one-sided linedef, every use-door in the episode, and a
rocket fired at a wall.

Its fifth check is ours alone, and has no counterpart upstream: it drives the
pause menu with **this machine's** Escape and asserts the menu offers only what
this platform can actually do. The section below says why that is not
paranoia.

## What a port has to check that upstream cannot

Upstream's gates all run on a desktop, and a desktop cannot see the two ways
this port differs. Both were found by reading `hid_kbd.h` before a sync rather
than by playing after one, and both would have left every gate on both sides
green while the feature simply did not work here.

**Escape is not one key code.** A desktop event carries ASCII 27. This machine
reports non-character keys as a SCANCODE with `uni` 0, and its Escape is
`0x17`. An engine that only knows 27 has a pause menu that never opens on the
device. The engine now accepts both (`is_esc`), and `duum_collide` here drives
the menu with `0x17` specifically, so a future sync cannot quietly lose it.

**A menu must only offer what the platform can do.** Quit is a frontend
action: the engine raises a flag and something closes the window. Our shell
owns its own windows and has no such call, so the row is not offered here at
all rather than sitting there doing nothing.

Key remapping was the same story until the five hooks landed. **They are
implemented now** (`uno_binds.c`, exposed through `mod_uno.c` as `bind_name`,
`bind_set`, `bind_reset`, `pref_get`, `pref_set`), so the Controls screen
works on the device and the FPS setting survives a reboot in `UNOPREF.CFG` on
the boot volume. The engine needed no porting layer for it: every one of the
five is probed with `hasattr`, so supplying them was the whole job.

Two things about that implementation are worth knowing before touching it:

- **A binding is stored against a KEY ID, not a scancode.** This machine has
  two keyboard transports in two different code spaces (HID Usage codes, PS/2
  Set-1), so a binding is held as unshifted ASCII, or `UNO_BK_*` for the keys
  with no character, and each producer translates its own space into that. A
  third transport means writing one translation, not another copy of the
  table.
- **Use refuses to be rebound here.** It is the one action the engine reads as
  a key EVENT rather than from the held bitmap, and the binding table only
  feeds the bitmap, so a stored Use binding would do nothing. It says no, and
  the menu says why, which is better than accepting it and going quiet.

There is also no frontend on this machine to capture the new key, so the
ENGINE captures it and hands us the raw event - a path that exists upstream
purely for ports like this one.

## Why the engine can be shared at all

Duum only ever asked its platform for six things (`size`, `read_at`, `beep`,
`quiet`, and optionally `ticks` and `keys_down`) and only ever drew through
a canvas with a span-writer contract. That is the whole porting surface, and
it is why the same file runs on a desktop and on pc64 unchanged: upstream
supplies a pure-Python rasteriser, we supply a C one, and the engine cannot
tell the difference.

The engine itself is pure Python on both. There is a C per-column rasteriser
in `mod_uno.c` (`cv_seg_cols`) from an earlier experiment; it is **no longer
called**, upstream's engine does that loop in Python, and survives only as a
reference transcription.

## Not in THIRD-PARTY.md, on purpose

`THIRD-PARTY.md` is the manifest of code belonging to **another entity**, and
its keys are checked against the notices UnoDOS ships in
`pc64/docs_esp/LICENSES.MD`. Duum is the same author under the same licence
(MPL-2.0) as UnoDOS, so it carries no third-party notice obligation and adding
a key would only add a shipped notice that says nothing. It is a separate
*repository*, not a separate *entity*.
