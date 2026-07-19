#!/usr/bin/env python3
"""Window-cap stress: open all 16 launcher apps without closing any."""
import sys, time, os, subprocess
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness as H

subprocess.run(["cp", H.OVMF_VARS, "build/vars.fd"], check=True)
if os.path.exists(H.QMP_SOCK):
    os.remove(H.QMP_SOCK)
argv = [
    "qemu-system-x86_64", "-machine", "q35", "-m", "256",
    "-drive", "if=pflash,format=raw,readonly=on,file=" + H.OVMF_CODE,
    "-drive", "if=pflash,format=raw,file=build/vars.fd",
    "-drive", "format=vvfat,file=fat:rw:build/esp",
    "-device", "qemu-xhci", "-device", "usb-tablet",
    "-nic", "none", "-display", "none",
    "-qmp", "unix:%s,server,nowait" % H.QMP_SOCK,
    "-debugcon", "file:build/ovmf.log", "-global", "isa-debugcon.iobase=0x402",
]
qemu = subprocess.Popen(argv)
try:
    q = H.Qmp(H.QMP_SOCK)
    time.sleep(18)
    for i in range(16):                     # every launcher app, kept open
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.6)
        H.keys(q, *(["down"] * i), gap=0.08)
        H.keys(q, "ret")
        time.sleep(1.2)
        if i == 13:                         # Runner3D fullscreens; back to desktop
            H.keys(q, "esc")
            time.sleep(0.6)
    time.sleep(2)
    H.shot(q, "probe_manywin")
finally:
    try:
        q.cmd("quit")
    except Exception:
        qemu.kill()
    qemu.wait()
