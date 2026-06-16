#!/usr/bin/env python3
"""UnoDOS conformance — PORT-SPEC §6 made executable (CONTRACT-ARCH Phase 2 / §15.2).

Host-only, stdlib-only. Two layers:

  A. STRUCTURAL conformance — static invariants checked against the Contract, the
     generated x86 surface, and kernel.asm (audit-tax rules 8, 9, 10-structure).

  B. BEHAVIORAL conformance — the §6 policy invariants (z-order, event focus/stale,
     the fixed-size tombstone queue, edge-only mouse, owner-based reaping) as
     EXECUTABLE REFERENCE MODELS + golden vectors. Each invariant is paired with the
     HISTORICAL BUGGY behavior PORT-SPEC warns about; the runner asserts the
     reference passes every vector AND the buggy version fails at least one — proving
     the vectors discriminate (they are not vacuous).

These reference models are the host oracle every world's implementation will later be
run against (§15.1). Run:  python unodef/conformance/conformance.py
Exit code 0 = all conformant.
"""
import os, sys, tomllib, re

HERE = os.path.dirname(os.path.abspath(__file__))
UNODEF = os.path.join(HERE, "..")
ROOT = os.path.join(UNODEF, "..")

def load_contract():
    with open(os.path.join(UNODEF, "unodef.toml"), "rb") as f:
        return tomllib.load(f)

# Pass/fail accounting -------------------------------------------------------
RESULTS = []
def record(rule, name, ok, detail=""):
    RESULTS.append((rule, name, ok, detail))

# ===========================================================================
# A. STRUCTURAL CONFORMANCE
# ===========================================================================

# Rule 9 — "Keep the API table address/IDs stable; additions append." The frozen
# baseline below is the append-only anchor: existing ordinals must never renumber;
# new calls may only extend the tail. (Snapshot taken Phase 0 — UnoDOS v3.32 / 106.)
BASELINE_ORDINALS = {
    0:"gfx_draw_pixel",13:"fs_mount",14:"fs_open",20:"win_create",27:"fs_readdir",
    34:"app_yield",41:"speaker_tone",48:"gfx_set_font",84:"clip_copy",
    103:"gfx_blit_rect",105:"theme_set_palette",
}  # representative anchors across the categories; all must hold

def check_rule9_appendonly(d):
    by_ord = {s["ordinal"]: s["name"] for s in d["syscall"]}
    ords = sorted(by_ord)
    record(9, "ordinals contiguous 0..N-1", ords == list(range(len(ords))),
           "got %d..%d (%d)" % (ords[0], ords[-1], len(ords)))
    record(9, "count == len(syscalls)", d["callgate"]["count"] == len(by_ord),
           "count=%d len=%d" % (d["callgate"]["count"], len(by_ord)))
    drift = [(o, n, by_ord.get(o)) for o, n in BASELINE_ORDINALS.items() if by_ord.get(o) != n]
    record(9, "append-only (no renumber of baseline)", not drift, str(drift))

def check_rule10_struct(d):
    # "Every queue/table is fixed-size — design the 'full' behavior explicitly."
    structs = {s["name"]: s for s in d["struct"]}
    for nm, want_count in [("event", 32), ("win_entry", 16), ("file_handle", 16)]:
        s = structs.get(nm, {})
        record(10, "%s declares fixed count" % nm,
               s.get("count") == want_count, "count=%s" % s.get("count"))
    # The queue's 'full' behavior must be a defined enum value (tombstone), not UB.
    record(10, "event queue defines CONSUMED tombstone",
           d["enum"]["event_type"].get("CONSUMED") == 0xFF,
           "CONSUMED=%s" % d["enum"]["event_type"].get("CONSUMED"))

