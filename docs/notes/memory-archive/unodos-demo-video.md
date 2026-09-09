# The UnoDOS pc64 demo-video pipeline (pc64/tools/demo + the demo-video skill), why it records in QEMU and not on metal, and the traps that cost the most

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-demo-video.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---


The narrated walkthrough of UnoDOS pc64, and the reusable pipeline behind it.
Skill: `~/.claude/skills/demo-video/SKILL.md` (sibling of `user-manual`).
Tools: `pc64/tools/demo/` - `scenes.py` (the spine s02-s10), `scene_boot.py` /
`scene_media.py` / `scene_outro.py` (s01 / s06 / s11), `stream_recv.py`,
`stitch.py`, `gen_vo.py`, `mux_vo.py`, `SCENES.md`.

Capture rides **unostream** (new subsystem, `pc64/unostream.c` + `UNOSTREAM.md`):
the guest dials a host receiver over its own TCP stack and pushes QOI
keyframe/tile-delta frames **with the cursor composited in**. Built because URC's
`screen record` is a 4 MB one-shot ring drained at ~21 KB/s **and has no cursor**.
Verbs: `stream start <ip> <port> [fps] [scale] | stop | status`.

## The finished film, and who consumes it

**THE HEADLINE DURATION IN THIS FILE HAS BEEN WRONG TWICE.** Go by the
timeline JSON beside the cut, never by a number written down here or by a
filename. As of 2026-08-21 the lineage is: 2026-08-08 = 9:41, refilmed s08 on
Freedoom = 7:44, the 2026-08-19 Duum-first re-edit = **7:37** (456.7 s,
`out/final2/unodos-demo-final.mp4`, the cut currently PUBLISHED), and the
v3.34.0 cut = **8:35** (514.6 s), rendered 2026-08-21, master in
**`out/v334/`** and copied to the canonical `out/final2/unodos-demo-final.mp4`
that `unodos-site/build.py` reads. It adds s16 (UnoCode) and is **PUBLISHED** as of 2026-08-21: `python3 build.py` in `unodos-site` (must exit 0) then `npx wrangler pages deploy dist --project-name unodos-site`, which is a PRODUCTION deploy on branch `master` despite printing a hash URL that looks like a preview. Give the edge a moment and re-check with a cache-busting query - a first probe can still return the previous file and read like a failed deploy. Manual pages redirect `/manual/x.html` -> `/manual/x` (308), so verify with `curl -L` or every grep comes back empty.

