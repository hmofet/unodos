# WADs for Duum

Duum (the Python Doom engine, `apps/DUUM.PY`) loads a Doom IWAD from this
directory. **No WAD is committed** - the whole `wads/` folder is gitignored,
exactly like the Wi-Fi firmware `fw-blobs/`.

Drop one of these in as `wads/DOOM1.WAD`:

- **`DOOM1.WAD`** - id Software's *Doom* shareware IWAD. Freely
  distributable (this is the shareware episode "Knee-Deep in the Dead"), but
  **not** redistributable inside this repo, so fetch it yourself:

      tools/fetch-wad.sh           # downloads DOOM1.WAD into wads/

- **`freedoom1.wad`** - the [Freedoom](https://freedoom.github.io/) Phase 1
  IWAD (BSD-licensed, drop-in compatible). This is the **shippable free
  default**: a distro may include it, and Duum falls back to it when
  `DOOM1.WAD` is absent.

The build stages whichever WAD is present onto the ESP as `DOOM1.WAD` (or
`FREEDOOM.WAD`) so Duum can `uno.read_at` it. Duum never loads the whole
~4 MB file into memory; it reads the directory, then lumps on demand.
