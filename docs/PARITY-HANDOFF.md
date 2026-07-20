# Parity program — handoff (2026-07-19)

A full-family parity audit ran on 2026-07-19, followed by a fix program that was
**cut short by an account spend limit** (six worker agents were killed mid-edit).
This document records what landed, what is parked, and exactly how to resume.

---

## The audit finding that started it

`docs/FEATURE-MATRIX.md` and the project notes claimed the eleven "3.1-fresh"
ports had been brought to the 11-app roster in June. **They had not.** A
whole-tree survey (this repo, the legacy remote, and a disk search) found no
Tracker/OutLast/Pac-Man/Paint sources for any fresh port. What survives of that
June effort is a set of **orphan compiled binaries** in gitignored build
directories (`unodos_ol/_pm/_tk/_paint`, `_fs*`) — outputs whose source was
never committed. The June session worked in an untracked copy of the tree.

**Ground truth today:** every fresh port (sms, nes, gb, gg, vic20, ws, pce, gba,
rpi, pinephone, ppcmac) ships **7 of 11 apps** — SysInfo, Clock, Notepad, Files,
Theme, Music, Dostris are real; **Tracker, OutLast, Pac-Man, Paint are launcher
placeholders** that open an empty framed window (`app_generic`). None has
storage persistence. The mature tier (x86, Amiga, Mac ×2, MacPlus, Genesis,
Apple II, IIGS, SNES, C64, PS2, Dreamcast) *is* at 11-app parity.

The lesson is procedural, and it is why the interrupted work below was committed
to a branch rather than left in a working tree: **uncommitted work does not
exist.**

---

## Landed (commit `5fc051e`, merged to master)

| Item | State |
|---|---|
| **x86 FAT12 seek fix** | Done, self-tested |
| **Apple II Files delete/rescan** | Was already wired; doc corrected, tests added, disk rebuilt |
| **SNES Notepad caret + Paint tools** | Done, Mesen2-verified |
| **Dreamcast unoui roster shell** | Done, Flycast-verified, now the default `dc` target |

**x86 FAT12** (`kernel/kernel.asm`) — `fat12_read` rejected any read at a
non-zero file position (`AUDIT-x86.md` B1) while `fat16_read` handled it. It now
mirrors the FAT16 seek: `skip = pos>>9` cluster-chain walk, then an unaligned
head bounce-read of `pos&511` before rejoining the cluster loop. Verified by a
gated boot self-test (`-DFAT12_SEEK_TEST`) that byte-compares three chunked read
patterns (700+700+700, 512+1588, 100+2000) against one reference read — PASS on
the fix, **FAIL on an unfixed kernel with the same test spliced in** (negative
control), plus a clean desktop regression shot.

**Apple II** — the README claimed `d`-delete and `r`-rescan were unwired; the
code (`files.i:76-86,145`) says otherwise. Doc corrected; added
`tests/files_delete.script` and `tests/files_delete_persist.script` proving the
delete + catalog flush survives a remount. `build/unodos_apple2.dsk` rebuilt (it
was a month stale).

**SNES** — Notepad is now a caret editor (insert/backspace at the caret, arrow
navigation by logical line preserving the column, inverted-cell caret); Paint
gained pen cycling with wrap and an eraser with an on-screen `ERASE` indicator.
Evidence: `snes/build/np_caret.png`, `pt_tools.png`, `m2_regress.png`.

**Dreamcast** — `dc_uui.c` grew from a static two-window demo (117 lines) into a
real shell (~650 lines): per-app windows over the roster, taskbar with window
chips, Start-menu launcher, maple d-pad navigation. `uui` is now the **default**
`dc` build target; the legacy Mac-compat core remains at `./build.sh dc legacy`.
Evidence: `dreamcast/build/dc_uui_desktop.png` — desktop, taskbar and four live
app windows (Paint with its tool palette, Files, Dostris, Tracker) in Flycast.

---

## Parked on branch `parity-wip` (commit `b2e40c1`) — **does not build**

Six ports were mid-edit when the agents were killed. The work is real and
substantial (new per-app sources for four ports), so it is committed on its own
branch. `master`/`pc64-usb-flasher` are unaffected and build clean.

| Port | Build at kill | What exists | What's missing |
|---|---|---|---|
| vic20 | **OK** | new app sources | dispatch not wired |
| rpi | **OK** | `tracker/pacman/outlast/paint/fs.inc.s` | dispatch not wired |
| nes | FAIL | `tracker/pacman/outlast/paint.inc` | "Source is not resolvable" — mid-wire |
| gba | FAIL | `tracker/fs.inc.s` | `outlast.inc.s` included at `kernel.s:697` but never written |
| ppcmac | FAIL | `tracker/pacman/outlast/paint/fs.inc.s` | `t_tracker` title-table entry |
| pinephone | FAIL | `tracker/pacman/outlast/fs.inc.s` | `paint.inc.s` included at `kernel.s:975` but never written |

To resume: `git checkout parity-wip`, finish the missing files + wiring per port,
then harness-verify each app before merging forward.

---

## Outstanding work, in recommended order

1. **Finish `parity-wip`** — rpi and vic20 are closest (they build; only dispatch
   wiring remains). Then ppcmac (one symbol), gba and pinephone (one file each),
   then nes.
2. **Remaining fresh ports** — the same four apps on **pce, gb, ws, gg**, and
   **sms** last (its windowed WM makes Pac-Man the hard case: a 28×25 maze into a
   32×24 screen needs a fitted/trimmed maze in a borderless window).
3. **USV1 storage** on the fresh ports — donor `c64/fs.i`, harness-persisted via
   a `--storage` sidecar; real media (battery SRAM, SD, eMMC) stays the
   documented hardware tail.
4. **PS2 unoui shell** — port the finished `dreamcast/dc_uui.c` shell to
   `ps2/ee_uui.c` and make it the default `ee` target. No PCSX2 on this box, so
   it is build-verified only; the DC version is the working reference.
5. **C64 IEC/1541 driver** (its remaining M2 item) and **refresh `mac/build/*.dsk`**
   (the disk images lag the rebuilt binaries by ~9h).
6. **Stale committed images** — `gb`, `gg`, `ws` ROMs still predate their sources
   (gba and apple2 were rebuilt during this program). These are now public
   download links from the manual, so they should be rebuilt and recommitted.
7. **`docs/FEATURE-MATRIX.md` refresh** — it is wrong in several places: no C64
   column at all; the pc64 storage row predates the native NVMe/AHCI/eMMC
   drivers; the "unoui not yet wired into the port glue `main()`s" line is
   obsolete (dc_uui.c/ee_uui.c both wire it, and DC now ships it by default);
   and the fresh-port rows should not imply 11-app parity.

## Verification tooling (all present and working)

- **Fresh ports** — per-port `harness.py`: Unicorn (AArch64/ARM/PPC/x86-16) or
  py65 instruction-level cores that render the framebuffer/tilemap to PNG
  headlessly. `--storage <sidecar>` persists the FS region across runs.
- **SNES** — `run_mesen.ps1` (Mesen2 F12 framebuffer captures).
- **Dreamcast** — Flycast at `C:\Users\arin\dc-tools\flycast\flycast.exe`.
  Capture with `~/.claude/tools/cc-capture.ps1 -Window Flycast` (focus-independent;
  hotkey/SendKeys capture does not work over RDP on this machine).
- **x86** — `tools/qemu_test.py` headless QEMU.
- **pc64** — `harness.py`, `docs_shots.py`, `tools/ui_verify.py`.
