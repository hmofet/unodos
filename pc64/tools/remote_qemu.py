#!/usr/bin/env python3
"""End-to-end gate for the unoautomate remote channel, in QEMU.

Boots the DEBUG image with a `remote=10.0.2.2:<port>` STRESS.CFG on a SLIRP
e1000 NIC, runs the dev-PC listener (unoauto_remote.UnoAutoLink) on the host,
and proves the round trip:
  1. pc64 dials in and streams LOG lines            (remote logging)
  2. host -> pc64 `probe` returns subsystem rows      (remote control)
  3. host -> pc64 `py print(6*7)` returns 42           (remote Python eval)
  4. host -> pc64 `launch 0` then `probe` shows a window (DRIVE)

Requires a debug build first:  UNO_DEBUG=1 ./build.sh
Run under WSL (needs qemu-system-x86_64, sgdisk, mformat/mcopy, OVMF), so that
the guest's SLIRP 10.0.2.2 maps to this process's loopback.  Exit 0 iff all
four checks pass.
"""
import os, sys, subprocess, time, threading
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink

ESP  = os.path.join(HERE, "..", "build", "esp")
DISK = "/tmp/remote_disk.img"
FAT  = "/tmp/remote_fat.img"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS = "/tmp/remote_vars.fd"
SECTOR, MIB = 512, 1 << 20
PORT = 5399


def sh(a, **k): return subprocess.run(a, **k)


def build_disk():
    cfg = os.path.join(os.path.dirname(DISK), "remote_stress.cfg")
    # remote=<host>:<port> arms the dev-PC link; `nonet` skips the slow boot
    # net test (the link brings the NIC up itself); no `poweroff` so the guest
    # stays up for us to drive.
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote=10.0.2.2:%d\nnonet\n" % PORT)
    disk_sectors = 96 * 2048
    with open(DISK, "wb") as f: f.truncate(disk_sectors * SECTOR)
    sh(["sgdisk", "--zap-all", DISK], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", DISK],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
    with open(FAT, "wb") as f: f.truncate(part_sectors * SECTOR)
    sh(["mformat", "-i", FAT, "-F", "-T", str(part_sectors), "::"],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for root, dirs, files in os.walk(ESP):
        rel = os.path.relpath(root, ESP)
        if rel != ".":
            sh(["mmd", "-i", FAT, "::/" + rel.replace(os.sep, "/")],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for fn in files:
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            sh(["mcopy", "-i", FAT, "-o", src, dst],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sh(["mcopy", "-i", FAT, "-o", cfg, "::/STRESS.CFG"],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(FAT, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b: break
            df.write(b)


def boot_qemu():
    sh(["cp", OVMF_VARS, VARS])
    return subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
    ], stderr=subprocess.DEVNULL)


def main():
    if not os.path.isdir(ESP) or not os.path.exists(os.path.join(ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1

    logs = []
    link = UnoAutoLink("127.0.0.1", PORT)
    link.on_log(lambda ch, t: logs.append((ch, t)))
    link.listen()

    build_disk()
    q = boot_qemu()
    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and cond

    try:
        connected = link.wait_connected(90)
        check(connected, "pc64 dialed in")
        if not connected:
            return 1
        # 1) remote logging: HELLO + at least one LOG line within a few seconds
        for _ in range(50):
            if logs: break
            time.sleep(0.2)
        check(len(logs) > 0, "LOG stream received", "%d line(s)" % len(logs))

        # 2) probe round-trip
        try:
            rows = link.probe(timeout=10)
            check(len(rows) > 0, "probe round-trip", "%d row(s)" % len(rows))
        except Exception as e:  # noqa: BLE001
            check(False, "probe round-trip", str(e))

        # 3) remote Python eval
        try:
            out = link.eval("print(6*7)", timeout=25)
            check(out == ["42"], "py eval print(6*7)==42", repr(out))
        except Exception as e:  # noqa: BLE001
            check(False, "py eval print(6*7)==42", str(e))

        # 4) DRIVE: launch app 0, confirm a window appears in a fresh probe
        try:
            link.launch(0, timeout=10)
            time.sleep(0.5)
            rows = link.probe(timeout=10)
            wins = [r for r in rows if r["kind"] == 1]
            check(len(wins) > 0, "launch -> window visible in probe",
                  "%d window row(s)" % len(wins))
        except Exception as e:  # noqa: BLE001
            check(False, "launch -> window visible in probe", str(e))

    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.5)
        q.kill()
        link.close()

    print("\n" + (">> remote channel OK" if ok else ">> remote channel FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
