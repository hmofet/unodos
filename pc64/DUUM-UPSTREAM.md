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
| `pc64/snd_pcm.c` (the effects voices), `pc64/snd_mus.c` | how this device answers the four optional sound calls. The engine names a sample and a volume; the mixing and the DAC are ours. |

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

Duum asks its platform for very little - `size`, `read_at`, `beep`, `quiet`,
and, all optional and `hasattr`-probed, `ticks`, `keys_down`, the five
binding hooks and (since 2026-08-19) the four sound calls above - and only
ever draws through a canvas with a span-writer contract. That is the whole
porting surface, and it is why the same file runs on a desktop and on pc64
unchanged: upstream
supplies a pure-Python rasteriser, we supply a C one, and the engine cannot
tell the difference.

The engine itself is pure Python on both. There is a C per-column rasteriser
in `mod_uno.c` (`cv_seg_cols`) from an earlier experiment; it is **no longer
called**, upstream's engine does that loop in Python, and survives only as a
reference transcription.

**And it should stay uncalled.** A/B'd on real hardware (ZimaBlade, 2026-08-18,
one alternating boot): the Python loop measured **15.608 ms per draw** against
the C path's **15.584 ms** on an identical spawn view, 0.15% apart, with the C
path nominally ahead, i.e. no difference. The MicroPython float-boxing worry did
not materialise. Duum's renderer is also only ~11.7 ms of a ~46 ms frame there,
so the frame is the present path and not this loop. Numbers and method in
[METAL-FINDINGS.md](METAL-FINDINGS.md); harness in
`tools/demo/duum_ab_metal.py`.

## Open reports for upstream

Things found on UnoDOS hardware that are **engine** bugs, so they are fixed in
`hmofet/duum` and come back here as a sync. Do not patch `pc64/apps/DUUM.PY`.

An entry stays here once it is answered, rewritten to say what came back rather
than what was wanted, so nobody re-reads a closed report as open work. What is
still genuinely outstanding is called out inside each entry.

### WALL COLLISION - reported 2026-08-18, fixed upstream, landed 2026-08-19

The player walked through walls, frequently and reproducibly.
`blocked(ox, oy, nx, ny, r, selfthing)` referenced no linedef and no vertex,
and its one wall-like guard, `point_sector(nx, ny) is None`, was dead code: a
BSP partitions the entire plane, so the descent always lands in a subsector and
always returns a sector.

Fixed in `hmofet/duum` on 2026-08-18 and vendored here by the sync of
2026-08-19. Upstream's standing check is its `tools/duum_collide.py`, which
exists because neither rendering gate can catch this class of bug: both take
the player's POSITION as an input, so a player standing inside a wall is an
invalid viewpoint fed to a working renderer and both gates pass while the game
is unplayable. It asserts about movement instead: a scripted walk into a known
wall, 36,000 randomised moves that may not cross a one-sided linedef, every
use-door in the episode, and a rocket fired at a wall.

Method, numbers, and the renderer A/B this was found during, are in
[METAL-FINDINGS.md](METAL-FINDINGS.md).

**What is still open, and no gate here can close it.** Door operation was
reported UNVERIFIED rather than known-broken, for want of anything solid to
stand against. All 110 use-doors in the episode now pass upstream's gate on the
desktop, so the ZimaBlade re-test is the remaining half of that report.

### SOUND - four optional calls offered 2026-08-19, IMPLEMENTED HERE the same day

Duum plays the WAD's own sound effects and its music through four calls that
are OPTIONAL and `hasattr`-probed, exactly like `ticks`, `keys_down` and the
five binding hooks:

```
sfx_load(slot, pcm, rate)     keep a sample under `slot`; sent once per sound
sfx_play(slot, vol, sep)      play it, mixed with whatever else is running
mus_play(smf, loop)           a whole Standard MIDI File
mus_stop()
```

**pc64 now implements all four**, so the device plays the WAD's effects and
its score instead of a square-wave note per event. The implementation is
ours and is described in [AUDIO.md](AUDIO.md): an effects bank and eight
voices summed into the DMA ring in `snd_pcm.c`, and `snd_mus.c`, which points
`unomedia`'s MIDI player at the bytes the engine hands over. The vendored
engine was not touched - it needed no change, which is what "optional and
probed" is for.

Verified on the device rather than assumed, by capturing QEMU's wav sink and
asserting on the samples the DAC consumed (`tools/duum_audio_test.py`): an
effect centred, one hard left and one hard right (L 5210 vs R 0, and its
mirror), a Standard MIDI File playing, and - the thing a one-source-at-a-time
ring could not do - five 100 ms windows carrying the score's tone and an
effect's tone at once. Then the engine end to end
(`tools/duum_audio_test.py duum`): `app.have_sfx` and `app.have_mus` both
True with `err` None, a WAD sample handed over on the first pistol shot, and
46 of 65 captured seconds non-silent.

Two things in the engine worth knowing about:

- `SFX` is a table of 49 rows naming a `DS` lump per game event, and `MSND`
  gives each monster sprite family its own voice. The midi/ticks pair in each
  row is still the fallback note, so a host without the four calls (or one
  whose PCM device failed to probe) sounds exactly as it always did.
- `mus_to_midi()` converts a `MUS` lump to a Standard MIDI File in Python and
  hands over the whole thing. Upstream offered to stream the conversion if the
  spike is too much for a device; it is not - the largest in the shareware WAD
  is `D_E1M8` at about 66 KB out, against a 32 MB kernel heap and a 16 MB
  MicroPython heap - so no change was asked for.

**Still open, and only hardware can close it:** none of this has been heard on
metal. QEMU's HDA and a real codec are not the same thing, and the same
"asserted in QEMU, never run on metal" gap is exactly where the wall collision
bug lived for months.

**The ZimaBlade cannot close it**, so do not go looking there: that box has no
audio hardware at all. What it DID close is the other half of the contract -
what these calls do on a machine with no DAC. They **raise `OSError`**, and
they have to, because returning False leaves the engine mute: `sound()`
ignores what `sfx_load` answers and `return`s after `sfx_play` whatever it
answers, so an exception is the only signal that reaches `self.snd()` and the
note the event has always made. Verified with `tools/duum_audio_test.py
nosnd`, which boots with no sound device at all: all three calls raise, and
after four shots the engine's own `have_sfx` and `have_mus` are both False -
it is beeping, not silent.

Metal verification of the audible path therefore waits on a machine with a
real codec.

## Not in THIRD-PARTY.md, on purpose

`THIRD-PARTY.md` is the manifest of code belonging to **another entity**, and
its keys are checked against the notices UnoDOS ships in
`pc64/docs_esp/LICENSES.MD`. Duum is the same author under the same licence
(MPL-2.0) as UnoDOS, so it carries no third-party notice obligation and adding
a key would only add a shipped notice that says nothing. It is a separate
*repository*, not a separate *entity*.
