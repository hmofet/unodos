#!/usr/bin/env python3
"""End-to-end gate for the URC host-attested guard (dead-man's switch), in QEMU.

Boots the DEBUG image with a `remote=` STRESS.CFG (same rig as remote_qemu.py)
but with **`-no-reboot`**, so ANY reset the guest issues makes QEMU *exit* - an
unambiguous "the guard fired" signal that doesn't depend on the (slow, flaky in
headless OVMF) OVMF cold-reboot + re-dial path. Proves three behaviors:

  (b) armed + kept petted        -> box stays up          (QEMU alive)
  (c) disarmed (safe) + silent   -> box stays up          (QEMU alive)
  (a) armed + host goes silent   -> guard fires a reset    (QEMU exits)

Case (a) exercises the MAIN-LOOP firing path (uno_dbg_heartbeat -> guard_check
-> wd_fire -> ResetSystem): a HEALTHY box whose host stopped calling home. The
ISR/firmware firing paths (dbg_timer_c / wd_event_cb / SetWatchdogTimer) catch a
box too WEDGED to run the main loop; those need real hardware / a detached boot
and are validated there, not here (headless QEMU-attached services no firmware
timer - the freeze watchdog has the same limitation).

Requires a debug build first:  UNO_DEBUG=1 ./build.sh
Run under WSL (qemu-system-x86_64, sgdisk, mformat/mcopy, OVMF). Exit 0 iff all
three checks pass.
"""
import os, sys, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink

ESP = os.path.join(HERE, "..", "build", "esp")
DISK = "/tmp/guard_disk.img"
FAT = "/tmp/guard_fat.img"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS = "/tmp/guard_vars.fd"
SECTOR, MIB = 512, 1 << 20
PORT = 5401                                  # distinct from remote_qemu's 5399


def sh(a): subprocess.run(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build_disk():
    cfg = "/tmp/guard_stress.cfg"
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote=10.0.2.2:%d\nnonet\n" % PORT)
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
    sh(["mcopy", "-i", FAT, "-o", cfg, "::/STRESS.CFG"])
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
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-no-reboot",                      # a guard reset -> QEMU exits (the signal)
        "-display", "none",
    ], stderr=subprocess.DEVNULL)


def main():
    if not os.path.isdir(ESP) or not os.path.exists(os.path.join(ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1

    link = UnoAutoLink("127.0.0.1", PORT)
    link.listen()
    build_disk()
    q = boot_qemu()
    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and cond

    alive = lambda: q.poll() is None
    try:
        connected = link.wait_connected(90)
        check(connected, "pc64 dialed in")
        if not connected:
            return 1

        # (b) armed + petted through a window > timeout: box must stay up.
        tok = link.guard(4, "reboot", timeout=5)
        check(tok is not None, "guard armed (token issued)", "token=%r" % tok)
        petted = True
        for _ in range(12):                 # 6 s of petting a 4 s guard
            time.sleep(0.5)
            try:
                link.pet(timeout=3)
            except Exception:               # noqa: BLE001
                petted = False; break
        check(petted and alive(), "petted guard: box stays up across the window",
              "qemu_alive=%s" % alive())
        link.safe(timeout=3)

        # (c) disarmed + silent for > (timeout + reset delay): still up.
        time.sleep(8)
        check(alive(), "disarmed guard: silence does NOT reset", "qemu_alive=%s" % alive())

        # (a) armed + host goes silent -> guard fires -> reset -> QEMU exits.
        link.guard(4, "reboot", timeout=5)
        t0 = time.time()
        rc = None
        while time.time() - t0 < 25:        # 4 s deadline + reset delay + margin
            rc = q.poll()
            if rc is not None: break
            time.sleep(0.5)
        check(rc is not None, "silent guard: box reset (QEMU exited on -no-reboot)",
              "after %.1fs" % (time.time() - t0) if rc is not None else "no exit in 25s")
    finally:
        try:
            if alive(): link.command("poweroff", timeout=2)
        except Exception:                   # noqa: BLE001
            pass
        time.sleep(0.5)
        try: q.kill()
        except Exception: pass
        link.close()

    print("\n" + (">> URC guard OK" if ok else ">> URC guard FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
