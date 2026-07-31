#!/usr/bin/env python3
"""Boot the legacy-BIOS image under QEMU + SeaBIOS, headless, and screendump it.

    python3 tools/bios_qemu.py [image] [-o shot.png] [--wait 8]

The UEFI harness (harness.py) boots OVMF; this is its BIOS sibling and exists
because the two paths share no firmware at all - a green UEFI run says nothing
about the BIOS one.

SeaBIOS is the right reference: it is stricter than most vendor CSMs about EDD
and about VBE mode reporting, so a chain that works here is unlikely to be
relying on a quirk. It is also the only BIOS available headlessly.

WHERE STAGE2'S MESSAGES GO: the screen, not the serial log. It prints through
the BIOS teletype (INT 10h), which lands in the VGA text buffer and nowhere
else - QEMU does not mirror it to serial, and the serial file will be empty on a
normal run. That is fine, because a screendump taken before the video mode is
set captures the TEXT screen, so a failure in the window where there is no
framebuffer yet still photographs its own error message. The serial log is kept
because the kernel gets one later; do not read its silence as a failure.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, QMP_SOCK  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="?", default="build/biostest.img")
    ap.add_argument("-o", "--out", default="shots/bios_boot.png")
    ap.add_argument("--wait", type=float, default=8.0,
                    help="seconds to let the boot settle before the screendump")
    ap.add_argument("--mem", default="256")
    args = ap.parse_args()

    if not os.path.exists(args.image):
        print("no image: %s (run tools/mkbios.sh test)" % args.image)
        return 2
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    serial = "build/bios_serial.log"
    for f in (QMP_SOCK, serial):
        if os.path.exists(f):
            os.remove(f)

    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", args.mem,
        # if=ide, not virtio: stage2 reads through INT 13h, and the BIOS only
        # provides that for disks it can itself see
        "-drive", "format=raw,if=ide,file=" + args.image,
        "-display", "none",
        "-serial", "file:" + serial,
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-no-reboot",          # a triple fault must STOP, not loop forever
    ]
    print("+ " + " ".join(argv))
    proc = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_SOCK)
        time.sleep(args.wait)
        ppm = os.path.abspath("build/bios_shot.ppm")
        q.cmd("screendump", filename=ppm)
        time.sleep(1.0)
        subprocess.run([sys.executable, "tools/ppm2png.py", ppm, args.out],
                       check=True)
        os.remove(ppm)
        print("shot: %s" % args.out)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if os.path.exists(serial):
        text = open(serial, "r", errors="replace").read().strip()
        print("---- serial ----")
        print(text if text else "(nothing)")
        print("----------------")
    return 0


if __name__ == "__main__":
    sys.exit(main())
