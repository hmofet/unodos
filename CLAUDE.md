# UnoDOS — agent instructions

## READ FIRST: [`/AGENTS.md`](AGENTS.md)

Before starting any work, read [`AGENTS.md`](AGENTS.md) at the repo root. It is the
one working agreement for **every** agent on this repo (lanes and the ownership
registry, shared choke-points, branch/merge discipline, claims/requests). It applies
symmetrically to all agents, including the unoautomate agent. The notes below in this
file are project state and history; AGENTS.md is the process you follow.

## Fresh-port parity — state as of 2026-07-20

The 2026-07-19 audit concluded the June parity work was "never committed and is
lost". **That was wrong.** It was committed and pushed on
`parity-push-fresh-ports`, a branch the survey missed, and it is now **merged to
master**.

- **sms, nes, gba, rpi, pinephone, ppcmac are at 11 of 11 apps** — Tracker,
  OutLast, Pac-Man and Paint are real and wired into dispatch. All six build.
- **gb, gg, vic20, ws, pce still ship 7 of 11** (those four are launcher
  placeholders). Storage persistence is outstanding across the whole fresh tier.
- `parity-wip` (`b2e40c1`, does not build by design) is now **fully superseded**
  by master and holds nothing worth recovering. Do not merge it; it is a
  deletion candidate.
- `docs/FEATURE-MATRIX.md` is stale (no C64 column, pc64 storage row predates
  the native drivers, fresh-port rows now understate six ports) — fix it as
  parity lands.

[`docs/PARITY-HANDOFF.md`](docs/PARITY-HANDOFF.md) carries the full history,
including the correction above; read it before resuming parity work.

**The procedural lesson:** before concluding work is lost, check every branch
and every remote, not just the mainline.

## `pc64-usb-flasher` branch — needs a diffed look before merge/delete (2026-07-20)

Local branch `pc64-usb-flasher` (tip `21d2bb7`) shows as 14 commits ahead of
master and 52 ahead of its own `origin/pc64-usb-flasher`. A first pass found
that every one of those 14 commits' *stated* content already exists on master
under a different hash (right-click launcher, Clock app with world map,
`flash/UnoFlashCli.cs`, the X1 trackpad fix, the dreamcast/ps2 module-loader
fix, `pc64/tools/install_confirm_test.py`) — master evidently reimplemented the
same features independently after the `unomedia` library refactor, and this
branch predates that refactor (its media work still assumes local
`dec_wav.c`/`dec_midi.c`/`dec_mp3.c`, which master deleted in favor of
`unomedia/`). `git merge-tree` against master conflicts on `pc64_media.c`,
`pc64_music.c`, `pc64_uui.c`, `pc64_uui_apps.c`, `pc64_icons.c`, `AUDIO.md`,
`MODULES.md`, `build.sh`, plus regenerated docs/screenshots and one tracked
binary (`ppcmac/build/unodos.bin`, which probably should not be tracked at
all).

That check was commit-message-level, not a real file-by-file diff — it is
plausible but NOT confirmed that 100% of the branch's content is subsumed.
**Before merging or deleting this branch**, diff it against master file by
file (not just `git log --grep`) to make sure nothing genuinely unique is
still sitting on it. Likely candidates to check closely: the X1 trackpad fix
(master's i2c-hid work moved further, but confirm no behavior regressed) and
anything under `dreamcast/`/`ps2/` not already covered by `f02fc57`.

If the diff confirms full supersession, delete the branch (local +
`origin/pc64-usb-flasher`) rather than merging - merging would resurrect the
dead pre-`unomedia` decoder architecture in `pc64_media.c`/`pc64_music.c`.

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

Intel WiFi firmware (`fw-blobs/`) is bundled into the DEBUG tree only (licence:
no redistribution), so the production image is clean. The raw dd/Rufus image
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
both prod + debug ESP trees per the rule above) still exists and works — use it
when you specifically want a bootable USB stick — but rebuilding/publishing it is
now **opt-in**, not an automatic step after `build.sh`.
