#!/usr/bin/env python3
"""duum_ab.py - A/B two Duum builds ON DEVICE, in a single boot.

Host timings (tools/duum_golden.py bench) run CPython, which hides the cost
that actually hurts on the guest: MicroPython boxes every float on the GC
heap, so removing allocations is worth far more there than CPython's clock
suggests. And a cross-session comparison is not trustworthy either - quill is
a nested guest on a shared host, and leviathan's load moves the guest's frame
rate without ever showing up as steal inside quill.

So: ONE boot, both builds, alternating rounds, same scripted movement. Host
drift hits both arms equally and cancels in the comparison.

Stage the baseline first, e.g.
    git show <sha>:pc64/apps/DUUM.PY > /tmp/DUUMOLD.PY
    python3 tools/mkuno.py pyapp /tmp/DUUMOLD.PY build/esp/APPS/DUUMOLD.UNO

    python3 duum_ab.py --rounds 3
"""
import argparse, json, os, statistics, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import duum_demo as DD                                       # noqa: E402


def scene(d):
    """Fixed workload: walk the corridor, look, fire. Movement is what forces
    a re-render, so the scene must never idle - an idle stretch measures the
    shell's dirty-flag logic, not the renderer."""
    d.beat("walk", settle=0.3)
    d.keys("UUUUUUUU")
    d.beat("look", settle=0.3)
    d.keys("LR")
    d.beat("fire", settle=0.3)
    d.keys("FFFF", settle=0.28)
    d.keys("UUUU")


def gaps(path):
    """Inter-frame gaps in ms from a .timing.jsonl sidecar."""
    ts = []
    with open(path) as f:
        for line in f:
            try:
                ts.append(json.loads(line)["t"])
            except Exception:
                pass
    ts.sort()
    return [(b - a) * 1000.0 for a, b in zip(ts, ts[1:])]


def summarise(tag, stats, timing):
    """FRAMES DELIVERED over a fixed, identical script is the metric.

    Do NOT filter to "small gaps and call that the moving frame rate": tried
    it, and it reported the SLOWER build as 130 fps against the faster one's
    48. unostream emits a keyframe and its tile deltas back to back, so the
    short gaps are one screen update's packets, not one frame each - the
    filter measures burstiness. Frame COUNT over the same scripted workload
    has no such hole.
    """
    g = gaps(timing) if os.path.exists(timing) else []
    med = statistics.median(g) if g else float("nan")
    dur = stats.get("dur") or 0.0
    fr = stats.get("frames") or 0
    return {"tag": tag, "frames": fr, "dur": dur,
            "frames_per_s": round(fr / dur, 2) if dur else None,
            "median_gap_ms": round(med, 1)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--apps", default="APPS\\DUUM.UNO,APPS\\DUUMOLD.UNO")
    args = ap.parse_args()
    apps = [a.strip() for a in args.apps.split(",")]

    d = DD.Duum().boot()
    rows = []
    try:
        vol = d.esp_vol()
        for r in range(args.rounds):
            for ai, app in enumerate(apps):
                d.clean_desktop()
                d.link.eval('import uno; uno.run_app(%d, "%s")'
                            % (vol, app.replace("\\", "\\\\")), timeout=30)
                up = False
                for _ in range(40):
                    if any("DUUM" in t.upper() for t in d.windows()):
                        up = True
                        break
                    time.sleep(2.0)
                if not up:
                    print("  %s did not open" % app)
                    continue
                time.sleep(2.0)
                d.park_cursor()
                name = "ab_r%d_%d" % (r, ai)
                st = d.record(name, scene, DD.STREAM_BASE + (ai % 4))
                rows.append(summarise(app, st,
                                      os.path.join(DD.OUT, name + ".timing.jsonl")))
                print("   -> %s" % json.dumps(rows[-1]))
    finally:
        d.stop()

    print("\n=== A/B result (one boot, alternating) ===")
    base = None
    for app in apps:
        mine = [r for r in rows if r["tag"] == app and r["frames_per_s"]]
        if not mine:
            continue
        fs = [r["frames_per_s"] for r in mine]
        med = statistics.median(fs)
        if base is None:
            base = med
        print("  %-22s frames/s %s  median %.2f  (%.2fx)"
              % (app, [round(x, 2) for x in fs], med, med / base))
    print("  NOTE: absolute numbers move with leviathan's load - quill is a\n"
          "  nested guest there and contention never shows as steal inside it.\n"
          "  Only the RATIO from a single alternating boot is meaningful.")


if __name__ == "__main__":
    main()
