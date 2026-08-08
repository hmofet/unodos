#!/usr/bin/env python3
"""Generate the demo-video voiceover with ElevenLabs, one clip per scene.

Per scene rather than one long read, so a retake costs one scene instead of the
whole track, and so each clip can be checked against the length of the footage
it has to sit over.

    python3 gen_vo.py --dry-run          # character counts + cost, spends nothing
    python3 gen_vo.py --voice <id>       # generate (SPENDS CREDITS)
    python3 gen_vo.py --scene s04        # regenerate one scene

The API key is read from the Under-a-Crescent-Moon .env (the only place it
lives on this machine); it is never printed.
"""
import argparse, hashlib, json, os, subprocess, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SCRIPT = os.path.join(HERE, "vo_script.json")
DEFAULT_OUT = os.path.join(HERE, "out", "vo")
KEY_PATHS = [
    r"\\wsl.localhost\Ubuntu-24.04\home\arin\Github\under-a-crescent-moon\.env",
    "/home/arin/Github/under-a-crescent-moon/.env",
]
def quota(key):
    """Characters used/remaining this billing period, so a dry run reports the
    real cost (quota already paid for) rather than an invented dollar figure."""
    try:
        req = urllib.request.Request(
            "https://api.elevenlabs.io/v1/user/subscription",
            headers={"xi-api-key": key})
        d = json.load(urllib.request.urlopen(req))
        return d.get("character_count"), d.get("character_limit"), d.get("tier")
    except Exception:
        return None, None, None


def api_key():
    for p in KEY_PATHS:
        try:
            with open(p, "r", encoding="utf-8", errors="ignore") as fh:
                for line in fh:
                    if line.startswith("ELEVENLABS_API_KEY"):
                        return line.split("=", 1)[1].strip()
        except OSError:
            continue
    sys.exit("no ELEVENLABS_API_KEY found (looked in the UACM .env)")


