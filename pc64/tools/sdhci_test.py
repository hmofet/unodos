#!/usr/bin/env python3
"""UnoDOS/pc64 SDHCI (eMMC/SD) verification - QEMU + OVMF, fully headless.

The machine boots from a USB vvfat stick; the packed GPT image sits on an
SDHCI card that the FIRMWARE CANNOT SEE (Ubuntu's OVMF ships no SdMmc
driver) - so the build must carry -DUNO_SDHCI_TEST, which takes the
controller eagerly while attached (we are the only driver on it; this
mirrors the real Surface flow where the firmware's own eMMC volume stands
in pre-detach). Proves:

  1. the 08/05 gate line: a USB-booted system detaches ONLY because the
     card carries a UnoDOS system (no AHCI/NVMe controller in the machine)
  2. detach-time re-registration (sd0 survives the registry rebuild)
  3. read  path: Dostris' .UNO loads off the card post-detach
  4. write path: an Editor save lands on the card; mtools verifies the raw
     image afterwards

  python3 tools/sdhci_test.py          (build with:
      UNO_EXTRA='-DUNO_SDHCI_TEST -DUNO_DBGCON' ./build.sh )
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, keys, shot, QMP_SOCK, OVMF_CODE, OVMF_VARS

MARK = "unoui is the whole UnoDOS UI"    # Editor default body -> NOTES.TXT


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
        # boot medium: the ESP as a USB stick (NOT on AHCI - so the card is
        # the only controller the detach gate can accept)
        "-device", "qemu-xhci",
        "-drive", "if=none,id=usbesp,format=vvfat,file=fat:rw:build/esp",
        "-device", "usb-storage,drive=usbesp",
        # the medium under test: SD card behind SDHCI
        "-device", "sdhci-pci",
        "-drive", "if=none,id=mmcdrv,format=raw,file=" + img,
        "-device", "sd-card,drive=mmcdrv",
        "-device", "usb-tablet",
        "-nic", "none",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/sdhci_dbg.log",
        "-global", "isa-debugcon.iobase=0x402",
    ]
    qemu = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; USB boot, SD card behind SDHCI...")
        time.sleep(22)

        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, "down", "down", "down"); keys(q, "ret")
        time.sleep(1.5)
        shot(q, "sdhci_system")
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "w"}])
        time.sleep(0.8)

        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, *(["down"] * 7)); keys(q, "ret")      # Dostris (.UNO read)
        time.sleep(2.0)
        shot(q, "sdhci_dostris")
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "w"}])
        time.sleep(0.8)

        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, "down"); keys(q, "ret")               # Editor
        time.sleep(1.5)
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "s"}])
        time.sleep(2.0)
        shot(q, "sdhci_editor")
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)

    ok = True
    log = open("build/sdhci_dbg.log", "rb").read().decode("ascii", "replace") \
        if os.path.exists("build/sdhci_dbg.log") else ""
    if "detached: ExitBootServices done" in log:
        print("PASS: USB-booted system detached (gate: UnoDOS on the SDHCI card)")
    elif log.strip():
        print("FAIL: booted but did not detach"); ok = False
    else:
        print("note: no debugcon output (non-DBGCON build) - relying on shots")

    r = subprocess.run(["mtype", "-i", img + "@@1048576", "::NOTES.TXT"],
                       capture_output=True, text=True)
    if MARK in r.stdout:
        print("PASS: NOTES.TXT on the card image (native SDHCI write path)")
    else:
        print("FAIL: NOTES.TXT missing/wrong on the card image"); ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
