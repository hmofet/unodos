"""Cross-check every narration cue against the beats actually recorded.

mux_vo drops a cue whose beat it cannot find to the TOP of the scene and
carries on, so a renamed or removed beat is silent: the film still assembles
and one line plays over the wrong picture. This makes that loud.
"""
import json, os, sys, glob

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = sys.argv[1]
OUTDIR = sys.argv[2]

scenes = json.load(open(SCRIPT, encoding="utf-8"))["scenes"]
timeline = json.load(open(os.path.join(OUTDIR, "master.timeline.json"), encoding="utf-8"))
tl = {s["id"]: s for s in (timeline if isinstance(timeline, list) else timeline["scenes"])
      if isinstance(s, dict)}

problems = 0
covered = {}
for sc in scenes:
    sid = sc["id"]
    bpath = os.path.join(OUTDIR, sid + ".beats.jsonl")
    if sid not in tl:
        print("  scene %-4s IN SCRIPT BUT NOT IN THE CUT" % sid)
        continue
    if not os.path.exists(bpath):
        print("  scene %-4s no beats file" % sid)
        problems += 1
        continue
    beats = [json.loads(l)["beat"] for l in open(bpath, encoding="utf-8")]
    for cue in sc["cues"]:
        at = cue["at"]
        if isinstance(at, (int, float)):
            continue                      # a raw offset, not a beat anchor
        if at not in beats:
            print("  %-4s cue %-6s -> BEAT NOT FOUND: %r" % (sid, cue["id"], at))
            problems += 1
    covered[sid] = len(sc["cues"])

print()
for sid, s in tl.items():
    n = covered.get(sid)
    if n is None:
        print("  scene %-4s %-28s %6.1fs  NO NARRATION AT ALL" % (sid, s["title"], s["duration"]))
        problems += 1

print("\nproblems: %d" % problems)
sys.exit(1 if problems else 0)
