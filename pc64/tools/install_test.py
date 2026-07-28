#!/usr/bin/env python3
"""UnoDOS/pc64 installer verification - QEMU + OVMF, fully headless.

Exercises the Install app end-to-end, both modes:

  python3 tools/install_test.py disk    boot the USB image + a BLANK disk,
                                        whole-disk install (clone + GPT
                                        relocation + boot entry), then REBOOT
                                        FROM THE INTERNAL DISK ONLY and verify
                                        the desktop comes up.
  python3 tools/install_test.py esp     boot the USB image + a disk that has
                                        an EXISTING FAT ESP with foreign
                                        content, ESP-install (non-destructive),
                                        verify the foreign content survived and
                                        the disk boots UnoDOS.
  python3 tools/install_test.py         both, disk first.

Prereqs (WSL/Linux): qemu-system-x86_64, OVMF, sgdisk, mtools;
build/unodos-uefi.img (python3 tools/mkuefi.py after ./build.sh).

NVRAM (build/inst-vars.fd) persists across the install boot and the from-disk
boot, so the Boot#### / BootOrder entry written by the installer is live.
"""
import json, os, re, socket, subprocess, sys, time

PC64 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(PC64)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
QMP_SOCK = "/tmp/unodos-inst-qmp.sock"
USB_IMG = "build/unodos-uefi.img"
DISK_IMG = "build/inst-disk.img"
VARS = "build/inst-vars.fd"

# The target disk must be BIGGER than the source stick: install_disk() refuses a
# target whose LastBlock < g_src_need + 33 (installer.c), where g_src_need is the
# source GPT's last used sector + 1, and it needs room to relocate the backup GPT.
# This used to be a hardcoded 256 MiB, which a 256 MiB stick can never satisfy -
# so the Install app listed the target as "[too small]", refused it, and the disk
# phase then "passed" having installed nothing (see the assertion note in
# run_phase). Derive it from the actual image instead, so the test cannot go stale
# when the stick is rebuilt at a different size.
DISK_MARGIN_MIB = 64


def disk_mib():
    usb_mib = (os.path.getsize(USB_IMG) + (1 << 20) - 1) >> 20
    return usb_mib + DISK_MARGIN_MIB


def require_prereqs():
    """Fail with a usable message instead of an unhandled ConnectionRefusedError.

    QEMU exits immediately when a -drive file is missing, so the first symptom
    used to be Qmp() failing to connect to a socket nothing was ever listening
    on - which says nothing about the actual cause."""
    missing = [p for p in (OVMF_CODE, OVMF_VARS, USB_IMG) if not os.path.exists(p)]
    if missing:
        print("FAIL: missing prerequisite(s):")
        for p in missing:
            print("   " + p)
        if USB_IMG in missing:
            print("\nBuild the USB image first:  python3 tools/mkuefi.py 256")
            print("(after ./build.sh - see the module docstring)")
        return False
    return True


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


def keys(q, *names, gap=0.12):
    for n in names:
        q.cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": 40})
        time.sleep(gap)


def combo(q, *names):
    q.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(0.2)


def tablet_route(q):
    """make the usb-tablet the 'current' mouse so untargeted abs events reach
    it (device= on input-send-event aborts QEMU 8.2 under -display none)"""
    mice = q.cmd("query-mice").get("return", [])
    for m in mice:
        if m.get("absolute"):
            q.cmd("human-monitor-command",
                  **{"command-line": "mouse_set %d" % m["index"]})
            return True
    return False


def mouse_move(q, x, y):
    # coordinates are fb pixels (640x400); abs range 0..32767 spans the panel
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 640)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 400)}}])
    time.sleep(0.1)


