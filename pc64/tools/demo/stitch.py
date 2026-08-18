#!/usr/bin/env python3
"""Stitch the per-scene demo recordings into one master timeline.

Emits master.mp4 plus timeline.json, which maps every scene to its start time in
the finished cut. That timeline is what the voiceover script is written against,
so the narration is fitted to real footage rather than guessed durations.

    python3 stitch.py --out-dir out --master out/master.mp4

Scenes are taken in SPINE order; any scene file that is missing is skipped and
reported, so a partial cut still assembles.
"""
import argparse, json, os, subprocess, sys

# The cut's ORDER, which is not the recording order: scenes.py records s09
# before s08 (Studio's Ctrl-R must run before anything touches the URC `py`
# verb - see the note above pyeval() there), and s14, the appliances scene,
# stays out of the cut entirely because it needs a hypervisor-capable host.
#
# Duum runs SECOND, right after the boot, as the early hook (2026-08-18, user):
# Doom on the machine is the strongest image in the film and it used to sit
# five minutes in. s09 (Automation in Python) is OUT of the cut for the same
# reordering pass - its footage, beats and narration still exist if it returns.
SPINE = [
    ("s01", "Cold boot"),
    ("s08", "Duum"),
    ("s02", "Desktop and window manager"),
    ("s03", "Themes"),
    ("s04", "UnoOffice"),
    ("s05", "Web browser"),
    ("s06", "Media"),
    ("s07", "Studio IDE"),
    # s12, the games and the tracker, is OUT of the cut (2026-08-17). Dostris,
    # Pac-Man and OutLast are driven by the machine's own sequencer, and under
    # QEMU the guest is too slow to keep the audio clean, so the music sounded
    # worse than it is. Put it back when the scene can be recorded somewhere
    # that keeps up: the footage, its beats and its narration all still exist.
    ("s13", "SSH"),
    ("s10", "Under the hood"),
    ("s11", "The family"),
]


def true_duration(mp4_path):
    """Wall-clock duration from the capture's timing sidecar, if there is one.

    stream_recv muxes at the REQUESTED fps, not the rate frames actually
    arrived, so a scene captured at 24 fps and muxed at 30 plays 25 percent
    fast. The sidecar records a wall clock per frame, so the honest duration is
    recoverable and the footage can be retimed instead of re-shot.
    """
    side = os.path.splitext(mp4_path)[0] + ".timing.jsonl"
    if not os.path.exists(side):
        return None
    first = last = None
    n = 0
    with open(side) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                t = json.loads(line)["t"]
            except Exception:
                continue
            if first is None:
                first = t
            last = t
            n += 1
    if n < 2 or last is None or last <= first:
        return None
    # n-1 inter-frame gaps, plus one frame's worth of display time at the end
    span = last - first
    return span + span / (n - 1)


def probe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,avg_frame_rate,nb_frames", "-show_entries",
         "format=duration", "-of", "json", path],
        capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    st = j["stream"][0] if "stream" in j else j["streams"][0]
    num, _, den = st["avg_frame_rate"].partition("/")
    fps = float(num) / float(den or 1) if float(den or 1) else 0.0
    return {"w": int(st["width"]), "h": int(st["height"]), "fps": fps,
            "duration": float(j["format"]["duration"])}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default="out")
    ap.add_argument("--master", default=None)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--width", type=int, default=0, help="0 = widest input")
    args = ap.parse_args()

    master = args.master or os.path.join(args.out_dir, "master.mp4")

    found, missing = [], []
    for sid, title in SPINE:
        p = os.path.join(args.out_dir, sid + ".mp4")
        if os.path.exists(p):
            info = probe(p)
            info.update(id=sid, title=title, path=p)
            found.append(info)
        else:
            missing.append(sid)

    if not found:
        sys.exit("no scene files in %s" % args.out_dir)
    for sid in missing:
        print("MISSING (skipped): %s" % sid)

    tw = args.width or max(f["w"] for f in found)
    th = max(f["h"] for f in found)
    # keep it even for yuv420p
    tw += tw % 2
    th += th % 2
    print("target: %dx%d @ %g fps" % (tw, th, args.fps))

    # Normalise each scene, then concat. Re-encoding throughout keeps a mixed
    # bag of sources (unostream captures, a screendump loop, an ffmpeg montage)
    # from producing a stream the concat demuxer refuses.
    norm_dir = os.path.join(args.out_dir, "_norm")
    os.makedirs(norm_dir, exist_ok=True)
    listfile = os.path.join(norm_dir, "concat.txt")
    timeline, t = [], 0.0
    with open(listfile, "w") as lf:
        for f in found:
            np_ = os.path.join(norm_dir, f["id"] + ".mp4")
            # Retime to wall clock first, if the sidecar disagrees with the
            # container by more than 2 percent.
            td = true_duration(f["path"])
            pts = ""
            if td and abs(td - f["duration"]) / f["duration"] > 0.02:
                ratio = td / f["duration"]
                pts = "setpts=%.6f*PTS," % ratio
                print("  %-4s RETIMED %.2fs -> %.2fs (container was %.0f%% fast)"
                      % (f["id"], f["duration"], td, (ratio - 1) * 100))
            # A pixel UI upscaled by an exact integer must use nearest
            # neighbour: 640x400 doubled to 1280x800 stays crisp, where a
            # bilinear scale would smear every one-pixel border and make the
            # smaller scenes look soft next to the natively-large ones.
            sw = tw / float(f["w"])
            sh = th / float(f["h"])
            integer_zoom = (abs(sw - round(sw)) < 0.001 and
                            abs(sh - round(sh)) < 0.001 and
                            round(sw) == round(sh) and round(sw) >= 2)
            flags = ":flags=neighbor" if integer_zoom else ""
            if integer_zoom:
                print("  %-4s %dx%d -> %dx%d nearest-neighbour x%d"
                      % (f["id"], f["w"], f["h"], tw, th, round(sw)))
            vf = (pts + "scale=%d:%d:force_original_aspect_ratio=decrease%s,"
                  "pad=%d:%d:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=%g"
                  % (tw, th, flags, tw, th, args.fps))
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", f["path"],
                            "-vf", vf, "-an", "-c:v", "libx264", "-preset", "veryfast",
                            "-crf", "18", "-pix_fmt", "yuv420p", np_], check=True)
            d = probe(np_)["duration"]
            lf.write("file '%s'\n" % os.path.abspath(np_).replace("\\", "/"))
            timeline.append({"id": f["id"], "title": f["title"],
                             "start": round(t, 3), "duration": round(d, 3),
                             "source_fps": round(f["fps"], 2)})
            print("  %-4s %-30s start %7.2fs  dur %6.2fs" % (f["id"], f["title"], t, d))
            t += d

    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                    "-i", listfile, "-c", "copy", master], check=True)

    tl_path = os.path.splitext(master)[0] + ".timeline.json"
    with open(tl_path, "w") as fh:
        json.dump({"master": master, "total": round(t, 3), "scenes": timeline,
                   "missing": missing}, fh, indent=2)
    print("\nmaster: %s  (%.1fs total)\ntimeline: %s" % (master, t, tl_path))


if __name__ == "__main__":
    main()
