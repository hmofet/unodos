#!/usr/bin/env python3
"""unodevices QEMU gate: enumerate a machine with real bridge topology.

tools/devmgr_test.c proves the enumerator against a synthetic bus; this proves
it against QEMU's, on a q35 deliberately built with a PCIe root port and a
controller BEHIND it - the case a flat 0..255 scan can list but cannot place,
and the reason the walk is recursive.

Consumes unoautomate's link and disk builder (tools/unoauto_remote.py,
tools/remote_qemu.py) as neutral APIs - nothing in either file is edited; this
file only adds its own QEMU topology and its own assertions.

Needs a debug build first:  UNO_DEBUG=1 ./build.sh
Run under WSL (qemu-system-x86_64 + OVMF + sgdisk/mtools), exit 0 iff green.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq                      # noqa: E402  (disk builder + paths)
from unoauto_remote import UnoAutoLink        # noqa: E402

# One device of each shape we care about, and an AHCI controller parked behind
# a PCIe root port so the registry has a non-trivial parent link to record.
TOPOLOGY = [
    "-device", "pcie-root-port,id=rp0,chassis=1,slot=1",
    "-device", "ahci,id=ahci0,bus=rp0",
    "-device", "qemu-xhci,id=xhci",
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
    esp = rq.ESP
    if not os.path.isdir(esp):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1
    rq.build_disk()
    link = UnoAutoLink("127.0.0.1", rq.PORT)
    link.listen()
    vm = boot(TOPOLOGY)
    try:
        if not link.wait_connected(180):
            print("FAIL: pc64 never dialed in")
            return 1
        rows = link.devices(timeout=15)
        for r in rows:
            print("   ", r["raw"])

        check(len(rows) >= 8, "enumerated the q35 machine", "%d devices" % len(rows))
        locs = {r["loc"]: r for r in rows}
        names = {r["name"] for r in rows}

        check("00:00.0" in locs and locs["00:00.0"]["name"] == "host-bridge",
              "host bridge at 00:00.0")
        check("ethernet" in names, "the e1000 the link itself is running on is listed")
        check("usb" in names, "qemu-xhci enumerated (class 0c/03)")
        check(any(r["name"] == "pci-bridge" for r in rows),
              "the PCIe root port is enumerated as a bridge")

        # the point of the recursive walk: the AHCI lives on a bus that only
        # exists because the root port declares it as its secondary
        behind = [r for r in rows if not r["loc"].startswith("00:")]
        check(bool(behind), "a device behind the root port is enumerated",
              ", ".join(r["raw"] for r in behind))
        check(any(r["name"] == "sata" for r in behind),
              "the AHCI behind the bridge decodes as sata")

        # phase 1 binds nothing: every row must say UNCLAIMED, and none may
        # claim a driver the manager has not actually bound
        check(all(r["driver"] is None for r in rows),
              "phase 1 reports no bindings (all UNCLAIMED)")
        check(all(len(r["name"].split()) == 1 for r in rows),
              "every class name is a single token (URC last-token parse)")

        # the same registry through pc64-python, over the `py` verb
        out = link.command("py import uno; print(len(uno.pci()), uno.pci()[0])",
                           timeout=15)
        txt = " ".join(out)
        check(str(len(rows)) in txt.split(" ")[0] if txt else False,
              "uno.pci() row count matches the URC listing", txt[:120])
    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:  # noqa: BLE001
            pass
        time.sleep(1)
        vm.kill()
        link.close()
    print(">> devmgr QEMU gate OK" if not fails else ">> FAILED: " + "; ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
