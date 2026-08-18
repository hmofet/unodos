#!/usr/bin/env python3
"""Lay the narration onto the stitched cut and produce the final film.

Narration is placed per CUE, not per scene. Each cue names the beat it should
land on, and the beat logs written during recording say exactly when that beat
happened, so the voice tracks the picture instead of drifting away from it
over a long scene.

A cue's `at` is either a beat name from the scene's .beats.jsonl, or a number
of seconds from the start of the scene. Cues are never allowed to overlap: a
cue that would start before the previous one finished is pushed later and
reported, because two voices talking over each other is worse than a late line.

Scene 6 also carries real audio captured from the guest while it decoded an
MP3, mixed underneath at the offset the capture harness measured (the wav
clock and the video clock are independent).

    python3 mux_vo.py --master out/final2/final.mp4 --bed "s06:out/final2/s06.wav:6.68:0.22"
"""
import argparse, bisect, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def dur(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                          "format=duration", "-of", "csv=p=0", path],
                         capture_output=True, text=True, check=True)
    return float(out.stdout.strip())


def beat_times(scene_dir, sid):
    """{beat name: seconds into the FINISHED scene where that beat is visible}.

    Both logs are wall clock. The finished scene is not: stream_recv writes
    frames at a constant rate and stitch retimes the scene to its true
    wall-clock duration, so every frame occupies an equal slice of the cut.
    The guest does NOT produce frames at a constant rate. It stalls precisely
    when it is busy, which is precisely when the things worth narrating happen,
    so wall-clock elapsed and on-screen elapsed drift apart inside a scene.

    Using the first clock for the second is what put the lines in the wrong
    place. Measured on the 2026-08-08 cut: `load-wikipedia` happened 39.1s into
    the recording but lands 41.8s into the footage, because the guest nearly
    stopped drawing while it fetched the page - so its line was spoken 2.7s
    before the page appeared. s02's `switcher-f2` drifts 2.6s the OTHER way,
    and its line arrived after the moment had passed.

    The frame log converts between the clocks: find the frame carrying the
    beat, then ask where that frame sits in the finished scene.
    """
    bpath = os.path.join(scene_dir, sid + ".beats.jsonl")
    tpath = os.path.join(scene_dir, sid + ".timing.jsonl")
    if not (os.path.exists(bpath) and os.path.exists(tpath)):
        return {}
    stamps = []
    with open(tpath, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                stamps.append(json.loads(line)["t"])
            except Exception:                        # noqa: BLE001
                continue
    if not stamps:
        return {}
    t0 = stamps[0]
    span = stamps[-1] - t0
    n = len(stamps)

    def on_screen(t):
        # Fall back to the wall clock when there is nothing to convert with;
        # a one-frame scene has no rate to speak of.
        if n < 2 or span <= 0:
            return t - t0
        i = bisect.bisect_left(stamps, t)
        if i >= n:
            i = n - 1
        return i * span / n

    out = {}
    with open(bpath, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                b = json.loads(line)
            except Exception:                        # noqa: BLE001
                continue
            if "beat" in b and "t" in b:
                out[b["beat"]] = max(0.0, on_screen(b["t"]))
    return out


def cues_of(scene):
    """Accepts either {"text": ...} or {"cues": [...]} in the script file."""
    if "cues" in scene:
        return scene["cues"]
    return [{"id": scene["id"], "at": 0.0, "text": scene["text"]}]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--master", default=os.path.join(HERE, "out/final2/final.mp4"))
    ap.add_argument("--timeline", default=None)
    ap.add_argument("--script", default=os.path.join(HERE, "vo_script.json"))
    ap.add_argument("--vo-dir", default=os.path.join(HERE, "out/vo"))
    ap.add_argument("--out",
                    default=os.path.join(HERE,
                                         "out/final2/unodos-demo-final.mp4"))
    ap.add_argument("--lead-in", type=float, default=0.6,
                    help="seconds after a beat fires before its line starts")
    ap.add_argument("--gap", type=float, default=0.35,
                    help="minimum silence between consecutive lines")
    ap.add_argument("--bed", action="append", default=[],
                    metavar="SCENE:WAV:TRIM:GAIN")
    args = ap.parse_args()

    tl_path = args.timeline or os.path.splitext(args.master)[0] + ".timeline.json"
    with open(tl_path, encoding="utf-8") as fh:
        tl = json.load(fh)
    starts = {s["id"]: s["start"] for s in tl["scenes"]}
    durs = {s["id"]: s["duration"] for s in tl["scenes"]}
    total = tl["total"]
    scene_dir = os.path.dirname(os.path.abspath(args.master))

    with open(args.script, encoding="utf-8") as fh:
        spec = json.load(fh)

    inputs, filters, labels = ["-i", args.master], [], []
    idx = 1
    placed = []

    for scene in spec["scenes"]:
        sid = scene["id"]
        if sid not in starts:
            print("scene %s is not in the cut, skipping its narration" % sid)
            continue
        beats = beat_times(scene_dir, sid)
        last_end = 0.0                                # relative to scene start
        for cue in cues_of(scene):
            clip = os.path.join(args.vo_dir, cue["id"] + ".mp3")
            if not os.path.exists(clip):
                print("no audio for cue %s" % cue["id"])
                continue
            at = cue.get("at", 0.0)
            if isinstance(at, str):
                if at not in beats:
                    print("  %-6s beat %r not in the log - placing at scene start"
                          % (cue["id"], at))
                    rel = 0.0
                else:
                    rel = beats[at]
            else:
                rel = float(at)
            rel += args.lead_in
            if rel < last_end + args.gap:
                print("  %-6s pushed %.1fs later to clear the previous line"
                      % (cue["id"], last_end + args.gap - rel))
                rel = last_end + args.gap
            d = dur(clip)
            if rel + d > durs[sid]:
                print("  WARNING %s runs %.1fs past the end of %s"
                      % (cue["id"], rel + d - durs[sid], sid))
            last_end = rel + d
            abs_ms = int(round((starts[sid] + rel) * 1000))
            inputs += ["-i", clip]
            filters.append("[%d:a]adelay=%d|%d[v%d]" % (idx, abs_ms, abs_ms, idx))
            labels.append("[v%d]" % idx)
            placed.append((cue["id"], starts[sid] + rel, d, cue.get("at")))
            idx += 1

    for spec_bed in args.bed:
        sid, wav, trim, gain = spec_bed.split(":")
        wav = wav if os.path.isabs(wav) else os.path.join(HERE, wav)
        if not os.path.exists(wav):
            print("bed %s missing, skipping" % wav)
            continue
        ms = int(round(starts[sid] * 1000))
        inputs += ["-i", wav]
        filters.append(
            "[%d:a]atrim=start=%s,asetpts=PTS-STARTPTS,volume=%s,adelay=%d|%d[v%d]"
            % (idx, trim, gain, ms, ms, idx))
        labels.append("[v%d]" % idx)
        print("  bed  %-6s at %7.2fs  (%s from %ss, gain %s)"
              % (sid, starts[sid], os.path.basename(wav), trim, gain))
        idx += 1

    if not labels:
        sys.exit("nothing to mix")

    print()
    for cid, at, d, anchor in placed:
        print("  %-6s %7.2fs  %5.1fs  <- %s" % (cid, at, d, anchor))
    spoken = sum(d for _, _, d, _ in placed)
    print("\nnarration covers %.0f%% of %.0fs" % (100.0 * spoken / total, total))

    filters.append("%samix=inputs=%d:normalize=0:duration=longest,"
                   "apad=whole_dur=%.3f,alimiter=limit=0.95[a]"
                   % ("".join(labels), len(labels), total))

    subprocess.run(["ffmpeg", "-y", "-loglevel", "error"] + inputs +
                   ["-filter_complex", ";".join(filters),
                    "-map", "0:v", "-map", "[a]",
                    "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
                    "-shortest", args.out], check=True)
    print("\n%s  (%.1fs)" % (args.out, dur(args.out)))


if __name__ == "__main__":
    main()
