#!/usr/bin/env python3
"""A8's visual proof: the guest's framebuffer in an Appliances window, and a
command typed THROUGH the window answered on the guest's own console.

Runs ON a box with KVM at L0 (leviathan, devbuntu), beside a staged image:

    # on the build box:
    UNO_DEBUG=1 UNO_DETACH=1 UNO_DBGCON=1 ./build.sh
    python3 tools/vm_stage.py
    # then ship build/unodos-uefi.img + tools/{vm_display_urc,unoauto_remote,
    # ppm2png}.py to the KVM box and run:
    python3 vm_display_urc.py /tmp/unodos-uefi.img

Boots the image under KVM with a slirp NIC, waits for the URC link, opens
Appliances by id, presses `d` (the Display view), and screenshots the desktop
with the guest's fbcon inside it.  Then types a marked command through the
window and screenshots again: the reply painting into the framebuffer is the
whole path - i8042 bytes in, IRQ1 through the PIC, the guest's VT, and its
pixels back out through the shared surface.

The image must carry `vm-selftest` (arms the guest), `noshutdown`, and a
`remote=10.0.2.2:<port>` line - vm_display_stage.py on the build box appends
the remote line after vm_stage.py.  This script checks rather than trusting.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink

PORT = 5399
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"


def ppm(path, w, h, rgba):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        px = bytearray()
        for i in range(0, w * h * 4, 4):
            px += rgba[i:i + 3]
        f.write(bytes(px))


def png_convert(ppm_path):
    try:
        import ppm2png
        w, h, px = ppm2png.read_ppm(ppm_path)
        out = ppm_path[:-4] + ".png"
        ppm2png.write_png(out, w, h, px)
        os.remove(ppm_path)
        return out
    except Exception:
        return ppm_path


def main():
    img = sys.argv[1] if len(sys.argv) > 1 else "/tmp/unodos-uefi.img"
    boot_grace = int(sys.argv[2]) if len(sys.argv) > 2 else 240
    with open(img, "rb") as f:
        blob = f.read()
    for key in (b"vm-selftest", b"remote=10.0.2.2"):
        if key not in blob:
            sys.exit("image does not carry %r - stage it first" % key)

    subprocess.run(["pkill", "-f", "qemu-system-x86_64.*" + os.path.basename(img)],
                   stderr=subprocess.DEVNULL)
    time.sleep(1)
    subprocess.run(["cp", OVMF_VARS, "/tmp/vmdisp_vars.fd"], check=True)

    link = UnoAutoLink("127.0.0.1", PORT)
    link.listen()
    q = subprocess.Popen([
        # 8 GB, so the carve steps up from 1.5 GB to 2 GB (uno_vmm_carve_mb
        # steps at 1800/3500/7000 MB of FREE memory).  Chromium in 1.5 GB
        # renders a page and then dies mid-session with "Aw, Snap! Error
        # code: 8", which reads as a browser bug and is a memory ceiling.
        "qemu-system-x86_64", "-machine", "q35", "-m", "8192",
        "-cpu", "host", "-enable-kvm",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=/tmp/vmdisp_vars.fd",
        "-drive", "format=raw,file=" + img,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
        "-debugcon", "file:/tmp/vmdisp.log",
        "-global", "isa-debugcon.iobase=0x402",
    ], stderr=subprocess.DEVNULL)

    def shot(tag):
        w, h, rgba = link.screen_grab(1, timeout=60)
        p = "/tmp/vmdisp_%s.ppm" % tag
        ppm(p, w, h, rgba)
        out = png_convert(p)
        print("shot:", out, "(%dx%d)" % (w, h))
        return w, h, rgba

    def key(uni, scan=0, settle=0.2):
        link.key(int(scan), int(uni), 0, timeout=10)
        time.sleep(settle)

    ok = True
    try:
        if not link.wait_connected(240):
            sys.exit("the guest never dialled in - is this the DEBUG build?")
        link.wait_hello(30.0)
        time.sleep(2)

        # The Linux guest boots in frame-loop slices; give it real wall time,
        # and read its progress line so the wait is evidence rather than hope.
        print("waiting %ds for the guest to reach its shells..." % boot_grace)
        deadline = time.time() + boot_grace
        while time.time() < deadline:
            time.sleep(10)
            try:
                out = link.eval("import uno; print(uno.vm_status())", timeout=10)
                print("  vm:", out)
                if out and "shell ANSWERED" in out[0]:
                    break
            except Exception:
                pass                      # older builds have no uno.vm_status

        link.command("launch", "vmgr", timeout=15)
        time.sleep(3)
        key(ord('d'))                     # the Display view
        time.sleep(2)

        w, h, rgba = shot("display")

        # The guest's framebuffer paints its boot log white-on-black; the
        # Appliances window face is the theme's grey.  Enough distinct dark
        # rows with light pixels inside the window area = the surface is
        # really being blitted.  Loose on purpose: this is "is there a
        # console in that window", not a pixel-perfect compare.
        lit = 0
        for y in range(h // 4, h - 80):
            row = rgba[y * w * 4:(y * w * 4) + w * 4]
            dark = light = 0
            for x in range(0, w * 4, 16):
                r8, g8, b8 = row[x], row[x + 1], row[x + 2]
                v = r8 + g8 + b8
                if v < 90: dark += 1
                elif v > 500: light += 1
            if dark > 40 and light > 2:
                lit += 1
        print("fb-looking rows: %d" % lit)
        if lit < 20:
            ok = False
            print("FAIL: the display view does not look like a console")

        # Type through the window: the marker lands in the guest's tty1
        # shell, and the shell echoes it into the framebuffer.
        for ch in "echo UNODOS-DISPLAY-OK":
            key(ord(ch), settle=0.15)
        key(ord('\r'), settle=0.2)
        time.sleep(8)
        shot("display_typed")

        print("typed through the window; check display_typed for the echo")

        # An appliance with a rootfs keeps going long past the shell - X and
        # Chromium take minutes of wall time at a slice per frame - so with
        # a fourth argument, keep photographing the window as it happens.
        extra = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        for i in range(extra):
            time.sleep(90)
            shot("display_t%02d" % i)

        # THE BROWSER IS DRIVEN, not merely watched.  A rendered page proves
        # Blink runs; typing a DIFFERENT address into the address bar and
        # getting that page proves the whole loop - host keystroke, i8042,
        # X, Chromium's omnibox, DNS, TLS, layout, and the surface coming
        # back out.  Ctrl+L focuses the omnibox on every platform Chromium
        # has ever shipped on.
        nav = sys.argv[4] if len(sys.argv) > 4 else ""
        if nav:
            # NO MODIFIERS IN THIS SEQUENCE, deliberately.  Ctrl+L is the
            # habit and it depends on the whole modifier path surviving the
            # URC verb, the shell's key hook and the emulated keyboard; F6
            # focuses Chromium's address bar with a bare keypress, and a run
            # of Backspace empties it without needing Ctrl+A either.  One
            # less thing between the test and what it is testing.
            print("driving the browser to %s ..." % nav)
            # BOTH WAYS INTO THE ADDRESS BAR, because F6 alone did not do it:
            # in Chromium F6 cycles between browser panes and where it lands
            # first depends on what is showing, while Ctrl+L is unambiguous.
            # Ctrl now survives the trip (the Display view forwards it as
            # ASCII 1..26, which the emulated keyboard turns back into a held
            # Ctrl), so it is worth the retry that earlier runs could not make.
            # Ctrl+L ALONE.  It is proven to focus the address bar (the guest
            # drives itself the same way), and the F6 presses that used to
            # follow it could only move the focus back off again.
            link.key(0, ord('l'), 1, timeout=10)      # Ctrl+L
            time.sleep(2.0)
            for _ in range(45):
                key(ord('\b'), settle=0.06)
            for ch in nav:
                key(ord(ch), settle=0.30)
            # PHOTOGRAPH THE TYPING BEFORE COMMITTING IT.  A shot taken 45
            # seconds after Enter cannot distinguish "the letters never
            # arrived" from "they arrived, navigated, and the page since
            # changed" - and those want opposite fixes.
            shot("display_urltyped")
            key(ord('\r'), settle=0.3)
            for i in range(4):
                time.sleep(45)
                shot("display_nav%02d" % i)
    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:
            pass
        time.sleep(0.5)
        q.kill()
        link.close()
    print(">> vm display %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