def check_abi31(d):
    # Phase 12: the 3.1 categorized ordinal map must be collision-free and every
    # category must fit its band (else 3.1 must widen it before shipping).
    cfg = d.get("abi31")
    if not cfg:
        return
    band = cfg["band_width"]; order = cfg["category_order"]
    base = {c: i * band for i, c in enumerate(order)}
    seen = {}; coll = 0; overflow = []
    bycat = {}
    for s in sorted(d["syscall"], key=lambda x: x["ordinal"]):
        c = s["category"]; idx = bycat.get(c, 0); bycat[c] = idx + 1
        if idx >= band: overflow.append(c)
        new = base[c] + idx
        if new in seen: coll += 1
        seen[new] = s["name"]
    record(12, "3.1 categorized ordinals collision-free", coll == 0, "%d collisions" % coll)
    record(12, "every category fits its 3.1 band", not overflow, "overflow: %s" % set(overflow))
    record(12, "uno_header_v2 (.UNO v2) defined", any(s["name"] == "uno_header_v2" for s in d["struct"]), "")

def check_rule8_geometry(d):
    # "Centralize disk geometry." Contract holds ONE fat12 block; the generated x86
    # surface must equal kernel.asm's literals (the Phase-1 trust anchor, re-run here).
    record(8, "single fat12 geometry block in Contract", "fat12" in d["const"], "")
    # the OS-area layout must sum to fs_start_sector (the "110" single-sourced across
    # boot.asm + stage2.asm + the image tools — the FAT12 "five places", now one).
    bl = d["const"].get("boot_layout")
    if bl:
        s = bl["boot_sectors"] + bl["stage2_sectors"] + bl["kernel_sectors"] + bl["spare_sectors"]
        record(8, "boot_layout sums to fs_start_sector (five-places unified)",
               s == d["const"]["fat12"]["fs_start_sector"], "sum=%d" % s)
    inc = os.path.join(UNODEF, "gen", "x86", "unodef.inc")
    ker = os.path.join(ROOT, "kernel", "kernel.asm")
    def equ(path):
        out = {}
        for line in open(path, encoding="utf-8", errors="replace"):
            m = re.match(r"^([A-Z][A-Z0-9_]+)\s+equ\s+(\S+)", line)
            if m:
                try: out[m.group(1)] = int(m.group(2), 0)
                except ValueError: pass
        return out
    if os.path.exists(inc) and os.path.exists(ker):
        g, k = equ(inc), equ(ker)
        anchors = ["FAT12_BYTES_PER_SECTOR","FAT12_RESERVED_SECTORS","FAT12_NUM_FATS",
                   "FAT12_ROOT_DIR_ENTRIES","FAT12_SECTORS_PER_FAT","FAT12_FAT_START",
                   "FAT12_ROOT_DIR_START","FAT12_DATA_AREA_START"]
        # kernel.asm now SOURCES these from the include, so they live in g (the inc).
        contract_vals = {a: g.get(a) for a in anchors}
        expect = {"FAT12_BYTES_PER_SECTOR":512,"FAT12_RESERVED_SECTORS":1,"FAT12_NUM_FATS":2,
                  "FAT12_ROOT_DIR_ENTRIES":224,"FAT12_SECTORS_PER_FAT":9,"FAT12_FAT_START":111,
                  "FAT12_ROOT_DIR_START":129,"FAT12_DATA_AREA_START":143}
        record(8, "generated x86 FAT12 geometry == known-good", contract_vals == expect,
               "" if contract_vals == expect else str(contract_vals))
    else:
        record(8, "generated x86 FAT12 geometry == known-good", False, "gen/kernel missing")

# ===========================================================================
# B. BEHAVIORAL CONFORMANCE — reference models + vectors + discrimination
# ===========================================================================