**PUBLISHED 2026-09-03: the past-pin recut is now the live film** (arin unpinned
v3.34.0 that day; see [[unodos-launch-version-freeze]]). It is copied to the
canonical `out/final2/unodos-demo-final.mp4` (16,756,607 B) with its timeline
beside it; the displaced 8:35 cut is `out/final2/_prev_published_8m35.mp4`.
Production deploy `39448d29`, live size verified with a cache-busting query.
**LANDED ON MASTER 2026-09-04 (`457c9c28`, rebase + fast-forward after a green
`tools/gate.sh` on quill: 4 builds, host gates, SPECTEST 87/0); the branch is
deleted.** The recut, formerly on `manual-video-past-pin` in the Documents checkout: `out/recut/unodos-demo-recut.mp4`,
**9:57** (597.2 s), 15 scenes, VO already muxed, audio verified in every scene.
It adds **s17 UnoTransfer** and **s18 the hypervisor** and drops the port
montage. The published film is untouched and still the 8:35 v3.34.0 cut, so
publishing this one crosses the launch pin [[unodos-launch-version-freeze]].
**s19 (UnoCode's assistant) is written in `scenes.py` but NOT recorded** and is
deliberately absent from `stitch.py`'s spine and from `vo_script.json`, so a
stitch today yields the 15-scene cut rather than a hole. It needs a real key at
`assets/anthropic_demo_key` (gitignored); its pre-flight skips with one line
without it. s18 cannot be recorded on a nested host and needs `UNO_DISK_MB`
raised.

**`mux_vo.py` DEFAULTS ITS OUTPUT to `out/final2/unodos-demo-final.mp4`** and
will overwrite the published film without asking. Pass `--out`. When this
happened on 2026-08-21 the previous cut was recovered byte-identical from
**https://unodos.arinbakht.com/assets/unodos-demo.mp4** (11,874,569 B) and kept
as `out/final2/_prev_published_7m37.mp4` - so the live site is a real backup of
whatever cut is currently deployed.

**Bed offsets come from the stats file beside the wav, not from mux_vo's
docstring** (which still shows an s06 trim of 6.68 from an older capture; the
measured value for the current s06.wav is **9.74**, anchored "stop beat vs end
of music"). `s08.stats.json` has no audio block at all, so s08's trim is argued
from construction: the wav is 42.23 s and the scene is 42.267 s, so the capture
spans the scene and trim is 0. Verify after muxing by measuring the film where
the wav is loud AND where it is silent - s08.wav is digitally silent for its own
first 12 s (measured on the recut: silent until about +20 s), so probing there
reads exactly like a bed that failed to mix.

**The other way to fake that reading is `ffmpeg -v error -af volumedetect`.**
volumedetect prints its numbers at INFO level, so `-v error` suppresses them
entirely and EVERY scene reads "no audio" - a whole film that looks silent and
is not. Use `-hide_banner` and grep `mean_volume`.

The published deliverable is the silent stitch plus the VO. Its scenes and
their start times are in the matching `*.timeline.json`.

**Three older cuts sit beside it in the same folder and none of them is the
film**: `unodos-demo.mp4` (6:34), `unodos-demo-interim.mp4` (7:41), and the
whole of `out/final/` (the 2026-08-07 10-scene pass). Go by duration, not by
name: the shortest name is the oldest cut.

**`out/` lives ONLY in the second checkout**, `C:\Users\arin\Documents\Github\unodos`
(see [[unodos-local-checkout-amanuensis]]). `unodos-3` has no `tools/demo/out`
at all, so looking there says "there is no demo video."

The site embeds it: `unodos-site/build.py` copies `VIDEO_SRC` to
`dist/assets/unodos-demo.mp4` at build time (`dist/` is gitignored, so the
bundle is rebuilt, never committed). Override with `UNODOS_DEMO_MP4`. The
default path is **hardcoded to this machine's home directory**, which is how it
went on pointing at `out/final/` and shipped the stale cut for nine days
(fixed 2026-08-17, along with `mux_vo.py`'s defaults, which named a `cut.mp4`
that never existed).

## Record in QEMU. Metal is a dead end today (measured 2026-08-07)

The X13 Yoga renders **~2.5 fps** and it is NOT an idle-desktop artifact: over a
test scene only **22 of 233 frames** arrived faster than 100 ms; 210 were 4 fps
or slower, *during motion*. Footage is unusably choppy. Ruled out: "Aurora lite
(no live compositing)" (2.4 vs 2.5 fps, no effect) and `mtrr-wc` (that boot came
up **TSC-uncalibrated**, so unostream - which paces off the ms clock - emitted
nothing at all; reverted). This is almost certainly the project's known
fleet-wide render/present slowness, and fixing it is an OS job, not a video one.

**A 2.5 fps tick also breaks input:** the shell samples pointer state once per
frame, so a click whose press and release are 180 ms apart falls between samples
and vanishes. That is why the resolution "Keep" click kept missing and the mode
silently auto-reverted. **Use a ~1.2 s press hold on a slow box** and it works.

## The Yoga's account store is on its INTERNAL disk, not the stick

`UNOSEC.DB` (24,784 B) lives on **volume 1 "NO NAME"**, so every boot hits a
sign-in gate **no matter which USB stick boots**, and reflashing the stick does
not clear it. Credentials arin supplied: **claude / [REDACTED]**. Overwriting
`UNOSEC.DB` on the *stick* does nothing (that copy is unused). A wrong-sized
store would re-seed empty (`db_load` needs an exact `sizeof` match) - but only on
the volume unosecure actually picked.

While any security dialog is modal the shell frame loop is replaced, so
**unostream sends nothing** (`on=1 sent=0 drops=0`) - a login sheet looks
exactly like a broken streamer. `ui-unlock` in DEBUG.CFG is what lets a harness
type into those dialogs at all.

## Two clocks, and the narration was on the wrong one (fixed 2026-08-17)

`beats.jsonl` and `timing.jsonl` are WALL CLOCK. The finished scene is not: it
plays its frames at a CONSTANT rate. The guest does not produce frames at a
constant rate, it stalls exactly when it is busy, which is exactly when the
narratable things happen. So a moment recorded at wall-clock T is not at T in
the footage, and `mux_vo` placed every line on the wrong clock.

Measured: `load-wikipedia` happened 39.1s into the recording and sits 41.8s
into the scene, so its line played 2.7s early, over a frame still showing the
previous demo in an unmaximized window. s02's `switcher-f2` drifts 2.6s the
OTHER way. `beat_times()` now converts through the frame log (find the frame
carrying the beat, ask where that frame sits in the scene). **If a scene ever
looks out of sync again, check this conversion before re-recording anything.**

## fmt.doc contained almost none of the formatting it advertises (fixed 2026-08-17)

The demo opened `fmt.doc`, whose own text says BOLDWORD / CENTREPARA, and
NOTHING was formatted, while the narration claimed "the fonts, the formatting
and the layout all come out of the original file". Two causes, both real:

1. **UnoWord threw it away.** `load_doc` called only `ud_doc_plain()`, never
   `ud_doc_chp_at`/`ud_doc_pap_at`. Fixed; open-then-save was also silently
   stripping documents on disk.
2. **The fixture was empty.** `mkcorpus.py`'s NS block never declared the
   **`fo:` namespace**, and every property except underline and strike is an
   `fo:` attribute. LibreOffice does not complain about an unknown namespace:
   it parses, drops those attributes, and writes a valid .doc with the
   formatting absent.

**Both earlier explanations in `UNOAUTOMATE-REQUESTS.md` are wrong** ("LibreOffice
turns automatic styles into Word character styles", "unodoc cannot read the
STSH yet"). unodoc is fine: a round trip through `ud_docw` reports bold, italic
and alignment correctly, and STSH resolution landed in phase 4b'. **Do not
start an STSH slice on this evidence.** Still not surviving: `INDENTPARA`'s
margin-left and `FIRSTPARA`'s text-indent.

**New, unfixed:** UnoWord does not RENDER strikethrough. unodoc reads
`strike=1`, the loader sets it, and nothing is drawn. `uoword.c`'s style
mapping only maps bold and italic to `UNO_FS_*`.

## RECORD WITH KVM. TCG cannot film the heavy scenes (2026-08-18)

`scenes.py` has had a **`UNO_DEMO_KVM=1`** path all along (`-cpu host
-enable-kvm`, 4 GB) and nothing was using it, so every scene in the 2026-08-08
cut was filmed under TCG.

Measured reshooting Duum after it became a full game: **TCG 1.3 fps, 54 frames
over 46 s** - a slideshow. **KVM 17.1 fps, 685 frames, 0 decode errors**, same
scene, same box. Thirteen times faster and the only difference is the flag.

quill can do it: `/dev/kvm` exists, `vmx`, `kvm_intel` loaded, QEMU lists the
`kvm` accelerator. The user was **not in the `kvm` group** - `sudo usermod -aG
kvm arin`, and a fresh ssh picks it up.

### "The guest never dialled in" means a PRODUCTION build. It is never KVM.

That error stopped every run after ~14:00 on 2026-08-18 and looked like a KVM
regression (it appeared right after `usermod -aG kvm`, and 12:41 had worked).
**KVM was never involved.** The tree had been rebuilt at 14:27 **without
`UNO_DEBUG=1`**, and a production build has no URC dial-out at all, so nothing
can ever connect. `UNO_DEBUG=1 bash build.sh` fixed it; s08 then recorded four
times running.

**The error names the build, and the build is what to check first** - the
message says so literally. Diagnose it by LOOKING AT THE ESP, not the harness:
a debug build stages **`build/esp/CRASH/`**, **`build/esp/DOCS/SYMBOLS.TXT`**
and a default **`DEBUG.CFG`**; a production build has none of the three. Stale
`build/uno_debug.o` from an earlier debug build survives a production rebuild
and is NOT evidence, and neither is `strings` finding `DEBUG.CFG`/`remote=` in
the binary - those live in code compiled either way.

**Test dial-in in isolation before touching QEMU flags**: bind 127.0.0.1:5399,
call `scenes.build_disk()` then `scenes.boot_qemu()`, and wait for `accept()`.
That took one minute and proved TCG failed **identically**, which is what
localises the fault to the build rather than the accelerator. Confirming the
symptom under BOTH accelerators is the whole trick; the harness swallows QEMU's
stderr (`stderr=subprocess.DEVNULL`), so it can only ever report "never
dialled in" no matter what actually broke.

Booting the KVM command by hand with `-serial file:` is worth it anyway: the
guest reached `UnoDOS 3.1 / pc64: firmware handoff...` and stopped, which shows
the guest boots fine under nested KVM and the fault is above the firmware.

### Average fps is a MOTION measurement, not a speed one

The restored runs averaged **8 fps against the known-good 17.1**, which reads
like a second regression and is not one. `s08.timing.jsonl` settles it: the
**median inter-frame gap is 45 ms (22 fps)** - exactly the rate `scene_games.py`
documents for 1280x800 with a stream attached. The guest renders at spec while
anything moves. What differs is dead time: **66% of the run stalled >200 ms vs
43%** in the known-good take, and 19.4 vs 27.9 fps during motion. **Compare gap
DISTRIBUTIONS, never frames/duration** - the average conflates render speed with
how much of the scene is standing still.

Ruled out for that gap, so do not re-test: UBSan (`UNO_UBSAN=0` rebuild changed
nothing, 8.0 vs 8.6), the scene body (both takes emit the **same 9 beats at the
same offsets**), resolution (1280x800 scale 1 in both `stats.json`), the WAD
(unchanged since January - `fetch-wad.sh` exits early when the file exists), and
any source edit (only `scenes.py` and `fetch-wad.sh` postdate the good build).
QEMU sat at **100% of one vCPU with zero steal inside quill**, so it is guest
compute-bound.

**The suspect is `leviathan`, and quill cannot see it.** quill is VM 203 on a
Broadwell E5-2695 v4 shared with other guests; during this session the host ran
a `zstd -dc | dd of=/dev/vmdata/controller` restore at load 5.1. Cache and
memory-bandwidth contention from a co-tenant slows a nested guest **without ever
appearing as steal in that guest**. Check `uptime` and `ps` on leviathan before
trusting any framerate measured on quill, and re-measure on an idle host before
concluding anything about the guest.

## s08 REFILMED ON FREEDOOM 2026-08-18, and its turn counts were stale

The film is now **464.4s (7:44)**, `out/final2/unodos-demo-final.mp4`; only s08
changed (40.0s -> 26.6s). The old id-WAD s08 is at `out/final2/_id_wad_backup/`.
See [[duum-demo-video]] for the licensing reason and the SKY1 test that proves
which WAD a frame came from.

**s08's turn counts were written for an engine Duum no longer is.** Its own
docstring still said "MOVE = 12 map units, TURN = 0.20 rad per press, a quarter
turn is 8 presses". Duum moves on HELD keys now: one press = a 0.30s hold =
**~96 units or ~53 degrees**, so the old 8-press turns were a **424-degree
spin**. Anywhere a scene's docstring quotes step sizes, re-measure before
trusting it.

**In a corridor, turn-and-then-walk is fatal**: two presses put you in a wall
(one take spent 20 of its 28s facing grey). Every turn is now a
**look-and-return**, and the scene ends in the corridor rather than jammed in a
corner bleeding health.

Three narration lines had to go: they claimed the WAD was "the original game
data" / "the same level geometry the original game did", which is false against
Freedoom's original maps. s08d now names Freedoom outright.

## Duum is a full game now, and the scene had to be rewritten (2026-08-18)

The s08 choreography walked in a straight line because walking WAS the whole
of Duum. It now has weapons, monsters that chase and shoot back, doors, lifts,
pickups, the real STBAR and E1M1-E1M9 progression, so the scene fires and
works a door as well.

**FOUR BEAT NAMES IN s08 ARE LOAD-BEARING**: `duum-running`,
`walk-into-the-room`, `turn-back-left`, `look-around` each carry a narration
cue. Renaming one does NOT fail the build - `mux_vo` prints a line and drops
the narration to the top of the scene, which is worse than an error because
the film still assembles. Same is true of every other scene's cue anchors.

Duum needs an IWAD in **`pc64/wads/`** (gitignored). `tools/fetch-wad.sh`
**defaults to Freedoom** (`freedoom1.wad`, 28,795,076 B, BSD, redistributable);
`--shareware` gets id's `DOOM1.WAD` instead. build.sh stages whichever it finds
as ESP `DOOM1.WAD`. Without one, `s08_pre` no-ops and the scene is skipped
silently. It is NOT in a `git archive`, so ship it separately to any build host,
along with `unodoc/test/corpus`.

## Building and reshooting without a local toolchain

pc64 needs `x86_64-w64-mingw32-gcc`, which amanuensis does not have. quill
does the whole job: `apt install gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64
nasm mtools qemu-system-x86 ovmf libreoffice-writer`, ship the tree with
`git archive`, `UNO_DEBUG=1 bash build.sh` (the demo lane needs URC), stage
`unodoc/test/corpus` by hand because it is **gitignored**, then
`python3 scenes.py --scene s04 --out-dir out/reshoot`. A single scene reshoots
in about 1.7 minutes. The harness wants Debian's OVMF paths, which is why
Linux and not Windows.

## s16 UnoCode, and three traps from filming it (2026-08-21)

Added for v3.34.0, 58 s, cut in AFTER s07 - Studio is the one with a compiler
behind it and leading with the editor reads as though it were the better of the
two. Beats: workbench, Go to File, command palette, an extension's JavaScript
raising a notification, `js 6*7` in the terminal, theme to Light+ then Nord.

- **UnoCode's default theme is ALREADY DARK.** The first take switched straight
  to Nord and the "the workbench repaints" line would have been narrating a
  change only a diff can see. Go to the built-in **Light+** first: dark -> light
  is unmistakable, and Nord after it is a second visible change AND the one that
  is actually an extension's file. Generally: before scripting a line about
  something changing appearance, put the before and after frames side by side.
- **Ctrl+backtick TOGGLES, it does not focus.** `uc_toggle_panel()` closes the panel
  when it is already visible, on the terminal tab, **and focused**. Pressing it
  a second time for a later command shut the terminal, so `js 6*7` went nowhere,
  the beat logged green, and only the extracted frame showed a closed panel.
  Press it exactly ONCE, while the panel is shut, and run every terminal command
  consecutively.
- **UnoCode reads Shift off the CASE of the character** (`unocode/uc_main.c`),
  so `d.ctrl("P")` is Ctrl+Shift+P (command palette) and `d.ctrl("p")` is
  Ctrl+P (Go to File). One letter apart in case only, two different overlays,
  and the wrong one types the filter text into a document.

## Copy the *.timing.jsonl sidecars, or the boot scene plays 23% fast

Assembling a cut in a FRESH out-dir means copying the previous takes in. Copy
the `.timing.jsonl` files with them. Without `s01`'s, `stitch.py` has only the
container frame rate to go on and the boot scene comes out **14.57 s instead of
17.86 s** - 23% fast, entirely plausible on screen, and it silently ruins every
narration offset after it. It is the same defect as the old stream_recv fps bug,
reached by a different route. `stitch.py` DOES announce the correction when it
can make it (`s01 RETIMED 14.57s -> 17.86s (container was 23% fast)`), so the
tell is the ABSENCE of that line for a scene you expected it on. Only s01 needs
it - every unostream-recorded scene matched to the millisecond either way.

## Traps worth keeping

- **The `py` verb runs ONE LINE.** A multi-line script returns EMPTY for paths
  that read perfectly. This made `scenes.py` report "no assets on the stick" for
  a stick carrying all of them, silently skipping Duum.
- **A native 1280x800 boot in QEMU:** `uefi_main.c` never calls SetMode and gives
  the desktop **half** the firmware's GOP mode, so OVMF's 1280x800 yields a
  640x400 desktop. Double the panel:
  `-vga none -device VGA,edid=on,xres=2560,yres=1600,vgamem_mb=64`. The
  `vgamem_mb` bump is required - 2560x1600x4 = 16.4 MB just exceeds the 16 MB
  default and the mode is otherwise not offered.
- **stream_recv used to mux at the REQUESTED fps, not the arrival rate**, so a
  scene captured at 24 fps and muxed at 30 played 26% fast while looking
  plausible - and destroyed narration timing. Fixed at source (`-itsscale` at
  close); `stitch.py` still retimes older footage from the `.timing.jsonl` wall
  clock and no-ops within 2%.
- **Two claims in the manual were wrong and nearly went into the narration:**
  "TLS from scratch" (it is BearSSL) and "~660 KB image" (a clean UNO_DEBUG=0
  build is **4,009,845 B**). Verify every claim against source before scripting.
- ElevenLabs: **Brian reads at ~2.9 words/sec**, not the 2.4 first budgeted,
  which left 28% of the film silent. Narration is now split into **cues anchored
  to named beats** from each scene's `.beats.jsonl`.

## SSH lines reworded and regenerated (2026-09-03)

Brian read s13b's "Curve twenty five five one nine ... Ed25519 ... AES" as a
string of letters and digits. All six SSH-related cues (s13a-d, s17b, s17f)
were reworded to plain words and regenerated; `vo_script.json` is edited on
`manual-video-past-pin` but NOT committed. s13b now says "modern elliptic
curve key exchange, a modern host key, and AES on the wire" and no longer
claims "all of it written for this project": unossh negotiates
curve25519-sha256 / ssh-ed25519 / aes256-ctr / hmac-sha2-256, and only the
transport and Ed25519 are the project's own - X25519, AES, SHA and HMAC are
BearSSL (`unossh.c`, `unossh_wire.c`).

- **The recut's s06 bed trim is 9.66, not 9.74.** 9.74 belonged to the v334
  wav. For `out/recut/s06.wav` the stats file gives music_end 29.1 minus
  stop_beat 19.44 = 9.66, and a mux at 9.66 reproduces the previous recut's s06
  audio sample for sample (residual 0.000); 9.74 landed the bed 80 ms early.
  Verify a re-mux by cross-correlating a narration-free slice (261.02-261.34 s
  of the film) against the previous cut - the narrated stretch correlates only
  0.74 even when the bed is right, because the voice dominates the argmax.
- **`gen_vo.py` without `--scene` generates EVERY cue that has no clip**, which
  includes scenes written but unrecorded: it spent ~450 characters on s19a-e
  this way. `out/vo/s19*.mp3` now exist and are in the manifest, so they will
  not be billed again. Pass `--scene` when only one scene changed.
- Script committed as `38bb0565` on `manual-video-past-pin` (with audit_cues.py
  and the s19 cues). **A pinned-cut variant with ONLY the SSH scene re-voiced
  exists: `out/v334/unodos-demo-final-ssh.mp4`** (14,027,710 B, 514.55 s),
  made by SPLICING s13's span (409.5-466.6 s) of a fresh mux of `out/v334/cut.mp4`
  into the published film's audio, because the pinned cut cannot simply be
  re-muxed: the recut re-filmed s16 with different beat names and rewrote
  s01b/s04d, and its clips overwrote the pinned-era ones in `out/vo`. Both
  joins sit on silence; outside s13 the audio correlates 1.0000 with the
  published film. Both variants were deployed to the `preview` branch on
  2026-09-03; production deploy was HELD pending arin's call under
  [[unodos-launch-version-freeze]] (the recut carries s17/s18 past the pin).
- **2026-09-04: master merged into `manual-video-past-pin` (`ad927ade`, 166
  commits, clean) and the film was NOT re-recorded.** Evidence: the only shell
  change since the branch point is the Wi-Fi pane and the tray chip (three
  commits in `pc64_uui.c`); on a wired QEMU box the chip still reads LAN, the
  System app is untouched, and no scene opens the Network tab, so no frame in
  the 9:57 recut shows anything master changed. Re-check this list before any
  future "does the video need redoing" question rather than re-filming on
  reflex.
- Previous audio kept as `out/recut/_prev_ssh_reword_9m57.mp4`; the changed
  scenes alone are in `out/recut/_excerpt_ssh_s13_s17.mp4`.

## Defects this shoot found (all filed in pc64/UNOAUTOMATE-REQUESTS.md)

- `unoamp_skin.c bmp_decode()` refused BI_RLE8, so **real Winamp skins never
  loaded** - FIXED + merged, plus a `skin load` URC verb so a re-skin can happen
  on camera.
- `uno_fs_isdir`/`uno_fat_list_ex` marks every FAT **file** as a directory,
  breaking Open-from-disk across the office suite; `uoshow.c` swallows Backspace.
- UnoAmp's visualiser only repaints when the title marquee advances, so with a
  short filename it **never visibly moves** - do not claim it in narration.
- Studio's scene typed C into a **.PY** file and the input path **drops `(`, `)`,
  `*`, `;`** - the "compile and run" beat was failing on camera.

See [[unodos-unostream]] if it exists, [[unodos-pc64-manual]], [[unodos-urc-listen-mode]].
