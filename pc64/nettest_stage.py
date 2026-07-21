#!/usr/bin/env python3
"""QEMU e2e for the BOOT-TIME network test stage (pc64_nettest.c).

Boots the debug build with the default armed STRESS.CFG and an e1000 on
SLIRP. Expected guest behaviour: the net test runs before the stress driver
(no USB eth, no WiFi in QEMU -> the wired fallback), does DHCP + gateway ping
+ DNS over SLIRP, writes CRASH\\NETLOG.TXT, then the bounded stress run powers
the guest off. We wait for the self-poweroff, then assert on the NETLOG the
vvfat write-back left in build/esp.

Also covers: `nonet` in STRESS.CFG must skip the test (second boot).
"""
import sys, os, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness

CFG = "build/esp/STRESS.CFG"
NETLOG = "build/esp/CRASH/NETLOG.TXT"

def boot_until_poweroff(tag, timeout_s):
    subprocess.run(["cp", harness.OVMF_VARS, "build/vars.fd"], check=True)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "256", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + harness.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=vvfat,file=fat:rw:build/esp",
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
    ], stderr=subprocess.PIPE)
    for t in range(timeout_s):
        time.sleep(1)
        if qemu.poll() is not None:
            print("[%s] guest powered off at t=%ds rc=%d" % (tag, t, qemu.returncode))
            return True
    print("[%s] FAIL: guest still running after %ds" % (tag, timeout_s))
    qemu.kill()
    return False

def main():
    if not os.path.exists(CFG):
        print("no", CFG, "- run build.sh first"); return 1

    # ---- scenario 1: armed default -> wired net test runs, NETLOG written --
    if os.path.exists(NETLOG):
        os.remove(NETLOG)
    if not boot_until_poweroff("armed", 180):
        return 1
    if not os.path.exists(NETLOG):
        print("FAIL: no CRASH/NETLOG.TXT after the armed boot"); return 1
    log = open(NETLOG, "r", errors="replace").read()
    print("---- NETLOG.TXT ----"); print(log); print("--------------------")
    for want in ["network hardware test", "plan: wired PCI NIC via e1000",
                 "DHCP lease", "== net test done: WIRED"]:
        if want not in log:
            print("FAIL: NETLOG missing %r" % want); return 1
    if "WIRED PASS" not in log:
        print("FAIL: wired suite did not PASS (see log above)"); return 1

    # ---- scenario 2: nonet -> the test is skipped, NETLOG absent ----------
    os.remove(NETLOG)
    cfg = open(CFG).read()
    open(CFG, "w").write(cfg + "\nnonet\n")
    try:
        if not boot_until_poweroff("nonet", 180):
            return 1
        if os.path.exists(NETLOG):
            print("FAIL: NETLOG written despite nonet"); return 1
    finally:
        open(CFG, "w").write(cfg)
    print("PASS: boot net stage (wired e2e) + nonet skip")
    return 0

if __name__ == "__main__":
    sys.exit(main())