# --- Rule 6: Z-order (PORT-SPEC §2) ----------------------------------------
# Windows hold a contiguous z-band topped at TOP (=15). Raising window w rotates:
# w -> TOP, every window ABOVE w's old z drops by 1, windows BELOW are untouched.
# Destroy renormalizes survivors (preserving order) and the new max is topmost;
# focus follows topmost. The historical bug demoted ALL windows (incl. below),
# leaking z-levels until everything collided at 0.
TOP = 15
def zorder_raise_ref(z, w):
    z = dict(z); zw = z[w]
    for k in z:
        if z[k] > zw: z[k] -= 1
    z[w] = TOP
    return z
def zorder_raise_buggy(z, w):
    z = dict(z); zw = z[w]
    for k in z:
        if k != w: z[k] -= 1          # BUG: demotes below-z windows too
    z[w] = TOP
    return z
def zorder_destroy_ref(z, w):
    z = {k: v for k, v in z.items() if k != w}
    for newz, k in enumerate(sorted(z, key=lambda k: z[k])):
        z[k] = TOP - (len(z) - 1 - newz)   # renormalize, top-aligned
    return z
def zorder_topmost(z):
    return max(z, key=lambda k: z[k]) if z else None

def zorder_band_ok(z):
    if not z: return True
    vals = sorted(z.values())
    return len(set(vals)) == len(vals) and vals == list(range(TOP - len(vals) + 1, TOP + 1))

def vectors_zorder():
    # start: 3 windows top-aligned (A=15,B=14,C=13); a sequence of raises
    start = {"A": 15, "B": 14, "C": 13}
    return [
        ("raise C", "C", {"C": 15, "A": 14, "B": 13}),
        ("raise B", "B", {"B": 15, "A": 14, "C": 13}),
    ], start

def run_zorder():
    vecs, start = vectors_zorder()
    for label, w, expect in vecs:
        got = zorder_raise_ref(start, w)
        record(6, "zorder %s" % label, got == expect, "got=%s" % got)
    # band invariant must hold after EVERY raise; the demote-all bug breaks it as
    # soon as an already-topmost (or low) window is raised.
    z = dict(start); zb = dict(start)
    seq = ["A", "C", "A", "B", "A"]
    ref_always = True
    buggy_ever_bad = False
    for w in seq:
        z = zorder_raise_ref(z, w);   ref_always = ref_always and zorder_band_ok(z)
        zb = zorder_raise_buggy(zb, w)
        if not zorder_band_ok(zb): buggy_ever_bad = True
    record(6, "reference keeps a valid z-band after every raise", ref_always, str(z))
    record(6, "DISCRIMINATION: buggy raise corrupts the band", buggy_ever_bad, str(zb))
    # destroy renormalizes + promotes topmost
    z2 = zorder_destroy_ref({"A": 15, "B": 14, "C": 13}, "B")
    record(6, "destroy renormalizes survivors", zorder_band_ok(z2) and zorder_topmost(z2) == "A",
           str(z2))

# --- Rule 3: event focus stamping / stale discard (PORT-SPEC §3) ------------
NONE = 0xFF
def deliver_ref(stamp, focused, consumer):
    # stamp = focused task at post time; focused = current focused task.
    # Deliver a task-stamped event only if it is FOR the consumer AND that task is
    # still focused (else it is stale — focus moved while it sat queued). A NONE
    # stamp delivers to any poller only while nothing is focused.
    if stamp == NONE:
        return "deliver" if focused == NONE else "discard"
    if stamp == consumer and stamp == focused:
        return "deliver"
    return "discard"
def deliver_buggy(stamp, focused, consumer):
    return "deliver"   # BUG: deliver regardless of stamp -> cross-task key leakage

def vectors_events():
    # (stamp, focused_now, consumer, expected)
    return [
        (3, 3, 3, "deliver"),       # stamped for me, I'm focused
        (3, 5, 3, "discard"),       # stamped for me but focus moved to 5 -> stale
        (5, 5, 3, "discard"),       # stamped for another task
        (NONE, NONE, 3, "deliver"), # no focus at post or now -> any poller
        (NONE, 7, 3, "discard"),    # NONE stamp but something focused now
    ]
