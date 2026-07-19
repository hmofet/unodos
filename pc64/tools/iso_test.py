#!/usr/bin/env python3
"""UnoDOS/pc64 ISO verification - QEMU + OVMF, fully headless.

Boots build/unodos-pc64.iso (tools/mkiso.py) both ways it ships:
  cd   attached as a CD-ROM        = the VM-hypervisor path (El Torito EFI)
  usb  attached raw as a USB stick = the Rufus-dd / BalenaEtcher / dd path
       (the ISO's MBR+GPT declares the embedded FAT image as an ESP)

Each boot must reach the desktop AND render text - the UI font is a TTF
loaded from the boot volume at runtime, so visible glyphs prove the file
tree is readable end-to-end (a mis-mounted volume once produced a textless
desktop). The check: open the System window and require its shot to differ
substantially from a windowless desktop in ink coverage.

  python3 tools/iso_test.py
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, keys, shot, QMP_SOCK, OVMF_CODE, OVMF_VARS


def ink(png):
    """Fraction of dark pixels - crude but robust text detector."""
    import struct, zlib
    with open(png, "rb") as f:
        data = f.read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", data[pos + 8:pos + 16])
        elif typ == b"IDAT":
            idat += data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    dark = total = 0
    for y in range(0, h, 4):                      # sample every 4th row/px
        row = raw[y * stride + 1:(y + 1) * stride]
        prev = None  # filters: harness PNGs are filter-0 rows
        for x in range(0, w * 3, 12):
            r, g, b = row[x], row[x + 1], row[x + 2]
            total += 1
            if r + g + b < 240:
                dark += 1
    return dark / max(1, total)


def boot(mode, extra):
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = ["qemu-system-x86_64", "-machine", "q35", "-m", "256",
            "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
            "-drive", "if=pflash,format=raw,file=build/vars.fd",
            "-device", "qemu-xhci", "-device", "usb-tablet",
            "-nic", "none", "-display", "none",
            "-qmp", "unix:%s,server,nowait" % QMP_SOCK] + extra
    qemu = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_SOCK)
        print("[%s] booting..." % mode)
        time.sleep(22)
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, "down", "down", "down"); keys(q, "ret")   # System window
        time.sleep(1.5)
        shot(q, "iso_" + mode)
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)
    frac = ink("shots/iso_%s.png" % mode)
    ok = frac > 0.02          # a texty System window is ~4-6%; textless ~0.5%
    print("[%s] %s: dark-pixel fraction %.3f" % (mode, "PASS" if ok else "FAIL", frac))
    return ok


def main():
    iso = "build/unodos-pc64.iso"
    if not os.path.exists(iso):
        sys.exit("no %s - run tools/mkiso.py first" % iso)
    ok = boot("cd", ["-cdrom", iso])
    ok &= boot("usb", ["-drive", "if=none,id=ustick,format=raw,file=" + iso,
                       "-device", "usb-storage,drive=ustick"])
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
