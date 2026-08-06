#!/usr/bin/env python3
"""renderer_switch_urc - prove the RENDERER switches at runtime, in one boot.

The renderer used to be chosen with -DUW_ENGINE, so the two paths could only
ever be compared across two builds and two boots. This drives both of them in
ONE kernel and asserts on pixels.

The discriminator is chosen so it cannot be faked by the page label changing:
the test page's colours live in LINKED stylesheets, and only the engine path
fetches those (fetch_link_sheets is engine-only, and the flow painter has no
cascade to feed them to anyway). So:

    flow painter  -> the page renders, in default colours, NO #1e5ac8 / #c81e28
    unoweb engine -> the same page renders WITH both colours

If the switch did nothing, the second measurement would equal the first.

    cd pc64 && UNO_DEBUG=1 ./build.sh          # note: NO BROWSER_ENGINE
    python3 tools/renderer_switch_urc.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi
import nettest_server

PORT = 8099
HOST = "10.0.2.2"
H1_BLUE  = (0x1e, 0x5a, 0xc8)      # c.css  h1{color:#1e5ac8}
TEXT_RED = (0xc8, 0x1e, 0x28)      # a.css  .k{color:#c81e28}
CONTENT  = (46, 115, 478, 328)


def frame(ui):
    w, h, rgba = ui.link.screen_grab(1, timeout=60)
    return w, h, [(rgba[i], rgba[i+1], rgba[i+2]) for i in range(0, len(rgba), 4)]


def count(px, want, tol=8):
    return sum(1 for p in px
               if abs(p[0]-want[0]) <= tol and abs(p[1]-want[1]) <= tol
               and abs(p[2]-want[2]) <= tol)


def ink(px, w, h, box):
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, min(y1, h)):
        row = y * w
        for x in range(x0, min(x1, w)):
            r, g, b = px[row+x]
            if r < 236 or g < 236 or b < 236:
                n += 1
    return n


def goto(ui, loc, settle=6.0):
    ui.key(ord('l'), ctrl=1)
    ui.key(0, scan=0x06, settle=0.05)
    for _ in range(50):
        ui.key(8, settle=0.02)
    ui.text(loc)
    ui.key(13)
    time.sleep(settle)


def open_browser(ui):
    n = ui.app_count()
    for i in range(n - 1, -1, -1):
        try:
            ui.link.command("launch", i, timeout=15)
        except RuntimeError:
            continue
        time.sleep(3.0)
        if any(t.startswith("Browser") for t in ui.windows()):
            return i
        try:
            ui.link.command("close", timeout=10)
        except RuntimeError:
            pass
        time.sleep(0.6)
    raise SystemExit("no app slot opens the Browser")


def measure(ui, tag):
    """Load the sheet-coloured page and report (ink, blue, red)."""
    goto(ui, "http://%s:%d/" % (HOST, PORT), settle=14.0)
    try:
        ui.shot(tag)
    except Exception:
        pass
    w, h, px = frame(ui)
    return ink(px, w, h, CONTENT), count(px, H1_BLUE), count(px, TEXT_RED)


def main():
    srv = nettest_server.start(PORT)
    results = []
    try:
        with UrcUi() as ui:
            open_browser(ui)

            # ---- 1. the kernel boots on the flow painter ----------------
            nettest_server.reset()
            flow_ink, flow_blue, flow_red = measure(ui, "rs_01_flow")
            print("  flow painter : ink %d, h1-blue %d, .k-red %d"
                  % (flow_ink, flow_blue, flow_red))
            results.append(("the flow painter draws the page", flow_ink > 300))
            results.append(("...and does NOT apply linked sheets",
                            flow_blue < 20 and flow_red < 20))

            # ---- 2. switch renderer, SAME BOOT --------------------------
            goto(ui, "uno:engine/render/unoweb", settle=5.0)
            try:
                ui.shot("rs_02_engine_page")
            except Exception:
                pass
            nettest_server.reset()
            uw_ink, uw_blue, uw_red = measure(ui, "rs_03_unoweb")
            print("  unoweb engine: ink %d, h1-blue %d, .k-red %d"
                  % (uw_ink, uw_blue, uw_red))
            results.append(("the unoweb engine draws the page", uw_ink > 300))
            # THE assertion: same kernel, same boot, same page, and now the
            # linked stylesheets are applied. Nothing but the renderer changed.
            results.append(("...and DOES apply them, after a runtime switch",
                            uw_blue >= 20 and uw_red >= 20))

            # ---- 3. and back again --------------------------------------
            # A switch that only goes one way is a latch, not a switch.
            goto(ui, "uno:engine/render/flow", settle=5.0)
            nettest_server.reset()
            back_ink, back_blue, back_red = measure(ui, "rs_04_back_to_flow")
            print("  back to flow : ink %d, h1-blue %d, .k-red %d"
                  % (back_ink, back_blue, back_red))
            results.append(("switching back restores the flow painter",
                            back_ink > 300 and back_blue < 20 and back_red < 20))
    finally:
        try:
            srv.close()
        except Exception:
            pass

    print()
    bad = 0
    for name, ok in results:
        print(("pass " if ok else "FAIL ") + name)
        bad += 0 if ok else 1
    print("\n%d pass, %d fail" % (len(results) - bad, bad))
    return 1 if bad else 0


sys.exit(main())
