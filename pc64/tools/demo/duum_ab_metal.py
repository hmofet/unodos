#!/usr/bin/env python3
"""duum_ab_metal.py - A/B two Duum engines on ONE boot of REAL HARDWARE.

Run this ON the URC bridge host (devbuntu), which owns the box's only link:

    python3 duum_ab_metal.py --rounds 3 --cycles 3

Stage the baseline arm first, e.g.
    git show 0c9e0de9:pc64/apps/DUUM.PY > /tmp/DUUMOLD.PY
    python3 tools/mkuno.py pyapp /tmp/DUUMOLD.PY build/esp/APPS/DUUMOLD.UNO
and push it to the boot volume alongside DUUM.UNO and TRIVIAL.UNO.

tools/demo/duum_ab.py is the QEMU sibling of this; it drives the guest over
UnoAutoLink and counts unostream frames. This one never attaches the stream.

WHAT IS MEASURED, AND WHY NOT THE OBVIOUS THING

Not frames off unostream.  That path QOI-encodes the whole desktop every frame,
so its rate is partly a measurement of the encoder, and its packets arrive in
bursts (a keyframe and its tile deltas back to back) - filtering those bursts
for a "moving frame rate" once ranked the SLOWER build at 130 fps against the
faster one's 48.

Not host wall-clock around a `py` eval either: uno.ticks() does not advance
while an eval blocks the shell loop, and the bridge's log timestamps are whole
seconds.

Instead: the kernel's own per-window draw profiler.  unoui_profile_win()
brackets each window's draw callback with rdtsc and accumulates cycles AND
calls per window title.  Two `probe` snapshots around a fixed input script give

    ms per draw = (cyc1 - cyc0) / tsc_per_ms / (calls1 - calls0)

which is the engine's cost per rendered frame, isolated from the shell, from
present, from the encoder, and from how often the window happened to be dirty.
Every quantity is read off the guest: nothing here depends on host timing.

Arms alternate within one boot so that drift hits both equally.
"""
import argparse, json, os, re, statistics, time

S_UP, S_DOWN, S_RIGHT, S_LEFT = 0x01, 0x02, 0x03, 0x04
K_FIRE = ord('f')


# ---------------------------------------------------------------- transports
class BridgeTransport(object):
    """The devbuntu URC bridge's file interface: one verb per line appended to
    cmd.txt, responses indented under a `>> ` echo in session.log.  The bridge
    owns the box's only link, so opening a second UnoAutoLink would evict it."""

    def __init__(self, d):
        self.d = os.path.expanduser(d)
        self.cmd = os.path.join(self.d, "cmd.txt")
        self.log = os.path.join(self.d, "session.log")

    def _lines(self):
        with open(self.log, "rb") as f:
            return f.read().decode("utf-8", "replace").splitlines()

    def send(self, verb, timeout=40.0):
        n = len(self._lines())
        with open(self.cmd, "a") as f:
            f.write(verb + "\n")
        t0 = time.time()
        while time.time() - t0 < timeout:
            time.sleep(0.05)
            body = []
            for l in self._lines()[n:]:
                m = re.match(r"^\[\d\d:\d\d:\d\d\]\s{4}(.*)$", l)
                if not m:
                    continue
                p = m.group(1)
                if p == "ok" or p.startswith("err") or p.startswith("!"):
                    return body
                body.append(p)
        raise RuntimeError("timeout waiting for %r" % verb)


class LinkTransport(object):
    """Direct UnoAutoLink, for the QEMU rehearsal where we own the listener."""

    def __init__(self, link):
        self.link = link

    def send(self, verb, timeout=40.0):
        p = verb.split(None, 1)
        rest = [p[1]] if len(p) > 1 else []
        return self.link.command(p[0], *rest, timeout=timeout)


# ------------------------------------------------------------------ plumbing
def probe(t):
    rows = []
    for l in t.send("probe"):
        p = l.split(None, 4)
        if len(p) < 5:
            continue
        rows.append({"kind": int(p[0]), "state": int(p[1]),
                     "v1": int(p[2]), "v2": int(p[3]), "name": p[4].strip()})
    return rows


def snap(t):
    """One measurement point.  kind 3 rows are the draw profiler: state=calls,
    v1=cycles.  They persist after a window closes, so an arm can be read
    afterwards as well as during."""
    d = {"prof": {}, "win": []}
    for r in probe(t):
        if r["kind"] == 2 and r["name"] == "perf":
            d["frames"], d["tsc_per_ms"] = r["v1"], r["v2"]
        elif r["kind"] == 2 and r["name"] == "shell":
            d["uptime"] = r["v1"]
        elif r["kind"] == 3:
            d["prof"][r["name"]] = {"calls": r["state"], "cyc": r["v1"],
                                    "max_us": r["v2"]}
        elif r["kind"] == 1 and r["name"]:
            d["win"].append(r["name"])
    return d


def vols(t):
    out = []
    for l in t.send("vols"):
        p = l.split(None, 3)
        if len(p) >= 4:
            out.append({"vol": int(p[0]), "kind": int(p[1]),
                        "writable": p[2] == "1", "name": p[3].strip()})
    return out


def clean_desktop(t):
    """Close every window, so the arm under test is the only thing drawing."""
    for _ in range(14):
        if not snap(t)["win"]:
            return
        t.send("close")
        time.sleep(0.6)


