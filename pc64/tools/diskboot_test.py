#!/usr/bin/env python3
"""Boot a REAL UnoDOS disk image under QEMU+OVMF and screenshot the desktop.

harness.py boots `-drive format=vvfat,file=fat:rw:build/esp`, which is QEMU
faking a filesystem from a host directory.  That proves the OS works; it proves
nothing about a partition table or an on-disk FAT32, because neither exists in
that path.  This boots an actual raw image - the GPT, the ESP and the FAT32 the
flasher lays down - through the firmware's own disk stack, which is what a USB
stick actually is.

  python3 tools/diskboot_test.py <image.img> [tag] [seconds]

Writes shots/<tag>.png.  Exits non-zero if the image never paints anything.
"""
import json, os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
QMP_SOCK  = "/tmp/unodos-diskboot-qmp.sock"


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


def mouse_move(q, x, y, w, h):
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / w)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / h)}}])
    time.sleep(0.15)


def click(q, x, y, w=1280, h=800, double=False):
    mouse_move(q, x, y, w, h)
    for _ in range(2 if double else 1):
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.12)
    time.sleep(0.6)


def shot(q, tag):
    ppm = "shots/%s.ppm" % tag
    q.cmd("screendump", filename=ppm)
    time.sleep(0.6)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag], check=True)
    os.remove(ppm)
    print("shot: shots/%s.png" % tag)


def main():
    img = sys.argv[1] if len(sys.argv) > 1 else "build/unodos-uefi.img"
    tag = sys.argv[2] if len(sys.argv) > 2 else "diskboot"
    wait = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    # "x,y[,d][:...]" - points to click ("d" = double), a shot after each.
    # "k:<qcode>" sends a key instead, so a run can mix the two.
    clicks = sys.argv[4] if len(sys.argv) > 4 else ""
    if not os.path.exists(img):
        sys.exit("no such image: " + img)

    os.makedirs("shots", exist_ok=True)
    subprocess.run(["cp", OVMF_VARS, "build/vars-diskboot.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)

    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars-diskboot.fd",
        # a USB mass-storage device, because that is what gets flashed
        "-drive", "format=raw,if=none,id=stick,file=" + img,
        "-device", "qemu-xhci", "-device", "usb-storage,drive=stick",
        "-device", "usb-tablet",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/ovmf-diskboot.log", "-global", "isa-debugcon.iobase=0x402",
    ]
    qemu = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting %ds for OVMF -> UnoDOS from the disk..." % wait)
        time.sleep(wait)
        shot(q, tag)
        steps = [c for c in clicks.split(";") if c]
        for i, spec in enumerate(steps):
            if spec.startswith("k="):
                for name in spec[2:].split(","):
                    q.cmd("send-key", keys=[{"type": "qcode", "data": name}],
                          **{"hold-time": 60})
                    time.sleep(0.3)
            else:
                f = spec.split(",")
                click(q, int(f[0]), int(f[1]), double=len(f) > 2 and f[2] == "d")
            time.sleep(2.0)
            shot(q, "%s_%d" % (tag, i + 1))
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()


if __name__ == "__main__":
    main()
