#!/usr/bin/env python3
"""Run the unoautomate UI smoke test (tools/AUTOMATE.PY) in QEMU.

Same real-FAT recipe as spectest_qemu.py: build a GPT+FAT32 disk from
build/esp, stage AUTOMATE.PY + a STRESS.CFG with `automate` (and `nonet`,
so the boot phase goes straight to the script), boot headless, wait for the
script's own unoauto.poweroff(), then read \\AUTORES.TXT back and judge it.
Exit 0 iff the smoke run reports 0 FAIL.
"""
import os, sys, subprocess, time, re
HERE = os.path.dirname(os.path.abspath(__file__))
ESP  = os.path.join(HERE, "..", "build", "esp")
PY   = os.path.join(HERE, "AUTOMATE.PY")
DISK = "/tmp/automate_disk.img"
FAT  = "/tmp/automate_fat.img"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS = "/tmp/automate_vars.fd"
SECTOR, MIB = 512, 1 << 20

def sh(a, **k): return subprocess.run(a, **k)

def build_disk():
    cfg = os.path.join(os.path.dirname(DISK), "automate_stress.cfg")
    with open(cfg, "w", newline="\r\n") as f:
        f.write("automate\nnonet\n")     # no spec, no net test, no auto-poweroff:
                                         # the SCRIPT owns the shutdown
    disk_sectors = 96 * 2048
    with open(DISK, "wb") as f: f.truncate(disk_sectors * SECTOR)
    sh(["sgdisk", "--zap-all", DISK], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", DISK],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
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
    sh(["mcopy", "-i", FAT, "-o", cfg, "::/STRESS.CFG"],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sh(["mcopy", "-i", FAT, "-o", PY, "::/AUTOMATE.PY"],
       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(FAT, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b: break
            df.write(b)

def run():
    sh(["cp", OVMF_VARS, VARS])
    q = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "256", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK,
        "-display", "none",
    ], stderr=subprocess.DEVNULL)
    for _ in range(180):
        time.sleep(1)
        if q.poll() is not None: return True
    q.kill(); return False

def read_file(name):
    with open(DISK, "rb") as df:
        df.seek(2048 * SECTOR)
        data = df.read(os.path.getsize(FAT))
    with open(FAT, "wb") as f: f.write(data)
    got = sh(["mtype", "-i", FAT, "::/" + name], capture_output=True, text=True)
    return got.stdout if got.returncode == 0 and got.stdout.strip() else None

def main():
    if not os.path.exists(PY):
        print("FAIL: tools/AUTOMATE.PY missing"); return 1
    build_disk()
    powered_off = run()
    txt = read_file("AUTORES.TXT")
    if not powered_off:
        print("FAIL: guest did not power off - the script never called "
              "unoauto.poweroff()%s" % (" (partial AUTORES below)" if txt else ""))
        if txt: print(txt)
        return 1
    if not txt:
        print("FAIL: powered off but no AUTORES.TXT (script died early? "
              "check CRASH\\ + the kernel log)"); return 1
    print(txt)
    m = re.search(r'SMOKE\s+(\d+)\s+OK,\s+(\d+)\s+FAIL', txt)
    if not m:
        print("FAIL: AUTORES.TXT has no SMOKE summary"); return 1
    if int(m.group(2)) > 0:
        print(">> %s app slot(s) FAILED to open" % m.group(2)); return 1
    print(">> UI smoke clean: %s apps opened + closed" % m.group(1)); return 0

if __name__ == "__main__":
    sys.exit(main())
