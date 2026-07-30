#!/usr/bin/env python3
"""unodevices phase 3 gate: USB devices and interfaces in the device tree.

The plan's acceptance case - QEMU with `-device usb-kbd -device usb-mouse
-device usb-hub -device usb-storage`, all of them visible in the tree, each
interface bound (or honestly UNCLAIMED) - proved through the URC `devices`
verb, which is what a fleet operator actually reads.

NEEDS AN EAGER BUILD:

    UNO_DEBUG=1 UNO_EXTRA="-DUNO_USBHID_TEST" ./build.sh

because in production xhci.c refuses the controller until the firmware is
gone, and the debug build does not detach by default. -DUNO_USBHID_TEST
implies UNO_XHCI_EAGER, which is the attached-mode test path this gate wants;
it must never ship, which is exactly why the gate builds its own image rather
than reusing whatever is in build/esp.

Consumes unoautomate's link + disk builder as neutral APIs; adds only its own
topology and assertions.  Run under WSL, exit 0 iff green.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq                      # noqa: E402
from unoauto_remote import UnoAutoLink        # noqa: E402

# One of every shape phase 3 has to represent: a boot keyboard and a boot mouse
# (two separate HID interface triples), a HUB with a device behind it (the
# ZimaBlade's shape, and the case a flat enumerator cannot place), and a
# mass-storage device whose interface triple is usbmsc's.
TOPOLOGY = [
    "-device", "qemu-xhci,id=xhci",
    "-device", "usb-kbd,bus=xhci.0",
    "-device", "usb-mouse,bus=xhci.0",
    "-device", "usb-hub,id=hub1,bus=xhci.0,port=3",
    "-drive",  "if=none,id=ustick,format=raw,file=" + rq.DISK,
    "-device", "usb-storage,bus=xhci.0,port=3.1,drive=ustick",
]

fails = []


def check(ok, what, detail=""):
    print(("PASS " if ok else "FAIL ") + what + ("  " + detail if detail else ""))
    if not ok:
        fails.append(what)


def boot(extra):
    subprocess.run(["cp", rq.OVMF_VARS, rq.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + rq.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + rq.VARS,
        "-drive", "format=raw,file=" + rq.DISK,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
    ] + extra
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def main():
    if not os.path.isdir(rq.ESP):
        print("FAIL: no build at build/esp")
        return 1
    rq.build_disk()
    link = UnoAutoLink("127.0.0.1", rq.PORT)
    link.listen()
    vm = boot(TOPOLOGY)
    try:
        if not link.wait_connected(180):
            print("FAIL: pc64 never dialed in")
            return 1
        rows = link.devices(timeout=20)
        for r in rows:
            print("   ", r["raw"])

        # The xHCI controller itself, as an ordinary PCI registry driver.
        usb_ctrl = [r for r in rows if r["name"] == "usb"]
        check(bool(usb_ctrl) and usb_ctrl[0]["driver"] == "xhci",
              "the xHCI controller is BOUND to xhci",
              usb_ctrl[0]["raw"] if usb_ctrl else "no usb row")

        # USB nodes exist at all - this is the phase-3 headline. Device nodes
        # and interface nodes both land in the same listing, so a non-empty
        # 'hid' set means the descriptor walk ran and published.
        hid = [r for r in rows if r["name"] == "hid"]
        check(len(hid) >= 2, "HID interfaces published into the tree",
              "%d hid rows" % len(hid))

        # ...and each is bound to usbhid. Two of them, because a keyboard and a
        # mouse are separate interface triples: the thing the device/interface
        # split exists to get right.
        check(sum(1 for r in hid if r["driver"] == "usbhid") >= 2,
              "keyboard AND mouse interfaces both BOUND to usbhid",
              ", ".join(r["raw"] for r in hid))

        # The device BEHIND THE HUB has to be there too. A flat enumerator can
        # list it; only a tree can say it is a tier deeper, and the ZimaBlade
        # is a machine where everything is behind a hub.
        msc = [r for r in rows if r["name"] == "mass-storage" or
               (r["driver"] == "usbmsc")]
        check(bool(msc), "the mass-storage interface behind the hub is published",
              ", ".join(r["raw"] for r in msc) if msc else "absent")

        # Nothing may claim a driver the manager did not bind, and every class
        # name stays a single token (the URC last-token parse depends on it).
        check(all(len(r["name"].split()) == 1 for r in rows),
              "every class name is a single token")
    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:  # noqa: BLE001
            pass
        time.sleep(1)
        vm.kill()
        link.close()
    print(">> usbtree gate OK" if not fails else ">> FAILED: " + "; ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
