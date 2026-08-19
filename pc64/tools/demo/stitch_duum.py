#!/usr/bin/env python3
"""Stitch the Duum scene recordings into one master cut: crop each 1280x800
desktop frame to the Duum game canvas, upscale by an integer factor with
nearest-neighbour (a pixel game must stay crisp), normalise to a common fps,
concatenate, and emit a timeline the narration is written against.

  python3 stitch_duum.py --out-dir out/duum --master out/duum/cut.mp4
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# game canvas within the 1280x800 desktop (measured off a probe frame)
CROP_W, CROP_H, CROP_X, CROP_Y = 518, 382, 53, 48
ZOOM = 2                        # integer, nearest-neighbour
FPS = 30

SCENES = ["s01", "s02", "s03", "s04", "s05"]
TITLES = {"s01": "Title", "s02": "The renderer", "s03": "Combat",
          "s04": "The pause menu", "s05": "The HUD"}


def wav_duration(wav):
    n = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                        "format=duration", "-of",
                        "default=noprint_wrappers=1:nokey=1", wav],
                       capture_output=True, text=True).stdout.strip()
    return float(n) if n else 0.0


def cut_audio(out_dir):
    """Slice the run's single capture into one wav per scene.

    QEMU's wav sink writes ONE file for the whole run, and it starts writing
    when the guest opens the stream rather than when the emulator starts - so
    its zero is not the recorder's zero and must not be assumed. It is
    derivable instead: the sink stops when QEMU is killed, so

        wav t=0  ==  t_end - (length of the wav)

    which needs no guess about when audio began, and folds the sink's own
    drift into one measured number. Each scene is then cut between its first
    and last FRAME timestamps, which are wall clock in that same clock.

    Prints the mapping, because a silent bed and a misaligned one look
    identical in a file listing."""
    idx_path = os.path.join(out_dir, "audio.json")
    src = os.path.join(out_dir, "audio.wav")
    if not (os.path.exists(idx_path) and os.path.exists(src)):
        print("no audio capture (%s) - the cut will be silent" % src)
        return {}
    idx = json.load(open(idx_path))
    dur = wav_duration(src)
    wav_t0 = idx["t_end"] - dur
    print("audio: %.1fs captured, its zero at t0+%.1fs"
          % (dur, wav_t0 - idx["t0"]))
    out = {}
    for s in SCENES:
        tj = os.path.join(out_dir, s + ".timing.jsonl")
        if not os.path.exists(tj):
            continue
        rows = [json.loads(l) for l in open(tj) if l.strip()]
        if not rows:
            continue
        a = rows[0]["t"] - wav_t0
        b = rows[-1]["t"] - wav_t0
        if a < 0 or b <= a:
            print("  %s: outside the capture, skipped" % s)
            continue
        dst = os.path.join(out_dir, s + ".wav")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", "%.3f" % a,
                        "-t", "%.3f" % (b - a), "-i", src, dst], check=True)
        out[s] = dst
        print("  %s: wav %.2f..%.2f (%.1fs)" % (s, a, b, b - a))
    return out


def true_duration(mp4):
    j = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                        "format=duration", "-of",
                        "default=noprint_wrappers=1:nokey=1", mp4],
                       capture_output=True, text=True).stdout.strip()
    return float(j) if j else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default="out/duum")
    ap.add_argument("--master", default=None)
    args = ap.parse_args()
    master = args.master or os.path.join(args.out_dir, "cut.mp4")

    vf = ("crop=%d:%d:%d:%d,scale=%d:%d:flags=neighbor,fps=%d,format=yuv420p"
          % (CROP_W, CROP_H, CROP_X, CROP_Y,
             CROP_W * ZOOM, CROP_H * ZOOM, FPS))

    beds = cut_audio(args.out_dir)
    norm = []
    for s in SCENES:
        src = os.path.join(args.out_dir, s + ".mp4")
        if not os.path.exists(src):
            print("skip (missing):", src)
            continue
        dst = os.path.join(args.out_dir, s + ".norm.mp4")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src,
                        "-vf", vf, "-an", "-c:v", "libx264", "-preset",
                        "medium", "-crf", "18", dst], check=True)
        norm.append((s, dst))

    listfile = os.path.join(args.out_dir, "concat.txt")
    with open(listfile, "w") as f:
        for _, dst in norm:
            f.write("file '%s'\n" % os.path.abspath(dst))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe",
                    "0", "-i", listfile, "-c", "copy", master], check=True)

    timeline, t = [], 0.0
    for s, dst in norm:
        dur = true_duration(dst)
        timeline.append({"id": s, "title": TITLES.get(s, s),
                         "start": round(t, 3), "dur": round(dur, 3),
                         "duration": round(dur, 3),   # mux_vo reads "duration"
                         "beats": os.path.join(args.out_dir, s + ".beats.jsonl")})
        t += dur
    tl = os.path.splitext(master)[0] + ".timeline.json"
    with open(tl, "w") as f:
        json.dump({"master": master, "total": round(t, 3),
                   "w": CROP_W * ZOOM, "h": CROP_H * ZOOM,
                   "scenes": timeline}, f, indent=1)
    print("master: %s  (%.1fs)\ntimeline: %s" % (master, t, tl))
    for e in timeline:
        print("  %s %-14s %5.1fs  @ %5.1f" % (e["id"], e["title"],
                                              e["dur"], e["start"]))
    if beds:
        print()
        print("beds for mux_vo (the game's own audio, under the narration):")
        print("  " + " ".join('--bed "%s:%s:0:GAIN"'
                              % (s, os.path.relpath(w, HERE))
                              for s, w in beds.items()))


if __name__ == "__main__":
    main()
