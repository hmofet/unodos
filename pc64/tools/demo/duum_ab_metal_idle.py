#!/usr/bin/env python3
"""duum_ab_metal_idle.py - the A/B with the camera confound removed.

The scripted-movement A/B feeds both engines the same keys, but Duum advances
the player from elapsed time, so the faster arm travels slightly further and
the two end up looking at almost-but-not-quite the same thing.  Same room, same
complexity, ammo identical - but not the same pixels, which leaves a sliver of
doubt about whether a 1% cost difference is the renderer or the view.

This removes it: measure at the SPAWN view, sending no input at all.  Both arms
then render the identical camera, and any difference in cost per draw is the
renderer and nothing else.
"""
import json, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import duum_ab_metal as measure                                 # noqa: E402

T = measure.BridgeTransport("~/urc-multi/zima")
VOL = 2
APPS = ["APPS/DUUM.UNO", "APPS/DUUMOLD.UNO"]
ROUNDS = 3
IDLE = 25.0


def arm(app):
    measure.clean_desktop(T)
    T.send("py import uno; print(uno.run_app(%d, '%s'))" % (VOL, app), timeout=150)
    title = None
    for _ in range(40):
        time.sleep(1.5)
        title = measure.match_window(app, measure.snap(T)["win"])
        if title:
            break
    if not title:
        return {"app": app, "error": "no window"}
    time.sleep(6.0)                       # let load-time frames fall out
    a = measure.snap(T)
    time.sleep(IDLE)                      # NO INPUT: the camera cannot move
    b = measure.snap(T)
    pa = a["prof"].get(title, {"calls": 0, "cyc": 0})
    pb = b["prof"].get(title, {"calls": 0, "cyc": 0})
    dc, dy = pb["calls"] - pa["calls"], pb["cyc"] - pa["cyc"]
    dup = b["uptime"] - a["uptime"]
    r = {"app": app, "draws": dc, "cyc": dy, "wall_ms": dup}
    if dc > 0:
        r["ms_per_draw"] = round(dy / float(b["tsc_per_ms"]) / dc, 3)
        r["draws_per_s"] = round(dc * 1000.0 / dup, 2)
    T.send("close")
    time.sleep(1.0)
    return r


rows = []
for rnd in range(ROUNDS):
    for app in APPS:
        r = arm(app)
        r["round"] = rnd
        rows.append(r)
        print("  " + json.dumps(r))
        sys.stdout.flush()

print("\n=== idle spawn view, identical camera ===")
import statistics
for app in APPS:
    v = [x["ms_per_draw"] for x in rows if x["app"] == app and x.get("ms_per_draw")]
    if v:
        print("  %-20s %s  median %.3f ms  n=%d"
              % (app, [round(x, 2) for x in v], statistics.median(v), len(v)))
json.dump(rows, open("/tmp/idle_metal.json", "w"), indent=1)
