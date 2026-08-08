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
import argparse, json, os, subprocess, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SCRIPT = os.path.join(HERE, "vo_script.json")
DEFAULT_OUT = os.path.join(HERE, "out", "vo")
KEY_PATHS = [
    r"\\wsl.localhost\Ubuntu-24.04\home\arin\Github\under-a-crescent-moon\.env",
    "/home/arin/Github/under-a-crescent-moon/.env",
]
# ElevenLabs list price for the usual subscription tiers, used only to show an
# estimate before spending. Verify against the account if it matters.
USD_PER_1K_CHARS = 0.30


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
    args = ap.parse_args()

    with open(args.script, encoding="utf-8") as fh:
        spec = json.load(fh)
    voice = args.voice or spec.get("voice_id")
    model = args.model or spec.get("model", "eleven_multilingual_v2")
    scenes = [s for s in spec["scenes"] if not args.scene or s["id"] == args.scene]
    if not scenes:
        sys.exit("no scenes matched")

    budgets = {}
    if args.timeline and os.path.exists(args.timeline):
        with open(args.timeline, encoding="utf-8") as fh:
            for s in json.load(fh)["scenes"]:
                budgets[s["id"]] = s["duration"]

    total_chars = sum(len(s["text"]) for s in scenes)
    print("scenes: %d   characters: %d   estimated cost: about $%.2f"
          % (len(scenes), total_chars, total_chars / 1000.0 * USD_PER_1K_CHARS))
    print("voice: %s   model: %s\n" % (voice or "(unset)", model))

    for s in scenes:
        words = len(s["text"].split())
        b = budgets.get(s["id"])
        fit = ""
        if b:
            # 2.4 words/sec is a comfortable narration pace
            est = words / 2.4
            fit = "  scene %.1fs, read ~%.1fs%s" % (
                b, est, "  OVER" if est > b * 0.95 else "")
        print("  %-4s %4d chars %3d words%s" % (s["id"], len(s["text"]), words, fit))

    if args.dry_run:
        print("\ndry run - nothing generated, no credits spent")
        return
    if not voice:
        sys.exit("no voice id: pass --voice or set voice_id in the script file")

    os.makedirs(args.out_dir, exist_ok=True)
    key = api_key()
    print()
    for s in scenes:
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

    print("\ndone. Check any TOO LONG scene: either tighten the line or hold the "
          "shot longer; do not speed the read up.")


if __name__ == "__main__":
    main()
