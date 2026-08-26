#!/usr/bin/env python3
"""Screenshot the Control Panel's Network pane under QEMU, with no Wi-Fi card.

The Wi-Fi pane only exists when iwl_present(), so on QEMU - where every gate,
every screenshot and every layout audit in this repo runs - it had never been
drawn at all, and the only way to look at a change to it was to walk a stick
over to a laptop. DEBUG.CFG `wifi-demo` forces the pane on and seeds example
rows (pc64_uui.c, cp_wifi_demo); this boots a debug build with that flag, opens
the Control Panel on the Network tab and pulls the framebuffer.

    UNO_DEBUG=1 ./build.sh
    python3 tools/wifiui_shot.py out.ppm            # the pane
    python3 tools/wifiui_shot.py out.ppm details    # with the addresses open
    python3 tools/wifiui_shot.py out.ppm tip        # the tray chip's tooltip

The rows are NOT real networks and the pane says so on screen. A machine that
HAS a card ignores the flag's seeding entirely - it scans.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq
from unoauto_remote import UnoAutoLink

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/wifiui.ppm"

rq.build_disk()
# rewrite the harness config with our flags, and splice it back into the image
cfg = os.path.join(os.path.dirname(rq.DISK), "remote_stress.cfg")
with open(cfg, "w", newline="\r\n") as f:
    f.write("remote=10.0.2.2:%d\nnonet\nnohud\nwifi-demo\n" % rq.PORT)
subprocess.run(["mcopy", "-i", rq.FAT, "-o", cfg, "::/DEBUG.CFG"], check=True)
with open(rq.FAT, "rb") as pf, open(rq.DISK, "r+b") as df:
    df.seek(2048 * rq.SECTOR)
    while True:
        b = pf.read(rq.MIB)
        if not b:
            break
        df.write(b)

link = UnoAutoLink(port=rq.PORT)
link.listen()
qemu = rq.boot_qemu()
try:
    if not link.wait_connected(90):
        print("FAIL: the guest never dialled in")
        sys.exit(1)
    link.wait_hello(30)
    print("linked:", link.probe())
    link.launch(0)                       # APP_CTRL - opens on the Network tab
    time.sleep(2.5)
    info = link.screen_info()
    print("screen:", info)
    def shot(path):
        w, h, rgba = link.screen_grab(scale=1)
        with open(path, "wb") as f:
            f.write((("P6" + chr(10) + "%d %d" + chr(10) + "255" + chr(10)) % (w, h)).encode())
            for i in range(0, len(rgba), 4):
                f.write(rgba[i:i+3])
        print("wrote", path, w, "x", h)

    def click(cx, cy):
        link.pointer(cx, cy, 0, timeout=10); time.sleep(0.3)
        link.pointer(cx, cy, 1, timeout=10); time.sleep(0.3)
        link.pointer(cx, cy, 0, timeout=10); time.sleep(0.8)

    if len(sys.argv) > 2 and sys.argv[2] == "details":
        click(195, 337)                     # the Details disclosure
        shot(OUT)
    elif len(sys.argv) > 2 and sys.argv[2] == "tip":
        link.pointer(525, 386, 0, timeout=10)   # hover the tray network chip
        time.sleep(1.2)
        shot(OUT)
    else:
        shot(OUT)
    sys.exit(0)
finally:
    try:
        link.poweroff()
    except Exception:
        pass
    time.sleep(2)
    qemu.terminate()
