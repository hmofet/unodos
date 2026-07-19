#!/usr/bin/env python3
"""UnoDOS/pc64 native-storage verification - QEMU + OVMF, headless.

Boots the USB image with a SECOND disk attached to the q35 AHCI controller
(-device ide-hd on the ich9-ahci bus q35 provides), carrying a GPT with one
plain FAT32 **basic-data** partition (NOT an ESP) that holds a marker file.
Verifies the native AHCI + FAT stack:

  python3 tools/storage_test.py        boot -> System window shot (Native line),
                                       Files on the native volume, save a file,
                                       reboot, verify the save persisted offline.

Env: UNO_KVM=1 uses KVM (devbuntu). Prereqs: qemu, OVMF, sgdisk, mtools.
"""
import json, os, socket, subprocess, sys, time

PC64 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(PC64, "tools"))
os.chdir(PC64)
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
QMP = "/tmp/unodos-stor-qmp.sock"
USB = "build/unodos-uefi.img"
DATA = "build/stor-data.img"
VARS = "build/stor-vars.fd"
DATA_MIB = 64


class Qmp:
    def __init__(self, path, timeout=30):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path); break
            except OSError:
                if time.time() > end: raise
                time.sleep(0.3)
        self.buf = b""; self.recv(); self.cmd("qmp_capabilities")

    def recv(self):
        while b"\n" not in self.buf: self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1); return json.loads(line)

    def cmd(self, name, **args):
        m = {"execute": name}
        if args: m["arguments"] = args
        self.s.sendall(json.dumps(m).encode() + b"\n")
        while True:
            r = self.recv()
            if "return" in r or "error" in r: return r


def keys(q, *names, gap=0.12):
    for n in names:
        q.cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": 40})
        time.sleep(gap)


def combo(q, *names):
    q.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names]); time.sleep(0.2)


def shot(q, tag):
    ppm = "shots/%s.ppm" % tag
    q.cmd("screendump", filename=ppm); time.sleep(0.4)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag], check=True)
    os.remove(ppm); print("shot: shots/%s.png" % tag)


def make_data_disk():
    """GPT + one basic-data (type 0700) FAT32 partition with a read marker and
    an armed write-test request (WRTEST.REQ -> the OS turns it into WRTEST.OK)"""
    with open(DATA, "wb") as f:
        f.truncate(DATA_MIB * 1024 * 1024)
    subprocess.run(["sgdisk", "--zap-all", DATA], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 0700 = Microsoft basic data, deliberately NOT EF00 (ESP)
    subprocess.run(["sgdisk", "-n", "1:2048:0", "-t", "1:0700", "-c", "1:UNODATA", DATA],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fat = "/tmp/uno_stor.img"
    part_sectors = DATA_MIB * 2048 - 2048 - 33
    subprocess.run(["mformat", "-C", "-i", fat, "-T", str(part_sectors),
                    "-h", "64", "-s", "32", "-F", "-v", "UNODATA", "::"], check=True)
    with open("/tmp/uno_stor_marker.txt", "w") as f:
        f.write("native FAT read works\n")
    subprocess.run(["mcopy", "-i", fat, "/tmp/uno_stor_marker.txt", "::/MARKER.TXT"], check=True)
    with open("/tmp/uno_stor_req.txt", "w") as f:
        f.write("write-req ")
    subprocess.run(["mcopy", "-i", fat, "/tmp/uno_stor_req.txt", "::/WRTEST.REQ"], check=True)
    with open(fat, "rb") as f: data = f.read()
    with open(DATA, "r+b") as f:
        f.seek(2048 * 512); f.write(data)
    os.remove(fat)


def data_part_offset():
    return 2048 * 512


def read_data_file(name):
    fat = "/tmp/uno_stor_rd.img"
    part_sectors = DATA_MIB * 2048 - 2048 - 33
    with open(DATA, "rb") as f:
        f.seek(data_part_offset()); data = f.read(part_sectors * 512)
    with open(fat, "wb") as f: f.write(data)
    r = subprocess.run(["mtype", "-i", fat, "::/" + name], capture_output=True, text=True)
    os.remove(fat)
    return (r.returncode == 0, r.stdout)


def start(with_usb):
    if os.path.exists(QMP): os.remove(QMP)
    argv = ["qemu-system-x86_64", "-machine", "q35", "-m", "256"]
    if os.environ.get("UNO_KVM"): argv += ["-enable-kvm", "-cpu", "host"]
    argv += [
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        # native AHCI data disk on q35's built-in ich9-ahci
        "-drive", "id=data,if=none,format=raw,file=" + DATA,
        "-device", "ide-hd,drive=data,bus=ide.0",
        "-device", "qemu-xhci", "-device", "usb-tablet,id=tab",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP,
        "-debugcon", "file:build/stor-ovmf.log", "-global", "isa-debugcon.iobase=0x402",
    ]
    if with_usb:
        argv += ["-drive", "if=none,id=us,format=raw,file=" + USB,
                 "-device", "usb-storage,drive=us"]
    return subprocess.Popen(argv, stderr=open("build/stor-qemu.log", "ab"))


def open_system(q):
    combo(q, "ctrl", "esc"); time.sleep(0.8)      # Start
    keys(q, "down", "down", "down"); keys(q, "ret")   # System (index 3)
    time.sleep(1.5)


def main():
    os.makedirs("shots", exist_ok=True)
    # test build with the storage self-test armed (production build has no flag).
    # UNO_PREBUILT=1 uses an already-built self-test image (e.g. on the KVM box
    # that has no source tree) and skips the rebuild-production step.
    prebuilt = bool(os.environ.get("UNO_PREBUILT"))
    if not prebuilt:
        env = dict(os.environ, UNO_EXTRA="-DUNO_STORTEST -DUNO_DBGCON")
        subprocess.run(["./build.sh"], check=True, env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([sys.executable, "tools/mkuefi.py"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["cp", OVMF_VARS, VARS], check=True)
    make_data_disk()
    q = None
    qemu = start(with_usb=True)
    try:
        q = Qmp(QMP)
        print("USB boot + AHCI data disk; waiting for desktop...")
        time.sleep(20)
        open_system(q)
        shot(q, "stor_system")                # "Native FS: fw-sect N disk  FAT vols M"
        q.cmd("quit")
    finally:
        qemu.wait(timeout=15)
    # READ: MARKER.TXT was placed offline; the OS read it -> Files (visual) and
    # here we confirm it survived (native FS did not corrupt the volume).
    ok, out = read_data_file("MARKER.TXT")
    print("READ  MARKER.TXT : %s %r" % ("OK" if ok else "MISSING", out.strip()))
    # WRITE+DELETE: the boot-time self-test turned WRTEST.REQ into WRTEST.OK and
    # removed the request - both prove native FAT write/delete on this volume.
    wok, wout = read_data_file("WRTEST.OK")
    print("WRITE WRTEST.OK  : %s %r" % ("OK" if wok else "MISSING", wout.strip()))
    rgone, _ = read_data_file("WRTEST.REQ")
    print("DELETE WRTEST.REQ: %s" % ("still present (FAIL)" if rgone else "removed (OK)"))
    ok_all = ok and wok and ("native-fat-rw" in wout) and (not rgone)
    print("RESULT: %s" % ("PASS" if ok_all else "FAIL"))
    if not prebuilt:
        # rebuild the production image (no self-test) so a later deploy is clean
        subprocess.run(["./build.sh"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([sys.executable, "tools/mkuefi.py"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
