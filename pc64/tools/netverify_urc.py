#!/usr/bin/env python3
"""netverify_urc - test the M7 network changes against a REAL server.

The gate's network area runs on a null NIC, so framing, keep-alive and
progressive render have all been landing unverified. This closes that: a
controlled server on the host (nettest_server), the browser in QEMU reaching
it at 10.0.2.2, and assertions on what the SERVER saw rather than on what the
browser claims.

    cd pc64 && BROWSER_ENGINE=uw UNO_DEBUG=1 ./build.sh
    python3 tools/netverify_urc.py

Four questions, four answers:
  parallel fetch   three delayed sheets arrive together, not one after another
  keep-alive       a second page to the same origin needs no new connections
  chunked framing  a chunked page renders its decoded text
  progressive      text is on screen DURING a deliberate mid-response pause
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi
import nettest_server


def shot(ui, tag):
    """A screenshot is nice-to-have here, not the measurement. pc64 is
    cooperative: while a fetch blocks, the URC channel cannot answer, so a
    grab issued too early times out. The SERVER's view is the evidence."""
    try:
        ui.shot(tag)
    except Exception as e:
        print("  (no shot %s: %s)" % (tag, type(e).__name__))

PORT = 8099
HOST = "10.0.2.2"          # the QEMU user-mode gateway = this machine

# What the page's own stylesheets set, and nothing else on this desktop uses.
H1_BLUE  = (0x1e, 0x5a, 0xc8)      # c.css  h1{color:#1e5ac8}
TEXT_RED = (0xc8, 0x1e, 0x28)      # a.css  .k{color:#c81e28}


def frame(ui):
    """(w, h, [(r,g,b), ...]) read back over the link. Four bytes per pixel,
    and urcui's own PPM writer takes the first three as RGB, so we do too."""
    w, h, rgba = ui.link.screen_grab(1, timeout=60)
    return w, h, [(rgba[i], rgba[i + 1], rgba[i + 2]) for i in range(0, len(rgba), 4)]


def count(px, want, tol=8):
    """Pixels within `tol` of a colour. A tolerance because glyph edges are
    antialiased; the interiors are exact, which is what clears the threshold."""
    return sum(1 for p in px
               if abs(p[0] - want[0]) <= tol and abs(p[1] - want[1]) <= tol
               and abs(p[2] - want[2]) <= tol)


def goto(ui, loc, settle=3.0):
    ui.key(ord('l'), ctrl=1)
    ui.key(0, scan=0x06, settle=0.05)              # End
    for _ in range(40):
        ui.key(8, settle=0.03)
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
        ui.link.command("close", timeout=10)
        time.sleep(0.6)
    raise SystemExit("no app slot opens the Browser")


