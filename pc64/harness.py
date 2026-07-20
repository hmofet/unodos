#!/usr/bin/env python3
"""UnoDOS/pc64 scripted verification harness - QEMU + OVMF, fully headless.

The pc64 counterpart of the family's harness.py pattern - but where the
console ports need a ROM-free CPU-core harness (no headless grab under RDP),
QEMU is natively scriptable: boot the ESP under OVMF with no display, drive
the desktop over QMP (send-key), and capture the GOP surface (screendump)
into shots/*.png at each step.

  python3 harness.py            boot -> desktop shot, then the scripted
                                keyboard pass: SysInfo, Clock, Notepad
                                (typing), Dostris (playing) - a shot each.
  python3 harness.py boot       boot -> desktop shot only.
"""
import json, os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
QMP_SOCK  = "/tmp/unodos-pc64-qmp.sock"   # NOT under build/: a Windows-mounted
                                          # drvfs tree cannot host unix sockets


class Qmp:
    def __init__(self, path, timeout=30):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.3)
        self.buf = b""
        self.recv()                          # greeting
        self.cmd("qmp_capabilities")

    def recv(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.s.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            r = self.recv()
            if "return" in r or "error" in r:
                return r


def keys(q, *names, hold=40, gap=0.12):
    for n in names:
        q.cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": hold})
        time.sleep(gap)


def text(q, s):
    QMAP = {" ": "spc", ".": "dot", ",": "comma", "-": "minus", "/": "slash"}
    for ch in s:
        if ch.isupper():
            q.cmd("send-key", keys=[{"type": "qcode", "data": "shift"},
                                    {"type": "qcode", "data": ch.lower()}])
        else:
            q.cmd("send-key", keys=[{"type": "qcode", "data": QMAP.get(ch, ch)}])
        time.sleep(0.1)


def mouse_move(q, x, y):
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 640)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 480)}}])
    time.sleep(0.1)


def mouse_btn(q, down):
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": down, "button": "left"}}])
    time.sleep(0.1)


def click(q, x, y):
    mouse_move(q, x, y)
    mouse_btn(q, True)
    mouse_btn(q, False)


def shot(q, tag):
    ppm = "shots/%s.ppm" % tag
    q.cmd("screendump", filename=ppm)
    time.sleep(0.4)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag],
                   check=True)
    os.remove(ppm)
    print("shot: shots/%s.png" % tag)


