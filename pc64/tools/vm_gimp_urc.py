#!/usr/bin/env python3
"""The multi-window appliance under unovirt: GIMP, and a host-driven pointer.

The sibling of `vm_display_urc.py`, for the appliance that is not a browser.
Where that one proves a host KEYSTROKE reaches Chromium's omnibox, this one
proves a host POINTER reaches a particular pixel of a particular window - which
is the thing a multi-window application needs and a kiosk never did.

Runs ON a box with KVM at L0 (leviathan), beside a staged image:

    # on the build box, with a GIMP rootfs at build/rootfs.img:
    UNO_DEBUG=1 UNO_DETACH=1 UNO_DBGCON=1 ./build.sh
    UNO_DISK_MIB=1200 python3 tools/vm_stage.py
    python3 tools/vm_display_stage.py 1200
    scp build/unodos-uefi.img <kvm-box>:/tmp/
    # on the kvm box:
    python3 vm_gimp_urc.py /tmp/unodos-uefi.img [boot_grace] [waits]

WHY A RELATIVE MOUSE NEEDS A CALIBRATION, and what this does about it.
`unovdev_pc.c` gives the guest a PS/2 mouse, which reports DELTAS - there is no
absolute pointer device, so nothing makes the host's cursor and the guest's
agree on a position, and `vmgr.c` says so in as many words.  Two things make it
deterministic anyway:

  - The guest end runs libinput with `accelProfile flat` and `pointerSpeed 0`
    (labwc's rc.xml in `apps/gimp.app`).  Without that, distance travelled is a
    function of how fast the deltas arrive, which under a hypervisor handing the
    guest a slice per frame is not a function of anything reproducible.

  - The host end PINS the cursor before it aims.  Re-entering the Display view
    resets vmgr's `g_mx` to -1, so the next pointer event produces a delta of
    zero and establishes a baseline; one sweep to the far corner then sends a
    delta larger than the guest's whole screen, and the compositor clamps the
    cursor to (0,0).  From there a move of N host pixels is N guest pixels, and
    the aim is arithmetic rather than hope.

The image must carry `vm-selftest`, `noshutdown` and a `remote=10.0.2.2:<port>`
line; this checks rather than trusting.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink

PORT = 5399
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"

# WHERE THE GUEST'S OWN VOICE COMES OUT.  UnoDOS echoes the appliance's ttyS0
# into its debug console, and QEMU writes that to a file on the box this
# script runs on - so the harness can READ the guest, not only photograph it.
# Without this a hypervisor-only failure is twenty-five minutes of watching a
# black rectangle: the first run of this script could not tell "GIMP is
# starting slowly" from "GIMP is not running at all", and they want opposite
# responses.
DBGCON = "/tmp/vmgimp.log"

# The guest's framebuffer, and the layout gimp-layout.sh builds on it.  Kept
# here as the harness's model of the appliance: if the appliance changes its
# layout, this is the one place the aim has to follow.
GUEST_W, GUEST_H = 800, 600
TOOLBOX_X, TOOLBOX_Y = 60, 120         # inside the tool palette
CANVAS_X, CANVAS_Y = 380, 300          # inside the image window's canvas


def ppm(path, w, h, rgba):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        px = bytearray()
        for i in range(0, w * h * 4, 4):
            px += rgba[i:i + 3]
        f.write(bytes(px))


def dark_fraction(w, h, rgba):
    """How much of the desktop is near-black.

    The same test `vm_display_urc.py` uses, and it works here for the same
    reason: the guest's surface is a text console - white on black, or cleared
    to black - until the session paints, and GIMP's dark-grey theme plus a
    white canvas is nowhere near black.  The UnoDOS desktop around it is light
    either way, so the black area IS the guest and it collapses when the
    appliance arrives.
    """
    dark = tot = 0
    for y in range(0, h, 4):
        row = rgba[y * w * 4:(y + 1) * w * 4]
        for x in range(0, w * 4, 32):
            if row[x] + row[x + 1] + row[x + 2] < 90:
                dark += 1
            tot += 1
    return dark / float(tot or 1)


def guest_log():
    """Every line the appliance has said on its ttyS0, in order.

    ON THE CHANNEL TAG, not on what the line says.  UnoDOS marks the Linux
    guest's console with `[lin]`, so this reads anything the guest printed -
    which matters because the useful lines are the ones NOBODY planned for:
    a GIMP backtrace and a `dmesg` tail do not begin with `uno`, and an
    earlier version of this filter would have dropped exactly the output it
    was added to collect.
    """
    try:
        with open(DBGCON, "rb") as f:
            raw = f.read().replace(b"\r", b"\n").decode("utf-8", "replace")
    except IOError:
        return []
    out = []
    for ln in raw.split("\n"):
        i = ln.find("[lin]")
        if i >= 0:
            t = ln[i + 5:].strip()
            if t:
                out.append(t)
    return out


def find_guest_rect(w, h, rgba):
    """Where on the UnoDOS desktop the guest's surface is being blitted.

    THE HARNESS CANNOT ASSUME IT.  `vmgr.c` blits the guest at its window's
    body origin and scales by `g_scale`, and neither is a number the host has.
    Aiming at raw desktop coordinates therefore misses in two ways at once,
    and one of them is silent: a pointer event OUTSIDE the body is dropped
    outright while `g_mx < 0`, so the baseline never gets set and every
    subsequent move is measured from nothing.

    So it is measured, from the console shot, before the appliance paints.
    The guest's framebuffer at that moment is a text console - near-black with
    white text - and the desktop around it is the theme's light grey, so the
    black area IS the guest and its bounding box is the body.  Returns
    (x, y, w, h) or None.
    """
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(0, h, 2):
        row = rgba[y * w * 4:(y + 1) * w * 4]
        run = 0
        for x in range(0, w):
            o = x * 4
            if row[o] + row[o + 1] + row[o + 2] < 110:
                run += 1
                # A RUN, NOT A PIXEL.  Window borders, text and the taskbar
                # are dark too; only the guest surface is dark for a hundred
                # pixels together.
                if run >= 100:
                    if x > maxx: maxx = x
                    if x - run + 1 < minx: minx = x - run + 1
                    if y < miny: miny = y
                    if y > maxy: maxy = y
            else:
                run = 0
    if maxx < 0 or maxx - minx < 200 or maxy - miny < 150:
        return None
    return (minx, miny, maxx - minx + 1, maxy - miny + 1)


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
    boot_grace = int(sys.argv[2]) if len(sys.argv) > 2 else 420
    waits = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    with open(img, "rb") as f:
        blob = f.read()
    for key_ in (b"vm-selftest", b"remote=10.0.2.2"):
        if key_ not in blob:
            sys.exit("image does not carry %r - stage it first" % key_)

    # IMAGE-SCOPED, NEVER `pkill qemu-system`.  leviathan runs the whole fleet
    # in QEMU - the dev box and quill included - so the bare form there is a
    # fleet-wide power cut.  Safe because this script ships as a FILE; an
    # inline ssh argument would match its own command line.
    subprocess.run(["pkill", "-f", "qemu-system-x86_64.*" + os.path.basename(img)],
                   stderr=subprocess.DEVNULL)
    time.sleep(1)
    subprocess.run(["cp", OVMF_VARS, "/tmp/vmgimp_vars.fd"], check=True)

    link = UnoAutoLink("127.0.0.1", PORT)
    link.listen()
    q = subprocess.Popen([
        # 8 GB, so the carve steps up to 2 GB (uno_vmm_carve_mb steps at
        # 1800/3500/7000 MB of FREE memory).  GIMP is lighter than Chromium,
        # but GEGL's tile cache grows to whatever it is given and the guest
        # has no swap but zram.
        "qemu-system-x86_64", "-machine", "q35", "-m", "8192",
        "-cpu", "host", "-enable-kvm",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=/tmp/vmgimp_vars.fd",
        "-drive", "format=raw,file=" + img,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
        "-debugcon", "file:/tmp/vmgimp.log",
        "-global", "isa-debugcon.iobase=0x402",
    ], stderr=subprocess.DEVNULL)

    shots = []

    def shot(tag):
        w, h, rgba = link.screen_grab(1, timeout=60)
        p = "/tmp/vmgimp_%s.ppm" % tag
        ppm(p, w, h, rgba)
        out = png_convert(p)
        shots.append(out)
        print("shot:", out, "(%dx%d)" % (w, h))
        return w, h, rgba

    def key(uni, scan=0, ctrl=0, settle=0.25):
        link.key(int(scan), int(uni), int(ctrl), timeout=10)
        time.sleep(settle)

    def enter_display():
        """F12 out to the list, then the Display accelerator - which is what
        resets vmgr's pointer baseline (`g_mx = -1`, see `vm_key`) so the next
        move is a zero delta."""
        link.key(0x16, 0, 0, timeout=10)          # F12: back to the list
        time.sleep(1.0)
        key(ord('d'), settle=1.5)                 # the Display view

    def guest_shell(cmd, settle=12.0):
        """Run a command in the appliance's ttyS0 shell and read its output.

        THE ONE WAY IN THAT IS NOT A PICTURE.  vmgr's Console view forwards
        typed characters to the guest's console (`uno_vm_con_key`), the
        appliance keeps a shell on ttyS0 for exactly this, and whatever that
        shell prints comes back out through the debug console this script
        reads.  So a log file INSIDE the guest - which is where the compositor
        puts its client's stderr, and therefore where every interesting
        failure lands - is reachable from the host after all.

        Leaves the view where it found it: the caller is usually mid-scene.
        """
        before = len(guest_log())
        link.key(0x16, 0, 0, timeout=10)          # F12: back to the list
        time.sleep(1.0)
        key(ord('c'), settle=1.5)                 # the Console view
        for ch in cmd:
            link.key(0, ord(ch), 0, timeout=10)
            time.sleep(0.04)
        link.key(0, 0x0D, 0, timeout=10)          # Enter
        time.sleep(settle)
        enter_display()
        return guest_log()[before:]

    def diagnose(why):
        """What a guest that will not paint has to be asked, in one place.

        Each question is separate because they fail separately: a client that
        died has a reason in its log, a client that could not start has no
        display or no memory, and a compositor with no DRM device has neither.
        """
        print("--- diagnosing: %s" % why)
        for cmd in (
            "tail -30 /tmp/x.log > /dev/ttyS0",
            "free -m | head -3 > /dev/ttyS0",
            "ls /dev/dri /tmp/.X11-unix 2>&1 | tr '\\n' ' ' > /dev/ttyS0",
            "dmesg | tail -12 > /dev/ttyS0",
        ):
            for ln in guest_shell(cmd):
                print("   guest| %s" % ln)

    ok = True
    results = {}
    try:
        if not link.wait_connected(240):
            sys.exit("the guest never dialled in - is this the DEBUG build?")
        link.wait_hello(30.0)
        time.sleep(2)

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
        w, h, rgba = shot("01_display")

        # MEASURED WHILE IT IS STILL A CONSOLE, which is the only moment the
        # guest's surface is reliably the dark rectangle on a light desktop.
        # Once GIMP paints, its own theme is dark grey and the boundary is a
        # matter of taste rather than of contrast.
        body = find_guest_rect(w, h, rgba)
        if body:
            bx, by, bw, bh = body
            print("guest surface at %d,%d %dx%d (%.2f of %dx%d)"
                  % (bx, by, bw, bh, bw / float(GUEST_W), GUEST_W, GUEST_H))
        else:
            print("could not find the guest surface on the desktop - "
                  "the pointer half will be skipped")

        # ---- 1. does the appliance arrive at all -----------------------------
        # WAIT FOR THE SESSION, NOT FOR A CLOCK.  GIMP on a guest scheduled a
        # slice per frame takes many minutes to put its windows up, and a run
        # that counted seconds at it photographed a cleared VT and called it a
        # failure once already in this directory's history.
        print("waiting for the appliance to paint (up to %d x 90s)..." % waits)
        painted = False
        seen = len(guest_log())
        crashes = 0
        for i in range(waits):
            time.sleep(90)
            # THE GUEST'S OWN WORDS FIRST, THE PIXELS SECOND.  A black
            # rectangle is the same picture whether the client is starting
            # slowly or dying on a loop, and the two want opposite responses -
            # wait longer, or stop and find out why.  The appliance says which
            # it is on every launch; this reads it.
            fresh = guest_log()[seen:]
            seen += len(fresh)
            for ln in fresh:
                print("  guest| %s" % ln)
                if "exited rc=" in ln:
                    crashes += 1
            w, h, rgba = shot("02_wait%02d" % i)
            d = dark_fraction(w, h, rgba)
            print("  guest surface %.0f%% dark" % (d * 100))
            if d < 0.12:
                painted = True
                print("  the guest is showing a session, not a console")
                break
            # DO NOT WAIT OUT A CRASH LOOP.  The restart loop in gimp.sh is
            # there to survive one bad launch; three in a row is a client that
            # cannot start on this machine, and every further 90 seconds spent
            # photographing it is 90 seconds not spent reading its log.
            if crashes >= 3:
                print("  the client has died %d times - it is not coming up" % crashes)
                break
        results["appliance painted"] = painted
        if not painted:
            ok = False
            print("FAIL: the guest surface never stopped being a console")
            diagnose("the client never painted")

        # ---- 2. what the guest itself says -----------------------------------
        # THE COUNTABLE HALF, and it does not depend on reading a picture.
        # gimp-layout.sh prints its window census to ttyS0 every launch, so by
        # here it is already in the guest log above - three top-levels is the
        # claim this appliance exists to make, and it is a number, not a
        # judgement about a screenshot.
        if painted:
            for ln in guest_log():
                if "uno-layout:" in ln or "top-levels" in ln:
                    print("  census| %s" % ln)

        # ---- 3. the pointer -------------------------------------------------
        if painted and body:
            bx, by, bw, bh = body
            # The blit is uniform in both axes (vmgr uses one step so circles
            # stay circles), so one ratio is the whole mapping.
            sc = bw / float(GUEST_W)
            ox, oy = bx + 2, by + 2

            def host(gx, gy):
                return int(ox + gx * sc), int(oy + gy * sc)

            def aim(gx, gy, btn=0, settle=0.8):
                hx, hy = host(gx, gy)
                link.pointer(hx, hy, btn, timeout=10)
                time.sleep(settle)

            print("pinning the guest cursor at 0,0 ...")
            enter_display()
            # Baseline: the first event after re-entering the view produces a
            # zero delta whatever coordinate it carries - but it must be
            # INSIDE the body, or vmgr drops it and no baseline is set at all.
            link.pointer(bx + bw - 4, by + bh - 4, 0, timeout=10)
            time.sleep(0.8)
            # One sweep wider than the guest's whole screen: whatever the
            # cursor's position was, the compositor clamps it to the corner.
            link.pointer(ox, oy, 0, timeout=10)
            time.sleep(1.5)

            # A HOVER FIRST, because it is the cheapest thing that can only be
            # true if the pointer arrived where it was SENT: GIMP puts a
            # tooltip under the tool the cursor is resting on, and the tooltip
            # names that tool.  A click that lands somewhere else still clicks.
            aim(TOOLBOX_X, TOOLBOX_Y)
            time.sleep(4)
            shot("03_hover_toolbox")

            # Then the canvas: click to focus that window - which is itself a
            # multi-window claim, since focus only means anything when there
            # is more than one - choose the pencil with its own accelerator
            # (the keyboard path is already proven), and drag.
            aim(CANVAS_X, CANVAS_Y)
            aim(CANVAS_X, CANVAS_Y, btn=1, settle=0.4)
            aim(CANVAS_X, CANVAS_Y, btn=0, settle=1.5)
            key(ord('n'), settle=2.0)          # GIMP: the Pencil tool
            shot("04_canvas_focused")

            # A black stroke on a white canvas cannot be produced by anything
            # except a button held down across real motion, which is the last
            # part of the pointer path a hover and a click do not cover.
            print("drawing a stroke...")
            aim(CANVAS_X, CANVAS_Y, btn=1, settle=0.5)
            for step in range(1, 9):
                aim(CANVAS_X + step * 14, CANVAS_Y + step * 9, btn=1, settle=0.35)
            aim(CANVAS_X + 8 * 14, CANVAS_Y + 8 * 9, btn=0, settle=3.0)
            shot("05_stroke")
            for i in range(3):
                time.sleep(60)
                shot("06_settle%02d" % i)
    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:
            pass
        time.sleep(0.5)
        q.kill()
        link.close()
    for k, v in results.items():
        print("  %-24s %s" % (k, "ok" if v else "FAILED"))
    print("shots: %s" % " ".join(shots))
    print(">> vm gimp %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