def duration(path):
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", path], capture_output=True, text=True, check=True)
        return float(out.stdout.strip())
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", default=DEFAULT_SCRIPT)
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("--timeline", default=None,
                    help="master.timeline.json, to check each clip against its scene")
    ap.add_argument("--voice", default=None, help="voice id (overrides the script file)")
    ap.add_argument("--model", default=None)
    ap.add_argument("--scene", default=None, help="regenerate just this scene id")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="regenerate even scenes whose text is unchanged")
    args = ap.parse_args()

    with open(args.script, encoding="utf-8") as fh:
        spec = json.load(fh)
    voice = args.voice or spec.get("voice_id")
    model = args.model or spec.get("model", "eleven_multilingual_v2")

    # A scene is either one block of narration or several cues anchored to
    # named beats. Flatten to cues: each is generated and billed separately, so
    # rewording one line costs that line.
    scenes = []
    for sc in spec["scenes"]:
        if args.scene and sc["id"] != args.scene and not args.scene.startswith(sc["id"]):
            continue
        for cue in (sc["cues"] if "cues" in sc
                    else [{"id": sc["id"], "text": sc["text"]}]):
            if args.scene and args.scene not in (sc["id"], cue["id"]):
                continue
            scenes.append({"id": cue["id"], "text": cue["text"],
                           "scene": sc["id"]})
    if not scenes:
        sys.exit("no scenes matched")

    budgets = {}
    if args.timeline and os.path.exists(args.timeline):
        with open(args.timeline, encoding="utf-8") as fh:
            for s in json.load(fh)["scenes"]:
                budgets[s["id"]] = s["duration"]

    # Measured on this voice rather than assumed: Brian reads at about 2.9
    # words per second, not the 2.4 the first pass budgeted for, which left
    # every line short of its scene and 28% of the film silent.
    WPS = float(os.environ.get("VO_WPS", "2.9"))

    total_chars = sum(len(s["text"]) for s in scenes)
    used, limit, tier = quota(api_key())
    if limit:
        left = limit - used
        print("scenes: %d   characters: %d   (%.2f%% of the %s plan's %s remaining)"
              % (len(scenes), total_chars, 100.0 * total_chars / left,
                 tier, format(left, ",")))
    else:
        print("scenes: %d   characters: %d   (quota unknown)"
              % (len(scenes), total_chars))
    print("voice: %s   model: %s\n" % (voice or "(unset)", model))

    per_scene = {}
    for s in scenes:
        per_scene.setdefault(s.get("scene", s["id"]), []).append(s)
    for sid, cues in per_scene.items():
        b = budgets.get(sid)
        words = sum(len(c["text"].split()) for c in cues)
        est = words / WPS
        fit = ("  scene %.1fs, read ~%.1fs (%.0f%% full)%s"
               % (b, est, 100.0 * est / b, "  OVER" if est > b * 0.95 else "")
               ) if b else ""
        print("  %-5s %d cue(s) %4d words%s" % (sid, len(cues), words, fit))
        for c in cues:
            print("      %-6s %4d chars %3d words ~%.1fs"
                  % (c["id"], len(c["text"]), len(c["text"].split()),
                     len(c["text"].split()) / WPS))

    if args.dry_run:
        print("\ndry run - nothing generated, no credits spent")
        return
    if not voice:
        sys.exit("no voice id: pass --voice or set voice_id in the script file")

    os.makedirs(args.out_dir, exist_ok=True)
    key = api_key()

    # Only pay for what actually changed. A scene is regenerated when its text,
    # voice or model differs from what produced the clip on disk - so re-running
    # after editing one line costs that line, not the whole script.
    man_path = os.path.join(args.out_dir, "vo_manifest.json")
    manifest = {}
    if os.path.exists(man_path):
        try:
            with open(man_path, encoding="utf-8") as fh:
                manifest = json.load(fh)
        except Exception:
            manifest = {}

    def fingerprint(s):
        h = hashlib.sha256()
        h.update(("%s|%s|%s" % (voice, model, s["text"])).encode("utf-8"))
        return h.hexdigest()

    todo, skipped = [], []
    for s in scenes:
        dest = os.path.join(args.out_dir, s["id"] + ".mp3")
        if (not args.force and os.path.exists(dest)
                and manifest.get(s["id"]) == fingerprint(s)):
            skipped.append(s["id"])
        else:
            todo.append(s)
    if skipped:
        print("unchanged, keeping existing audio: %s" % ", ".join(skipped))
    if not todo:
        print("nothing to generate - every scene is current")
        return
    print("generating %d scene(s), %d characters\n"
          % (len(todo), sum(len(s["text"]) for s in todo)))

    for s in todo:
        dest = os.path.join(args.out_dir, s["id"] + ".mp3")
        body = json.dumps({
            "text": s["text"],
            "model_id": model,
            "voice_settings": spec.get("voice_settings",
                                       {"stability": 0.5, "similarity_boost": 0.75}),
        }).encode("utf-8")
        req = urllib.request.Request(
            "https://api.elevenlabs.io/v1/text-to-speech/%s" % voice,
            data=body, headers={"xi-api-key": key, "Content-Type": "application/json"})
        with urllib.request.urlopen(req) as r, open(dest, "wb") as fh:
            fh.write(r.read())
        d = duration(dest)
        b = budgets.get(s["id"])
        flag = ""
        if d and b:
            flag = "  vs scene %.1fs%s" % (b, "   TOO LONG" if d > b else "")
        print("  %-4s -> %s  %.1fs%s" % (s["id"], dest, d or -1, flag))
        manifest[s["id"]] = fingerprint(s)
        with open(man_path, "w", encoding="utf-8") as fh:
            json.dump(manifest, fh, indent=2)

    print("\ndone. Check any TOO LONG scene: either tighten the line or hold the "
          "shot longer; do not speed the read up.")


if __name__ == "__main__":
    main()