def click_fb(q, x, y):
    mouse_move(q, x, y)
    q.cmd("input-send-event",
          events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(0.15)
    q.cmd("input-send-event",
          events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(0.15)


def shot(q, tag):
    ppm = "shots/%s.ppm" % tag
    q.cmd("screendump", filename=ppm)
    time.sleep(0.4)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag],
                   check=True)
    os.remove(ppm)
    print("shot: shots/%s.png" % tag)


def start_qemu(with_usb):
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "256"]
    if os.environ.get("UNO_KVM"):          # devbuntu: hardware acceleration
        argv += ["-enable-kvm", "-cpu", "host"]
    argv += [
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK_IMG,          # internal AHCI disk
        "-device", "qemu-xhci", "-device", "usb-tablet,id=tab",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/inst-ovmf.log",
        "-global", "isa-debugcon.iobase=0x402",
    ]
    if with_usb:
        argv += ["-drive", "if=none,id=us,format=raw,file=" + USB_IMG,
                 "-device", "usb-storage,drive=us"]
    return subprocess.Popen(argv, stderr=open("build/inst-qemu.log", "ab"))


def menu_apps():
    """The Start-menu order, read from the shell's OWN table.

    These positions were hardcoded, and they drift: dropping the Network app
    (3aa37d1) moved Install and this test then opened whatever now sat at index 6
    - Music - so it drove the wrong app for weeks. Nothing noticed, because the
    disk phase asserted nothing. Derive from kAppNames so both sides share one
    source of truth."""
    with open("pc64_uui.c", encoding="utf-8", errors="replace") as f:
        src = f.read()
    m = re.search(r"kAppNames\[NNATIVE\]\s*=\s*\{(.*?)\}", src, re.S)
    if not m:
        raise RuntimeError("cannot find kAppNames[] in pc64_uui.c")
    return re.findall(r'"([^"]+)"', m.group(1))


def open_install(q):
    idx = menu_apps().index("Install")     # menu order = app order
    combo(q, "ctrl", "esc")                # Start menu
    time.sleep(0.8)
    keys(q, *(["down"] * idx))
    keys(q, "ret")
    time.sleep(1.5)


# Install window fixed at fb (150, 60), 400x286.  Button centres calibrated
# against shots/inst_disk_win.png (Aurora theme content origin ~(8, 39)).
BTN_INSTALL = (503, 278)
BTN_RESCAN = (220, 278)


def type_text(q, s):
    for ch in s:
        keys(q, ch.lower())
        time.sleep(0.10)


def run_install(q, tag, double_confirm):
    shot(q, tag + "_win")
    if double_confirm:
        # Whole-disk install is gated on TYPING the word ERASE into a confirm box
        # (see tools/install_confirm_test.py, which is the spec for this gate).
        # Two bare `i` presses no longer commit anything - which is what this test
        # was still doing, so it armed nothing and installed nothing.
        keys(q, "c")                       # C puts the caret in the confirm box
        time.sleep(0.5)
        type_text(q, "erase")
        time.sleep(0.4)
        keys(q, "esc")                     # leave the box; accelerators return
        time.sleep(0.4)
        shot(q, tag + "_armed")
        keys(q, "i")                       # arms
        time.sleep(0.8)
        keys(q, "i")                       # commits
    else:
        keys(q, "i")                       # ESP install: single accelerator
    # the copy runs synchronously; poll with shots until it settles
    time.sleep(4)
    for i in range(24):
        time.sleep(5)
        try:
            q.cmd("query-status")
        except Exception:
            break
    shot(q, tag + "_done")


