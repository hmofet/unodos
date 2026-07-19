#!/usr/bin/env python3
"""UnoDOS/pc64 eMMC verification - Windows QEMU 11 (which has `-device emmc`;
the WSL QEMU 8.2 only models SD cards, so the MMC handshake - CMD1, host-
assigned RCA, CSD/EXT_CSD capacity - is exercised here).

Same scenario as tools/sdhci_test.py: USB vvfat boot stick + the packed GPT
image on an eMMC behind sdhci-pci, build carrying -DUNO_SDHCI_TEST
-DUNO_DBGCON. Asserts the detach (debugcon) and the Editor save reaching the
raw image (dir entry + content, checked in pure python - no mtools needed).

  python tools\\emmc_test_win.py         (Windows python, run from pc64/)
"""
import json, os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)

QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
CODE = r"C:\Program Files\qemu\share\edk2-x86_64-code.fd"
VARS = r"C:\Program Files\qemu\share\edk2-i386-vars.fd"   # paired vars store
QMP_PORT = 4489
MARK = b"unoui is the whole UnoDOS UI"


class Qmp:
    def __init__(self, port, timeout=30):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 2)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.3)
        self.buf = b""
        self.recv()
        self.cmd("qmp_capabilities")

    def recv(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.s.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            r = self.recv()
            if "return" in r or "error" in r:
                return r


def keys(q, *names):
    for n in names:
        q.cmd("send-key", keys=[{"type": "qcode", "data": n}])
        time.sleep(0.12)


def ctrl(q, key):
    q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                            {"type": "qcode", "data": key}])


def main():
    img = os.path.join("build", "unodos-uefi.img")
    dbg = os.path.join("build", "emmc_dbg.log")
    if not os.path.exists(img):
        sys.exit("no %s - run tools/mkuefi.py first" % img)
    vars_copy = os.path.join("build", "win_vars.fd")
    with open(VARS, "rb") as f, open(vars_copy, "wb") as g:
        g.write(f.read())
    if os.path.exists(dbg):
        os.remove(dbg)
    argv = [
        QEMU, "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE,
        "-drive", "if=pflash,format=raw,file=" + vars_copy,
        "-device", "qemu-xhci",
        "-drive", "if=none,id=usbesp,format=vvfat,file=fat:rw:build/esp",
        "-device", "usb-storage,drive=usbesp",
        "-device", "sdhci-pci",
        "-drive", "if=none,id=mmcdrv,format=raw,file=" + img,
        "-device", "emmc,drive=mmcdrv",
        "-device", "usb-tablet",
        "-nic", "none",
        "-display", "none",
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
        "-debugcon", "file:" + dbg,
        "-global", "isa-debugcon.iobase=0x402",
    ]
    qemu = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_PORT)
        print("qemu(win) up; USB boot, eMMC behind SDHCI...")
        time.sleep(25)
        ctrl(q, "esc"); time.sleep(0.8)
        keys(q, "down"); keys(q, "ret")           # Editor
        time.sleep(1.5)
        ctrl(q, "s")                              # save NOTES.TXT
        time.sleep(2.5)
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=15)

    ok = True
    log = open(dbg, "rb").read().decode("ascii", "replace") \
        if os.path.exists(dbg) else ""
    if "detached: ExitBootServices done" in log:
        print("PASS: USB-booted system detached (gate: UnoDOS on the eMMC)")
    else:
        print("FAIL: did not detach (log: %r...)" % log[:120]); ok = False

    if fat32_file_starts_with(img, 1048576, b"NOTES   TXT", MARK):
        print("PASS: NOTES.TXT on the raw eMMC image points at the saved text")
    else:
        print("FAIL: NOTES.TXT missing/wrong on the eMMC image")
        ok = False
    sys.exit(0 if ok else 1)


def fat32_file_starts_with(img, part_off, name83, prefix):
    """Minimal FAT32 root-dir lookup: find `name83`, follow its start cluster,
    check the file data begins with `prefix`. (A raw substring search would be
    fooled - the Editor's default text is also inside BOOTX64.EFI on the same
    image; this checks the DIRECTORY ENTRY actually points at the data - the
    exact linkage the dirty-line fat.c bug broke.)"""
    import struct
    with open(img, "rb") as f:
        f.seek(part_off)
        bs = f.read(512)
        rsvd  = struct.unpack("<H", bs[14:16])[0]
        nfats = bs[16]
        spc   = bs[13]
        fatsz = struct.unpack("<I", bs[36:40])[0]
        rootc = struct.unpack("<I", bs[44:48])[0]
        data0 = part_off + (rsvd + nfats * fatsz) * 512
        clus = rootc
        for _ in range(64):                      # walk the root chain
            f.seek(data0 + (clus - 2) * spc * 512)
            sect = f.read(spc * 512)
            for o in range(0, len(sect), 32):
                e = sect[o:o + 32]
                if e[:11] == name83 and e[0] not in (0x00, 0xE5):
                    start = (struct.unpack("<H", e[20:22])[0] << 16) \
                          |  struct.unpack("<H", e[26:28])[0]
                    size  = struct.unpack("<I", e[28:32])[0]
                    if start < 2 or size == 0:
                        return False
                    f.seek(data0 + (start - 2) * spc * 512)
                    return f.read(len(prefix)) == prefix
            f.seek(part_off + rsvd * 512 + clus * 4)   # next cluster (FAT#0)
            clus = struct.unpack("<I", f.read(4))[0] & 0x0FFFFFFF
            if clus >= 0x0FFFFFF8 or clus < 2:
                return False
    return False


if __name__ == "__main__":
    main()
