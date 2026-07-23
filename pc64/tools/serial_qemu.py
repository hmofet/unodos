#!/usr/bin/env python3
"""End-to-end gate for the NIC-INDEPENDENT (serial) unoautomate transport, in QEMU.

This is the twin of remote_qemu.py, but it proves URC works with **no network at
all**: the guest boots with a `remote-serial` STRESS.CFG and NO NIC device, and
the dev-PC end drives it over the guest's COM1, which QEMU exposes as a TCP
socket (`-serial tcp:...,server`).  This is exactly the ZimaBlade r8169 case
(Request 2, UNOAUTOMATE-REQUESTS.md): a box whose only NIC is the broken one can
still be driven live over a serial cable.

It asserts the same round trip remote_qemu does over TCP:
  1. pc64 streams LOG lines over the UART              (remote logging)
  2. host -> pc64 `probe` returns subsystem rows        (remote control)
  3. host -> pc64 `py print(6*7)` returns 42             (remote Python eval)
  4. host -> pc64 `launch 0` then `probe` shows a window  (DRIVE)

Requires a debug build first:  UNO_DEBUG=1 ./build.sh
Run under WSL (needs qemu-system-x86_64, sgdisk, mformat/mcopy, OVMF).
Exit 0 iff all four checks pass.
"""
import os, sys, subprocess, time, socket
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink

ESP  = os.path.join(HERE, "..", "build", "esp")
DISK = "/tmp/serial_disk.img"
FAT  = "/tmp/serial_fat.img"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS = "/tmp/serial_vars.fd"
SECTOR, MIB = 512, 1 << 20
SERPORT = 5401          # QEMU exposes the guest's COM1 on this TCP port


def sh(a, **k): return subprocess.run(a, **k)


def build_disk():
    cfg = os.path.join(os.path.dirname(DISK), "serial_stress.cfg")
    # `remote-serial=3e8` arms the UART transport on COM3 (0x3E8), NOT COM1/COM2:
    # the attached debug build leaves UEFI firmware alive, and OVMF's serial
    # console (SerialDxe/TerminalDxe) polls COM1 *and* COM2 for console input,
    # stealing bytes from our RX FIFO.  COM3 is not a UEFI console, so URC has the
    # wire to itself.  `nonet` skips the boot net test (no NIC anyway); no
    # `poweroff` so the guest stays up.
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote-serial=3e8\nnonet\n")
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
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK,
        # NO NIC device at all - this is the point: URC must not need the network.
        # COM1 (0x3F8) stays the firmware boot console - discard it.  URC rides
        # COM3 (0x3E8) via an explicit isa-serial device (index=2), bridged to a
        # TCP socket the host connects to.  COM3 is not a UEFI console, so the
        # still-alive firmware doesn't steal our RX bytes.  server=on lets QEMU
        # listen; wait=off boots without blocking on the host.
        "-serial", "null",
        "-chardev", "socket,id=urc,host=127.0.0.1,port=%d,server=on,wait=off" % SERPORT,
        "-device", "isa-serial,chardev=urc,index=2",
        "-display", "none",
    ]
    if os.environ.get("URC_DBGCON"):
        cmd += ["-debugcon", "file:" + os.environ["URC_DBGCON"],
                "-global", "isa-debugcon.iobase=0x402"]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def connect_serial(timeout=30.0):
    """Connect to QEMU's COM1 TCP socket, retrying until QEMU is listening."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", SERPORT), timeout=2)
            s.settimeout(None)
            return s
        except OSError:
            time.sleep(0.2)
    return None


def main():
    if not os.path.isdir(ESP) or not os.path.exists(os.path.join(ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1

    logs = []
    link = UnoAutoLink()
    link.on_log(lambda ch, t: logs.append((ch, t)))

    build_disk()
    q = boot_qemu()
    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and cond

    try:
        sock = connect_serial(30)
        check(sock is not None, "connected to guest COM1 (QEMU serial socket)")
        if not sock:
            return 1
        link.attach_stream(sock)

        # The guest owns COM1 as its firmware/boot console until UnoDOS is up, and
        # serial has no connection handshake - so wait for pc64's HELLO before
        # driving it, or early commands vanish into an unread FIFO. The device
        # re-emits HELLO every ~2 s until it hears us, so this is robust even if
        # the guest booted before we connected.
        check(link.wait_hello(90), "guest HELLO (pc64 booted + reading)")

        # 1) remote logging over the UART: at least one LOG line within a few s.
        for _ in range(100):
            if logs: break
            time.sleep(0.2)
        check(len(logs) > 0, "LOG stream received over serial", "%d line(s)" % len(logs))

        # 2) probe round-trip (the crashed run's first failing check)
        try:
            rows = link.probe(timeout=15)
            check(len(rows) > 0, "probe round-trip", "%d row(s)" % len(rows))
        except Exception as e:  # noqa: BLE001
            check(False, "probe round-trip", str(e))

        # 3) remote Python eval
        try:
            out = link.eval("print(6*7)", timeout=25)
            check(out == ["42"], "py eval print(6*7)==42", repr(out))
        except Exception as e:  # noqa: BLE001
            check(False, "py eval print(6*7)==42", str(e))

        # 4) DRIVE: launch app 0, confirm a window appears (the run's other fail)
        try:
            link.launch(0, timeout=15)
            time.sleep(0.5)
            rows = link.probe(timeout=15)
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

    print("\n" + (">> serial transport OK" if ok else ">> serial transport FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
