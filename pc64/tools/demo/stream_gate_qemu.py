#!/usr/bin/env python3
"""End-to-end QEMU gate for unostream (pc64/UNOSTREAM.md).

Boots the DEBUG image exactly like tools/remote_qemu.py (guest dials the URC
listener out over SLIRP), starts a StreamReceiver on a SECOND host port, then:

  1. `stream start 10.0.2.2 <port> 30`   - the guest dials the receiver
  2. drives ~10 s of visible activity over URC (Start-corner click, launch an
     app, a sweep of small `pointer` moves ending at a KNOWN position)
  3. `stream stop`

and asserts:
  - the mp4 exists and carries > 100 frames
  - frames arrived at >= 15 fps average (receiver wall clock)
  - the timing log shows DELTAS (not all keyframes)
  - the receiver's decode-error counter is 0 (deltas applied cleanly)
  - the cursor glyph is visible on the final canvas at the commanded pointer
    position (the saved out.png snapshot is checked pixel-for-pixel against
    the kCursor arrow: black outline + white fill)

Run under WSL (qemu-system-x86_64, OVMF, mtools, ffmpeg) after
`UNO_DEBUG=1 ./build.sh`. Exit 0 iff every check passes.
"""
import json, os, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)

import remote_qemu as RQ                     # noqa: E402
from unoauto_remote import UnoAutoLink       # noqa: E402
from stream_recv import StreamReceiver       # noqa: E402

SPORT = 5398                                 # the stream's own port (URC uses RQ.PORT)
OUTDIR = "/tmp/unostream_gate"
OUT = os.path.join(OUTDIR, "out.mp4")


def main():
    esp = RQ.ESP
    if not os.path.isdir(esp) or not os.path.exists(os.path.join(esp, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1
    os.makedirs(OUTDIR, exist_ok=True)
    for f in os.listdir(OUTDIR):
        try:
            os.unlink(os.path.join(OUTDIR, f))
        except OSError:
            pass

    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and cond

    link = UnoAutoLink("127.0.0.1", RQ.PORT)
    link.listen()
    RQ.build_disk()
    q = RQ.boot_qemu()

    rx = StreamReceiver(SPORT, out=OUT, host="127.0.0.1")
    rx.listen()
    rx_thread = threading.Thread(target=rx.serve_once, kwargs={"accept_timeout": 120.0},
                                 daemon=True)
    rx_thread.start()

    try:
        if not link.wait_connected(180):
            check(False, "pc64 dialed in (URC)")
            return 1
        check(True, "pc64 dialed in (URC)")
        link.wait_hello(30.0)
        time.sleep(2.0)                                  # let the desktop settle

        w, h = link.screen_info(timeout=15)
        check(w > 0 and h > 0, "screen info", "%dx%d" % (w, h))

        # 1) start the stream (guest -> 10.0.2.2:SPORT over SLIRP, 30 fps)
        r = link.command("stream", "start", "10.0.2.2", SPORT, 30, timeout=10)
        check(bool(r) and r[0].startswith("dialing"), "stream start accepted", "%r" % r)

        for _ in range(100):                             # wait for the dial-in
            if rx.connected:
                break
            time.sleep(0.15)
        check(rx.connected, "guest connected to the receiver")
        if not rx.connected:
            return 1

        # 2) ~10 s of visible activity. Every injection changes pixels the
        #    deltas must carry: the Start-corner click pops shell UI, `launch`
        #    opens a real window, and the pointer sweep paints cursor motion.
        link.pointer(20, h - 10, 0, timeout=8)           # toward the Start corner
        time.sleep(0.15)
        link.pointer(20, h - 10, 1, timeout=8)
        time.sleep(0.2)
        link.pointer(20, h - 10, 0, timeout=8)
        time.sleep(0.8)
        link.launch(0, timeout=15)                       # open an app window
        time.sleep(2.0)
        for i in range(24):                              # pointer sweep
            x = 40 + i * max(1, (w - 80) // 24)
            y = 60 + (i * 9) % max(1, h // 2)
            link.pointer(x, y, 0, timeout=8)
            time.sleep(0.25)
        # park the cursor at a KNOWN spot for the snapshot check, on the app
        # window's canvas (mid-screen), and give the stream time to carry it
        cx, cy = w // 2, h // 3
        link.pointer(cx, cy, 0, timeout=8)
        time.sleep(1.5)

        # 3) stop + status
        st = link.command("stream", "status", timeout=8)
        r = link.command("stream", "stop", timeout=8)
        check(bool(st) and st[0].startswith("on=1"), "stream status while live", "%r" % st)
        check(bool(r) and r[0].startswith("on=0"), "stream stop", "%r" % r)

        rx_thread.join(30.0)
        check(not rx_thread.is_alive(), "receiver finished (EOF + trailers)")
        check(rx.error is None, "receiver saw no protocol error", "%r" % rx.error)

        # ---- assertions over the recording -----------------------------------
        check(os.path.exists(OUT) and os.path.getsize(OUT) > 0, "mp4 exists",
              "%s (%d bytes)" % (OUT, os.path.getsize(OUT) if os.path.exists(OUT) else 0))
        check(rx.frames > 100, "> 100 frames received", "%d" % rx.frames)
        if rx.t_first and rx.t_last and rx.t_last > rx.t_first:
            fps = (rx.frames - 1) / (rx.t_last - rx.t_first)
        else:
            fps = 0.0
        check(fps >= 15.0, ">= 15 fps average arrival", "%.1f fps" % fps)
        check(rx.deltas > 0 and rx.deltas > rx.keyframes,
              "timing log shows deltas (not all keyframes)",
              "%d delta / %d key" % (rx.deltas, rx.keyframes))
        check(rx.decode_errors == 0, "0 receiver decode errors", "%d" % rx.decode_errors)

        # cursor visible at the commanded position: kCursor row 4 is "BWWWB",
        # so (cx,cy+4) is black outline, (cx+1..3,cy+4) white fill, (cx+4,cy+4)
        # black outline again; (cx,cy) is the black hotspot. Alpha is opaque.
        def is_black(p):
            return p[0] < 40 and p[1] < 40 and p[2] < 40

        def is_white(p):
            return p[0] > 215 and p[1] > 215 and p[2] > 215

        if rx.canvas is not None and cx + 4 < rx.w and cy + 4 < rx.h:
            tip = rx.px(cx, cy)
            row4 = [rx.px(cx + i, cy + 4) for i in range(5)]
            cur_ok = (is_black(tip) and is_black(row4[0]) and
                      all(is_white(p) for p in row4[1:4]) and is_black(row4[4]))
            check(cur_ok, "cursor glyph visible at commanded position",
                  "tip=%r row4=%r png=%s" % (tip, row4,
                                             os.path.splitext(OUT)[0] + ".png"))
        else:
            check(False, "cursor glyph visible at commanded position",
                  "no canvas / out of bounds")

        stats = os.path.splitext(OUT)[0] + ".stats.json"
        if os.path.exists(stats):
            print("stats: " + json.dumps(json.load(open(stats))))

    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:      # noqa: BLE001
            pass
        time.sleep(0.5)
        q.kill()
        link.close()

    print("\n" + (">> unostream gate OK" if ok else ">> unostream gate FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