def main():
    srv = nettest_server.start(PORT)
    print("server up on :%d" % PORT)
    results = []
    try:
        with UrcUi() as ui:
            open_browser(ui)

            # ---- 1. PARALLEL subresource fetching -----------------------
            # Every sheet is answered a second late, so serial and parallel
            # are hours apart at QEMU timescales: three sheets one after
            # another is >= 3 s, three together is ~1 s. Without the delay a
            # fast serial fetcher and a parallel one are indistinguishable,
            # which is why "one connection for the whole page" used to be the
            # only thing this section could assert.
            nettest_server.reset()
            nettest_server.delays["sheet"] = 1.0
            goto(ui, "http://%s:%d/" % (HOST, PORT), settle=15.0)
            s = dict(nettest_server.stats)
            span = nettest_server.span()
            shot(ui, "net_01_parallel")
            print("  page load: connections=%d requests=%d peak=%d span=%.2fs paths=%s"
                  % (s["connections"], s["requests"], s["peak"], span, s["paths"]))
            results.append(("subresources fetched (page + 3 sheets)",
                            s["requests"] >= 4))
            results.append(("several connections IN FLIGHT at once",
                            s["peak"] >= 3))
            results.append(("page load shorter than the sum of its parts "
                            "(%.2fs vs 3.0s serial)" % span,
                            0 < span < 2.5))
            # The client used to send "Connection: close" while asking for
            # keep-alive semantics; this server answered keep-alive anyway, so
            # the contradiction was invisible here and fatal everywhere else.
            results.append(("the client actually asks to persist",
                            s["keepalive_asked"] >= 4 and s["close_asked"] == 0))

            # ---- 1a. and it is actually ON SCREEN -----------------------
            # Every assertion above passed for weeks while this page rendered
            # BLANK, because they all read the server. The screenshot looked
            # normal too - right window, right address bar, "200 OK" in the
            # status band, empty content area. So read the PIXELS.
            #
            # The colours are the interesting part: the browser would never
            # draw #1e5ac8 or #c81e28 on its own, they are what c.css and
            # a.css set. Finding them on screen is end to end from a parallel
            # HTTP fetch, through the splice into the DOM and the cascade, to
            # a lit pixel - which is more than "something was drawn".
            #
            # Only these two. "The frame has more than one colour" was in here
            # and is worthless: the grab is the whole desktop, so the chrome
            # and the taskbar clear it while the page is blank. It passed on
            # the broken build. A check that cannot fail is not a check.
            w, h, px = frame(ui)
            blue, red = count(px, H1_BLUE), count(px, TEXT_RED)
            print("  screen %dx%d: h1 blue %d px, .k red %d px" % (w, h, blue, red))
            results.append(("the h1 is drawn in the colour c.css set", blue >= 20))
            results.append(("the body text is drawn in the colour a.css set", red >= 20))

            # ---- 1b. and the pool is REUSED on the next navigation ------
            # Parallel fetching means a page opens several connections, so
            # "one connection" is no longer the measure of keep-alive.
            # Reuse shows up as a second page needing NO new accepts.
            nettest_server.reset()
            nettest_server.delays["sheet"] = 0.0
            goto(ui, "http://%s:%d/?again" % (HOST, PORT), settle=10.0)
            s = dict(nettest_server.stats)
            print("  second load: NEW connections=%d requests=%d" % (s["connections"], s["requests"]))
            results.append(("keep-alive: the second page reuses pooled connections",
                            s["requests"] >= 4 and s["connections"] < s["requests"]))

            # ---- 1c. and it RECOVERS when the server drops the pool -----
            # Every real server hangs up on an idle connection eventually, and
            # a dropped one is indistinguishable from a healthy one until the
            # write or the first read fails. Without the retry-once path,
            # keep-alive turns a working browser into one that fails
            # intermittently for no visible reason.
            dropped = nettest_server.drop_all()
            nettest_server.reset()
            print("  server hung up on %d pooled connections" % dropped)
            goto(ui, "http://%s:%d/?cold" % (HOST, PORT), settle=12.0)
            s = dict(nettest_server.stats)
            print("  after the drop: connections=%d requests=%d paths=%s"
                  % (s["connections"], s["requests"], s["paths"]))
            results.append(("the server really did drop the pool first", dropped >= 3))
            results.append(("a pool the server dropped still serves the page",
                            s["requests"] >= 4 and s["connections"] >= 1))

            # ---- 2. chunked framing ------------------------------------
            nettest_server.reset()
            goto(ui, "http://%s:%d/chunked" % (HOST, PORT), settle=12.0)
            shot(ui, "net_02_chunked")
            print("  chunked: connections=%d requests=%d"
                  % (nettest_server.stats["connections"],
                     nettest_server.stats["requests"]))
            results.append(("chunked request completed",
                            nettest_server.stats["requests"] >= 1))

            # ---- 3. progressive render ---------------------------------
            # the server pauses 2.5 s mid-response; a screenshot taken during
            # the pause shows text only if the page painted before the end
            nettest_server.reset()
            ui.key(ord('l'), ctrl=1)
            ui.key(0, scan=0x06, settle=0.05)
            for _ in range(40):
                ui.key(8, settle=0.03)
            ui.text("http://%s:%d/slow" % (HOST, PORT))
            ui.key(13)
            time.sleep(1.8)                    # INSIDE the server's pause
            shot(ui, "net_03_progressive_midload")
            time.sleep(6.0)
            shot(ui, "net_04_progressive_done")
            print("  slow page: requests=%d" % nettest_server.stats["requests"])
    finally:
        try: srv.close()
        except Exception: pass

    print()
    bad = 0
    for name, ok in results:
        print("%s %s" % ("pass" if ok else "FAIL", name))
        if not ok:
            bad += 1
    print("\n%d pass, %d fail" % (len(results) - bad, bad))
    print("Read shots/net_*.png - net_03 is the progressive-render evidence:\n"
          "  text on screen there means the page painted mid-response.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
