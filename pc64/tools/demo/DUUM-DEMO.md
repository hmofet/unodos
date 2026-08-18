# DUUM-DEMO.md - the standalone Duum demo video

`duum_demo.py` records a short demo of **Duum** (the Python Doom engine,
`pc64/apps/DUUM.PY`) as its own film, separate from the main OS demo
(`scenes.py`). Same spirit and the same plumbing: it boots the DEBUG image
once, drives the real game over URC, and records each scene through
**unostream** (`pc64/UNOSTREAM.md`) - nothing is mocked.

```
duum_demo.py       the recorder: boot, launch Duum, record scenes as mp4 + sidecars
stitch_duum.py     crop each scene to the game canvas, 2x nearest, concat -> cut.mp4
duum_vo_script.json  the narration, as cues anchored to named beats
```

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
python3 gen_vo.py  --script duum_vo_script.json --out-dir out/duum/vo \
                   --timeline out/duum/cut.timeline.json --dry-run    # cost, no spend
python3 gen_vo.py  --script duum_vo_script.json --out-dir out/duum/vo \
                   --timeline out/duum/cut.timeline.json              # spends TTS quota
python3 mux_vo.py  --master out/duum/cut.mp4 --timeline out/duum/cut.timeline.json \
                   --script duum_vo_script.json --vo-dir out/duum/vo \
                   --out out/duum/duum-demo-final.mp4
```

`stitch_duum.py` crops the 1280x800 desktop to the game canvas
(`crop=518:382:53:48`) and doubles it with **nearest-neighbour** so the pixels
stay crisp. `mux_vo.py` anchors each narration cue to its named beat using the
per-scene `.timing.jsonl`, correcting for the guest's variable frame rate.

## The one honest limit worth knowing

**Keys-only combat cannot reliably frame a clean on-screen kill.** The URC `key`
verb injects no fine aim, the hitscan tolerance is tight, and the monster AI is
non-deterministic per boot (same seed, different real-time timing). Takes come
out either "enemies well framed but the player is overwhelmed" or "player
survives but the enemies stay off to the sides." `--hero-takes N` records N
takes to cherry-pick from; it is not guaranteed. The shipped combat scene is the
honest one: firing in the courtyard with health ticking down under enemy fire.
The courtyard also renders ~8-9 fps (full-canvas flats + sky through a Python
renderer under emulation); the interior scenes run ~21 fps. unostream retimes so
the film plays at true speed.
