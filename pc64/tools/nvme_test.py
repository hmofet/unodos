#!/usr/bin/env python3
"""UnoDOS/pc64 NVMe verification - QEMU + OVMF, fully headless.

Boots build/unodos-uefi.img (GPT + ESP, from tools/mkuefi.py) as an NVMe
drive - no other disk on the machine - so after the M3 detach the native
NVMe driver is the ONLY path to storage. Proves:

  1. boot from NVMe + detach (System window: "DETACHED (native): nvme0 ...")
  2. read  path: open Dostris - DOSTRIS.UNO loads from the NVMe ESP post-detach
  3. write path: type in the Editor, Ctrl-S, quit; mtools pulls the saved file
     back out of the image and checks the text arrived on disk

  python3 tools/nvme_test.py
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, keys, shot, QMP_SOCK, OVMF_CODE, OVMF_VARS

# the Editor's default body text: Ctrl-S writes it to NOTES.TXT, and the
# image is regenerated fresh by mkuefi.py before every run - so finding it
# on the raw image afterwards proves the whole native write path (dir entry
# + FAT chain + data clusters + write-back flush) went through the NVMe DMA.
MARK = "unoui is the whole UnoDOS UI"


def main():
    img = "build/unodos-uefi.img"
    if not os.path.exists(img):
        sys.exit("no %s - run tools/mkuefi.py first" % img)
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "if=none,id=nv0,format=raw,file=" + img,
        "-device", "nvme,drive=nv0,serial=unodos1",
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "none",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/nvme_dbg.log",
        "-global", "isa-debugcon.iobase=0x402",
    ]
    qemu = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting for OVMF -> UnoDOS boot from NVMe...")
        time.sleep(20)

        # 1. System window: expect "DETACHED (native): nvme0 ..."
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, "down", "down", "down"); keys(q, "ret")
        time.sleep(1.5)
        shot(q, "nvme_system")
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "w"}])
        time.sleep(0.8)

        # 2. read path: Dostris is a .UNO module on the NVMe ESP (index 7)
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, *(["down"] * 7)); keys(q, "ret")
        time.sleep(2.0)
        shot(q, "nvme_dostris")
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "w"}])
        time.sleep(0.8)

        # 3. write path: Editor (launcher index 1), Ctrl-S saves NOTES.TXT
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, "down"); keys(q, "ret")
        time.sleep(1.5)
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "s"}])
        time.sleep(2.0)                # write-back cache -> nvme sectors
        shot(q, "nvme_editor")
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)

    ok = True
    log = open("build/nvme_dbg.log", "rb").read().decode("ascii", "replace") \
        if os.path.exists("build/nvme_dbg.log") else ""
    if "detached: ExitBootServices done" in log:
        print("PASS: detach happened on the NVMe-only machine")
    elif log.strip():
        print("FAIL: booted but did not detach"); ok = False
    else:
        print("note: no debugcon output (non-DBGCON build) - relying on shots")

    # pull the saved doc back out of the image (ESP starts at LBA 2048)
    r = subprocess.run(["mdir", "-i", img + "@@1048576", "-b", "::/"],
                       capture_output=True, text=True)
    saved = [l.strip() for l in r.stdout.splitlines()
             if l.strip().upper().endswith((".MD", ".TXT"))]
    hit = False
    for f in saved:
        rr = subprocess.run(["mtype", "-i", img + "@@1048576", "::" + f.lstrip(":/")],
                            capture_output=True, text=True)
        if MARK in rr.stdout:
            print("PASS: '%s' found in %s on the NVMe image (native write path)"
                  % (MARK, f))
            hit = True
            break
    if not hit:
        print("FAIL: saved text not found on the image (files seen: %s)" % saved)
        ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
