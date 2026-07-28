#!/usr/bin/env python3
"""Smoke test for the PCH TCO hardware watchdog (pc64/uno_hw_wdt.c) on QEMU.

QEMU q35 carries an ICH9-LPC that emulates a v2 TCO - the exact generation this
driver's RCBA-GCS NO_REBOOT + TCOv2 timer path targets - so the mechanism is
demonstrable without metal.  We boot the DEBUG image with a STRESS.CFG
`hw-wdt-selftest=<seconds>` key: at the end of init the OS arms the TCO and then
`cli; for(;;){}`.  That IRQs-off spin is precisely the wedge the *software* guard
cannot recover (no ISR, no main loop, no TPL cycle) - so if the box resets, ONLY
the TCO could have done it.  With `-no-reboot`, that reset makes QEMU exit, which
is our unambiguous pass signal (the same trick guard_qemu.py uses).

  arm + cli-spin, TCO fires  ->  QEMU exits within ~(boot + 2*timeout)  ->  PASS
  no reset                   ->  QEMU still alive at the deadline        ->  FAIL

QEMU caveat: ICH9-LPC's `noreboot` property defaults to false (reboot ENABLED),
and the TCO reset fires on the second timeout regardless of the guest's GCS bit,
so this exercises the timer programming + two-timeout sizing end to end.  It does
NOT prove the guest's NO_REBOOT *clear* is honoured (QEMU may not wire the GCS
bit to the reset path) - that, and the PMC-class metal targets, are the metal
gate.  See HWWATCHDOG.md §4, §7.

Requires a debug build first:  UNO_DEBUG=1 ./build.sh
Run under WSL (qemu-system-x86_64, sgdisk, mformat/mmd/mcopy, OVMF).  The TCO
timeout is short, so the whole run is ~one boot.  Exit 0 iff the TCO reset fired.
"""
import os, sys, subprocess, time

HERE = os.path.dirname(os.path.abspath(__file__))
ESP = os.path.join(HERE, "..", "build", "esp")
DISK = "/tmp/hwwdt_disk.img"
FAT = "/tmp/hwwdt_fat.img"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS = "/tmp/hwwdt_vars.fd"
SECTOR, MIB = 512, 1 << 20
TIMEOUT_S = 4                                   # TCO backstop window we request


def sh(a): subprocess.run(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build_disk(selftest):
    cfg = "/tmp/hwwdt_stress.cfg"
    with open(cfg, "w", newline="\r\n") as f:
        f.write(("hw-wdt-selftest=%d\n" % TIMEOUT_S) if selftest else "")
        f.write("nonet\n")
    disk_sectors = 96 * 2048
    with open(DISK, "wb") as f: f.truncate(disk_sectors * SECTOR)
    sh(["sgdisk", "--zap-all", DISK])
    sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", DISK])
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
    with open(FAT, "wb") as f: f.truncate(part_sectors * SECTOR)
    sh(["mformat", "-i", FAT, "-F", "-T", str(part_sectors), "::"])
    for root, dirs, files in os.walk(ESP):
        rel = os.path.relpath(root, ESP)
        if rel != ".":
            sh(["mmd", "-i", FAT, "::/" + rel.replace(os.sep, "/")])
        for fn in files:
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            sh(["mcopy", "-i", FAT, "-o", src, dst])
    # MUST be DEBUG.CFG, not STRESS.CFG: the debug build SHIPS a DEBUG.CFG on
    # the ESP (build.sh), and dbg_cfg_read (pc64_stress.c) reads DEBUG.CFG
    # first and only falls back to the legacy STRESS.CFG when DEBUG.CFG is
    # absent - so a STRESS.CFG written here is SHADOWED and every key in it
    # is silently ignored. That is what left this harness's guest booting a
    # plain desktop under the shipped `passes=3` and never powering off.
    sh(["mcopy", "-i", FAT, "-o", cfg, "::/DEBUG.CFG"])
    with open(FAT, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b: break
            df.write(b)


def boot_qemu():
    sh(["cp", OVMF_VARS, VARS])
    return subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-global", "ICH9-LPC.noreboot=false",     # let the TCO actually reset
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK,
        "-no-reboot",                              # a TCO reset -> QEMU exits
        "-display", "none",
    ], stderr=subprocess.DEVNULL)


def main():
    if not os.path.isdir(ESP) or not os.path.exists(os.path.join(ESP, "BOOTX64.EFI")) \
            and not os.path.exists(os.path.join(ESP, "EFI", "BOOT", "BOOTX64.EFI")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1

    def run(selftest, deadline):
        build_disk(selftest)
        q = boot_qemu()
        t0 = time.time()
        rc = None
        try:
            while time.time() - t0 < deadline:
                rc = q.poll()
                if rc is not None: break
                time.sleep(0.5)
        finally:
            if q.poll() is None:
                try: q.kill()
                except Exception: pass
        return rc, time.time() - t0

    ok = True
    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + "  " + label + (("  " + detail) if detail else ""))
        ok = ok and cond

    # CONTROL: same image, no selftest key -> boots to the desktop and stays up.
    # Proves the exit in the test phase is the TCO, not a boot crash (which would
    # also make QEMU exit under -no-reboot).
    rc, dt = run(selftest=False, deadline=30)
    check(rc is None, "control (no selftest): box boots and stays up",
          "still alive at 30s" if rc is None else "exited at %.1fs (unexpected)" % dt)

    # TEST: arm the TCO then cli-spin -> only the TCO can reset -> QEMU exits.
    rc, dt = run(selftest=True, deadline=45)
    check(rc is not None, "selftest: armed TCO resets the cli-spun box (QEMU exited)",
          "after %.1fs (arm %ds)" % (dt, TIMEOUT_S) if rc is not None else "no reset in 45s")

    print("\n>> hwwdt QEMU smoke " + ("OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
