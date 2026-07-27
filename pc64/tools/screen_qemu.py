#!/usr/bin/env python3
"""End-to-end gate for the URC `screen` verb (remote desktop, OUT half), in QEMU.

Boots the DEBUG image the same way remote_qemu.py does (it reuses that harness's
disk build + boot), then proves the screen round trip:
  1. pc64 dials in
  2. host -> pc64 `screen info` returns a sane desktop size
  3. host -> pc64 `screen grab` returns a QOI frame that decodes to exactly
     w*h*4 RGBA bytes and is non-blank (more than one distinct pixel)

It also drops the decoded frame to /tmp/urc_screen.ppm so a human can eyeball it.

Requires a debug build first:  UNO_DEBUG=1 ./build.sh
Run under WSL (needs qemu-system-x86_64, sgdisk, mformat/mcopy, OVMF), like
remote_qemu.py.  Exit 0 iff all three checks pass.
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq              # reuse build_disk / boot_qemu / globals
from unoauto_remote import UnoAutoLink, qoi_decode


def save_ppm(path, w, h, rgba):
    with open(path, "wb") as f:
        f.write(("P6\n%d %d\n255\n" % (w, h)).encode())
        # RGBA -> RGB
        f.write(bytes(rgba[i] for i in range(len(rgba)) if i % 4 != 3))


def main():
    esp = rq.ESP
    if not os.path.isdir(esp) or not os.path.exists(os.path.join(esp, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1

    link = UnoAutoLink("127.0.0.1", rq.PORT)
    link.listen()
    rq.build_disk()
    with open(rq.DISK2, "wb") as f:
        f.truncate(128 * rq.MIB)       # boot_qemu attaches a second (blank) disk
    q = rq.boot_qemu()
    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and bool(cond)

    try:
        if not link.wait_connected(90):
            print("FAIL: pc64 never dialed in")
            return 1
        check(True, "pc64 dialed in")

        w = h = 0
        try:
            w, h = link.screen_info(timeout=10)
            check(w > 0 and h > 0, "screen info", "%dx%d" % (w, h))
        except Exception as e:  # noqa: BLE001
            check(False, "screen info", str(e))

        try:
            W, H, rgba = link.screen_grab(1, timeout=25)
            check(W == w and H == h, "grab dims match info", "%dx%d" % (W, H))
            check(len(rgba) == W * H * 4, "decoded RGBA size", "%d bytes" % len(rgba))
            distinct = len(set(bytes(rgba[i:i + 4]) for i in range(0, len(rgba), 4)))
            check(distinct > 1, "frame is non-blank", "%d distinct colours" % distinct)
            if W and H:
                save_ppm("/tmp/urc_screen.ppm", W, H, rgba)
                print("     wrote /tmp/urc_screen.ppm (%dx%d)" % (W, H))
        except Exception as e:  # noqa: BLE001
            check(False, "screen grab + decode", str(e))

        # ---- delta / dirty-rect streaming ----
        try:
            time.sleep(1.0)                       # let the desktop settle
            W1, H1, base = link.screen_grab(1, timeout=25)   # seed the snapshot + canvas
            canvas = bytearray(base)
            u = link.screen_grab_delta(1, timeout=25)        # a delta vs that snapshot
            check(not u["keyframe"], "grab delta returns a delta (snapshot exists)",
                  "nch=%d" % u.get("nch", -1))
            rows = (H1 + u["th"] - 1) // u["th"]
            check(u["cols"] > 0 and u["th"] > 0, "delta grid sane",
                  "cols=%d tw=%d th=%d" % (u["cols"], u["tw"], u["th"]))
            check(all(0 <= t < u["cols"] * rows for t in u["idx"]),
                  "delta tile indices in range")
            check(u["nch"] <= (u["cols"] * rows) // 2 + 1,
                  "delta suppresses most tiles (dirty detection)",
                  "%d of %d tiles" % (u["nch"], u["cols"] * rows))

            # Composite exactly this delta onto the seeded canvas (client<->device
            # lockstep), then compare to a fresh full grab. A placement bug would
            # corrupt large regions; a live clock/cursor drifts only a few pixels.
            if u["nch"] > 0:
                strip = qoi_decode(u["qoi"])      # tw x (nch*th) RGBA
                tw, th, cols = u["tw"], u["th"], u["cols"]
                for i, t in enumerate(u["idx"]):
                    col, row = t % cols, t // cols
                    dx, dy = col * tw, row * th
                    vw, vh = min(tw, W1 - dx), min(th, H1 - dy)
                    for yy in range(max(0, vh)):
                        so = ((i * th + yy) * tw) * 4
                        do = ((dy + yy) * W1 + dx) * 4
                        canvas[do:do + vw * 4] = strip[so:so + vw * 4]
            W2, H2, full2 = link.screen_grab(1, timeout=25)
            check((W2, H2) == (W1, H1), "dims stable across grabs", "%dx%d" % (W2, H2))
            if (W2, H2) == (W1, H1):
                diff = sum(1 for i in range(0, len(canvas), 4)
                           if canvas[i:i + 4] != full2[i:i + 4])
                frac = diff / max(1, W1 * H1)
                check(frac < 0.02, "delta reconstruct matches a full grab",
                      "%.3f%% px differ" % (frac * 100))

            uk = link.screen_grab_delta(2, timeout=25)        # new scale -> keyframe
            check(uk["keyframe"], "a scale change forces a keyframe")
            check(len(uk["qoi"]) > 0, "keyframe carries a full-frame QOI",
                  "%d bytes" % len(uk["qoi"]))
        except Exception as e:  # noqa: BLE001
            check(False, "delta streaming", str(e))
    finally:
        link.close()
        try:
            q.terminate(); q.wait(timeout=10)
        except Exception:  # noqa: BLE001
            try: q.kill()
            except Exception: pass

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