def phase_boot_from_disk(tag):
    """Boot the internal disk ALONE and assert UnoDOS actually comes up.

    Returns True/False - it used to return nothing and the caller ignored it,
    so a disk that dropped straight to the UEFI shell still "passed"."""
    ok = False
    qemu = start_qemu(with_usb=False)
    try:
        q = Qmp(QMP_SOCK)
        print("from-disk boot; waiting...")
        time.sleep(20)
        ok = shot_assert_desktop(q, tag + "_fromdisk")
        # decoupling proof: apps are .UNO modules, so opening one on the
        # installed system exercises the loader against the installed volume
        # (\EFI\UNODOS\APPS on an ESP install, the cloned APPS\ on whole-disk).
        combo(q, "ctrl", "esc")            # Start menu
        time.sleep(0.8)
        # first entry past the native apps = the first legacy (.UNO) app, Dostris
        keys(q, *(["down"] * len(menu_apps())))
        keys(q, "ret")
        time.sleep(2.0)
        shot(q, tag + "_fromdisk_app")
        q.cmd("quit")
    finally:
        qemu.wait(timeout=15)
    return ok


def part_extent(img):
    """(first_lba, sectors) of GPT partition entry 1, or None if there isn't one.

    Read from the disk rather than recomputed from a constant: after a whole-disk
    clone the ESP is the SOURCE stick's size, not the target's, so any arithmetic
    based on the target geometry extracts the wrong byte range."""
    with open(img, "rb") as f:
        f.seek(2 * 512)
        e = f.read(128)
    if len(e) < 128 or e[:16] == b"\x00" * 16:
        return None
    first = int.from_bytes(e[32:40], "little")
    last = int.from_bytes(e[40:48], "little")
    return (first, last - first + 1) if last >= first else None


def extract_part(img, dest):
    ext = part_extent(img)
    if not ext:
        return False
    first, sectors = ext
    with open(img, "rb") as f:
        f.seek(first * 512)
        data = f.read(sectors * 512)
    with open(dest, "wb") as f:
        f.write(data)
    return True


def ppm_ink(path):
    """Fraction of non-black pixels in a P6 PPM, and mean luminance.

    Distinguishes "the installed disk booted UnoDOS" from "it dropped to the UEFI
    shell", with no image library. The shell is a black screen with a few lines of
    text (~3% ink); any desktop, light or dark, covers essentially the whole frame.
    Ink fraction is used for the verdict because it survives a theme change in a
    way that mean brightness would not."""
    with open(path, "rb") as f:
        data = f.read()
    fields, i = [], 2                       # skip the "P6" magic
    while len(fields) < 3 and i < len(data):
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":           # comment line
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    px = data[i + 1:]                       # single whitespace byte after maxval
    ink = lum = n = 0
    for k in range(0, len(px) - 2, 3 * 97):  # sample: 1 MPx is plenty at 1/97
        r, g, b = px[k], px[k + 1], px[k + 2]
        if r > 8 or g > 8 or b > 8:
            ink += 1
        lum += r + g + b
        n += 1
    return (ink / float(n), lum / (3.0 * n)) if n else (0.0, 0.0)


def shot_assert_desktop(q, tag):
    """screenshot + assert the frame actually shows a booted desktop"""
    ppm = "shots/%s.ppm" % tag
    q.cmd("screendump", filename=ppm)
    time.sleep(0.4)
    ink, lum = ppm_ink(ppm)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag],
                   check=True)
    os.remove(ppm)
    ok = ink >= 0.50
    print("shot: shots/%s.png  ink %.0f%% luma %.0f  -> %s"
          % (tag, ink * 100, lum,
             "desktop" if ok else "NOT A DESKTOP (UEFI shell / black screen)"))
    return ok


def make_blank_disk():
    with open(DISK_IMG, "wb") as f:
        f.truncate(disk_mib() * 1024 * 1024)


