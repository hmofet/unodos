#!/usr/bin/env python3
"""Run SPECTEST in QEMU on a REAL FAT image (not vvfat).

vvfat corrupts multi-cluster writes on readback, and SPECTEST.TXT is a large
growing file - so this builds a genuine GPT+FAT32 disk with mformat/mcopy
(the dbg_crash_test recipe), boots it with a `spec` STRESS.CFG, waits for the
bounded run to power off, then reads CRASH\\<MACHINE>\\SPECTEST.TXT back out
with mtype and prints it. Exit 0 iff the suite reports 0 FAIL.
"""
import os, sys, subprocess, time, re
HERE = os.path.dirname(os.path.abspath(__file__))
ESP  = os.path.join(HERE, "..", "build", "esp")
DISK = "/tmp/spectest_disk.img"
FAT  = "/tmp/spectest_fat.img"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS = "/tmp/spectest_vars.fd"
SECTOR, MIB = 512, 1 << 20

def sh(a, **k): return subprocess.run(a, **k)

def build_disk():
    # The test's own STRESS.CFG goes into the DISK IMAGE only - never into
    # build/esp. This script used to write it into build/esp directly, and
    # since build.sh only recreated the default when the file was absent, the
    # dev-run config rode onto every image the flasher shipped afterwards.
    cfg = os.path.join(os.path.dirname(DISK), "spectest_stress.cfg")
    # `poweroff` = shut the guest down when the one-shot suites finish, so this
    # harness ends deterministically. (Was `passes=1`, which relied on the
    # now-removed stress driver's auto-shutdown; the one-shot spec/net path
    # honours `poweroff` directly - see pc64_nettest.c nettest_finish.)
    with open(cfg, "w", newline="\r\n") as f:
        f.write("poweroff\nspec\nnonet\n")
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
    # overlay the test config on top of the shipped default, image-only
    sh(["mcopy", "-i", FAT, "-o", cfg, "::/STRESS.CFG"],
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
        # SLIRP e1000 so the NETWORK live checks (S-NET-30 / S-AI-01/02) run
        # for real: DHCP against SLIRP, TCP/DNS via the host resolver, TLS out
        # to the provider. Needs the build host online; without it those
        # checks FAIL (they are part of the baseline now, not SKIPs).
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
    ], stderr=subprocess.DEVNULL)
    for _ in range(180):
        time.sleep(1)
        if q.poll() is not None: return True
    q.kill(); return False

def read_back():
    # re-extract the partition and find SPECTEST.TXT under CRASH\<machine>\
    with open(DISK, "rb") as df:
        df.seek(2048 * SECTOR)
        data = df.read(os.path.getsize(FAT))
    with open(FAT, "wb") as f: f.write(data)
    # CRASH\<MACHINE>\SPECTEST.TXT - find the machine subdir, then read it
    r = sh(["mdir", "-i", FAT, "::/CRASH"], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "<DIR>" and parts[0] not in (".", ".."):
            got = sh(["mtype", "-i", FAT, "::/CRASH/%s/SPECTEST.TXT" % parts[0]],
                     capture_output=True, text=True)
            if got.returncode == 0 and got.stdout.strip():
                return got.stdout
    return None

def main():
    build_disk()
    if not run():
        print("FAIL: guest did not power off (hang?)"); return 1
    txt = read_back()
    if not txt:
        print("FAIL: no SPECTEST.TXT on the disk"); return 1
    print(txt)
    # Authoritative verdict: the summary's FAIL count. (A naive `"FAIL" in txt`
    # trips on the header legend "(PASS/FAIL/SKIP)" and on any FAIL-detail word.)
    m = re.search(r'(\d+)\s+PASS,\s+(\d+)\s+FAIL', txt)
    if not m:
        # fall back to counting result lines shaped `S-XXX-NN FAIL ...`
        fails = len(re.findall(r'(?m)^\S+\s+FAIL\b', txt))
    else:
        fails = int(m.group(2))
    if fails > 0:
        print(">> %d contract(s) FAILED" % fails); return 1
    print(">> SPECTEST clean"); return 0

if __name__ == "__main__":
    sys.exit(main())
