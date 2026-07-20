# Duum - Doom, in Python

Duum is a Doom engine **written in Python** (`SDK\DUUM.PY`), the proof that
Python is a first-class app language on UnoDOS: it loads a real Doom IWAD,
parses the map, and renders a first-person, BSP-traversed view of the level
you can walk around - all from Python, through the `uno` API.

It exercises the whole platform at once: file I/O (it streams the multi-MB WAD
with `uno.read_at`, never loading it whole), heavy compute (the BSP walk and
the column renderer), the framebuffer (the `uno` canvas), keyboard input, and
floating-point math (projection and trig).

## Getting a WAD

Duum needs a Doom-format **IWAD** on the disk as `DOOM1.WAD`. None ships with
UnoDOS - game data belongs to its makers - so supply one yourself:

- **Freedoom** (freedoom.github.io) - a free, from-scratch, Doom-compatible
  IWAD under a BSD license. This is the recommended free option; rename
  `freedoom1.wad` to `DOOM1.WAD`.
- **Doom shareware** - id Software's freely distributable shareware
  `DOOM1.WAD` ("Knee-Deep in the Dead"). `tools/fetch-wad.sh` downloads it.

Put the file next to the apps on the boot disk (the build stages it there
automatically when you have one in `pc64/wads/`). Without a WAD, Duum opens
and tells you it is missing.

## Running it

Duum is a Python app, so it runs on the Python runtime (`APPS\PYRT.UNO`) like
any other. Two ways to launch:

- **From Files** - open the Files app, find `APPS\DUUM.UNO`, and press
  **Enter** (or double-click). Any `.UNO` app launches this way.
- **From Studio** - open `SDK\DUUM.PY`, press **Ctrl-R**. Studio packages the
  Python source and runs it.

## Controls

| Key | Action |
|---|---|
| Up / Down | move forward / back |
| Left / Right | turn |
| `,` / `.` | strafe left / right |

The first frame takes a moment - the interpreter parses the WAD and walks the
BSP tree - then the view redraws as you move.

## Scope

This is v1: the walls, floors and ceilings are **flat-shaded** (by distance,
sector light and wall orientation), with sky filling open areas. It renders
one level (E1M1) and has simple wall collision. **Texture mapping, things
(monsters/items), doors and combat are not implemented** - Duum is a proof
that the platform can carry a real engine, not a complete port. The renderer
already produces per-column spans, which is the right shape to add textures
later.

See **[Python apps](PYTHON.MD)** for the app model and **[the `uno` API](PYAPI.MD)**
for what Python code can reach.
