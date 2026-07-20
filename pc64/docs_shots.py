#!/usr/bin/env python3
"""Capture the UnoDOS/pc64 screenshots the user manual needs.

Self-contained QMP driver (same technique as pc64/harness.py): boots the real
ESP under QEMU + OVMF headless, drives the desktop over QMP, and dumps the GOP
surface to shots/manual/<tag>.png at each scene.

  python3 docs_shots.py [scene ...]     run named scenes (default: all core)

The desktop is deterministic, so the Start-menu order is fixed:
  0 Control 1 Editor 2 Files 3 System 4 Clock 5 Install 6 Music 7 Dostris
  8 Pac-Man 9 OutLast 10 Tracker 11 Paint 12 Network 13 Runner3D 14 Browser
Launch app N: Ctrl-Esc, Down*N, Enter.  Close focused window: Ctrl-W.
"""
import json, os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
QMP_SOCK  = "/tmp/unodos-pc64-docs-qmp.sock"
OUTDIR    = "shots/manual"
W, H = 1280, 800                       # GOP surface (screendump size)


class Qmp:
    def __init__(self, path, timeout=40):
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
        self.recv()
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


def key(q, *names, hold=40, gap=0.12):
    for n in names:
        q.cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": hold})
        time.sleep(gap)


def combo(q, *names, gap=0.2):
    q.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(gap)


QMAP = {" ": "spc", ".": "dot", ",": "comma", "-": "minus", "/": "slash",
        ":": "shift+semicolon", "_": "shift+minus"}


def text(q, s, gap=0.06):
    for ch in s:
        if ch.isupper():
            q.cmd("send-key", keys=[{"type": "qcode", "data": "shift"},
                                    {"type": "qcode", "data": ch.lower()}])
        elif ch in QMAP and "+" in QMAP[ch]:
            a, b = QMAP[ch].split("+")
            q.cmd("send-key", keys=[{"type": "qcode", "data": a},
                                    {"type": "qcode", "data": b}])
        else:
            q.cmd("send-key", keys=[{"type": "qcode", "data": QMAP.get(ch, ch)}])
        time.sleep(gap)


def mmove(q, x, y):
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / H)}}])
    time.sleep(0.1)


def mbtn(q, down):
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": down, "button": "left"}}])
    time.sleep(0.12)


def click(q, x, y):
    mmove(q, x, y); mbtn(q, True); mbtn(q, False); time.sleep(0.25)


def drag(q, x0, y0, x1, y1, steps=8):
    mmove(q, x0, y0); mbtn(q, True)
    for i in range(1, steps + 1):
        mmove(q, x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
    mbtn(q, False); time.sleep(0.3)


def shot(q, tag):
    os.makedirs(OUTDIR, exist_ok=True)
    ppm = "%s/%s.ppm" % (OUTDIR, tag)
    q.cmd("screendump", filename=ppm)
    time.sleep(0.4)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "%s/%s.png" % (OUTDIR, tag)],
                   check=True)
    os.remove(ppm)
    print("shot: %s/%s.png" % (OUTDIR, tag), flush=True)


def wait_splash(q, timeout=30):
    """Poll screendumps until the UnoDOS splash is actually on screen (deep
    navy backdrop), so the splash shot is never the pre-GOP black frame or
    the firmware's own logo. Returns once seen (or on timeout)."""
    os.makedirs(OUTDIR, exist_ok=True)
    probe = "%s/_probe.ppm" % OUTDIR
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            q.cmd("screendump", filename=probe)
            time.sleep(0.25)
            with open(probe, "rb") as f:
                if f.readline().strip() != b"P6":
                    raise ValueError
                line = f.readline()
                while line.startswith(b"#"):
                    line = f.readline()
                w, h = map(int, line.split())
                f.readline()
                px = f.read()
            o = (10 * w + 10) * 3                     # a corner pixel
            r, g, b = px[o], px[o + 1], px[o + 2]
            if b > 34 and b > r + 12 and g < 90:      # the navy backdrop
                os.remove(probe)
                return True
        except Exception:
            pass
        time.sleep(0.5)
    if os.path.exists(probe):
        os.remove(probe)
    return False


def close_all(q):
    """Close any open windows so we return to a bare desktop."""
    for _ in range(6):
        combo(q, "ctrl", "w"); time.sleep(0.2)
    time.sleep(0.3)


def launch(q, idx, settle=1.6):
    """Open Start menu and pick app `idx` by keyboard."""
    combo(q, "ctrl", "esc"); time.sleep(0.6)
    for _ in range(idx):
        key(q, "down", gap=0.09)
    key(q, "ret"); time.sleep(settle)


