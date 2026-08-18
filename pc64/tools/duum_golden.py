#!/usr/bin/env python3
"""duum_golden.py - pixel-exact regression gate for Duum renderer refactors.

duum_verify.py proves the renderer agrees with an INDEPENDENT model of the
level; this proves a change to the renderer produced BYTE-IDENTICAL frames.
That is the check you want when optimising, where the output is supposed to
be unchanged and "looks the same" is not good enough.

  python3 tools/duum_golden.py save   [--wad W] [--out golden.json]
  python3 tools/duum_golden.py check  [--wad W] [--in  golden.json]
  python3 tools/duum_golden.py bench  [--wad W]      # ms/frame, render only

`check` exits 1 on any mismatch and names the viewpoint, so it drops straight
into a build gate.
"""
import os, sys, json, time, math, hashlib, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import duum_host                                            # noqa: E402

CW, CH = 518, 382                    # the device's Duum canvas

# Viewpoints chosen to exercise every path in render(): one-sided walls,
# two-sided portals with upper/lower pieces, masked midtextures, sky, flats,
# sprites and the HUD.  Player start plus each level's first deathmatch spawn.
LEVELS = ["E1M%d" % i for i in range(1, 10)]
ANGLES = [0, 90, 180, 270]


def views(app, mod):
    out = []
    lvl = app.lvl
    px, py, ang = lvl.player_start()
    for a in ANGLES:
        out.append(("start%+d" % a, float(px), float(py),
                    math.radians((ang + a) % 360)))
    n = 0
    for (x, y, tang, typ, fl) in lvl.things:
        if typ == 11 and n < 2:                             # deathmatch spawns
            out.append(("dm%d" % n, float(x), float(y), math.radians(tang)))
            n += 1
    return out


def frames(app, cv, mod):
    """-> {key: md5 of the rendered canvas} over every level and viewpoint."""
    got = {}
    for level in LEVELS:
        try:
            app.load_level(level)
        except Exception as e:
            print("  skip %s (%r)" % (level, e))
            continue
        for (name, x, y, pa) in views(app, mod):
            app.px = x; app.py = y; app.pa = pa
            app.psec_i = app.point_secidx(x, y)
            app.dirty = True
            app.render()
            cv.clear(0)
            app.draw(cv)
            got["%s/%s" % (level, name)] = hashlib.md5(
                bytes(cv.buf)).hexdigest()
    return got


def load_app(wad):
    if wad:
        duum_host.WAD_OVERRIDE = os.path.abspath(wad)
    mod = duum_host.load_app()
    app = mod.app
    cv = duum_host.Canvas(CW, CH)
    app.build(cv)
    if app.err:
        sys.exit("app.build failed: %s" % app.err)
    return mod, app, cv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["save", "check", "bench"])
    ap.add_argument("--wad", default=None)
    ap.add_argument("--out", default=os.path.join(HERE, "duum_golden.json"))
    ap.add_argument("--in", dest="inp", default=None)
    ap.add_argument("-n", type=int, default=15)
    args = ap.parse_args()

    mod, app, cv = load_app(args.wad)

    if args.cmd == "bench":
        # Report the geometry pass on its own: it is the same Python on the
        # device, where only the span writers are C.
        per_level = []
        for level in ("E1M1", "E1M3", "E1M5"):
            app.load_level(level)
            app.render()
            t0 = time.perf_counter()
            for _ in range(args.n):
                app.render()
            ms = (time.perf_counter() - t0) / args.n * 1e3
            per_level.append((level, ms, len(app.frame)))
            print("  %s  render %6.2f ms/frame   %5d display-list ops"
                  % (level, ms, len(app.frame)))
        avg = sum(m for _, m, _ in per_level) / len(per_level)
        print("mean render: %.2f ms/frame" % avg)
        return

    got = frames(app, cv, mod)
    if args.cmd == "save":
        with open(args.out, "w") as f:
            json.dump(got, f, indent=1, sort_keys=True)
        print("wrote %s (%d viewpoints)" % (args.out, len(got)))
        return

    ref_path = args.inp or args.out
    with open(ref_path) as f:
        ref = json.load(f)
    bad = [k for k in sorted(set(ref) | set(got))
           if ref.get(k) != got.get(k)]
    for k in bad:
        print("MISMATCH %-16s golden %s  now %s"
              % (k, (ref.get(k) or "-")[:12], (got.get(k) or "-")[:12]))
    print("%d/%d viewpoints identical" % (len(got) - len(bad), len(got)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
