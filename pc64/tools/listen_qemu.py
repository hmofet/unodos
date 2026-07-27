#!/usr/bin/env python3
"""End-to-end gate for URC LISTEN mode (the dev PC dials INTO the box), in QEMU.

With `listen=<port>` in DEBUG.CFG the box is a URC SERVER: it brings its NIC up,
binds+listens, and accepts an inbound connection instead of dialing out. This
proves it: boot a debug e1000 image whose DEBUG.CFG says `listen=5099`, forward a
host port to the guest's listener with QEMU hostfwd, dial in from the host, drive
it over the same URC protocol (via UnoAutoLink.attach_stream), and confirm the
listener PERSISTS by reconnecting and driving again.

Needs UNO_DEBUG=1 ./build.sh + WSL (qemu/OVMF/mtools). Exit 0 iff all checks pass.
"""
import os, sys, time, socket, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq
from unoauto_remote import UnoAutoLink

DISK = "/tmp/listen_disk.img"
FAT  = "/tmp/listen_fat.img"
LPORT = 5099          # the guest's URC listen port (DEBUG.CFG `listen=`)
HFWD  = 5599          # host port -> guest :LPORT
SECTOR, MIB = 512, 1 << 20


def build_disk_listen():
    """build/esp with a DEBUG.CFG of `listen=<LPORT>` (server mode)."""
    disk_sectors = 96 * 2048
    with open(DISK, "wb") as f:
        f.truncate(disk_sectors * SECTOR)
    rq.sh(["sgdisk", "--zap-all", DISK], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rq.sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", DISK],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
    with open(FAT, "wb") as f:
        f.truncate(part_sectors * SECTOR)
    rq.sh(["mformat", "-i", FAT, "-F", "-T", str(part_sectors), "::"],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for root, dirs, files in os.walk(rq.ESP):
        rel = os.path.relpath(root, rq.ESP)
        if rel != ".":
            rq.sh(["mmd", "-i", FAT, "::/" + rel.replace(os.sep, "/")],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for fn in files:
            if rel == "." and fn.lower() in ("debug.cfg", "stress.cfg"):
                continue                         # replace with our own
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            rq.sh(["mcopy", "-i", FAT, "-o", src, dst], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cfg = "/tmp/listen_debug.cfg"
    with open(cfg, "w", newline="\r\n") as f:
        f.write("listen=%d\n" % LPORT)
    rq.sh(["mcopy", "-i", FAT, "-o", cfg, "::/DEBUG.CFG"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(FAT, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b:
                break
            df.write(b)


def boot_qemu():
    rq.sh(["cp", rq.OVMF_VARS, rq.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + rq.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + rq.VARS,
        "-drive", "format=raw,file=" + DISK,
        "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:%d" % (HFWD, LPORT),
        "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def connect_drive(deadline):
    """Dial into the guest's forwarded listener and drive it. Retries the whole
    connect+attach on failure - the box may still be booting, and a single dirty
    connection (e.g. one left in the accept backlog by a prior attempt) is just
    dropped and re-dialed cleanly. Returns ((uptime, probe), link, sock) or
    (None, None, None)."""
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", HFWD), timeout=3)
        except OSError:
            time.sleep(1.0); continue           # not listening yet
        link = UnoAutoLink()
        link.attach_stream(s)
        if link.wait_hello(6):                  # got the box's HELLO -> a clean link
            up = link.command("uptime", timeout=10)
            pr = link.probe(timeout=10)
            return (up, pr), link, s
        try: link.close()
        except Exception: pass
        try: s.close()
        except Exception: pass
        time.sleep(0.5)
    return None, None, None


def main():
    if not os.path.exists(os.path.join(rq.ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)"); return 1

    build_disk_listen()
    q = boot_qemu()
    ok = True
    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and bool(cond)

    try:
        res, link, s = connect_drive(time.time() + 90)
        check(res is not None, "dialed INTO the box's listener and got its HELLO (:%d)" % LPORT)
        if res is not None:
            up, pr = res
            check(len(up) == 1 and up[0].strip().isdigit(), "uptime responded over the dial-in", str(up))
            check(len(pr) > 0, "probe returned rows", "%d rows" % len(pr))
        try:
            if link: link.close()
            if s: s.close()
        except Exception: pass

        # The listener must persist: after the first client leaves, dial in again.
        time.sleep(1.0)
        res2, link2, s2 = connect_drive(time.time() + 30)
        check(res2 is not None and len(res2[0]) == 1,
              "reconnected and drove the box again (listener persists)")
        try:
            if link2: link2.close()
            if s2: s2.close()
        except Exception: pass

        print("\n>> " + ("URC listen mode OK" if ok else "FAILURES ABOVE"))
        return 0 if ok else 1
    finally:
        try:
            q.terminate(); q.wait(timeout=10)
        except Exception:  # noqa: BLE001
            try: q.kill()
            except Exception: pass


if __name__ == "__main__":
    sys.exit(main())
