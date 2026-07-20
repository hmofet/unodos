#!/usr/bin/env python3
"""UnoDOS/pc64 Photos-app verification - QEMU + OVMF, fully headless.

The in-OS half of the image chain's tests (the host half is
../unomedia/test/run_tests.py, which proves the decoders pixel-exact): boot
the shell, open Photos through the Start menu, walk the staged PICTURES set
- one file per unomedia decoder family - with the Right key, and screenshot
each. Two asserts keep it honest, not just pretty:

  * consecutive step shots must DIFFER (each format really decoded and
    rendered - a stuck viewer would pass a shot-exists check), and
  * two shots of ORBIT.GIF taken ~1.2 s apart must DIFFER (the animation
    pump is advancing frames through the module frame() hook).

Keyboard-driven throughout (the machine detaches under QEMU, so only a
relative PS/2 mouse survives - Photos is fully keyboard-operable by design).

  python3 tools/photos_test.py
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, keys, QMP_SOCK, OVMF_CODE, OVMF_VARS

PHOTOS_MENU_INDEX = 16   # 7 native + 6 bridge + Runner + Browser + Studio

# FAT order = build.sh's copy order = alphabetical
PICS = ["bloom_png", "flag_bmp", "grad_tga", "lagoon_web",
        "moon_pgm", "orbit_gif", "sunset_jpg", "tiles_qoi"]
GIF_STEP = 5             # ORBIT.GIF's index in that walk


def crop_view(ppm):
    """The centre-right band of the screen (the picture area) as raw bytes -
    NOT the whole frame: the taskbar clock ticks every second, so whole-frame
    compares always differ and would wave a stuck viewer through."""
    hdr, rest = ppm.split(b"255\n", 1)
    dims = hdr.split()
    w, h = int(dims[1]), int(dims[2])
    x0, x1 = int(w * 0.40), int(w * 0.85)
    y0, y1 = int(h * 0.25), int(h * 0.75)
    return b"".join(rest[(y * w + x0) * 3:(y * w + x1) * 3]
                    for y in range(y0, y1))


def shot_raw(q, tag):
    """screendump -> keep the cropped view bytes for comparison + save PNG."""
    ppm = "shots/%s.ppm" % tag
    q.cmd("screendump", filename=ppm)
    time.sleep(0.5)
    data = crop_view(open(ppm, "rb").read())
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm,
                    "shots/%s.png" % tag], check=True)
    os.remove(ppm)
    print("shot: shots/%s.png" % tag)
    return data


def main():
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=vvfat,file=fat:rw:build/esp",
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/ovmf.log",
        "-global", "isa-debugcon.iobase=0x402",
    ]
    qemu = subprocess.Popen(argv)
    fails = []
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting for OVMF -> UnoDOS boot...")
        time.sleep(18)

        # Start menu -> Photos (launcher order = app order)
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, *(["down"] * PHOTOS_MENU_INDEX))
        keys(q, "ret")
        time.sleep(2.5)                # opens straight into PICTURES\BLOOM.PNG

        prev = None
        for i, name in enumerate(PICS):
            if i:
                keys(q, "right")       # next image in the folder
                time.sleep(2.0)
            cur = shot_raw(q, "photos_" + name)
            if prev is not None and cur == prev:
                fails.append("step %s rendered identically to the previous "
                             "image" % name)
            prev = cur
            if i == GIF_STEP:
                # the 10-frame loop plays in ~0.5-1 s, so two samples can
                # ALIAS onto the same frame - sample several times at a
                # non-harmonic spacing and require ANY difference
                samples = [cur]
                for k in range(4):
                    time.sleep(0.73)
                    samples.append(shot_raw(q, "photos_orbit_anim%d" % k))
                if all(s == cur for s in samples[1:]):
                    fails.append("ORBIT.GIF did not animate (5 samples "
                                 "over ~3 s all identical)")
                prev = samples[-1]

        # zoom: 100% then two steps in on the last image (the 16.16 scale +
        # anchor math), and back to fit - each must repaint differently
        keys(q, "1"); time.sleep(1.0)
        z1 = shot_raw(q, "photos_zoom_100")
        keys(q, "equal"); keys(q, "equal"); time.sleep(1.0)
        z2 = shot_raw(q, "photos_zoom_in")
        if z2 == z1:
            fails.append("zoom + did not change the view")
        keys(q, "f"); time.sleep(1.0)
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)

    if fails:
        print("FAIL:")
        for f in fails:
            print("  -", f)
        sys.exit(1)
    print("PASS: all %d formats rendered, animation advancing" % len(PICS))


if __name__ == "__main__":
    main()
