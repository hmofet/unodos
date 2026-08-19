# Duum - Doom, in Python

Duum is a Doom engine **written in Python** (`SDK\DUUM.PY`), the proof that
Python is a first-class app language on UnoDOS: it loads a real Doom IWAD and
plays it - not a corridor demo, the game.  BSP-traversed texture-mapped
walls, perspective-correct floors and ceilings, the sky, monsters that see
you, chase you and shoot back, weapons, doors, lifts, switches, teleporters,
keycards, pickups, exploding barrels, the status bar, and level-to-level
progression across all of Knee-Deep in the Dead.

It exercises the whole platform at once: streaming file I/O (`uno.read_at`
walks the multi-MB WAD lump by lump, never loading it whole), heavy float
compute (the BSP walk, the column renderer, monster thinking), the
framebuffer through three C fast paths (`cv.wall_span`, `cv.mask_span`,
`cv.flat_span` - one call per rendered column, the per-pixel loops in C),
keyboard input through the live held-key level (`uno.keys_down()`), the
60Hz clock (`uno.ticks()`), and - since 2026-08-19 - **sampled audio**: the
WAD's own sound effects mixed by the system, and its music played through
the OS's MIDI player.

## Getting a WAD

Duum needs a Doom-format **IWAD** on the disk as `DOOM1.WAD`. None ships with
UnoDOS - game data belongs to its makers - so supply one yourself:

- **Freedoom** (freedoom.github.io) - a free, from-scratch, Doom-compatible
  IWAD under a BSD license. Rename `freedoom1.wad` to `DOOM1.WAD`.
- **Doom shareware** - id Software's freely distributable shareware
  `DOOM1.WAD` ("Knee-Deep in the Dead"). `tools/fetch-wad.sh` downloads it.
- A **registered** `DOOM.WAD` renamed to `DOOM1.WAD` works too (episode 1 is
  what the level progression plays).

Put the file next to the apps on the boot disk (the build stages it there
automatically when you have one in `pc64/wads/`). Without a WAD, Duum opens
and tells you it is missing.

## Running it

- **From the desktop or the Start menu** - Duum has its own row and icon,
  like any other app.
- **From Files** - open `APPS\DUUM.UNO` and press **Enter**.
- **From Studio** - open `SDK\DUUM.PY`, press **Ctrl-R**.

The first frame takes a moment - the interpreter parses the WAD and composes
the visible textures - then the game runs continuously.

## Controls

| Key | Action |
|---|---|
| Up / Down | move forward / back |
| Left / Right | turn |
| `,` / `.` | strafe left / right |
| `F` (or Ctrl) | fire |
| Space / `E` | use - doors, lifts, switches |
| `1`..`6` | select weapon |
| Esc | the pause menu |

**The pause menu** holds Resume and Options; Options holds an FPS counter and
a **Controls** screen listing what every action is bound to, which you can
change. The list is not a picture: it asks the system what each action is
bound to, so it shows this machine's own keys. Arrows move, Enter chooses,
and Esc backs out one screen at a time.

On native keyboards (USB, laptop I2C-HID, or PS/2 after detach) movement is
true held-key: keys move you for exactly as long as they are down.  On the
firmware input path there are no key-release events, so held movement falls
back to typematic repeat.

## The game

E1M1 through E1M9, in order, with the E1M3 secret exit taking you to E1M9.
Monsters (troopers, sergeants, imps, demons, and the E1M8 barons) idle until
they see or hear you, then chase and attack; they take pain, die, and stay
dead on the floor.  Weapons: fist, pistol, shotgun, chaingun, rocket
launcher, plasma - hitscan or projectile, with ammo tracked per type and the
real STBAR status bar (health, armor, ammo, arms, keycards, and the face)
along the bottom.  Locked doors want their keycard.  Barrels explode.  The
nukage hurts.  Dying offers the level again; the E1M8 exit ends the episode.

## Sound

Duum plays the WAD's own audio, and it needs no setup: the effects are the
`DS` sound lumps out of the same file the levels come from, and the music is
that file's score.

Two things follow from where the mixing happens. The effects are mixed **on
top of** the music rather than instead of it, so firing does not cut the
score off. And on a machine with no sound hardware at all, Duum falls back
to the single note each event has always made - the way it sounded before
any of this - rather than going silent.

## Scope

What's deliberately not here: savegames, the automap, and difficulty
selection (things spawn at Hurt-Me-Plenty).  Everything you see is data-driven from the WAD - textures,
flats, sprites, maps, the status bar - so Freedoom reskins the whole game.

See **[Python apps](PYTHON.MD)** for the app model and **[the `uno` API](PYAPI.MD)**
for what Python code can reach.

## Where Duum comes from

Duum is its own project - **https://github.com/hmofet/duum** - under the
Mozilla Public License 2.0, and it runs anywhere Python does, not just here.
UnoDOS ships a build of it; the version is in the first line of `SDK\DUUM.PY`.
