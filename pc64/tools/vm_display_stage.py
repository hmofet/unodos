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

# A DESKTOP BIG ENOUGH TO HOLD THE GUEST.  The guest's surface is 800x600 and
# the default mode here is 640x400, so the Display view scales it to about
# 60% - legible for a console, useless for a browser.  At 1024x768 the
# appliance window shows it 1:1.  An unavailable mode is ignored by the
# shell, so this is safe on any firmware.
shell = os.path.join("build", "esp", "SHELL.CFG")
if not os.path.exists(shell) or b"res=" not in open(shell, "rb").read():
    with open(shell, "ab") as f:
        f.write(b"res=1024x768\n")
    print("SHELL.CFG: res=1024x768")
sys.exit(subprocess.run([sys.executable, "tools/mkuefi.py"] + sys.argv[1:]).returncode)
