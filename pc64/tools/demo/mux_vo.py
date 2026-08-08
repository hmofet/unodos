#!/usr/bin/env python3
"""Lay the per-scene narration onto the stitched cut and produce the final film.

Each clip is placed at its own scene's start (plus a short lead-in so the shot
establishes before the voice arrives), which is why the narration was written
against the stitcher's timeline in the first place: no clip has to be stretched
or nudged by hand.

Scene 6 also carries real audio captured from the guest while it decoded the
MP3. That is mixed in underneath the narration at the offset measured by the
capture harness (the wav clock and the video clock are independent).

    python3 mux_vo.py --master out/final/cut_v2.mp4 --out out/final/unodos-demo.mp4
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def dur(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                          "format=duration", "-of", "csv=p=0", path],
                         capture_output=True, text=True, check=True)
    return float(out.stdout.strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--master", default=os.path.join(HERE, "out/final/cut_v2.mp4"))
    ap.add_argument("--timeline", default=None)
    ap.add_argument("--vo-dir", default=os.path.join(HERE, "out/vo"))
    ap.add_argument("--out", default=os.path.join(HERE, "out/final/unodos-demo.mp4"))
    ap.add_argument("--lead-in", type=float, default=0.8,
                    help="seconds of picture before the voice starts in each scene")
    ap.add_argument("--bed", action="append", default=[],
                    metavar="SCENE:WAV:TRIM:GAIN",
                    help="extra audio under a scene, e.g. s06:out/final/s06.wav:6.68:0.22")
    args = ap.parse_args()

    tl_path = args.timeline or os.path.splitext(args.master)[0] + ".timeline.json"
    with open(tl_path, encoding="utf-8") as fh:
        tl = json.load(fh)
    starts = {s["id"]: s["start"] for s in tl["scenes"]}
    total = tl["total"]

    inputs, filters, labels = ["-i", args.master], [], []
    idx = 1

    for sid in sorted(starts):
        clip = os.path.join(args.vo_dir, sid + ".mp3")
        if not os.path.exists(clip):
            print("no narration for %s, leaving it silent" % sid)
            continue
        at = starts[sid] + args.lead_in
        d = dur(clip)
        end = at + d
        scene_end = starts[sid] + next(s["duration"] for s in tl["scenes"]
                                       if s["id"] == sid)
        if end > scene_end:
            print("WARNING %s: narration ends %.1fs past its scene"
                  % (sid, end - scene_end))
        ms = int(round(at * 1000))
        inputs += ["-i", clip]
        filters.append("[%d:a]adelay=%d|%d,volume=1.0[v%d]" % (idx, ms, ms, idx))
        labels.append("[v%d]" % idx)
        print("  %-4s voice at %7.2fs  (%.1fs, scene ends %.1fs)" % (sid, at, d, scene_end))
        idx += 1

    for spec in args.bed:
        sid, wav, trim, gain = spec.split(":")
        wav = wav if os.path.isabs(wav) else os.path.join(HERE, wav)
        at = starts[sid]
        ms = int(round(at * 1000))
        inputs += ["-i", wav]
        filters.append(
            "[%d:a]atrim=start=%s,asetpts=PTS-STARTPTS,volume=%s,adelay=%d|%d[v%d]"
            % (idx, trim, gain, ms, ms, idx))
        labels.append("[v%d]" % idx)
        print("  %-4s bed  at %7.2fs  (%s from %ss, gain %s)"
              % (sid, at, os.path.basename(wav), trim, gain))
        idx += 1

    if not labels:
        sys.exit("nothing to mix")

    filters.append("%samix=inputs=%d:normalize=0:duration=longest,"
                   "apad=whole_dur=%.3f,alimiter=limit=0.95[a]"
                   % ("".join(labels), len(labels), total))

    cmd = (["ffmpeg", "-y", "-loglevel", "error"] + inputs +
           ["-filter_complex", ";".join(filters),
            "-map", "0:v", "-map", "[a]",
            "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
            "-shortest", args.out])
    subprocess.run(cmd, check=True)
    print("\n%s  (%.1fs)" % (args.out, dur(args.out)))


if __name__ == "__main__":
    main()
