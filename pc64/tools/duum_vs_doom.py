#!/usr/bin/env python3
"""duum_vs_doom.py - side-by-side gallery: Duum vs the original Doom.

For every requested level this renders Duum at the player start (via the
duum_host shim) and captures the same spawn view from Chocolate Doom (the
vanilla-exact source port) running the same IWAD, then composes labelled
side-by-side sheets for eyeball comparison.  Geometry assertions live in
duum_verify.py; this tool is the human check next to the real game.

Windows-only (uses cc-capture.ps1, the focus-independent window grabber).

  python3 tools/duum_vs_doom.py --wad DOOM1.WAD --choco path/chocolate-doom.exe
  python3 tools/duum_vs_doom.py ... --levels E1M1,E1M2 --out out/vsdoom

Chocolate Doom is launched per level with -warp -nomonsters -nosound in a
window; Duum's monsters are filtered out of the render to match.  Expect the
light falloff to differ (Duum's is deliberately gentler than the COLORMAP);
everything geometric should agree.
"""
import os, sys, math, time, argparse, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import duum_host

CAPTURE = os.path.expandvars(r"%USERPROFILE%\.claude\tools\cc-capture.ps1")


def duum_shot(app, mod, level, out):
    if app.level != level:
        app.load_level(level)
    app.things_live = [t for t in app.things_live
                       if t[3] not in mod.MONST]     # match -nomonsters
    app.render()
    cv = duum_host.Canvas()
    app.draw(cv)
    cv.save(out)


def choco_shot(choco, wad, epi, mapn, out, warmup):
    p = subprocess.Popen(
        [choco, "-iwad", wad, "-window", "-nograbmouse", "-nosound",
         "-nomonsters", "-skill", "3", "-warp", str(epi), str(mapn)],
        cwd=os.path.dirname(choco))
    try:
        time.sleep(warmup)
        subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", CAPTURE, "-Out", out, "-Window", "Chocolate"],
            check=True, capture_output=True)
    finally:
        p.kill()
        p.wait()


def compose(duum_png, choco_png, level, out):
    from PIL import Image, ImageDraw
    a = Image.open(duum_png).convert("RGB")
    b = Image.open(choco_png).convert("RGB")
    # match heights
    if b.height != a.height:
        b = b.resize((round(b.width * a.height / b.height), a.height))
    pad = 8; label = 18
    img = Image.new("RGB", (a.width + b.width + 3 * pad,
                            a.height + label + 2 * pad), (24, 24, 28))
    d = ImageDraw.Draw(img)
    d.text((pad, 4), "Duum  " + level, fill=(240, 240, 220))
    d.text((a.width + 2 * pad, 4), "Chocolate Doom (vanilla)  " + level,
           fill=(240, 240, 220))
    img.paste(a, (pad, label + pad))
    img.paste(b, (a.width + 2 * pad, label + pad))
    img.save(out)
    print("wrote", out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", required=True)
    ap.add_argument("--choco", required=True,
                    help="path to chocolate-doom.exe")
    ap.add_argument("--levels", default=",".join(
        "E1M%d" % i for i in range(1, 10)))
    ap.add_argument("--out", default="out/vsdoom")
    ap.add_argument("--warmup", type=float, default=8.0,
                    help="seconds to let the game reach the spawn view")
    args = ap.parse_args()

    duum_host.WAD_OVERRIDE = os.path.abspath(args.wad)
    mod = duum_host.load_app()
    app = mod.app
    cv = duum_host.Canvas()
    app.build(cv)
    if app.err:
        sys.exit("app.build failed: %s" % app.err)

    os.makedirs(args.out, exist_ok=True)
    for level in args.levels.split(","):
        level = level.strip().upper()
        epi, mapn = int(level[1]), int(level[3])
        dp = os.path.join(args.out, "duum_%s.png" % level)
        cp = os.path.join(args.out, "choco_%s.png" % level)
        duum_shot(app, mod, level, dp)
        choco_shot(args.choco, os.path.abspath(args.wad), epi, mapn, cp,
                   args.warmup)
        compose(dp, cp, level, os.path.join(args.out, "vs_%s.png" % level))


if __name__ == "__main__":
    main()