def main():
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    # Storage: a vvfat view of build/esp by default (no image build needed).
    # vvfat's read-write mode corrupts multi-cluster WRITES, though, so tests
    # that write on-device (Studio build-and-run of a larger app) set UNO_DISK
    # to a real FAT image built by tools/mkuefi.py, where writes are reliable.
    disk = os.environ.get("UNO_DISK")
    disk_arg = ("format=raw,file=" + disk) if disk else "format=vvfat,file=fat:rw:build/esp"
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", disk_arg,
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "none",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/ovmf.log", "-global", "isa-debugcon.iobase=0x402",
    ]
    if len(sys.argv) > 1 and sys.argv[1] == "acpi":
        # unoacpi verification: inject the synthetic battery/lid SSDT (50%).
        # Build it first if needed:  iasl tools/testbat.asl
        argv += ["-acpitable", "file=tools/testbat.aml"]
    if len(sys.argv) > 1 and sys.argv[1] == "lidsleep":
        # lid-close sleep: inject the TOGGLING-lid SSDT (open ~5s / closed ~5s).
        # Build it first if needed:  iasl tools/testlid.asl
        argv += ["-acpitable", "file=tools/testlid.aml"]
    qemu = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting for OVMF -> UnoDOS boot...")
        time.sleep(18)                        # OVMF + splash + first desktop paint
        shot(q, "m1_desktop")

        if len(sys.argv) > 1 and sys.argv[1] == "boot":
            return

        # ---- Duum: the Python Doom engine.  Studio greets the Duum source
        # (staged as SAMPLE.PY by the runner), Ctrl-B packs it, Ctrl-R runs it;
        # it streams DOOM1.WAD and renders E1M1.  Generous waits: the first BSP
        # render is heavy in the interpreter.  Move forward, shoot a second
        # frame to prove the view changes. ------------------------------------
        if len(sys.argv) > 1 and sys.argv[1] == "duum":
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "esc"}])
            time.sleep(0.8)
            keys(q, *(["down"] * 15)); keys(q, "ret")   # Studio
            time.sleep(2.5)
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "b"}])
            time.sleep(2.5)
            shot(q, "duum_build")
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "r"}])
            time.sleep(28)                              # first render (+texture compose)
            shot(q, "duum_a")
            for _ in range(4):                          # walk forward
                keys(q, "up"); time.sleep(5)
            shot(q, "duum_b")
            keys(q, "left"); time.sleep(6)             # turn
            shot(q, "duum_c")
            return

        # ---- Python: Studio greets SDK\SAMPLE.PY; Ctrl-B packs it into a
        # UNO_MODF_PYAPP container, Ctrl-R hands it to PYRT.UNO which compiles
        # and runs it.  Two shots a beat apart prove the ball animates. -------
        if len(sys.argv) > 1 and sys.argv[1] == "py":
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "esc"}])
            time.sleep(0.8)
            keys(q, *(["down"] * 15)); keys(q, "ret")   # last app = Studio
            time.sleep(2.5)
            shot(q, "py_studio")                         # editor with SAMPLE.PY
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "b"}])
            time.sleep(2.0)
            shot(q, "py_build")                          # "Packed SAMPLE.UNO"
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "r"}])
            time.sleep(2.0)
            shot(q, "py_run_a")                          # the Python app running
            time.sleep(1.3)
            shot(q, "py_run_b")                          # a later frame (ball moved)
            return

        # ---- lid-close sleep: the toggling-lid SSDT closes the lid ~5s after
        # boot (screen blanks = asleep) and reopens ~5s later (desktop wakes).
        # Sample across a full cycle. --------------------------------------
        if len(sys.argv) > 1 and sys.argv[1] == "lidsleep":
            for i in range(12):
                time.sleep(2)
                shot(q, "lid_%02d" % i)
            return

        # ---- unoacpi: synthetic battery boot (harness.py acpi) -------------
        # Open System via the Start menu, keyboard-only: Ctrl-Esc opens the
        # launcher, Down x3 = System (menu order = app order), Enter activates.
        if len(sys.argv) > 1 and sys.argv[1] == "acpi":
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "esc"}])
            time.sleep(0.8)
            keys(q, "down", "down", "down"); keys(q, "ret")
            time.sleep(1.5)
            shot(q, "acpi_system")             # "ACPI AML: up ... bat 50% lid open"
            return

        # ---- M2 (decoupling): every bridge app is a .UNO module loaded from
        # storage; open each through the launcher (menu order = app order,
        # bridge apps at indices 7..13) and screenshot it running. ----------
        if len(sys.argv) > 1 and sys.argv[1] == "unoapps":
            bridge = ["dostris", "pacman", "outlast", "music",
                      "tracker", "paint", "network"]
            for i, name in enumerate(bridge):
                q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                        {"type": "qcode", "data": "esc"}])
                time.sleep(0.8)
                keys(q, *(["down"] * (7 + i)))
                keys(q, "ret")
                time.sleep(2.0)
                shot(q, "uno_" + name)
                q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                        {"type": "qcode", "data": "w"}])
                time.sleep(0.8)
            return

        # ---- Studio: the IDE loads from APPS\STUDIO.UNO, edits + builds +
        # runs a UnoC app on-device.  Launcher order = app order; with the IDE
        # shipped, Studio is the last listed app (after Runner + Browser). ----
        if len(sys.argv) > 1 and sys.argv[1] == "ide":
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "esc"}])
            time.sleep(0.8)
            keys(q, *(["down"] * 15))          # 0..14 apps, 15 = Studio
            keys(q, "ret")
            time.sleep(2.5)
            shot(q, "ide_open")                # editor with SAMPLE.C loaded
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "b"}])
            time.sleep(2.5)
            shot(q, "ide_build")               # SAMPLE.UNO written
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "r"}])
            time.sleep(2.5)
            shot(q, "ide_run")                 # the built app runs in its window
            return

        # ---- M4: mouse (usb-tablet -> EFI Absolute Pointer) ----------------
        if len(sys.argv) > 1 and sys.argv[1] == "mouse":
            click(q, 232, 50)                  # select the Files icon
            time.sleep(0.5)
            click(q, 232, 50)                  # second click = double: launch
            time.sleep(1.5)
            shot(q, "m4_mouse_open")
            mouse_move(q, 180, 48)             # grab the Files title bar
            mouse_btn(q, True)
            for step in range(1, 7):           # outline drag, WIn3.1 style
                mouse_move(q, 180 + step * 30, 48 + step * 25)
            mouse_btn(q, False)
            time.sleep(1.0)
            shot(q, "m4_mouse_drag")
            click(q, 500, 300)                 # background click
            time.sleep(0.5)
            shot(q, "m4_mouse_done")
            return

        # ---- M2: keyboard navigation. Desktop icon 0 = SysInfo. ----------
        keys(q, "ret"); time.sleep(1.5)
        shot(q, "m2_sysinfo")
        keys(q, "esc"); time.sleep(1.0)

        keys(q, "right"); keys(q, "ret"); time.sleep(1.5)      # icon 1 = Clock
        shot(q, "m2_clock")
        keys(q, "esc"); time.sleep(1.0)

        # ---- M2: Files (RAM disk listing: README.TXT) ---------------------
        keys(q, "right"); keys(q, "ret"); time.sleep(1.5)      # icon 2 = Files
        shot(q, "m2_files")
        keys(q, "esc"); time.sleep(1.0)

        # ---- M2: Notepad + typing + Ctrl-S (cmdKey) save -------------------
        keys(q, "right"); keys(q, "ret"); time.sleep(1.5)      # icon 3 = Notepad
        text(q, "UnoDOS on a modern PC via UEFI GOP.")
        time.sleep(1.0)
        shot(q, "m2_notepad")
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "s"}])
        time.sleep(1.0)
        keys(q, "esc"); time.sleep(1.0)

        # ---- M2: Files again - the saved file on the FAT volume ------------
        keys(q, "left"); keys(q, "ret"); time.sleep(1.5)       # icon 2 = Files
        keys(q, "r"); time.sleep(1.0)                          # refresh listing
        shot(q, "m2_files_saved")
        keys(q, "esc"); time.sleep(1.0)

        # ---- M3: Dostris ---------------------------------------------------
        keys(q, "right", "right", "right"); keys(q, "ret"); time.sleep(1.5)  # icon 5
        keys(q, "n"); time.sleep(0.5)          # new game
        for _ in range(6):
            keys(q, "left", gap=0.2)
            keys(q, "spc", gap=0.3)            # hard-drop a few pieces
        time.sleep(0.8)
        shot(q, "m3_dostris")
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)


if __name__ == "__main__":
    main()