def make_esp_disk():
    """a disk with an existing GPT + FAT32 ESP holding foreign content"""
    make_blank_disk()
    subprocess.run(["sgdisk", "--zap-all", DISK_IMG], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:OTHER-ESP",
                    DISK_IMG], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fat = "/tmp/uno_inst_esp.img"
    part_sectors = part_extent(DISK_IMG)[1]
    subprocess.run(["mformat", "-C", "-i", fat, "-T", str(part_sectors),
                    "-h", "64", "-s", "32", "-F", "-v", "OTHEROS", "::"], check=True)
    subprocess.run(["mmd", "-i", fat, "::/EFI"], check=True)
    subprocess.run(["mmd", "-i", fat, "::/EFI/OTHER"], check=True)
    with open("/tmp/uno_marker.txt", "w") as f:
        f.write("foreign OS data - must survive the install\n")
    subprocess.run(["mcopy", "-i", fat, "/tmp/uno_marker.txt", "::/EFI/OTHER/MARKER.TXT"],
                   check=True)
    with open(fat, "rb") as f:
        data = f.read()
    with open(DISK_IMG, "r+b") as f:
        f.seek(2048 * 512)
        f.write(data)
    os.remove(fat)


APPS = ["DOSTRIS", "PACMAN", "OUTLAST", "MUSIC", "TRACKER", "PAINT", "NETWORK"]


def verify_paths(label, paths):
    """offline (mtools) check that the installed volume holds `paths`"""
    fat = "/tmp/uno_inst_esp.img"
    if not extract_part(DISK_IMG, fat):
        print("[%s] FAIL: no GPT partition on the target - nothing was installed" % label)
        return False
    ok = True
    for path in paths:
        r = subprocess.run(["mdir", "-i", fat, path], capture_output=True)
        print("%-40s %s" % (path, "OK" if r.returncode == 0 else "MISSING"))
        ok = ok and r.returncode == 0
    os.remove(fat)
    return ok


def verify_esp_disk():
    """post-install: foreign marker intact + \\EFI\\UNODOS\\BOOTX64.EFI present"""
    return verify_paths("esp", ["::/EFI/OTHER/MARKER.TXT", "::/EFI/UNODOS/BOOTX64.EFI"] +
                        ["::/EFI/UNODOS/APPS/%s.UNO" % a for a in APPS])


def verify_disk_clone():
    """post-install (whole disk): the clone boots via the removable-media path,
    so \\EFI\\BOOT\\BOOTX64.EFI and the APPS\\ modules must be on the target."""
    return verify_paths("disk", ["::/EFI/BOOT/BOOTX64.EFI"] +
                        ["::/APPS/%s.UNO" % a for a in APPS])


def run_phase(mode):
    subprocess.run(["cp", OVMF_VARS, VARS], check=True)
    if mode == "disk":
        make_blank_disk()
    else:
        make_esp_disk()
    qemu = start_qemu(with_usb=True)
    try:
        q = Qmp(QMP_SOCK)
        print("[%s] USB boot; waiting for the desktop..." % mode)
        time.sleep(20)
        tablet_route(q)
        shot(q, "inst_%s_desktop" % mode)
        open_install(q)
        run_install(q, "inst_" + mode, double_confirm=(mode == "disk"))
        q.cmd("quit")
    finally:
        qemu.wait(timeout=15)
    # Offline verify BOTH modes. The disk phase used to check nothing at all and
    # then return True unconditionally, so it stayed green while the Install app
    # was refusing the target outright ("[too small]") and installing nothing.
    verified = verify_esp_disk() if mode == "esp" else verify_disk_clone()
    if not verified:
        print("[%s] OFFLINE VERIFY FAILED" % mode)
        return False
    booted = phase_boot_from_disk("inst_" + mode)
    if not booted:
        print("[%s] FROM-DISK BOOT FAILED - the installed disk did not reach the desktop" % mode)
        return False
    print("[%s] PASS - installed, and the disk boots UnoDOS on its own" % mode)
    return True


def main():
    if not require_prereqs():
        sys.exit(2)
    os.makedirs("shots", exist_ok=True)
    modes = sys.argv[1:] or ["disk", "esp"]
    for m in modes:
        if not run_phase(m):
            print(">> install-test FAILED (%s)" % m)
            sys.exit(1)
    print(">> install-test OK (%s)" % ", ".join(modes))


if __name__ == "__main__":
    main()
