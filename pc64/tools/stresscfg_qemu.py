#!/usr/bin/env python3
"""Regression for the STRESS.CFG control keys the flasher writes (the exact
scenarios from the 2026-07-21 field report: "off doesn't turn it off" and
"passes has no effect / passes=1 loops forever" - which turned out to be a
stale on-stick OS, but the current OS's key handling deserves a direct proof).

Boots the DEBUG build (UNO_DBGCON=1 required - asserts on the debugcon log)
on a real GPT+FAT32 disk (the spectest_qemu recipe) twice:

  1. `nostress` + nonet  -> the fuzz driver must log itself DISABLED and must
     NOT arm; the guest keeps running (nothing to shut down) - we kill it.
  2. `passes=1` + nonet  -> the driver must arm with passes=1, complete the
     single pass, and POWER OFF by itself (bounded-run contract). QEMU
     exiting inside the timeout IS the assertion.

Exit 0 iff both behave.
"""
import os, sys, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__))
ESP  = os.path.join(HERE, "..", "build", "esp")
DISK = "/tmp/stresscfg_disk.img"
FAT  = "/tmp/stresscfg_fat.img"
LOG  = "/tmp/stresscfg_dbg.log"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS = "/tmp/stresscfg_vars.fd"
SECTOR, MIB = 512, 1 << 20

def sh(a, **k): return subprocess.run(a, **k)

def build_disk(cfg_text):
    cfg = "/tmp/stresscfg_stress.cfg"
    with open(cfg, "w", newline="\r\n") as f:
        f.write(cfg_text)
    disk_sectors = 96 * 2048
    with open(DISK, "wb") as f: f.truncate(disk_sectors * SECTOR)
    sh(["sgdisk", "--zap-all", DISK], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", DISK],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_sectors = disk_sectors - 2048 - 2048
    with open(FAT, "wb") as f: f.truncate(part_sectors * SECTOR)
    sh(["mformat", "-i", FAT, "-F", "-T", str(part_sectors), "::"],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for root, dirs, files in os.walk(ESP):
        rel = os.path.relpath(root, ESP)
        if rel != ".":
            sh(["mmd", "-i", FAT, "::/" + rel.replace(os.sep, "/")],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for fn in files:
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            sh(["mcopy", "-i", FAT, "-o", src, dst],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # overlay the scenario config, image-only (never into build/esp)
    sh(["mcopy", "-i", FAT, "-o", cfg, "::/STRESS.CFG"],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(FAT, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(2048 * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b: break
            df.write(b)

def boot(timeout_s):
    """Returns (powered_off, log_text)."""
    sh(["cp", OVMF_VARS, VARS])
    if os.path.exists(LOG): os.remove(LOG)
    q = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "256", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK,
        "-chardev", "file,id=dcon,path=" + LOG,
        "-device", "isa-debugcon,iobase=0x402,chardev=dcon",
        "-no-reboot", "-display", "none", "-serial", "null",
    ], stderr=subprocess.DEVNULL)
    off = False
    for _ in range(timeout_s):
        time.sleep(1)
        if q.poll() is not None: off = True; break
    if not off: q.kill(); q.wait()
    log = ""
    if os.path.exists(LOG):
        log = open(LOG, "r", errors="replace").read()
    return off, log

def main():
    if not os.path.exists(os.path.join(ESP, "BOOTENV.TXT")) and \
       not os.path.exists(os.path.join(ESP, "STRESS.CFG")):
        print("no debug build staged - run UNO_DEBUG=1 UNO_DBGCON=1 ./build.sh first"); return 1
    fails = 0

    # -- scenario 1: nostress = the real OFF switch --------------------------
    build_disk("# flasher-style config\nnostress\nnonet\n")
    off, log = boot(45)
    if "nostress in STRESS.CFG - fuzz driver disabled" not in log:
        print("FAIL[nostress]: no 'fuzz driver disabled' line in the log"); fails += 1
    elif "stress: ARMED" in log:
        print("FAIL[nostress]: driver ARMED despite nostress"); fails += 1
    else:
        print("PASS[nostress]: driver disabled, never armed%s" %
              (" (guest kept running as expected)" if not off else ""))

    # -- scenario 2: passes=1 = one bounded pass, then self power-off --------
    build_disk("passes=1\nnonet\n")
    off, log = boot(150)
    if "ARMED (passes=1" not in log:
        print("FAIL[passes=1]: driver did not arm with passes=1"); fails += 1
    elif not off:
        print("FAIL[passes=1]: guest still running after 150 s (the reported "
              "'infinite loop')"); fails += 1
    else:
        print("PASS[passes=1]: armed, one pass, powered off by itself")

    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