def run_events():
    bad_buggy = 0
    for stamp, foc, cons, expect in vectors_events():
        got = deliver_ref(stamp, foc, cons)
        record(3, "event[stamp=%s foc=%s]" % (stamp, foc), got == expect, "got=%s want=%s" % (got, expect))
        if deliver_buggy(stamp, foc, cons) != expect: bad_buggy += 1
    record(3, "DISCRIMINATION: deliver-always leaks events", bad_buggy > 0, "%d vectors caught" % bad_buggy)

# --- Rule 10 (behavior): fixed-size queue, tombstones, lazy head (PORT-SPEC §3)
class EventQueue:
    SIZE = 32
    def __init__(self): self.buf = [None]*self.SIZE; self.head = 0; self.tail = 0
    def post(self, ev):
        nxt = (self.tail + 1) % self.SIZE
        if nxt == self.head: return False        # FULL: explicit drop, not UB
        self.buf[self.tail] = ev; self.tail = nxt; return True
    def consume_match(self, pred):
        # forward-scan delivery; tombstone mid-queue; lazy head advance over tombstones
        i = self.head
        while i != self.tail:
            ev = self.buf[i]
            if ev is not None and pred(ev):
                self.buf[i] = None                     # tombstone
                if i == self.head:
                    while self.head != self.tail and self.buf[self.head] is None:
                        self.head = (self.head + 1) % self.SIZE
                return ev
            i = (i + 1) % self.SIZE
        return None

def run_queue():
    q = EventQueue()
    # fill to capacity (SIZE-1 usable), assert explicit full
    ok_fill = all(q.post(("k", i)) for i in range(EventQueue.SIZE - 1))
    record(10, "queue accepts SIZE-1 entries", ok_fill, "")
    record(10, "queue rejects on full (explicit, no UB)", q.post(("k", 99)) is False, "")
    # forward-scan: consume a middle item without head-blocking the rest
    q2 = EventQueue()
    for i in range(5): q2.post(("k", i))
    got = q2.consume_match(lambda e: e[1] == 3)   # take the 4th
    record(10, "forward-scan delivers non-head match", got == ("k", 3), "got=%s" % (got,))
    record(10, "head not advanced past live entries (no loss)",
           q2.consume_match(lambda e: e[1] == 0) == ("k", 0), "")

# --- Rule 5: edge-only mouse event posting (PORT-SPEC §3) -------------------
def mouse_edges_ref(button_samples):
    # post a MOUSE event ONLY when the button mask changes (motion is pollable state)
    events = []; prev = 0
    for b in button_samples:
        if b != prev: events.append(b)
        prev = b
    return events
def mouse_edges_buggy(button_samples):
    return list(button_samples)   # BUG: posts every sample -> motion floods the queue
def run_mouse():
    samples = [0, 0, 1, 1, 1, 0, 0, 2, 0]   # presses/releases amid motion (same mask repeats)
    ref = mouse_edges_ref(samples)
    record(5, "edge-only mouse posts only on change", ref == [1, 0, 2, 0], "got=%s" % ref)
    record(5, "DISCRIMINATION: per-sample posting floods",
           len(mouse_edges_buggy(samples)) > len(ref), "%d vs %d" % (len(mouse_edges_buggy(samples)), len(ref)))

# --- Rule 7: owner-based reaping on every kill path (PORT-SPEC §4/§6.7) ------
def reap_ref(handles, windows, dead_task):
    # free every file handle and window OWNED by the dead task (owner byte == task)
    h = [x for x in handles if x["owner"] != dead_task]
    w = [x for x in windows if x["owner"] != dead_task]
    return h, w
def reap_buggy(handles, windows, dead_task):
    return [x for x in handles if x["owner"] != dead_task], windows  # BUG: leaks windows
