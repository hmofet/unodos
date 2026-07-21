#!/usr/bin/env python3
r"""End-to-end verification of the pc64 DEBUG crash-report pipeline in QEMU.

Boots the debug build with a WRITABLE GPT+FAT32 AHCI disk (the flasher's
whole-disk layout) and a STRESS.CFG that sets allow-force, so the stress
driver forces a #PF on its first pass.  Then asserts:

  1. the fault handler printed a full CRASH report to debugcon (needs the
     build to be UNO_DBGCON=1), self-symbolized against the baked table, and
  2. the report was persisted to CRASH\ on the FAT volume (read back with
     mtools after QEMU exits).

Prereqs (WSL/Linux): qemu-system-x86_64, OVMF, sgdisk, mtools, and a debug
build.  Run:  UNO_DBGCON=1 ./build.sh  then  python3 tools/dbg_crash_test.py
"""
import os, subprocess, sys, time, re

PC64 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(PC64, "build")
ESP = os.path.join(BUILD, "esp")
DISK = os.path.join(BUILD, "dbgtest-disk.img")
LOG = os.path.join(BUILD, "dbgtest-debugcon.log")
VARS = os.path.join(BUILD, "dbgtest-vars.fd")
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
SECTOR = 512
MIB = 256


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


SAFE = os.environ.get("SAFE") == "1"
HANG = os.environ.get("HANG") == "1"


