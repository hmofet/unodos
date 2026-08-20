#!/usr/bin/env python3
"""One screen carrying a real Word document, a real spreadsheet and Duum.

Why this is not a docs_shots scene: the Open dialog is pointer-only (arrow
keys never reach its list) and QEMU's own tablet events do not reach the
shell, so the only pointer that works here is URC's - which needs the DEBUG
build and a guest that dials home. That is exactly the rig tools/demo uses,
so this borrows it: debug ESP + DEBUG.CFG(nohud) so no perf HUD paints over
the shot, files pushed into the RAM disk (the dialog's default volume, and
the only one it opens on), then clicks at the dialog offsets scenes.py
already measured.

  python3 poster_shot.py [out.png]
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.environ.get("PC64", "/work/unodos-flasher/pc64")
sys.path.insert(0, os.path.join(PC64, "tools"))
sys.path.insert(0, os.path.join(PC64, "tools", "demo"))
os.chdir(PC64)

import remote_qemu as RQ                                  # noqa: E402
from unoauto_remote import UnoAutoLink                    # noqa: E402

URC_PORT = 5433
# The EDID is asked for at DOUBLE the desktop we want: the shell halves it
# (UI scale), so 2560x1600 is how tools/demo gets its 1280x800 frames. Asking
# for 1280x800 here produced a 640x400 desktop and every measured click landed
# somewhere else.
GOP_W, GOP_H = 2560, 1600
DESK_W, DESK_H = GOP_W // 2, GOP_H // 2
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/office_duum.png"

# The Open dialog, straight out of tools/demo/scenes.py (uod_open's own
# centring formula, so it follows the resolution rather than being a literal).
DLG_W, DLG_H = 294, 244
DLG_REL = {"row0": (77, 60), "open": (254, 176), "name": (127, 176)}
ROW_PITCH = 18


def dlg(what, row=0, w=None, h=None):
    w = w or DESK_W
    h = h or DESK_H
    x0 = max(0, (w - DLG_W) // 2)
    y0 = max(0, (h - DLG_H) // 3)
    dx, dy = DLG_REL[what]
    return (x0 + dx, y0 + dy + row * ROW_PITCH)


def build_disk():
    RQ.build_disk()
    cfg = "/tmp/poster.cfg"
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote=10.0.2.2:%d\nnonet\nnostress\nnoshutdown\nnohud\n" % URC_PORT)
    subprocess.run(["mcopy", "-i", RQ.FAT, "-o", cfg, "::/DEBUG.CFG"], check=True)
    with open(RQ.FAT, "rb") as pf, open(RQ.DISK, "r+b") as df:
        df.seek(2048 * 512)
        while True:
            b = pf.read(1 << 20)
            if not b:
                break
            df.write(b)


def boot_qemu():
    subprocess.run(["cp", RQ.OVMF_VARS, RQ.VARS], check=True)
    kvm = ["-cpu", "host", "-enable-kvm", "-smp", "4"] if os.path.exists("/dev/kvm") \
          else ["-cpu", "max"]
    return subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "3072",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + RQ.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + RQ.VARS,
        "-drive", "format=raw,file=" + RQ.DISK,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-vga", "none",
        "-device", "VGA,edid=on,xres=%d,yres=%d,vgamem_mb=64" % (GOP_W, GOP_H),
        "-display", "none",
    ] + kvm, stderr=subprocess.DEVNULL)


class Rig(object):
    def __init__(self, link):
        self.link = link
        self.px = self.py = 0

    def move(self, x, y, btn=0, settle=0.12):
        x, y = int(x), int(y)
        self.link.pointer(x, y, btn, timeout=8)
        self.px, self.py = x, y
        time.sleep(settle)

    def sweep(self, x1, y1, step=24, pace=0.035):
        """Walk the pointer there rather than teleporting it. The shell tracks
        MOTION; a single jump to a new position can be read as a click at the
        old one, which is how the dialog rows kept missing."""
        x0, y0 = self.px, self.py
        dx, dy = int(x1) - x0, int(y1) - y0
        n = max(1, int(max(abs(dx), abs(dy)) / step))
        for i in range(1, n + 1):
            self.move(x0 + dx * i // n, y0 + dy * i // n, settle=pace)
        self.move(x1, y1, settle=0.15)

    def click(self, x, y, settle=0.7):
        """Move, press, release as THREE injections: the shell samples pointer
        state once per frame, so a press+release inside one sample cancels."""
        self.sweep(x, y)
        self.move(x, y, 0, settle=0.15)
        self.move(x, y, 1, settle=0.20)
        self.move(x, y, 0, settle=settle)

    def launch(self, app_id, settle=3.0):
        self.link.command("launch", app_id, timeout=20)
        time.sleep(settle)

    def ctrl(self, ch, settle=0.4):
        """URC's key verb is (scan, uni, ctrl) - there is no Alt modifier at
        all, which is why the windows below are arranged with the shell's own
        Tile command through the pointer rather than with Alt+arrow."""
        self.link.key(0, ord(ch), 1, timeout=8)
        time.sleep(settle)

    def open_doc(self, row):
        self.ctrl("o"); time.sleep(1.6)
        self.click(*dlg("row0", row))
        self.click(*dlg("open"), settle=3.0)

    def tile(self):
        self.click(45, 771, settle=1.4)        # the Start button
        self.click(385, 261, settle=3.0)       # Windows > Tile


def main():
    if not os.path.isdir(RQ.ESP):
        raise SystemExit("no build/esp - stage the DEBUG esp there first")
    link = UnoAutoLink("127.0.0.1", URC_PORT)
    link.listen()
    build_disk()
    q = boot_qemu()
    try:
        if not link.wait_connected(240):
            raise SystemExit("guest never dialled in - is build/esp the DEBUG build?")
        link.wait_hello(30.0)
        time.sleep(3.0)
        w, h = link.screen_info(timeout=15)
        print("desktop %dx%d" % (w, h))
        if (w, h) != (DESK_W, DESK_H):
            raise SystemExit(
                "desktop is %dx%d, not the %dx%d every click below is measured "
                "for - stop rather than shoot a picture of missed clicks"
                % (w, h, DESK_W, DESK_H))
        r = Rig(link)

        # The dialog opens on the RAM disk and cannot be pointed elsewhere by
        # keyboard, so the documents go where it is already looking. README.TXT
        # is seeded by the OS at row 0, so these land on rows 1 and 2 in push
        # order - the same arrangement tools/demo/scenes.py relies on.
        for name in ("RESUME.DOC", "BUDGET.XLS"):
            src = os.path.join(PC64, "tools", "demo", "assets", name.lower())
            link.push_file(0, name, src)
            print("pushed", name)

        r.launch("uoword"); r.open_doc(1)          # RESUME.DOC
        r.launch("uocalc"); r.open_doc(2)          # BUDGET.XLS
        r.launch("duum", settle=8.0)
        print("duum: waiting for the first frame...")
        time.sleep(60.0)                           # PYRT + WAD parse + render
        r.tile()                                   # all three, side by side
        time.sleep(2.0)
        link.pointer(1180, 700, 0, timeout=8)      # park the cursor off the art
        time.sleep(1.0)

        from stream_recv import write_png
        w, h, rgba = link.screen_grab(1, timeout=120)
        write_png(OUT, w, h, rgba)
        print("wrote", OUT, w, h)
    finally:
        try:
            link.command("power", "off", timeout=5)
        except Exception:
            pass
        time.sleep(2)
        q.kill()


if __name__ == "__main__":
    main()
