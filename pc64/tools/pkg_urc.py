#!/usr/bin/env python3
"""pkg_urc - double-click a real Android package in Files and get a real app.

    UNO_DEBUG=1 ./build.sh
    python3 tools/pkg_urc.py /path/to/firefox-x86_64.apk

This is the end-to-end half of the unopkg gate (pc64/UNOPKG.md).  The other
half is tools/pkg_test.sh, which runs the SAME readers natively in a second;
the split is deliberate:

  - pkg_test.sh proves the READING - zip central directory, binary-XML string
    pool, attribute walk, the descriptor and blob rewrite - against a shipping
    138 MB APK, in a second, with no machine involved.
  - this file proves the PATH: that Files arms on the first Enter and installs
    on the second, that the app registry then finds a file nobody compiled a
    slot for, and that its window opens with the package's own name on it.

Neither substitutes for the other.  A reader that works and a shell that never
reaches it is exactly the shape of a feature that "works" and is unusable.

WHY THIS BUILDS ITS OWN BOOT DISK.  remote_qemu's is 96 MiB, and a real
Firefox APK is 138 MB - so the package could not be on the volume at all.  The
disk is rebuilt here at a size that fits, by the same sgdisk/mformat/mcopy
sequence, and nothing in remote_qemu.py changes.  The alternative was to drive
the volume dropdown to a second disk, which is several fragile clicks to
arrive at the same place.
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import remote_qemu as RQ                                   # noqa: E402
from urcui import UrcUi                                    # noqa: E402

APK = sys.argv[1] if len(sys.argv) > 1 else None
if not APK or not os.path.isfile(APK):
    sys.exit("usage: pkg_urc.py <app.apk>   (a real x86_64 Android package)")

APK_NAME = "FIREFOX.APK"        # 8.3, because that is what the volume holds
WANT_ID = "firefox"
WANT_NAME = "Firefox"

# OS framebuffer coordinates (640x400), read off shots/pkg_files_open.png.
# The Files window opens at a fixed place, so these are stable; if the toolbar
# ever reflows, the screenshots this gate writes are how you find that out.
VOL_DD_X, VOL_DD_Y = 275, 103   # the left pane's volume dropdown
PANE_X, ROW0_Y = 300, 207       # first FILE row, under the volume header
DESK1_X, DESK1_Y = 101, 385     # the virtual-desktop chips in the taskbar
DESK2_X, DESK2_Y = 127, 385

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


def sh(a):
    return subprocess.run(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build_disk_with_package():
    """remote_qemu.build_disk, at a size that fits the package beside the OS."""
    need_mib = 128 + (os.path.getsize(APK) >> 20)
    disk_sectors = need_mib * 2048
    cfg = os.path.join(os.path.dirname(RQ.DISK), "remote_stress.cfg")
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote=10.0.2.2:%d\nnonet\n" % RQ.PORT)
    with open(RQ.DISK, "wb") as f:
        f.truncate(disk_sectors * RQ.SECTOR)
    if not os.path.exists(RQ.DISK2):
        with open(RQ.DISK2, "wb") as f:
            f.truncate(128 * RQ.MIB)
    sh(["sgdisk", "--zap-all", RQ.DISK])
    sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", RQ.DISK])
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
    with open(RQ.FAT, "wb") as f:
        f.truncate(part_sectors * RQ.SECTOR)
    sh(["mformat", "-i", RQ.FAT, "-F", "-T", str(part_sectors), "::"])
    # THE PACKAGE GOES IN FIRST, and that is the whole reason this gate is
    # deterministic.  Files lists a directory in FAT order, which is creation
    # order, so copying the package before the OS tree puts it on the first
    # row - and the gate clicks a known row instead of hunting for one.
    # Hunting was tried and is worse than fragile: the root holds directories
    # too, and an Enter that lands on one NAVIGATES INTO IT, after which every
    # later keystroke is exploring somewhere else entirely.
    sh(["mcopy", "-i", RQ.FAT, "-o", APK, "::/" + APK_NAME])
    for root, dirs, files in os.walk(RQ.ESP):
        rel = os.path.relpath(root, RQ.ESP)
        if rel != ".":
            sh(["mmd", "-i", RQ.FAT, "::/" + rel.replace(os.sep, "/")])
        for fn in files:
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            sh(["mcopy", "-i", RQ.FAT, "-o", src, dst])
    sh(["mcopy", "-i", RQ.FAT, "-o", cfg, "::/DEBUG.CFG"])
    with open(RQ.FAT, "rb") as pf, open(RQ.DISK, "r+b") as df:
        df.seek(part_start * RQ.SECTOR)
        while True:
            b = pf.read(RQ.MIB)
            if not b:
                break
            df.write(b)
    print("boot disk: %d MiB, carrying %s (%.1f MB)"
          % (need_mib, APK_NAME, os.path.getsize(APK) / 1e6))


RQ.build_disk = build_disk_with_package

with UrcUi() as ui:
    before = ui.apps()
    print("app slots before: %d" % len(before))
    check(not any(a[0] == WANT_ID for a in before),
          "the package is not installed yet")

    # By ID, never by index: urcui.launch_id's own docstring is the reason -
    # an index is this boot's ordering of whatever is installed, and this test
    # installs something midway through.
    fid = [a[0] for a in before if a[1] == "Files" or a[0] == "files"]
    print("roster: %s" % ", ".join("%s(%s)" % (i, n) for i, n in before))
    check(bool(fid), "the Files app is in the registry")
    ui.launch_id(fid[0])
    print("Files opened (id '%s'); windows: %s" % (fid[0], ", ".join(ui.windows())))
    ui.shot("pkg_files_open")

    # Files opens on the RAM disk, which holds one README.  Switch the left
    # pane to the FAT volume: the control is a dropdown, so that is a click to
    # open it and a click on the second row.
    ui.click(VOL_DD_X, VOL_DD_Y)
    ui.shot("pkg_volume_menu")
    ui.click(VOL_DD_X, VOL_DD_Y + 43)        # the FAT volume, under "RAM"
    ui.shot("pkg_volume_fat")

    # The package is the first row, by construction (see build_disk_with_...).
    # The first click arms - selection is already row 0, so it reaches
    # pane_enter - and the second installs.  Both are sent as clicks rather
    # than keys because the mouse path is the one a person uses, and because
    # a canvas that has never been clicked does not hold the keyboard focus.
    ui.click(PANE_X, ROW0_Y)
    ui.shot("pkg_armed")
    ui.click(PANE_X, ROW0_Y)
    ui.shot("pkg_installed")

    # Belt and braces: if the arm landed on the click but the install did not,
    # Enter now goes to a canvas that certainly has focus.
    if not any(a[0] == WANT_ID for a in ui.apps()):
        ui.key(13)
        ui.key(13)
        ui.shot("pkg_installed_kbd")

    after = ui.apps()
    print("app slots after:  %d" % len(after))
    check(any(a[0] == WANT_ID for a in after),
          "two presses on the package installed it")
    row = [a for a in after if a[0] == WANT_ID]
    check(bool(row), "an app with id '%s' is in the registry" % WANT_ID)
    if row:
        print("registered as: id='%s' name='%s'" % (row[0][0], row[0][1]))
        check(row[0][1] == WANT_NAME,
              "it is named '%s', from the package's own manifest" % WANT_NAME)
    check(len(after) == len(before) + 1,
          "exactly one app appeared (%d -> %d)" % (len(before), len(after)))

    # The icon.  Switching to an empty virtual desktop is the cheapest way to
    # photograph it: desktop icons are global, the windows are not, so nothing
    # has to be closed or moved to get an unobstructed view.
    ui.click(DESK2_X, DESK2_Y)
    ui.shot("pkg_desktop_icon")
    ui.click(DESK1_X, DESK1_Y)

    print("\nopen it")
    ui.launch_id(WANT_ID)
    titles = ui.windows()
    print("windows open: %s" % ", ".join(titles))
    check(any(t.startswith(WANT_NAME) for t in titles),
          "a window titled '%s' opened" % WANT_NAME)
    ui.shot("pkg_app_window")

# Reboot, on the SAME disk, and look again.  This is the check that separates
# "installed" from "registered until the power goes off": the shim and its
# record are files on a volume, so a second boot that still finds the app is
# the only evidence that they really were written rather than remembered.
print("\nreboot, same disk")
RQ.build_disk = lambda: None                 # keep what the first boot wrote
with UrcUi() as ui:
    after_boot = ui.apps()
    print("app slots after reboot: %d" % len(after_boot))
    row = [a for a in after_boot if a[0] == WANT_ID]
    check(bool(row), "the app is still installed after a reboot")
    if row:
        check(row[0][1] == WANT_NAME, "still named '%s'" % WANT_NAME)
    ui.shot("pkg_after_reboot")

print("\n%d checks failed" % len(fails))
sys.exit(1 if fails else 0)
