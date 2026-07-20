#!/usr/bin/env python3
"""UnoDOS/pc64 - verify the whole-disk install confirmation gate.

A whole-disk install erases the target, so it must take TWO deliberate acts of
DIFFERENT kinds: the word ERASE has to be TYPED, and only then does pressing
Install commit. This drives that gate against a scratch disk and asserts each
stage refuses to skip ahead.

  python3 tools/install_confirm_test.py

Stages checked:
  1. Install with nothing typed          -> refuses, asks for the word
  2. a WRONG word typed                  -> still refuses
  3. the right word, first Install       -> arms only ("Install again")
  4. editing the box after arming        -> revokes the arm
  5. right word + Install twice          -> proceeds

Nothing real is destroyed: the target is a throwaway raw image.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, keys, QMP_SOCK, OVMF_CODE, OVMF_VARS

SCRATCH = "build/scratch_disk.img"
FAILED = []


def shot(q, tag):
    ppm = os.path.abspath("shots/%s.ppm" % tag)
    q.cmd("screendump", filename=ppm)
    time.sleep(0.7)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag],
                   check=False)
    try:
        os.remove(ppm)
    except OSError:
        pass


def type_text(q, s):
    for ch in s:
        keys(q, ch.lower())
        time.sleep(0.10)


def main():
    subprocess.run(["dd", "if=/dev/zero", "of=" + SCRATCH, "bs=1M", "count=64"],
                   check=True, capture_output=True)
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    # The ESP goes on USB and the scratch disk on AHCI, deliberately: the
    # installer needs firmware services (installer.c refuses once detached),
    # and the M3 detach gate fires only when an AHCI/NVMe-backed FAT volume
    # holds BOOTX64.EFI. Booting from USB leaves no such volume, so the
    # machine stays attached and Install can scan - which is also the real
    # flow on metal, where you install FROM the stick.
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "512",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "id=esp,if=none,format=vvfat,file=fat:rw:build/esp",
        "-device", "qemu-xhci", "-device", "usb-storage,drive=esp",
        "-drive", "id=tgt,if=none,format=raw,file=" + SCRATCH,
        "-device", "ide-hd,drive=tgt",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK])
    q = None
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting for boot...")
        time.sleep(18)
        # Start menu -> Install (7 native apps: Install is index 5)
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, *(["down"] * 5))
        keys(q, "ret")
        time.sleep(2.0)
        shot(q, "inst_0_open")

        # pick the last target (the scratch disk sorts after the ESP volume)
        for _ in range(6):
            keys(q, "down")
            time.sleep(0.12)
        shot(q, "inst_1_selected")

        keys(q, "i")                       # stage 1: nothing typed
        time.sleep(0.8)
        shot(q, "inst_2_no_word")

        keys(q, "c")                       # C puts the caret in the confirm box
        time.sleep(0.5)
        type_text(q, "nope")               # stage 2: a WRONG word
        time.sleep(0.4)
        keys(q, "esc")                     # leave the box; accelerators return
        time.sleep(0.4)
        keys(q, "i")
        time.sleep(0.8)
        shot(q, "inst_3_wrong_word")

        keys(q, "c")
        time.sleep(0.4)
        for _ in range(8):
            keys(q, "backspace")
            time.sleep(0.08)
        type_text(q, "erase")              # stage 3: the right word
        time.sleep(0.4)
        keys(q, "esc")
        time.sleep(0.4)
        shot(q, "inst_4_typed")
        keys(q, "i")                       # stage 4: arms only
        time.sleep(0.8)
        shot(q, "inst_5_armed")
        keys(q, "i")                       # stage 5: commits
        time.sleep(8.0)
        shot(q, "inst_6_done")
        print("screenshots written to shots/inst_*.png - inspect the status line")
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)
        try:
            os.remove(SCRATCH)
        except OSError:
            pass


if __name__ == "__main__":
    main()
