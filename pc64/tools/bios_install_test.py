#!/usr/bin/env python3
"""Install onto a blank disk from a BIOS boot, then boot THAT disk alone.

    python3 tools/bios_install_test.py

The BIOS sibling of tools/install_test.py, and the only test that can prove the
installer's legacy target, because the thing being verified is not "did the
install report success" but "does the resulting disk boot on its own". Those are
different claims, and this port has already produced one of them without the
other: phase B booted a desktop off an image with no filesystem.

So the run is two boots:

  1. SeaBIOS, hybrid image as disk 0 + a blank disk 1. Drive the Install app to
     a whole-disk install onto disk 1 (MBR + 0xEF partition + the boot chain in
     the reserved area, see unostorage_prepare_mbr / write_bootchain).
  2. SeaBIOS, THE INSTALLED DISK AS THE ONLY DISK. If the desktop comes up, the
     boot sector, stage2, the kernel, the partition table and the copied tree
     are all correct together - which is what "installed" has to mean.

Prereq: ./build.sh  (produces build/unodos-hybrid.img).
"""
import os
import subprocess
import sys
import time

PC64 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(PC64)
sys.path.insert(0, PC64)
from harness import Qmp, QMP_SOCK  # noqa: E402

SRC = "build/unodos-hybrid.img"
TARGET = "build/bios_installed.img"
TARGET_MIB = 256


def keys(q, *names, gap=0.12):
    for n in names:
        q.cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": 40})
        time.sleep(gap)


def combo(q, *names):
    q.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(0.3)


def shot(q, tag):
    ppm = os.path.abspath("build/%s.ppm" % tag)
    q.cmd("screendump", filename=ppm)
    time.sleep(0.8)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag],
                   check=True)
    os.remove(ppm)
    print("shot: shots/%s.png" % tag)


def boot(disks, wait):
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = ["qemu-system-x86_64", "-machine", "pc", "-cpu", "core2duo",
            "-m", "512", "-display", "none", "-no-reboot",
            "-qmp", "unix:%s,server,nowait" % QMP_SOCK]
    for d in disks:
        argv += ["-drive", "format=raw,if=ide,file=" + d]
    print("+ " + " ".join(argv))
    p = subprocess.Popen(argv)
    q = Qmp(QMP_SOCK)
    time.sleep(wait)
    return p, q


def stop(p):
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


def main():
    if not os.path.exists(SRC):
        sys.exit("no %s - run ./build.sh" % SRC)
    with open(TARGET, "wb") as f:          # a blank disk, every run
        f.truncate(TARGET_MIB * 1024 * 1024)

    # ---- boot 1: install ------------------------------------------------
    p, q = boot([SRC, TARGET], 20)
    try:
        combo(q, "ctrl", "esc")            # Start menu
        time.sleep(1.0)
        # menu order is kAppNames order; Install is index 5
        keys(q, *(["down"] * 5))
        keys(q, "ret")
        time.sleep(2.0)
        shot(q, "bios_inst_win")

        # PICK THE TARGET. The list is: the running system's own volume, the
        # disk it lives on, then any other disk. Leaving the default selected
        # asks the installer to install onto itself, which it correctly refuses
        # with "cannot find the running system's volume" - a refusal that reads
        # like a bug if you are not expecting it. Two downs reach the blank
        # second disk.
        keys(q, "down", "down")
        time.sleep(0.5)

        # The whole-disk install is gated on typing ERASE, then two `i` presses
        # (arm, commit) - the same gate tools/install_confirm_test.py specifies.
        keys(q, "c")
        time.sleep(0.5)
        for ch in "erase":
            keys(q, ch)
        time.sleep(0.4)
        keys(q, "esc")
        time.sleep(0.4)
        keys(q, "i")
        time.sleep(1.0)
        keys(q, "i")

        # WAIT FOR THE CHAIN, NOT FOR A DURATION. The copy is tens of MB over
        # PIO ATA and takes as long as it takes; a fixed sleep that expires
        # mid-copy leaves a disk with a formatted volume, a partial tree and the
        # zeroed boot sector prepare_mbr wrote - which then "boots" to a hang
        # and looks exactly like a broken boot chain rather than an unfinished
        # install. QEMU writes through to the raw file, so the honest completion
        # signal is the boot sector appearing in it: bios_write_chain runs last,
        # after the tree and after uno_fat_sync.
        deadline = time.time() + 300
        done = False
        while time.time() < deadline:
            time.sleep(5)
            try:
                with open(TARGET, "rb") as f:
                    if b"UnoDOS pc64" in f.read(512):
                        done = True
                        break
            except OSError:
                pass
        shot(q, "bios_inst_done")
        if not done:
            print("!! the boot chain never reached the target disk")
    finally:
        stop(p)

    # ---- boot 2: the installed disk, ALONE -------------------------------
    p, q = boot([TARGET], 22)
    try:
        shot(q, "bios_inst_booted")
    finally:
        stop(p)
    print("\nRead shots/bios_inst_booted.png: a desktop there means the "
          "installed disk boots on its own.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
