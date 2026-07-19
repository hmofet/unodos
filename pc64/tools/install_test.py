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
import json, os, socket, subprocess, sys, time

PC64 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(PC64)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
QMP_SOCK = "/tmp/unodos-inst-qmp.sock"
USB_IMG = "build/unodos-uefi.img"
DISK_IMG = "build/inst-disk.img"
VARS = "build/inst-vars.fd"
DISK_MIB = 256


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
    ]
    if with_usb:
        argv += ["-drive", "if=none,id=us,format=raw,file=" + USB_IMG,
                 "-device", "usb-storage,drive=us"]
    return subprocess.Popen(argv, stderr=open("build/inst-qemu.log", "ab"))


def open_install(q):
    combo(q, "ctrl", "esc")                # Start menu
    time.sleep(0.8)
    keys(q, *(["down"] * 6))               # menu order = app order; Install = 6
    keys(q, "ret")
    time.sleep(1.5)


# Install window fixed at fb (150, 60), 400x286.  Button centres calibrated
# against shots/inst_disk_win.png (Aurora theme content origin ~(8, 39)).
BTN_INSTALL = (503, 278)
BTN_RESCAN = (220, 278)


def run_install(q, tag, double_confirm):
    shot(q, tag + "_win")
    keys(q, "i")                           # Install (keyboard accelerator)
    time.sleep(0.5)
    if double_confirm:
        shot(q, tag + "_armed")
        keys(q, "i")                       # confirm the erase
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
    qemu = start_qemu(with_usb=False)
    try:
        q = Qmp(QMP_SOCK)
        print("from-disk boot; waiting...")
        time.sleep(20)
        shot(q, tag + "_fromdisk")
        q.cmd("quit")
    finally:
        qemu.wait(timeout=15)


def make_blank_disk():
    with open(DISK_IMG, "wb") as f:
        f.truncate(DISK_MIB * 1024 * 1024)


def make_esp_disk():
    """a disk with an existing GPT + FAT32 ESP holding foreign content"""
    make_blank_disk()
    subprocess.run(["sgdisk", "--zap-all", DISK_IMG], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:OTHER-ESP",
                    DISK_IMG], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fat = "/tmp/uno_inst_esp.img"
    part_sectors = DISK_MIB * 2048 - 2048 - 33
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


def verify_esp_disk():
    """post-install: foreign marker intact + \\EFI\\UNODOS\\BOOTX64.EFI present"""
    fat = "/tmp/uno_inst_esp.img"
    part_sectors = DISK_MIB * 2048 - 2048 - 33
    with open(DISK_IMG, "rb") as f:
        f.seek(2048 * 512)
        data = f.read(part_sectors * 512)
    with open(fat, "wb") as f:
        f.write(data)
    ok = True
    for path in ["::/EFI/OTHER/MARKER.TXT", "::/EFI/UNODOS/BOOTX64.EFI"]:
        r = subprocess.run(["mdir", "-i", fat, path], capture_output=True)
        print("%-28s %s" % (path, "OK" if r.returncode == 0 else "MISSING"))
        ok = ok and r.returncode == 0
    os.remove(fat)
    return ok


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
    if mode == "esp" and not verify_esp_disk():
        print("[esp] OFFLINE VERIFY FAILED")
        return False
    phase_boot_from_disk("inst_" + mode)
    return True


def main():
    os.makedirs("shots", exist_ok=True)
    modes = sys.argv[1:] or ["disk", "esp"]
    for m in modes:
        if not run_phase(m):
            sys.exit(1)


if __name__ == "__main__":
    main()