# --------------------------------------------------------------------- scene
def scene(t, cycles, pace):
    """The fixed workload.  Movement is what dirties the window, so the script
    must never idle - an idle stretch measures the shell's dirty-flag logic
    rather than the renderer.  Identical for every arm, including the control,
    which ignores input entirely but must be given the same wall clock."""
    seq = ([(S_UP, 0)] * 8 + [(S_LEFT, 0)] + [(S_RIGHT, 0)]
           + [(0, K_FIRE)] * 4 + [(S_UP, 0)] * 4)
    for _ in range(cycles):
        for scan, uni in seq:
            t.send("key %d %d 0" % (scan, uni), timeout=15)
            time.sleep(pace)


def match_window(app, wins):
    stem = app.split("/")[-1].split(".")[0].upper()
    for w in wins:
        if app.upper() in w.upper() or stem in w.upper():
            return w
    return None


def run_arm(t, vol, app, cycles, pace, settle):
    clean_desktop(t)
    t.send("py import uno; print(uno.run_app(%d, '%s'))" % (vol, app),
           timeout=150)
    title = None
    for _ in range(40):
        time.sleep(1.5)
        title = match_window(app, snap(t)["win"])
        if title:
            break
    if not title:
        return {"app": app, "error": "window never opened"}
    time.sleep(settle)                    # let one-off first-frame costs fall out
    a = snap(t)
    scene(t, cycles, pace)
    b = snap(t)
    # Every PYAPP window shares ONE profiler slot: g_wprof keys on the title
    # POINTER, and the shell hands each app the same reused title buffer, so
    # the slot is simply renamed to whichever app is currently open.  Deltas
    # are still exact - only one arm draws at a time - but a lookup by name can
    # miss if the slot was renamed between the two snapshots, and a miss would
    # silently return the whole cumulative total as the delta.  So: prefer the
    # named slot, and fall back to whichever non-shell slot actually moved.
    def pick(sa, sb):
        if title in sa and title in sb:
            return sa[title], sb[title]
        best, zero = None, {"calls": 0, "cyc": 0}
        for k, v in sb.items():
            if k in ("(shell)",):
                continue
            grew = v["calls"] - sa.get(k, zero)["calls"]
            if grew > 0 and (best is None or grew > best[0]):
                best = (grew, sa.get(k, zero), v)
        return (best[1], best[2]) if best else (zero, zero)

    pa, pb = pick(a["prof"], b["prof"])
    dcalls = pb["calls"] - pa["calls"]
    dcyc = pb["cyc"] - pa["cyc"]
    dfr = b.get("frames", 0) - a.get("frames", 0)
    dup = b.get("uptime", 0) - a.get("uptime", 0)
    tpm = b.get("tsc_per_ms") or 0
    r = {"app": app, "window": title, "draws": dcalls, "cyc": dcyc,
         "shell_frames": dfr, "wall_ms": dup, "tsc_per_ms": tpm,
         "max_us": pb.get("max_us")}
    if dcalls > 0 and tpm:
        r["ms_per_draw"] = round(dcyc / float(tpm) / dcalls, 3)
        r["draws_per_s"] = round(dcalls * 1000.0 / dup, 2) if dup else None
    if dup:
        r["shell_fps"] = round(dfr * 1000.0 / dup, 2)
    t.send("close")
    time.sleep(0.8)
    return r


def report(rows, apps):
    print("\n=== ms per draw (one boot, alternating arms) ===")
    for app in apps:
        v = [x["ms_per_draw"] for x in rows
             if x["app"] == app and x.get("ms_per_draw")]
        if v:
            print("  %-20s %s   median %.3f ms   n=%d"
                  % (app, [round(x, 2) for x in v], statistics.median(v), len(v)))
    print("\n=== draws per second over the same script ===")
    for app in apps:
        v = [x["draws_per_s"] for x in rows
             if x["app"] == app and x.get("draws_per_s")]
        if v:
            print("  %-20s %s   median %.2f/s"
                  % (app, [round(x, 2) for x in v], statistics.median(v)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="~/urc-multi/zima")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--pace", type=float, default=0.30)
    ap.add_argument("--settle", type=float, default=4.0)
    ap.add_argument("--apps",
                    default="APPS/DUUM.UNO,APPS/DUUMOLD.UNO,APPS/TRIVIAL.UNO")
    ap.add_argument("--out", default="/tmp/ab_results.json")
    a = ap.parse_args()

    t = BridgeTransport(a.dir)
    vs = vols(t)
    print("vols: %r" % (vs,))
    boot = next((v["vol"] for v in vs if v["name"].upper() == "UNODOS"), None)
    if boot is None:
        raise SystemExit("no UNODOS volume - refusing to guess an index")
    print("boot volume = %d" % boot)
    s = snap(t)
    print("perf: frames=%s tsc_per_ms=%s uptime=%s"
          % (s.get("frames"), s.get("tsc_per_ms"), s.get("uptime")))
    if not s.get("tsc_per_ms"):
        raise SystemExit("no perf probe row - the box is not running this kernel")

    apps = [x.strip() for x in a.apps.split(",")]
    rows = []
    for rnd in range(a.rounds):
        for app in apps:
            r = run_arm(t, boot, app, a.cycles, a.pace, a.settle)
            r["round"] = rnd
            rows.append(r)
            print("  %s" % json.dumps(r))
            with open(a.out, "w") as f:
                json.dump(rows, f, indent=1)
    report(rows, apps)


if __name__ == "__main__":
    main()
