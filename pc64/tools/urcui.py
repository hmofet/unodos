#!/usr/bin/env python3
"""urcui - drive the pc64 UI over URC: real clicks, real keys, real screenshots.

WHY THIS EXISTS.  QEMU's usb-tablet delivers NO POINTER MOTION to this guest.
Every harness that tried (the window manager's, UnoAmp's wheel work, and
UnoWord's) discovered the same thing the same way: mouse-down arrives at the
framebuffer centre whatever coordinate QMP was told, so a click can never
reach a menu and no mouse gesture is testable.  Each lane rediscovered that
and stopped.

URC has injected pointer and key events all along (`pointer`, `key`, and the
`screen grab` half of remote desktop), so the capability was never missing -
only the two hundred lines that put boot, connect, click and screenshot in
one place.  That is this file.  Any lane wanting to prove a mouse gesture on
pc64 should import it rather than write a fourth tablet harness.

    from urcui import UrcUi
    with UrcUi() as ui:                 # boots the DEBUG image, links up
        ui.launch(ui.app_count() - 1)   # open the last app
        ui.click(54, 66)                # OS framebuffer coords, not host
        ui.shot("menu_open")            # -> shots/<tag>.png

COORDINATES ARE THE OS FRAMEBUFFER'S (typically 640x400), not the host
window's.  `ui.size()` reports them.

THE ONE THING THAT IS NOT OBVIOUS, and it cost UnoAmp a day: the shell
SAMPLES pointer state each frame and rebuilds the button mask from hardware,
so an injected press that lives less than one sample is invisible however
correct it is.  uefi_main.c latches and QUEUES injected states for exactly
this reason.  This helper therefore sends move / press / release as three
separate injections with a gap between them, and never merges a click.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

import remote_qemu as RQ
from unoauto_remote import UnoAutoLink

SHOTS = os.path.join(os.path.dirname(HERE), "shots")


def _ppm(path, w, h, rgba):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        row = bytearray()
        for i in range(0, len(rgba), 4):
            row += rgba[i:i + 3]
        f.write(bytes(row))


class UrcUi(object):
    def __init__(self, boot_wait=None, port=None):
        self.qemu = None
        self.link = None
        self.boot_wait = boot_wait
        self.port = port or RQ.PORT
        self._w = self._h = 0

    # ---- lifecycle ---------------------------------------------------------
    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *a):
        self.stop()
        return False

    def start(self):
        esp = RQ.ESP
        if not os.path.isdir(esp):
            raise SystemExit("no build/esp - run ./build.sh first")
        RQ.build_disk()
        self.link = UnoAutoLink(port=self.port)
        self.link.listen()
        self.qemu = RQ.boot_qemu()
        if not self.link.wait_connected(self.boot_wait or 180.0):
            raise SystemExit("the guest never dialled in - is this the DEBUG build?")
        self.link.wait_hello(30.0)
        time.sleep(2.0)                    # let the desktop settle
        try:
            self._w, self._h, _ = self.link.screen_info(timeout=15)
        except Exception:
            self._w, self._h = 640, 400
        return self

    def stop(self):
        try:
            if self.link:
                self.link.close()
        except Exception:
            pass
        if self.qemu:
            self.qemu.terminate()
            try:
                self.qemu.wait(timeout=10)
            except Exception:
                self.qemu.kill()

    def size(self):
        return self._w, self._h

    # ---- input -------------------------------------------------------------
    def move(self, x, y, settle=0.12):
        self.link.pointer(int(x), int(y), 0, timeout=8)
        time.sleep(settle)

    def click(self, x, y, settle=0.45):
        """Move, press, release - as THREE injections.

        Not one: the shell samples pointer state per frame, so a press and a
        release inside a single sample cancel out.  uefi_main.c queues
        injected states one per poll precisely so this sequence survives."""
        self.link.pointer(int(x), int(y), 0, timeout=8)
        time.sleep(0.12)
        self.link.pointer(int(x), int(y), 1, timeout=8)
        time.sleep(0.18)
        self.link.pointer(int(x), int(y), 0, timeout=8)
        time.sleep(settle)

    def dblclick(self, x, y):
        self.click(x, y, settle=0.12)
        self.click(x, y, settle=0.5)

    def key(self, uni, scan=0, ctrl=0, settle=0.15):
        self.link.key(int(scan), int(uni), int(ctrl), timeout=8)
        time.sleep(settle)

    def text(self, s, settle=0.05):
        for ch in s:
            self.key(ord(ch), settle=settle)

    # ---- the shell ---------------------------------------------------------
    def app_count(self):
        r = self.link.command("apps", timeout=10)
        for line in r:
            for tokn in line.split():
                if tokn.isdigit():
                    return int(tokn)
        return 0

    def launch(self, index, settle=4.0):
        self.link.command("launch", index, timeout=15)
        time.sleep(settle)

    def apps(self):
        """[(id, name)] for every app slot, in this boot's order."""
        out = []
        for line in self.link.command("apps", "list", timeout=15):
            s = line.strip()
            if not s or s.startswith("end"):
                continue
            if s.startswith("ok "):
                s = s[3:].strip()
            id_, _, name = s.partition(" ")
            if id_:
                out.append((id_, name.strip()))
        return out

    def launch_id(self, app_id, settle=4.0):
        """Open the app with this ID - one command, no searching, no drift.

        Prefer this to launch(index) and to launch_named(): an id is the app's
        own stable name, while an index is this boot's ordering of whatever is
        installed. The registry can add a row between two boots (someone
        installs an app), and a test that counted rows would then drive the
        wrong one without failing."""
        self.link.command("launch", app_id, timeout=15)
        time.sleep(settle)

    def rescan(self):
        """Re-read APPS\\ and return the new app count (no reboot)."""
        r = self.link.command("rescan", timeout=20)
        for line in r:
            for tokn in line.split():
                if tokn.isdigit():
                    return int(tokn)
        return 0

    def windows(self):
        """Titles of the windows currently open (PROBE kind 1)."""
        return [r["name"] for r in self.link.probe(timeout=10) if r["kind"] == 1]

    def launch_named(self, name, settle=4.0, tries=6):
        """Open the app whose window is titled `name`, and PROVE it opened.

        Slot indices shift the moment anyone adds an app, and a test that
        launches app_count()-1 does not fail when that happens - it silently
        drives the new app instead.  uoword_urc.py spent a run doing exactly
        that to UnoCalc.  So search from the end, check the window that
        actually appeared, and close anything else we opened on the way."""
        n = self.app_count()
        for i in range(n - 1, max(-1, n - 1 - tries), -1):
            self.link.command("launch", i, timeout=15)
            time.sleep(settle)
            for t in self.windows():
                if t.startswith(name):
                    return i
            self.link.command("close", timeout=10)
            time.sleep(0.6)
        raise SystemExit("no app slot opens a window titled %r "
                         "(searched the last %d of %d)" % (name, tries, n))

    # ---- output ------------------------------------------------------------
    def shot(self, tag, scale=1):
        os.makedirs(SHOTS, exist_ok=True)
        w, h, rgba = self.link.screen_grab(scale, timeout=40)
        ppm = os.path.join(SHOTS, tag + ".ppm")
        png = os.path.join(SHOTS, tag + ".png")
        _ppm(ppm, w, h, rgba)
        conv = os.path.join(os.path.dirname(HERE), "..", "ps2", "tools", "ppm2png.py")
        conv = os.path.normpath(conv)
        if os.path.exists(conv):
            subprocess.run([sys.executable, conv, ppm, png],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                os.remove(ppm)
            except OSError:
                pass
            print("shot:", png)
            return png
        print("shot:", ppm)
        return ppm
