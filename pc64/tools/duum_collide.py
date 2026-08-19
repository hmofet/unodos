#!/usr/bin/env python3
"""duum_collide.py - collision and door gate, against the VENDORED engine.

The sibling of upstream's `tools/duum_collide.py`, pointed at `apps/DUUM.PY`
through `duum_host` instead of at the `duum` package - so it tests the file
this OS actually ships, after a sync has landed it, the same way duum_golden
and duum_verify here test the pixels the DEVICE would produce rather than
upstream's reference rasteriser.

Neither rendering gate can see this class of bug: both take the player's
POSITION as an input, so a player standing inside a wall is an invalid
viewpoint fed to a working renderer and both pass while the game is
unplayable.  That is exactly how "Duum has no wall collision" survived to a
ZimaBlade bring-up (see METAL-FINDINGS.md).  This one asserts about MOVEMENT:

  walk    the reproduction from that bring-up: the E1M1 spawn faces a solid
          wall 736 units away, one 96-unit press per call, pinned by press 8
  sweep   randomised moves per level that must not cross a one-sided linedef
  doors   every use-door: shut it blocks, `use` moves it, open it passes.
          Door operation was UNVERIFIED on hardware for want of anything
          solid to stand against
  proj    a rocket fired at a wall stops at it
  menu    the pause menu opens with the DEVICE's Escape, and offers only what
          this platform can actually do

  python3 tools/duum_collide.py [--wad W] [--level E1M1] [-n 4000]

Exit code 0 = every check passed, 1 = at least one failed.
"""
import os
import sys
import math
import random
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import duum_host                                            # noqa: E402

CW, CH = 518, 382                    # the device's Duum canvas
engine = None                        # the vendored module, set by load()
uno_mod = None                       # the `uno` the engine is talking to

LEVELS = ["E1M%d" % i for i in range(1, 10)]
PR = 16                              # player radius, as step_player uses


def load(wad):
    global engine
    if wad:
        duum_host.WAD_OVERRIDE = os.path.abspath(wad)
    engine = duum_host.load_app()
    global uno_mod
    uno_mod = sys.modules["uno"]
    app = engine.app
    app.build(duum_host.Canvas(CW, CH))
    if app.err:
        sys.exit("app.build failed: %s" % app.err)
    return app


def slide(app, ox, oy, nx, ny, r=PR):
    """step_player's move, without the frame around it."""
    if not app.blocked(ox, oy, nx, ny, r, None):
        return nx, ny
    if not app.blocked(ox, oy, nx, oy, r, None):
        return nx, oy
    if not app.blocked(ox, oy, ox, ny, r, None):
        return ox, ny
    return ox, oy


def solid_lines(m):
    return [i for i in range(len(m.lines))
            if m.lines[i][6] == 0xFFFF or m.lines[i][5] == 0xFFFF]


# ---- walk -----------------------------------------------------------------
def check_walk(app):
    """The original report's reproduction, press for press."""
    app.load_level("E1M1")
    px, py, ang = app.lvl.player_start()
    app.px, app.py = float(px), float(py)
    app.pa = math.radians(90)
    ca, sa = math.cos(app.pa), math.sin(app.pa)
    step = engine.MOVSPD * 0.30              # the 96-unit press
    pinned_at = None
    for n in range(1, 21):
        oy = app.py
        app.px, app.py = slide(app, app.px, app.py,
                               app.px + ca * step, app.py + sa * step)
        if app.py == oy and pinned_at is None:
            pinned_at = n
    ok = pinned_at is not None and app.py < -2880 - PR
    print("  walk    pinned at press %s, settled y=%.0f (wall at y=-2880)  %s"
          % (pinned_at, app.py, "ok" if ok else "FAIL"))
    if pinned_at is None:
        print("          the player never stopped: 20 presses, 1920 units, "
              "straight through the level")
    return ok


# ---- sweep ----------------------------------------------------------------
def check_sweep(app, levels, n):
    """No randomised walk may ever cross a one-sided linedef."""
    random.seed(20260818)
    bad = 0
    moves = 0
    for level in levels:
        try:
            app.load_level(level)
        except Exception as e:
            print("  sweep   skip %s (%r)" % (level, e))
            continue
        m = app.lvl
        solid = solid_lines(m)
        px, py, ang = m.player_start()
        app.px, app.py = float(px), float(py)
        a = math.radians(ang)
        leaks = 0
        for _ in range(n):
            a += (random.random() - 0.5) * 0.9
            d = 16.0 + random.random() * 80.0      # 16 units up to a 0.3s press
            ox, oy = app.px, app.py
            app.px, app.py = slide(app, ox, oy,
                                   ox + math.cos(a) * d, oy + math.sin(a) * d)
            moves += 1
            if ox == app.px and oy == app.py:
                continue
            for li in solid:
                ld = m.lines[li]
                ax, ay = m.verts[ld[0]]
                bx, by = m.verts[ld[1]]
                if engine.seg_cross(ox, oy, app.px, app.py, ax, ay, bx, by):
                    leaks += 1
                    if leaks < 4:
                        print("  sweep   LEAK %s line %d  (%.0f,%.0f)->(%.0f,%.0f)"
                              % (level, li, ox, oy, app.px, app.py))
                    break
        bad += leaks
    print("  sweep   %d moves, %d crossed a one-sided wall  %s"
          % (moves, bad, "ok" if bad == 0 else "FAIL"))
    return bad == 0


