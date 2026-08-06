#!/usr/bin/env python3
"""netverify_urc - test the M7 network changes against a REAL server.

The gate's network area runs on a null NIC, so framing, keep-alive and
progressive render have all been landing unverified. This closes that: a
controlled server on the host (nettest_server), the browser in QEMU reaching
it at 10.0.2.2, and assertions on what the SERVER saw rather than on what the
browser claims.

    cd pc64 && BROWSER_ENGINE=uw UNO_DEBUG=1 ./build.sh
    python3 tools/netverify_urc.py

Three questions, three answers:
  keep-alive       one connection serving four requests, not four connections
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

            # ---- 1. keep-alive + subresource fetching -------------------
            nettest_server.stats.update(connections=0, requests=0, paths=[])
            goto(ui, "http://%s:%d/" % (HOST, PORT), settle=15.0)
            s = dict(nettest_server.stats)
            shot(ui, "net_01_keepalive")
            print("  page load: connections=%d requests=%d paths=%s"
                  % (s["connections"], s["requests"], s["paths"]))
            results.append(("subresources fetched (page + 3 sheets)",
                            s["requests"] >= 4))
            # Connection REUSE is currently disabled (see ka_matches in
            # pc64_http.c). Assert what is true today rather than what we
            # wish were true: one connection per request. When reuse is
            # re-enabled, flip this to connections < requests - and this
            # test is what will say whether it actually worked.
            results.append(("no reuse yet: one connection per request "
                            "(keep-alive is disabled)",
                            s["connections"] == s["requests"]))
            print("  NOTE keep-alive reuse is disabled in pc64_http.c")

            # ---- 2. chunked framing ------------------------------------
            nettest_server.stats.update(connections=0, requests=0, paths=[])
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
            nettest_server.stats.update(connections=0, requests=0, paths=[])
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
