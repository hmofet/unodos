#!/usr/bin/env python3
"""Capture the user-manual's VirtualBox screenshot - a REAL boot of
build/unodos-pc64.iso in VirtualBox on the devbuntu box.

Part of the manual pipeline (like docs_shots.py, but through a second,
independent hypervisor): pushes the ISO to devbuntu, creates a throwaway
EFI VM, boots it headless, grabs the guest framebuffer through VirtualBox's
own control channel (VBoxManage controlvm screenshotpng - no display or
focus needed), and lands the PNG in shots/manual/virtualbox.png. The VM is
deleted afterwards; the VirtualBox install stays (it is reused every time
the manual is regenerated).

Prereqs on the host (one-time): VirtualBox installed, passwordless ssh.

  python3 tools/vbox_shot.py [host]        default host: devbuntu
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)

HOST = sys.argv[1] if len(sys.argv) > 1 else "devbuntu"
VM = "unodos-manual"
ISO = "build/unodos-pc64.iso"
OUT = "shots/manual/virtualbox.png"


def ssh(cmd, check=True):
    return subprocess.run(["ssh", HOST, cmd], check=check,
                          capture_output=True, text=True)


def accent_pixels(png):
    """Count UnoDOS-accent-blue pixels (Start button, title underlines,
    slider). Firmware/EFI screens have none, so this cleanly separates 'the
    desktop is up' from 'stuck at firmware' regardless of PNG size. Full
    PNG decode (all 5 filters), sampled every 2nd row."""
    import struct, zlib
    with open(png, "rb") as f:
        data = f.read()
    pos, idat, w, h, bpp = 8, b"", 0, 0, 3
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", data[pos + 8:pos + 16])
            bpp = 4 if data[pos + 8 + 9] == 6 else 3      # RGBA or RGB
        elif typ == b"IDAT":
            idat += data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * bpp
    out_prev = bytearray(stride)
    hits = 0
    p = 0
    for y in range(h):
        filt = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = out_prev[x]
            c = out_prev[x - bpp] if x >= bpp else 0
            if   filt == 1: line[x] = (line[x] + a) & 0xFF
            elif filt == 2: line[x] = (line[x] + b) & 0xFF
            elif filt == 3: line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif filt == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        out_prev = line
        if y % 2 == 0:
            for x in range(0, stride, bpp * 2):
                r, g, bl = line[x], line[x + 1], line[x + 2]
                if bl > 180 and r < 130 and g < 150 and bl - r > 80:
                    hits += 1
    return hits


def main():
    if not os.path.exists(ISO):
        sys.exit("no %s - run tools/mkiso.py first" % ISO)
    print("pushing ISO to %s..." % HOST)
    subprocess.run(["scp", "-q", ISO, "%s:/tmp/unodos-pc64.iso" % HOST], check=True)

    ssh("VBoxManage controlvm %s poweroff 2>/dev/null; sleep 1; "
        "VBoxManage unregistervm %s --delete 2>/dev/null; true" % (VM, VM),
        check=False)                                     # stale leftovers
    print("creating the EFI VM...")
    ssh("VBoxManage createvm --name %s --ostype Other_64 --register" % VM)
    ssh("VBoxManage modifyvm %s --memory 512 --vram 32 --firmware efi "
        "--graphicscontroller vmsvga --mouse usbtablet --usb on" % VM)
    ssh("VBoxManage storagectl %s --name SATA --add sata --controller IntelAhci "
        "--portcount 2" % VM)
    ssh("VBoxManage storageattach %s --storagectl SATA --port 0 --device 0 "
        "--type dvddrive --medium /tmp/unodos-pc64.iso" % VM)
    print("booting headless...")
    ssh("VBoxManage startvm %s --type headless" % VM)

    try:
        # EFI + splash + first desktop paint; poll until the frame shows the
        # desktop (accent-blue pixels - firmware screens have none)
        os.makedirs(os.path.dirname(OUT), exist_ok=True)
        hits = 0
        for i in range(12):
            time.sleep(6)
            r = ssh("VBoxManage controlvm %s screenshotpng /tmp/unodos-vbox.png"
                    % VM, check=False)
            if r.returncode != 0:
                continue
            subprocess.run(["scp", "-q", "%s:/tmp/unodos-vbox.png" % HOST, OUT],
                           check=True)
            hits = accent_pixels(OUT)
            print("  t+%2ds accent-blue pixels: %d" % ((i + 1) * 6, hits))
            if hits > 300:
                time.sleep(4)                            # let the clock settle
                ssh("VBoxManage controlvm %s screenshotpng /tmp/unodos-vbox.png"
                    % VM, check=False)
                subprocess.run(["scp", "-q", "%s:/tmp/unodos-vbox.png" % HOST,
                                OUT], check=True)
                break
        print("wrote %s" % OUT)
    finally:
        ssh("VBoxManage controlvm %s poweroff 2>/dev/null; sleep 2; "
            "VBoxManage unregistervm %s --delete 2>/dev/null; "
            "rm -f /tmp/unodos-pc64.iso /tmp/unodos-vbox.png; true" % (VM, VM),
            check=False)
    if accent_pixels(OUT) <= 300:
        sys.exit("FAIL: the frame never showed the UnoDOS desktop")
    print("PASS: VirtualBox booted the ISO to the desktop")


if __name__ == "__main__":
    main()