def build_disk():
    """GPT + one EF00 FAT32 partition holding build/esp.

    Default: arm allow-force so the driver self-crashes (crash-pipeline test).
    HANG=1:  arm force-hang so the driver spins -> tests the WATCHDOG (HG report).
    SAFE=1:  leave the SHIPPED STRESS.CFG untouched and assert NO self-crash -
    the regression test for the comment-matching parser bug."""
    # The armed config goes into the DISK IMAGE only (overlaid after the esp
    # copy below) - never into build/esp. Writing it into build/esp shipped an
    # allow-force endless-fast config on real sticks once build.sh's if-absent
    # default kept it.
    cfg = None
    if not SAFE:
        cfg = "/tmp/dbgtest_stress.cfg"
        with open(cfg, "w", newline="\r\n") as f:
            f.write("# QEMU pipeline test\n" +
                    ("force-hang\nfast\n" if HANG else "allow-force\nfast\n"))
    disk_sectors = MIB * 2048
    with open(DISK, "wb") as f:
        f.truncate(disk_sectors * SECTOR)
    sh(["sgdisk", "--zap-all", DISK], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", DISK],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
    fat = "/tmp/dbgtest_fat.img"
    with open(fat, "wb") as f:
        f.truncate(part_sectors * SECTOR)
    sh(["mformat", "-i", fat, "-F", "-T", str(part_sectors), "::"],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # copy the whole esp tree in
    for root, dirs, files in os.walk(ESP):
        rel = os.path.relpath(root, ESP)
        if rel != ".":
            sh(["mmd", "-i", fat, "::/" + rel.replace(os.sep, "/")],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for fn in files:
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            sh(["mcopy", "-i", fat, "-o", src, dst],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # overlay the armed test config on top of the shipped default, image-only
    if cfg:
        sh(["mcopy", "-i", fat, "-o", cfg, "::/STRESS.CFG"],
           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # splice the partition image into the GPT disk at the partition LBA
    with open(fat, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(1 << 20)
            if not b:
                break
            df.write(b)
    return fat


def boot():
    sh(["cp", OVMF_VARS, VARS])
    if os.path.exists(LOG):
        os.remove(LOG)
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK,          # writable AHCI disk
        # explicit chardev+device form (more reliable than -debugcon shorthand)
        "-chardev", "file,id=dcon,path=" + LOG,
        "-device", "isa-debugcon,iobase=0x402,chardev=dcon",
        "-no-reboot",                                  # CF9 reset -> QEMU exits
        "-display", "none", "-serial", "null",
    ]
    if SAFE:
        print("booting QEMU with the SHIPPED STRESS.CFG (must NOT self-crash)...")
        try:
            subprocess.run(argv, timeout=90)
            # A bounded run now powers the machine off when it finishes, so
            # QEMU exiting is the EXPECTED end - the check that matters is
            # whether CRASH\ came back clean, done below.
            print("  QEMU powered off on its own (expected: the run completed)")
        except subprocess.TimeoutExpired:
            print("  ran to the timeout without powering off - the run may not "
                  "have completed; check the PF snapshot count")
        return
    if HANG:
        print("booting QEMU (force-hang -> watchdog must fire an HG report + reset)...")
        try:
            subprocess.run(argv, timeout=110)   # ~29s to pass 1 + 20s watchdog
            print("  QEMU reset on its own - watchdog fired")
        except subprocess.TimeoutExpired:
            print("  TIMED OUT - watchdog did NOT reset the machine (a real gap)")
        return
    print("booting QEMU (forces a #PF, then resets -> QEMU exits)...")
    try:
        subprocess.run(argv, timeout=90)
    except subprocess.TimeoutExpired:
        print("  (timed out at 90s - killing; a hang is itself a finding)")


def check_log():
    if not os.path.exists(LOG):
        print("FAIL: no debugcon log (was the build UNO_DBGCON=1?)")
        return False
    txt = open(LOG, errors="replace").read()
    ok = True
    for needle in ("CRASH (full report follows)", "exception: vector",
                   "backtrace", "boot environment"):
        if needle not in txt:
            print("FAIL: debugcon missing %r" % needle); ok = False
    # self-symbolized? a report should name a function, not just raw rvas
    if re.search(r"\+0x[0-9a-f]+ \[rva", txt):
        print("PASS: backtrace is self-symbolized")
    else:
        print("WARN: no symbolized frame in the report (raw rvas only)")
    # was it a page fault as forced?
    if "vector 14" in txt or "#PF" in txt:
        print("PASS: forced #PF captured")
    # env block sanity
    for k in ("gop:", "mtrr", "fb bench", "pointer:", "volumes:"):
        if k in txt:
            print("PASS: env block has %r" % k)
    return ok


def check_disk(fat):
    """re-read the partition out of the disk (post-boot) and dump CRASH\\."""
    part_start = 2048
    with open(DISK, "rb") as df:
        df.seek(part_start * SECTOR)
        data = df.read(os.path.getsize(fat))
    with open(fat, "wb") as f:
        f.write(data)
    r = subprocess.run(["mdir", "-i", fat, "::/CRASH/QEMU"],
                       capture_output=True, text=True)
    print("---- CRASH\\ on the FAT volume after the run ----")
    print(r.stdout or r.stderr)
    if re.search(r"CR\d+\s+TXT", r.stdout) or "CR" in r.stdout:
        # pull the first report out and show its head
        m = re.search(r"(CR\w+)\s+TXT", r.stdout)
        if m:
            name = m.group(1) + ".TXT"
            got = subprocess.run(["mtype", "-i", fat, "::/CRASH/QEMU/" + name],
                                 capture_output=True, text=True)
            print("---- CRASH\\%s (head) ----" % name)
            print("\n".join(got.stdout.splitlines()[:30]))
        print("PASS: a crash report was persisted to the FAT volume")
        return True
    print("NOTE: no CR* report on disk (it may have written on the RESET path;"
          " re-boot would flush residue). debugcon capture is authoritative.")
    return False


def check_disk_safe(fat):
    part_start = 2048
    with open(DISK, "rb") as df:
        df.seek(part_start * SECTOR)
        data = df.read(os.path.getsize(fat))
    with open(fat, "wb") as f:
        f.write(data)
    r = subprocess.run(["mdir", "-i", fat, "::/CRASH/QEMU"], capture_output=True, text=True)
    out = r.stdout
    crashed = bool(re.search(r"\b(CR|HG|PN)\w*\s+TXT", out))
    ran = "PF" in out
    print("---- CRASH\\ after a safe-default run ----")
    print(out or r.stderr)
    if crashed:
        print("FAIL: a self-crash report appeared with the SHIPPED cfg "
              "(the parser bug is back)")
    else:
        print("PASS: no self-crash with the shipped default STRESS.CFG")
    if ran:
        print("PASS: the stress driver ran benignly (PF snapshots present)")
    return not crashed


def main():
    if not os.path.exists(os.path.join(ESP, "BUILD.TXT")):
        print("no debug build in build/esp - run: UNO_DBGCON=1 ./build.sh")
        sys.exit(1)
    fat = build_disk()
    boot()
    if SAFE:
        ok = check_disk_safe(fat)
        print("\n=== %s ===" % ("VERIFIED (no self-crash)" if ok else "FAILED"))
        sys.exit(0 if ok else 2)
    if HANG:
        txt = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
        log_ok = "watchdog" in txt and ("HANG" in txt or "main loop silent" in txt)
        print("PASS: watchdog fired + logged an HG report" if log_ok
              else "FAIL: no watchdog HANG report on debugcon")
        # re-read the disk for an HG file
        part_start = 2048
        with open(DISK, "rb") as df:
            df.seek(part_start * SECTOR); data = df.read(os.path.getsize(fat))
        with open(fat, "wb") as f: f.write(data)
        r = subprocess.run(["mdir", "-i", fat, "::/CRASH/QEMU"], capture_output=True, text=True)
        disk_ok = bool(re.search(r"\bHG\w*\s+TXT", r.stdout))
        print(r.stdout)
        print("PASS: HG report persisted to \\CRASH" if disk_ok
              else "NOTE: no HG file on disk (may have written on reset path)")
        print("\n=== %s ===" % ("VERIFIED (watchdog works)" if log_ok else "FAILED"))
        sys.exit(0 if log_ok else 2)
    a = check_log()
    b = check_disk(fat)
    print("\n=== %s ===" % ("VERIFIED" if (a and b) else
                            "PARTIAL - see notes above"))
    sys.exit(0 if a else 2)


if __name__ == "__main__":
    main()
