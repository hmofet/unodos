#!/usr/bin/env python3
"""Boot pc64 with a USB-net gadget on the xHCI bus, open the System window,
capture the USB diagnostic line. Reuses harness.py's Qmp/keys/shot helpers."""
import os, sys, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

def main():
    subprocess.run(["cp", H.OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(H.QMP_SOCK):
        os.remove(H.QMP_SOCK)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + H.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=vvfat,file=fat:rw:build/esp",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-tablet,bus=xhci.0",
        "-netdev", "user,id=n0",
        "-device", "usb-net,bus=xhci.0,netdev=n0",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % H.QMP_SOCK,
        "-debugcon", "file:build/ovmf.log", "-global", "isa-debugcon.iobase=0x402",
    ])
    try:
        q = H.Qmp(H.QMP_SOCK)
        print("qemu up; booting...")
        time.sleep(75)
        H.shot(q, "usb_desktop")
        # Control Panel auto-opens at boot and holds focus. Close it (X button
        # at real ~162,37 -> 480-space y=44), then double-click System.
        # Mode is 640x400 but harness maps y by /480: real-y*480/400.
        H.click(q, 162, 44); time.sleep(1.0)
        # fast double-click the System icon (real 57,188 -> 480-space y=226)
        H.mouse_move(q, 57, 226)
        H.mouse_btn(q, True); H.mouse_btn(q, False)
        H.mouse_btn(q, True); H.mouse_btn(q, False)
        time.sleep(2.0)
        H.shot(q, "usb_system")
    finally:
        try: q.cmd("quit")
        except Exception: qemu.kill()
        qemu.wait(timeout=10)

if __name__ == "__main__":
    main()