# ---- scenes ---------------------------------------------------------------
def sc_desktop(q):
    close_all(q)                     # boot opens Control Panel; close it
    shot(q, "desktop")

def sc_startmenu(q):
    close_all(q)
    combo(q, "ctrl", "esc"); time.sleep(0.8)
    shot(q, "startmenu")
    combo(q, "ctrl", "esc"); time.sleep(0.3)

def sc_controlpanel(q):
    close_all(q)
    launch(q, 0)
    shot(q, "controlpanel")

def sc_themes(q):
    # Keyboard-only: Tab focuses the Theme dropdown (first focusable widget);
    # Down cycles kThemes live, re-skinning the whole desktop. Order:
    # 0 Aurora Light 1 Aurora Dark 2 UnoDOS 3 Mac OS 7 4 Mac Plus 5 Windows 3.1
    # 6 Amiga 7 C64 8 Apple II 9 NeXTSTEP
    close_all(q); launch(q, 0)
    key(q, "tab"); time.sleep(0.3)                   # focus Theme dropdown
    shot(q, "theme_aurora_light")
    for tag in ["aurora_dark", "unodos", "macos7", "macplus",
                "win31", "amiga", "c64", "apple2", "next"]:
        key(q, "down"); time.sleep(0.45)
        shot(q, "theme_" + tag)
    for _ in range(9):                               # back to Aurora Light so
        key(q, "up", gap=0.3)                        # later scenes match

def sc_fonts(q):
    # Tab x3 -> Font dropdown; Down picks a TrueType face (re-skins all UI text).
    close_all(q); launch(q, 0)
    key(q, "tab", "tab", "tab"); time.sleep(0.3)
    key(q, "down"); time.sleep(0.5)                  # 1st TTF (Sans)
    shot(q, "font_ttf")

def sc_resolution(q):
    # Tab x2 -> Resolution dropdown; Down bumps to the next mode.
    close_all(q); launch(q, 0)
    key(q, "tab", "tab"); time.sleep(0.3)
    key(q, "down"); time.sleep(0.6)
    shot(q, "resolution")

def sc_uiscale(q):
    # Tab x4 -> the UI scale dropdown (100/125/150/200%). Each change rescales
    # every font and rebuilds the shell, so refocus (Tab x4) between steps.
    # Ends by resetting to 100% so later scenes shoot at the normal scale.
    close_all(q); launch(q, 0)
    key(q, "tab", "tab", "tab", "tab"); time.sleep(0.3)
    key(q, "down"); time.sleep(1.2)                  # 125% (shell rebuilds)
    key(q, "tab", "tab", "tab", "tab"); time.sleep(0.3)
    key(q, "down"); time.sleep(1.2)                  # 150%
    shot(q, "uiscale")
    for _ in range(2):                               # back to 100%
        key(q, "tab", gap=0.12); key(q, "tab", gap=0.12)
        key(q, "tab", gap=0.12); key(q, "tab", gap=0.12)
        key(q, "up"); time.sleep(1.2)

def sc_editor(q):
    close_all(q); launch(q, 1)
    shot(q, "editor")
    # rich text: select all, bold + italic via the Ctrl accelerators
    combo(q, "ctrl", "a"); time.sleep(0.3)
    combo(q, "ctrl", "b"); time.sleep(0.5)
    shot(q, "editor_rich")

def sc_files(q):
    close_all(q); launch(q, 2)
    shot(q, "files")
    text(q, "2"); time.sleep(0.6)                    # two-pane commander view
    shot(q, "files_two")

def sc_system(q):
    close_all(q); launch(q, 3)
    shot(q, "system")

def sc_clock(q):
    close_all(q); launch(q, 4)
    shot(q, "clock")

def sc_install(q):
    close_all(q); launch(q, 5, settle=2.0)
    shot(q, "install")

def sc_dostris(q):
    close_all(q); launch(q, 7)
    key(q, "n"); time.sleep(0.5)
    for _ in range(4):
        key(q, "left", gap=0.15); key(q, "spc", gap=0.25)
    time.sleep(0.5)
    shot(q, "dostris")

def sc_pacman(q):
    close_all(q); launch(q, 8)
    key(q, "n"); time.sleep(0.4)
    for _ in range(3):
        key(q, "right", gap=0.2)
    time.sleep(0.5)
    shot(q, "pacman")

def sc_outlast(q):
    close_all(q); launch(q, 9)
    key(q, "n"); time.sleep(0.5)
    shot(q, "outlast")

