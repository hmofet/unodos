#!/usr/bin/env python3
"""Arm the staged image for the display test (vm_display_urc.py).

Run AFTER vm_stage.py.  Prepends the `remote=` line the URC link needs (and
`nonet`, so the boot's own network test does not race it), then rebuilds the
image.  Any argument is passed to mkuefi.py (the image size in MiB - use
1200 when a rootfs is staged).

    python3 tools/vm_stage.py
    python3 tools/vm_display_stage.py [mib]
    scp build/unodos-uefi.img <kvm-box>:/tmp/
    # on the kvm box: python3 vm_display_urc.py /tmp/unodos-uefi.img
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)

cfg = "build/esp/DEBUG.CFG"
if not os.path.exists(cfg):
    sys.exit("no %s - run ./build.sh and tools/vm_stage.py first" % cfg)
body = open(cfg, "rb").read()
add = [k for k in (b"remote=10.0.2.2:5399", b"nonet") if k not in body]
if add:
    open(cfg, "wb").write(b"\n".join(add) + b"\n" + body)
print("DEBUG.CFG: remote link armed%s" % ("" if add else " (already)"))
sys.exit(subprocess.run([sys.executable, "tools/mkuefi.py"] + sys.argv[1:]).returncode)
