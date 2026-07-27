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
from unoauto_remote import UnoAutoLink


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
