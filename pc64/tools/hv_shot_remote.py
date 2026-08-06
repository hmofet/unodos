#!/usr/bin/env python3
"""Runs ON the remote KVM box (scp'd there by hv_remote.py --shot).

Boots the image, waits until the A3 slice test is running, and screendumps the
GOP surface. The point is the one thing a boot log cannot show: that the
desktop is still being painted while a guest is spinning in an infinite loop
in the gaps between frames.
"""
import json, os, socket, subprocess, sys, time

IMG = "/tmp/unodos-uefi.img"
SOCK = "/tmp/hv-qmp.sock"
OUT = "/tmp/hv_shot.ppm"
AT = float(sys.argv[1]) if len(sys.argv) > 1 else 26.0


class Qmp:
    def __init__(self, path, timeout=40):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
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
        self.s.sendall(json.dumps({"execute": name, "arguments": args}).encode() + b"\n")
        while True:
            m = self.recv()
            if "return" in m or "error" in m:
                return m


def main():
    for f in (SOCK, OUT):
        if os.path.exists(f):
            os.remove(f)
    subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", "/tmp/hv_vars.fd"], check=True)
    q = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "4096", "-cpu", "host",
        "-enable-kvm",
        "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
        "-drive", "if=pflash,format=raw,file=/tmp/hv_vars.fd",
        "-drive", "format=raw,file=" + IMG,
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % SOCK,
        "-debugcon", "file:/tmp/hv.log", "-global", "isa-debugcon.iobase=0x402",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = Qmp(SOCK)
        time.sleep(AT)
        print("screendump at t+%.0fs:" % AT, m.cmd("screendump", filename=OUT))
        time.sleep(8)                      # let the slice test finish + log
        m.cmd("quit")
    finally:
        try:
            q.wait(timeout=20)
        except subprocess.TimeoutExpired:
            q.kill()
    print(subprocess.run(["grep", "-a", "vm slice", "/tmp/hv.log"],
                         capture_output=True, text=True).stdout.strip()
          or "(no slice line yet)")


if __name__ == "__main__":
    main()
