#!/usr/bin/env python3
"""End-to-end gate for pc64_net_boot() - boot-time proactive network bring-up.

pc64_net_boot() runs at boot to bring a NIC up and take a DHCP lease WITHOUT the
debug net test, URC, or discovery. In a debug build it runs only when there is no
DEBUG.CFG (the installed-OS case); production always runs it. This isolates it:
boot a DEBUG e1000 image whose ESP has had DEBUG.CFG REMOVED, onto the netdisc L2
harness (a real broadcast segment + DHCP server, since SLIRP can't be
introspected), and assert the guest leases at boot with nothing else armed - so
the lease can only have come from pc64_net_boot. (SLIRP would also lease, but
gives us no way to see it; the L2 peer does.)

Needs UNO_DEBUG=1 ./build.sh + WSL (qemu/OVMF/mtools). Exit 0 iff the guest leases
at boot with no DEBUG.CFG.
"""
import os, sys, time, subprocess, threading, socket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq          # OVMF paths, disk geometry, mtools helpers
import netdisc_qemu as nd         # the L2 host stack: DHCP server + UNODISC peer

DISK = "/tmp/netboot_disk.img"
FAT  = "/tmp/netboot_fat.img"
SECTOR, MIB = 512, 1 << 20


def build_disk_no_cfg():
    """Same geometry as netdisc_qemu, but copy build/esp with DEBUG.CFG (and any
    legacy STRESS.CFG) REMOVED - so the guest boots with no config file at all
    and pc64_net_boot is the only thing that could raise the link."""
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
    skip = {"debug.cfg", "stress.cfg"}
    for root, dirs, files in os.walk(rq.ESP):
        rel = os.path.relpath(root, rq.ESP)
        if rel != ".":
            rq.sh(["mmd", "-i", FAT, "::/" + rel.replace(os.sep, "/")],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for fn in files:
            if rel == "." and fn.lower() in skip:      # drop the config at ESP root
                continue
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            rq.sh(["mcopy", "-i", FAT, "-o", src, dst],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
        "-netdev", "socket,id=n0,udp=127.0.0.1:%d,localaddr=127.0.0.1:%d" % (nd.HOSTPORT, nd.GUESTPORT),
        "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def main():
    if not os.path.exists(os.path.join(rq.ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)"); return 1
    had_cfg = (os.path.exists(os.path.join(rq.ESP, "DEBUG.CFG")) or
               os.path.exists(os.path.join(rq.ESP, "STRESS.CFG")))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", nd.HOSTPORT))
    sock.settimeout(0.3)
    peer = nd.Peer(sock)

    build_disk_no_cfg()
    q = boot_qemu()
    stop = threading.Event()

    def pump():
        while not stop.is_set():
            try:
                frame, tun = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                peer.feed(frame, tun)
            except Exception as e:  # noqa: BLE001
                print("  (feed error: %s)" % e)
    threading.Thread(target=pump, daemon=True).start()

    ok = True
    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and bool(cond)

    try:
        check(had_cfg, "debug ESP shipped a DEBUG.CFG (removed for this boot)")
        deadline = time.time() + 75
        while time.time() < deadline:
            if peer.leased:
                break
            time.sleep(0.3)
        check(peer.leased, "guest DHCP-leased at boot with NO DEBUG.CFG (pc64_net_boot ran)")
        # isolation: no discovery and no URC were armed (no DEBUG.CFG keys), so the
        # lease proves pc64_net_boot specifically - not discover/remote=.
        check(not peer.saw_probe, "no discovery PROBE was sent (not `discover`)", "saw_probe=%s" % peer.saw_probe)
        check(not peer.saw_syn, "no URC dial was attempted (not `remote=`)", "saw_syn=%s" % peer.saw_syn)
        print("\n>> " + ("net-boot OK" if ok else "FAILURES ABOVE"))
        return 0 if ok else 1
    finally:
        stop.set()
        try:
            q.terminate(); q.wait(timeout=10)
        except Exception:  # noqa: BLE001
            try: q.kill()
            except Exception: pass
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