# ---- doors ----------------------------------------------------------------
def door_stand(app, li):
    """A point 40 units off the door's front face, and the angle to face it.

    -> (x, y, angle, door_sector) or None if neither side is a place to stand.
    """
    m = app.lvl
    ld = m.lines[li]
    ax, ay = m.verts[ld[0]]
    bx, by = m.verts[ld[1]]
    mx, my = (ax + bx) / 2.0, (ay + by) / 2.0
    ex, ey = bx - ax, by - ay
    ln = math.sqrt(ex * ex + ey * ey)
    if ln == 0 or ld[6] == 0xFFFF or ld[5] == 0xFFFF:
        return None
    nx, ny = ey / ln, -ex / ln                  # right-hand (front) normal
    door = m.sectors[m.sides[ld[6]][5]]         # a manual door IS its back sector
    for sign in (1.0, -1.0):
        x = mx + nx * 40.0 * sign
        y = my + ny * 40.0 * sign
        if app.point_sector(x, y) is not door:
            return x, y, math.atan2(my - y, mx - x), door
    return None


def check_doors(app, levels):
    """Shut it blocks, `use` moves it, open it lets you through.

    Each door gets a FRESH level.  Testing them back to back on one map does
    not work and fails in a way that looks like an engine bug: a door left
    standing open, or a one-shot D1 line gone dead, changes the world the next
    door is tested in.  Reloading is a second per level and removes the whole
    class of false failure.
    """
    tried = fails = skipped = 0
    for level in levels:
        try:
            app.load_level(level)
        except Exception:
            continue
        m = app.lvl
        want = [li for li in range(len(m.lines))
                if m.lines[li][3] in engine.DOOR_USE
                and engine.DOOR_USE[m.lines[li][3]][0] == 0]   # no key doors
        for li in want:
            app.load_level(level)
            m = app.lvl
            spot = door_stand(app, li)
            if spot is None:
                continue
            x, y, ang, door = spot
            ld = m.lines[li]
            fs = m.sectors[m.sides[ld[5]][5]]
            if min(fs[1], door[1]) - max(fs[0], door[0]) >= engine.MINHEAD:
                skipped += 1          # already open on load: nothing to prove
                continue
            tried += 1
            # The door is what is under test, not the level's population: on
            # E1M3 line 459 there is a zombieman standing just behind the door
            # who stops the player perfectly correctly, and a gate that cannot
            # tell that apart from a broken door is a gate that cries wolf.
            app.things_live = []
            app.px, app.py, app.pa = x, y, ang
            app.psec_i = app.point_secidx(x, y)
            app.now = 0.0
            ca, sa = math.cos(ang), math.sin(ang)

            def push(k):
                for _ in range(k):
                    app.px, app.py = slide(app, app.px, app.py,
                                           app.px + ca * 16, app.py + sa * 16)

            push(6)
            ax, ay = m.verts[ld[0]]
            bx, by = m.verts[ld[1]]
            shut_stops = not engine.seg_cross(x, y, app.px, app.py,
                                              ax, ay, bx, by)
            ceil0 = door[1]
            app.do_use()
            used = len(app.movers) > 0
            for _ in range(200):                       # 4 s of door
                app.now += 0.02
                app.run_movers(0.02)
            moved = door[1] != ceil0
            # Some special-1 sectors are closets and windows, not doorways:
            # a door opens to the lowest neighbouring ceiling minus 4, and if
            # that leaves under 56 of headroom, or the sill is a step over 24,
            # vanilla will not let a player in either.  Assert passability
            # only where the OPENED geometry actually admits one.
            walkable = (min(fs[1], door[1]) - max(fs[0], door[0])
                        >= engine.MINHEAD
                        and door[0] - fs[0] <= engine.MAXSTEP)
            hx, hy = app.px, app.py
            push(10)
            through = (not walkable or
                       engine.seg_cross(hx, hy, app.px, app.py, ax, ay, bx, by))
            if not (shut_stops and used and moved and through):
                fails += 1
                if fails <= 6:
                    print("  doors   %s line %d  shut-blocks=%s use=%s "
                          "moved=%s passable=%s  FAIL"
                          % (level, li, shut_stops, used, moved, through))
    print("  doors   %d use-doors (%d already open, skipped), %d failed  %s"
          % (tried, skipped, fails, "ok" if fails == 0 and tried else "FAIL"))
    return fails == 0 and tried > 0