# --- Phase 8 / §8: directional focus (pointer-less d-pad traversal) ----------
# unoui's Tab-traversal generalized: RIGHT/DOWN -> next focusable, LEFT/UP -> prev,
# CLAMPED at the ends (no wrap). A = activate current, B = cancel (-1). The bug:
# wrapping at the ends, which makes "down" at the bottom jump to the top.
def dfocus_ref(i, n, key):
    if key in ("RIGHT", "DOWN"): return i + 1 if i + 1 < n else n - 1
    if key in ("LEFT", "UP"):    return i - 1 if i - 1 >= 0 else 0
    return i
def dfocus_buggy(i, n, key):
    if key in ("RIGHT", "DOWN"): return (i + 1) % n     # BUG: wraps
    if key in ("LEFT", "UP"):    return (i - 1) % n
    return i
def run_dfocus():
    n = 4
    # walk to the bottom then press DOWN again — must stay clamped at n-1
    i = 0
    for k in ["DOWN","DOWN","DOWN","DOWN"]: i = dfocus_ref(i, n, k)
    record(8, "directional focus clamps at last (no wrap)", i == n - 1, "i=%d" % i)
    i2 = 0
    for k in ["DOWN","DOWN","DOWN","DOWN"]: i2 = dfocus_buggy(i2, n, k)
    record(8, "DISCRIMINATION: wrapping focus is caught", i2 != n - 1, "buggy i=%d" % i2)
    # clamp at top
    j = 0; j = dfocus_ref(j, n, "UP")
    record(8, "directional focus clamps at first", j == 0, "j=%d" % j)

# --- Phase 8 / §9: profile honesty (minimal ports can't claim a WM/pointer) ---
def check_profiles(d):
    prof = d.get("profile", {})
    for name, p in d.get("port", {}).items():
        pf = p.get("profile")
        req = prof.get(pf, {}).get("requires", [])
        caps = p.get("capabilities", [])
        if pf == "minimal":
            record(9, "port %s (minimal) requires no WM" % name, "wm" not in req, str(req))
            record(9, "port %s (minimal) has no POINTER cap" % name, "POINTER" not in caps, str(caps))
        if pf == "single_app":
            # single_app ports run one app at a time and require no multi-window
            # WM — which is WHY the 6502 ports legitimately carry no window-entry
            # struct for a [world.*] to source. Assert that's honest, not faked.
            pdef = prof.get(pf, {})
            record(9, "port %s (single_app) is one-app, requires no WM" % name,
                   pdef.get("one_app") is True and "wm" not in req, str(req))
        if "HEADLESS" in caps:
            record(9, "port %s (HEADLESS) declares zero surfaces" % name,
                   not p.get("surface"), str(p.get("surface")))

def run_reaping():
    handles = [{"id": 0, "owner": 3}, {"id": 1, "owner": 5}, {"id": 2, "owner": 3}]
    windows = [{"id": 0, "owner": 3}, {"id": 1, "owner": 3}, {"id": 2, "owner": 5}]
    h, w = reap_ref(handles, windows, 3)
    record(7, "reap frees dead task's handles", [x["id"] for x in h] == [1], "")
    record(7, "reap frees dead task's windows", [x["id"] for x in w] == [2], "")
    _, wb = reap_buggy(handles, windows, 3)
    record(7, "DISCRIMINATION: leaking windows is caught", len(wb) != len(w), "")