def sc_music(q):
    close_all(q); launch(q, 6)
    shot(q, "music")

def sc_tracker(q):
    close_all(q); launch(q, 10)
    shot(q, "tracker")

def sc_paint(q):
    close_all(q); launch(q, 11)
    shot(q, "paint")

def sc_runner3d(q):
    close_all(q); launch(q, 13, settle=2.2)
    time.sleep(1.0)
    shot(q, "runner3d")

def sc_browser_disk(q):
    close_all(q); launch(q, 14, settle=2.0)
    shot(q, "browser_files")

def _browser_open(q, row, tag, settle=1.6):
    # Fresh browser each time. Entering the list from the address bar lands on
    # row 1 (Sample.html); Up from row 0 jumps BACK to the address bar, so we
    # navigate RELATIVE to row 1 and never go above row 0.
    close_all(q); launch(q, 14, settle=2.0)
    key(q, "down"); time.sleep(0.3)                  # address bar -> list row 1
    delta = row - 1
    for _ in range(abs(delta)):
        key(q, "up" if delta < 0 else "down", gap=0.14)
    key(q, "ret"); time.sleep(settle); shot(q, tag)

def sc_browser_docs(q):
    _browser_open(q, 0, "browser_markdown")          # Welcome.md  (Markdown)
    _browser_open(q, 1, "browser_html")              # Sample.html (HTML+CSS)
    _browser_open(q, 2, "browser_js", settle=2.0)    # Script.html (JavaScript)

def sc_net_selftest(q):
    # Needs a NIC (run with UNO_NIC=1). SLIRP provides DHCP/gw/TFTP.
    close_all(q); launch(q, 12, settle=1.6)          # Network app
    time.sleep(24)                                   # DHCP+ping+UDP+TCP+TLS
    shot(q, "network")

def sc_browser_http(q):
    close_all(q); launch(q, 14, settle=2.0)
    text(q, "http://example.com/"); time.sleep(0.3)
    key(q, "ret"); time.sleep(5.0)                   # DHCP+DNS+TCP+GET+render
    shot(q, "browser_http")

def sc_browser_https(q):
    close_all(q); launch(q, 14, settle=2.0)
    text(q, "https://example.com/"); time.sleep(0.3)
    key(q, "ret"); time.sleep(10.0)                  # + TLS 1.2 handshake
    shot(q, "browser_https")

SCENES = {
    "desktop": sc_desktop, "startmenu": sc_startmenu, "controlpanel": sc_controlpanel,
    "themes": sc_themes, "fonts": sc_fonts, "resolution": sc_resolution,
    "uiscale": sc_uiscale,
    "editor": sc_editor, "files": sc_files, "system": sc_system, "clock": sc_clock,
    "install": sc_install, "dostris": sc_dostris, "pacman": sc_pacman,
    "outlast": sc_outlast, "music": sc_music, "tracker": sc_tracker,
    "paint": sc_paint, "runner3d": sc_runner3d, "browser_disk": sc_browser_disk,
    "browser_docs": sc_browser_docs, "net_selftest": sc_net_selftest,
    "browser_http": sc_browser_http, "browser_https": sc_browser_https,
}
CORE = list(SCENES.keys())


def main():
    want = sys.argv[1:] or CORE
    use_nic = os.environ.get("UNO_NIC") == "1"
    full = os.environ.get("UNO_NETDEV")              # full SLIRP string override
    cpu = ["-cpu", "max"] if full else []            # RDRAND for the TLS test
    if full:
        net = ["-netdev", full, "-device", "e1000,netdev=n0"]
    elif use_nic:
        net = ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]
    else:
        net = ["-nic", "none"]
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=vvfat,file=fat:rw:build/esp",
        "-device", "qemu-xhci", "-device", "usb-tablet",
        # an HD Audio device, so the System window's Audio line shows the real
        # PCM backend (the "none" audiodev just swallows the samples headless)
        "-audiodev", "none,id=snd0",
        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/ovmf.log", "-global", "isa-debugcon.iobase=0x402",
    ] + cpu + net)
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting for boot...", flush=True)
        if "splash" in want:
            want = [w for w in want if w != "splash"]
            wait_splash(q); time.sleep(0.6); shot(q, "splash")
            time.sleep(15.0)
        else:
            time.sleep(18)
        for name in want:
            if name not in SCENES:
                print("?? unknown scene", name); continue
            print("=== scene:", name, flush=True)
            try:
                SCENES[name](q)
            except Exception as e:
                print("!! scene %s failed: %r" % (name, e), flush=True)
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)


if __name__ == "__main__":
    main()
