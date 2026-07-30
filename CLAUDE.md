# UnoDOS, agent instructions

## READ FIRST: [`/AGENTS.md`](AGENTS.md)

Before starting any work, read [`AGENTS.md`](AGENTS.md) at the repo root. It is the
one working agreement for **every** agent on this repo (lanes and the ownership
registry, shared choke-points, branch/merge discipline, claims/requests). It applies
symmetrically to all agents, including the unoautomate agent. The notes below in this
file are project state and history; AGENTS.md is the process you follow.

## Fresh-port parity, state as of 2026-07-20

The 2026-07-19 audit concluded the June parity work was "never committed and is
lost". **That was wrong.** It was committed and pushed on
`parity-push-fresh-ports`, a branch the survey missed, and it is now **merged to
master**.

- **sms, nes, gba, rpi, pinephone, ppcmac are at 11 of 11 apps**: Tracker,
  OutLast, Pac-Man and Paint are real and wired into dispatch. All six build.
- **gb, gg, vic20, ws, pce still ship 7 of 11** (those four are launcher
  placeholders). Storage persistence is outstanding across the whole fresh tier.
- `parity-wip` (`b2e40c1`, does not build by design) is now **fully superseded**
  by master and holds nothing worth recovering. Do not merge it; it is a
  deletion candidate.
- `docs/FEATURE-MATRIX.md` is stale (no C64 column, pc64 storage row predates
  the native drivers, fresh-port rows now understate six ports), fix it as
  parity lands.

[`docs/PARITY-HANDOFF.md`](docs/PARITY-HANDOFF.md) carries the full history,
including the correction above; read it before resuming parity work.

**The procedural lesson:** before concluding work is lost, check every branch
and every remote, not just the mainline.

## `pc64-usb-flasher` branch - RESOLVED, safe to delete (2026-07-30)

The open question here (was its content really subsumed by master?) is closed.
Checked three ways rather than by commit message:

- **Patch equivalence.** `git cherry origin/master pc64-usb-flasher` marks 9 of
  its 11 commits as already upstream, including both of the ones this note
  singled out as needing a close look: the X1 trackpad fix (`6fc5980b`) and the
  dreamcast/ps2 module-loader fix (`21d2bb77`).
- **File by file.** The only files existing on the branch and not on master were
  `pc64/dec_aac.c`, `dec_midi.c`, `dec_mp3.c`, `dec_wav.c`, `mp3_tables.h` and
  `tools/mkmp3tables.py` - exactly the pre-`unomedia` decoder architecture this
  note warned that merging would resurrect. Master replaces all six one for one
  with `unomedia/um_aac.c`, `um_midi.c`, `um_mp3.c`, `um_wav.c`, `aac_tables.h`,
  `mp3_tables.h` and both table generators.
- **The two non-equivalent commits.** `71e7c643` is that dead media stack.
  `1c24462f` is 102 files of screenshots regenerated from a fortnight-old build
  plus click-to-enlarge, which master already has in `docs/build_site.py`.

So there is nothing to merge and a clear reason not to merge. The branch is
safe to delete, local and `origin/pc64-usb-flasher`, whenever someone wants the
tidy-up; nothing on it is unrecoverable, since every commit is either upstream
already or content master deliberately replaced.

## Standing rule (2026-07-21): the flasher embeds BOTH production + debug builds

**Supersedes the 2026-07-20 "ships ONE flasher = the debug build" rule.** The
single flasher (`flash/build-flasher.ps1`) now builds BOTH the production OS
(`UNO_DEBUG=0`) and the debug/stress OS (`UNO_DEBUG=1`) and embeds both ESP
trees as resources `unodos_esp_prod` / `unodos_esp_debug`. It **formats the
whole disk as one FAT32 volume** either way.

- **Developer options OFF (default)** → flashes the clean PRODUCTION build.
- **Developer options ON** → flashes the DEBUG build AND writes a `\STRESS.CFG`
  from the test toggles (conformance `spec`, WiFi/Ethernet network test,
  `mtrr-wc`, stress passes, auto power-off). WiFi/Eth toggles map to
  `net-force-wifi` / `net-eth-only` / `nonet`.

Intel WiFi firmware (`fw-blobs/`) is bundled into **both** trees as of
2026-07-29 (user ruling): a production stick with a dead radio is not a
shippable OS, and the blobs on your own stick are not redistribution. The
licence constraint applies when an image is PUBLISHED - build those with
`UNO_NOFW=1 ./build.sh`. The raw dd/Rufus image
is the production build. `build.sh` populates `build/esp` incrementally, so the
flasher wipes it before each of the two builds. Deploy is unchanged
(`build-flasher.ps1` → `deploy-to-share.ps1`).

## Flasher deploy is no longer mandatory after a build (2026-07-23)

**Retired:** the former standing rule that *every* pc64 build must be followed by
`pc64\flash\deploy-to-share.ps1` to publish a fresh USB flasher to
`\\behemoth\unreplicated\unodos\pc64\`. Now that the OS can be installed and
updated **over the network** (the URC `install <disk>` verb + `unostorage`
clone-over-link, see `pc64/REMOTE.md`), a running box no longer depends on a
freshly-staged USB flasher to receive a new build.

The flasher (`pc64\flash\build-flasher.ps1` → `deploy-to-share.ps1`, embedding
both prod + debug ESP trees per the rule above) still exists and works, use it
when you specifically want a bootable USB stick, but rebuilding/publishing it is
now **opt-in**, not an automatic step after `build.sh`.