# ---- projectiles ----------------------------------------------------------
def check_proj(app):
    """A rocket fired at a wall must not come out the other side."""
    app.load_level("E1M1")
    px, py, ang = app.lvl.player_start()
    app.px, app.py = float(px), float(py)
    app.pa = math.radians(90)
    app.psec_i = app.point_secidx(app.px, app.py)
    app.proj = []
    app.fx = []
    app.spawn_proj(0, b"MISL", 20)
    far = app.py
    for _ in range(200):
        if not app.proj:
            break
        app.run_proj(0.02)
        for p in app.proj:
            if p[1] > far:
                far = p[1]
    ok = far < -2880
    print("  proj    rocket reached y=%.0f (wall at y=-2880)  %s"
          % (far, "ok" if ok else "FAIL"))
    return ok


# ---- menu -----------------------------------------------------------------
def check_menu(app):
    """The menu, through this port's eyes rather than a desktop's.

    Everything here is a thing that is invisible upstream and breakable here:

    - Escape is ASCII 27 on a desktop and SCANCODE 0x17 with uni 0 on this
      machine (hid_kbd.h).  An engine that only knows the first has a menu
      that never opens, and every desktop gate stays green.
    - Rebinding goes through uno_binds.c, whose semantics duum_host mirrors:
      a KEY ID is unshifted ASCII or 0x101-0x105, and Use is REFUSED because
      it is read as a key event rather than from the held bitmap the binding
      table feeds.  A stored Use binding would do nothing.
    - There is no frontend here, so the ENGINE captures the rebind and hands
      the host the raw event.  That path only exists for ports like this one.
    - Quit is still not offered: the shell owns its windows.
    - draw_menu goes through the C canvas mirror, so this also proves the menu
      needs nothing the span-writer contract does not already have.
    """
    app.load_level("E1M1")
    app.render()
    ok = True

    def bad(msg):
        print("  menu    %s  FAIL" % msg)

    if app.menu_open():
        bad("the menu was already open"); ok = False
    app.key(0, 0x17, 0)
    if not app.menu_open():
        bad("the device's Esc scancode (0x17) did not open it")
        return False
    if "Quit" in [r[0] for r in app.menu_rows()]:
        bad("Quit is offered, and nothing here can act on it"); ok = False
    if not app.can_bind():
        bad("the host hooks are there but the menu does not see them"); ok = False

    # rebind Turn left onto K, with no frontend in the picture
    app.menu = [app.M_KEYS, 2]
    app.key(13, 0, 0)
    if app.capture != engine.A_TURNL:
        bad("selecting the row did not start a capture"); ok = False
    app.key(ord("k"), 0, 0)
    if app.capture is not None:
        bad("the capture never ended"); ok = False
    if "K" not in app.bind_name(engine.A_TURNL):
        bad("the new key is not on the action: %r"
            % app.bind_name(engine.A_TURNL)); ok = False
    if duum_host.bind_bits(ord("k")) != engine.A_TURNL:
        bad("the keyboard would not report the new key as turn-left"); ok = False
    if duum_host.bind_bits(duum_host.BK_LEFT) != 0:
        bad("the old key still turns left"); ok = False

    # Use is refused, and says so rather than storing something inert
    app.menu = [app.M_KEYS, 7]
    app.key(13, 0, 0)
    app.msg = ""
    app.key(ord("j"), 0, 0)
    if duum_host.bind_bits(ord("j")) & engine.A_USE:
        bad("Use accepted a binding it cannot honour"); ok = False
    if not app.msg:
        bad("a refused rebind said nothing"); ok = False

    if hasattr(uno_mod, "bind_reset"):
        uno_mod.bind_reset()
    if duum_host.bind_bits(duum_host.BK_LEFT) != engine.A_TURNL:
        bad("reset did not restore the defaults"); ok = False

    # the FPS toggle is remembered through the host, not in the app
    app.menu = [app.M_OPTS, 0]
    was = app.show_fps
    app.key(13, 0, 0)
    if app.show_fps == was:
        bad("the FPS toggle did nothing"); ok = False
    if uno_mod.pref_get("fps") != ("1" if app.show_fps else "0"):
        bad("the FPS setting was not written through to the host"); ok = False

    app.menu = [app.M_MAIN, 0]
    app.show_fps = True
    try:
        cv = duum_host.Canvas(CW, CH)
        cv.clear(0)
        app.draw(cv)
    except Exception as e:
        bad("drawing it through the device canvas raised %r" % e); ok = False
    app.show_fps = False
    app.key(0, 0x17, 0)
    if app.menu_open():
        bad("the device's Esc did not close it"); ok = False
    print("  menu    device Esc, rebinding, refusals and prefs  %s"
          % ("ok" if ok else "FAIL"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", default=None)
    ap.add_argument("--level", default=None, help="one level instead of all")
    ap.add_argument("-n", type=int, default=4000,
                    help="randomised moves per level in the sweep")
    args = ap.parse_args()

    app = load(args.wad)
    levels = [args.level] if args.level else LEVELS

    results = [check_walk(app),
               check_sweep(app, levels, args.n),
               check_doors(app, levels),
               check_proj(app),
               check_menu(app)]
    bad = results.count(False)
    print("%d/%d check(s) passed" % (len(results) - bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