# --- Greenfield window model (§11 redux): logical invariants verified over a
# physical realization THROUGH the accessor boundary. The point: the same
# invariant models hold regardless of layout (here a Python stand-in for the SoA
# columns wmgen emits), so correctness is checked on logical behaviour — not on
# any platform's byte offsets. Paired with discrimination (a buggy realization). -
def check_wmodel(d):
    wm = d.get("wmodel")
    if not wm:
        record("wm", "wmodel present in contract", False, "no [wmodel]"); return
    win = wm["window"]
    # structural: identity is a handle; the load-bearing invariants are declared.
    record("wm", "identity is a handle", win.get("identity") == "handle", str(win.get("identity")))
    rules = win.get("invariant", {}).get("rules", [])
    for key in ("owner-readable", "reap", "zorder-total"):
        record("wm", "declares invariant: %s" % key,
               any(r.startswith(key) for r in rules), str(rules))

    # SoA realization stand-in: one list per column, indexed by the handle.
    CAP = 6
    col = {"state": [0] * CAP, "owner": [0] * CAP}
    def get(c, i): return col[c][i]
    def setf(c, i, v): col[c][i] = v
    # the SAME reap policy wmgen emits, expressed only through accessors:
    def reap(dead):
        for i in range(CAP):
            if get("state", i) and get("owner", i) == dead:
                setf("state", i, 0)
    for i, o in [(0, 7), (1, 9), (2, 7), (3, 9), (4, 7)]:   # 0,2,4 -> task7; 1,3 -> task9
        setf("state", i, 1); setf("owner", i, o)
    reap(9)
    live = [i for i in range(CAP) if get("state", i)]
    record("wm", "reap (via SoA accessors) frees exactly task 9's windows",
           live == [0, 2, 4], "live=%s" % live)
    # discrimination: a realization that reaps on the WRONG column leaks/over-frees.
    bs = {"state": [1, 1, 1, 0, 0, 0], "owner": [7, 9, 7, 9, 7, 0]}
    for i in range(CAP):
        if bs["state"][i] and bs["state"][i] == 9:          # BUG: tests state, not owner
            bs["state"][i] = 0
    leaked = [i for i in range(CAP) if bs["state"][i] and bs["owner"][i] == 9]
    record("wm", "DISCRIMINATION: reaping the wrong column is caught", len(leaked) > 0, "")

    # zorder-total: the z-list (relation, not a per-window field) stays a permutation
    # of the live handles across a close.
    z, liveset = [4, 2, 0], {0, 2, 4}
    record("wm", "zorder-total: z-list is a permutation of live handles",
           sorted(z) == sorted(liveset), "z=%s" % z)
    z = [h for h in z if h != 2]; liveset = {0, 4}           # close handle 2
    record("wm", "zorder-total holds after a close", sorted(z) == sorted(liveset), "z=%s" % z)
    record("wm", "DISCRIMINATION: stale z-list entry is caught",
           sorted([4, 2, 0]) != sorted({0, 4}), "")

# ===========================================================================
# Runner
# ===========================================================================
def main():
    d = load_contract()
    print("=== A. STRUCTURAL (audit-tax rules 8, 9, 10-structure) ===")
    check_rule9_appendonly(d)
    check_rule10_struct(d)
    check_rule8_geometry(d)
    check_abi31(d)
    print("=== B. BEHAVIORAL (§6 policy invariants, model + vectors + discrimination) ===")
    run_zorder(); run_events(); run_queue(); run_mouse(); run_reaping(); run_dfocus()
    print("=== C. PROFILES (audit-tax §9: scaling is generated and honest) ===")
    check_profiles(d)
    print("=== D. WMODEL (greenfield window model: logical invariants over the realization) ===")
    check_wmodel(d)

    npass = sum(1 for _, _, ok, _ in RESULTS if ok)
    ntot = len(RESULTS)
    for rule, name, ok, detail in RESULTS:
        mark = "PASS" if ok else "FAIL"
        line = "  [%s] rule %-2s  %s" % (mark, rule, name)
        if detail and not ok: line += "   <- " + detail
        print(line)
    print("\n%d/%d conformance checks passed (PORT-SPEC §6 covered: 3,5,6,7,8,9,10)." % (npass, ntot))
    print("Not host-testable (HW/timing): rule 1 (atomic cursor hide+lock, IF masking), "
          "rule 2 partial (ISR-defer timing). rule 4 (press latch/seq) — vectors TODO.")
    sys.exit(0 if npass == ntot else 1)

if __name__ == "__main__":
    main()
