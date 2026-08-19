# DUUM-DEMO.md - the standalone Duum demo video

`duum_demo.py` records a short demo of **Duum** (the Python Doom engine,
`pc64/apps/DUUM.PY`) as its own film, separate from the main OS demo
(`scenes.py`). Same spirit and the same plumbing: it boots the DEBUG image
once, drives the real game over URC, and records each scene through
**unostream** (`pc64/UNOSTREAM.md`) - nothing is mocked.

```
duum_demo.py       the recorder: boot, launch Duum, record scenes as mp4 + sidecars
stitch_duum.py     crop each scene to the game canvas, 2x nearest, concat -> cut.mp4
                   ...and slice the run's audio capture into one wav per scene
duum_vo_script.json  the narration, as cues anchored to named beats
```

The spine is five scenes: title, the renderer walk, combat, **the pause menu
and its Controls screen**, and the HUD hold.

Voiceover reuses the OS demo's `gen_vo.py` + `mux_vo.py` unchanged.

## Where it runs

QEMU with **KVM** (a `-vga none -device VGA,edid=on,xres=2560,yres=1600` panel
gives a 1280x800 desktop; the Duum window is cropped out in post). WSL2 does not
work on the usual Windows box, so the recording host is a Linux box with the
mingw toolchain, qemu, OVMF, mtools and ffmpeg - see the machine notes. Outputs
land in `out/duum/` (gitignored).

## Record

```bash
UNO_DEBUG=1 UNO_DETACH=1 ./build.sh          # from pc64/, with a WAD in pc64/wads/
cd tools/demo
python3 duum_demo.py --list                  # the scenes
python3 duum_demo.py --all                    # boot once, record every scene
python3 duum_demo.py --scene s02,s03          # just these (comma-separated)
python3 duum_demo.py --rehearse "a=UUUUUUUU b=RRR"  # drive + screenshot each beat
```

Each scene **relaunches Duum fresh** so it starts from the E1M1 start room, and
the WAD parse + first frame happen before the stream starts (off camera). The
DEBUG.CFG carries `nohud` so the red perf overlay is not burned into the frames.

Keys are Duum's own: `U/D` forward/back, `L/R` turn, `,/.` strafe, `F` fire,
space use, `1`-`6` weapon. A `key` event moves the player for its 0.30 s settle,
so a press is one step; turns are coarse (~one big swing per key), which the
scene scripts are written around.

## Stitch, narrate, mux

```bash
python3 stitch_duum.py --out-dir out/duum --master out/duum/cut.mp4
# ...it prints the --bed arguments for mux_vo, one per scene
python3 gen_vo.py  --script duum_vo_script.json --out-dir out/duum/vo \
                   --timeline out/duum/cut.timeline.json --dry-run    # cost, no spend
python3 gen_vo.py  --script duum_vo_script.json --out-dir out/duum/vo \
                   --timeline out/duum/cut.timeline.json              # spends TTS quota
python3 mux_vo.py  --master out/duum/cut.mp4 --timeline out/duum/cut.timeline.json \
                   --script duum_vo_script.json --vo-dir out/duum/vo \
                   --out out/duum/duum-demo-final.mp4 \
                   --bed "s01:out/duum/s01.wav:0:0.25"      # ...one per scene
```

Gain 0.25 was measured, not guessed: it puts the game about 9 dB under the
voice during gunfire and 13 dB under it during speech, which keeps the score
present without competing with the narration.

`stitch_duum.py` crops the 1280x800 desktop to the game canvas
(`crop=518:382:53:48`) and doubles it with **nearest-neighbour** so the pixels
stay crisp. `mux_vo.py` anchors each narration cue to its named beat using the
per-scene `.timing.jsonl`, correcting for the guest's variable frame rate.

## Audio: the game has a voice now

The film carries the guest's own sound - the WAD's effects and its score -
mixed under the narration. Three things about that are not obvious.

**AC'97, not Intel HDA.** With an `intel-hda` device attached the guest never
reaches URC dial-out **under KVM**: measured 4 times out of 4, and identically
on a build from before any of the sound work, so it is not a regression from
it. The same build with `-device AC97` dials in every time, and the same HDA
device under TCG is fine. `snd_pcm` sits above both backends, so the recording
captures the same mixer either way. Filed in `../../UNOAUTOMATE-REQUESTS.md`.

**One capture per RUN, so reshoot with `--all`.** QEMU's wav sink writes ONE
file for the whole process; re-recording a single scene starts a new QEMU and
overwrites it, which leaves every other scene's audio belonging to a run that
no longer exists. The stitcher says so (`sNN: outside the capture`).

**The wav's zero is derived, never assumed.** The sink starts writing when the
guest opens the stream, not when QEMU starts - 18 s into a run here. Since it
stops when QEMU is killed, `wav t=0 == t_end - (length of the wav)`, and each
scene is cut between its own first and last frame timestamps, which are wall
clock in the same clock. Checked against an event rather than trusted: the
gunshot transients in the combat scene land within ~0.2 s of the fire beats.

## Routes, and the limits that are real

**Walk in straight lines.** One press is a 0.30 s hold: about 96 map units
forward or 53 degrees of turn, and the guest's clamped `dt` means it travels a
slightly different distance every run. Turning and then walking therefore
accumulates heading drift, and a rehearsal that turned once and walked six
presses ended up jammed in a dark corner for the rest of the take. Every scene
here goes straight, or looks and returns.

**Stop before the corridor unless the scene is combat.** Duum's zombies chase.
The ten-press version of the renderer walk arrived at 1% health with two of
them at point-blank, which is a fine combat shot and useless under narration
about the renderer.

**Combat CAN frame a kill on Freedoom**, which it could not on id's E1M1 - the
corridor walks a zombie into you, so walking straight and firing straight is
enough. That old "cannot frame a clean kill" limit was a property of id's map,
not of the key model.

**The frame rate is the device's own.** The walk and the fight run 9-11 fps
(full-canvas flats through a Python renderer under emulation), the menu 25 fps,
the title and HUD 16-19. unostream retimes each scene so the film plays at true
speed rather than pretending to be smooth.

## The engine is vendored

`pc64/apps/DUUM.PY` is generated from the upstream Duum repository
(https://github.com/hmofet/duum) and must not be edited here; a gameplay or
rendering change for the film goes upstream first. This recorder, and the
device A/B in `duum_ab.py`, are UnoDOS-side and are edited normally. See
[`../../DUUM-UPSTREAM.md`](../../DUUM-UPSTREAM.md).
