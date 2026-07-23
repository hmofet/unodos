#!/usr/bin/env python3
"""End-to-end gate for the unoautomate remote channel, in QEMU.

Boots the DEBUG image with a `remote=10.0.2.2:<port>` STRESS.CFG on a SLIRP
e1000 NIC, runs the dev-PC listener (unoauto_remote.UnoAutoLink) on the host,
and proves the round trip:
  1. pc64 dials in and streams LOG lines            (remote logging)
  2. host -> pc64 `probe` returns subsystem rows      (remote control)
  3. host -> pc64 `py print(6*7)` returns 42           (remote Python eval)
  4. host -> pc64 `launch 0` then `probe` shows a window (DRIVE)

Requires a debug build first:  UNO_DEBUG=1 ./build.sh
Run under WSL (needs qemu-system-x86_64, sgdisk, mformat/mcopy, OVMF), so that
the guest's SLIRP 10.0.2.2 maps to this process's loopback.  Exit 0 iff all
four checks pass.
"""
import os, sys, subprocess, time, threading
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink

ESP  = os.path.join(HERE, "..", "build", "esp")
DISK  = "/tmp/remote_disk.img"
DISK2 = "/tmp/remote_disk2.img"     # a SECOND blank disk for the partition/format e2e
FAT   = "/tmp/remote_fat.img"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS = "/tmp/remote_vars.fd"
SECTOR, MIB = 512, 1 << 20
PORT = 5399


def sh(a, **k): return subprocess.run(a, **k)


def build_disk():
    cfg = os.path.join(os.path.dirname(DISK), "remote_stress.cfg")
    # remote=<host>:<port> arms the dev-PC link; `nonet` skips the slow boot
    # net test (the link brings the NIC up itself); no `poweroff` so the guest
    # stays up for us to drive.
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote=10.0.2.2:%d\nnonet\n" % PORT)
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
    with open(FAT, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b: break
            df.write(b)


def boot_qemu():
    sh(["cp", OVMF_VARS, VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + DISK,
        "-drive", "format=raw,file=" + DISK2,     # blank; the disk verbs partition/format it
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    if os.environ.get("URC_DBGCON"):
        cmd += ["-debugcon", "file:" + os.environ["URC_DBGCON"],
                "-global", "isa-debugcon.iobase=0x402"]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def main():
    if not os.path.isdir(ESP) or not os.path.exists(os.path.join(ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1

    logs = []
    link = UnoAutoLink("127.0.0.1", PORT)
    link.on_log(lambda ch, t: logs.append((ch, t)))
    link.listen()

    build_disk()
    with open(DISK2, "wb") as f: f.truncate(128 * MIB)   # a blank 128 MB disk B
    q = boot_qemu()
    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and cond

    try:
        connected = link.wait_connected(90)
        check(connected, "pc64 dialed in")
        if not connected:
            return 1
        # 1) remote logging: HELLO + at least one LOG line within a few seconds
        for _ in range(50):
            if logs: break
            time.sleep(0.2)
        check(len(logs) > 0, "LOG stream received", "%d line(s)" % len(logs))

        # 2) probe round-trip
        try:
            rows = link.probe(timeout=10)
            check(len(rows) > 0, "probe round-trip", "%d row(s)" % len(rows))
        except Exception as e:  # noqa: BLE001
            check(False, "probe round-trip", str(e))

        # 3) remote Python eval
        try:
            out = link.eval("print(6*7)", timeout=25)
            check(out == ["42"], "py eval print(6*7)==42", repr(out))
        except Exception as e:  # noqa: BLE001
            check(False, "py eval print(6*7)==42", str(e))

        # 4) DRIVE: launch app 0, confirm a window appears in a fresh probe
        try:
            link.launch(0, timeout=10)
            time.sleep(0.5)
            rows = link.probe(timeout=10)
            wins = [r for r in rows if r["kind"] == 1]
            check(len(wins) > 0, "launch -> window visible in probe",
                  "%d window row(s)" % len(wins))
        except Exception as e:  # noqa: BLE001
            check(False, "launch -> window visible in probe", str(e))

        # 5) A/B OS-update path: push a scratch file to a writable volume and
        #    read it back byte-for-byte. Prefer firmware SFS (kind 2 - what an
        #    attached USB stick actually is) to exercise uno_efifs_write.
        try:
            import tempfile
            vols = link.vols(timeout=10)
            cand = ([v for v in vols if v["writable"] and v["kind"] == 2] or
                    [v for v in vols if v["writable"] and v["kind"] != 0] or
                    [v for v in vols if v["writable"]])
            if not cand:
                check(False, "push: a writable volume exists", "vols=%r" % vols)
            else:
                V, kind = cand[0]["vol"], cand[0]["kind"]
                payload = bytes((i * 7 + 3) & 0xFF for i in range(2000))
                exp = sum((i + 1) * b for i, b in enumerate(payload)) & 0xFFFFFFFF
                tf = tempfile.NamedTemporaryFile(delete=False)
                tf.write(payload); tf.close()
                try:
                    verified = link.push_file(V, "URCTEST.BIN", tf.name, chunk=750)
                finally:
                    os.unlink(tf.name)
                check(verified, "push_file finalize+verify",
                      "vol %d kind %d (%s)" % (V, kind,
                       "firmware-SFS" if kind == 2 else "native-FAT" if kind == 1 else "RAM"))
                out = link.eval('import uno; d=uno.read(%d,"URCTEST.BIN"); '
                                'print(len(d), sum((i+1)*b for i,b in enumerate(d))&0xffffffff)'
                                % V, timeout=25)
                got = out[0].split() if out else []
                check(len(got) == 2 and int(got[0]) == 2000 and int(got[1]) == exp,
                      "read-back bytes match exactly", "got=%r" % out)
        except Exception as e:  # noqa: BLE001
            check(False, "A/B push + read-back", str(e))

        # 6) bootnext: SetVariable reachable while attached (OVMF has NV vars)
        try:
            link.bootnext(0, timeout=5)
            check(True, "bootnext SetVariable ok")
        except Exception as e:  # noqa: BLE001
            check(False, "bootnext SetVariable ok", str(e))

        # 7) LARGE write to a NATIVE-FAT volume (the 1.5 MB `put` hang repro):
        #    push a ~1.5 MB file (create), then push it AGAIN (overwrite - the
        #    fat_alloc O(n^2) path), reading back byte-for-byte each time. QEMU
        #    can't reproduce the firmware-BlockIO *timing*, but it proves the
        #    fixed allocator writes a large multi-hundred-cluster file correctly
        #    and finalize returns (no infinite loop / corruption).
        try:
            import tempfile
            vols = link.vols(timeout=10)
            natfat = [v for v in vols if v["writable"] and v["kind"] == 1]
            if not natfat:
                check(False, "a native-FAT (kind 1) volume exists", "vols=%r" % vols)
            else:
                V = natfat[0]["vol"]
                big = os.path.join(HERE, "..", "build", "BOOTX64.EFI")   # ~1.5 MB, realistic
                data = open(big, "rb").read()
                exp = sum((i + 1) * b for i, b in enumerate(data)) & 0xFFFFFFFF
                rb = ('import uno; d=uno.read(%d,"BIGTEST.BIN"); '
                      'print(len(d), sum((i+1)*b for i,b in enumerate(d))&0xffffffff)' % V)
                for phase in ("create", "overwrite"):
                    t0 = time.time()
                    verified = link.push_file(V, "BIGTEST.BIN", big)   # default chunk
                    dt = time.time() - t0
                    check(verified, "big push %s finalize+verify (vol %d native-FAT)" % (phase, V),
                          "%d bytes in %.1fs" % (len(data), dt))
                    out = link.eval(rb, timeout=40)
                    g = out[0].split() if out else []
                    check(len(g) == 2 and int(g[0]) == len(data) and int(g[1]) == exp,
                          "big read-back bytes match (%s)" % phase, "got=%r" % out)
        except Exception as e:  # noqa: BLE001
            check(False, "large native-FAT push + read-back", str(e))

        # 8) RAW-DISK AUTHORING (unostorage): partition + format the blank disk B,
        #    then confirm the fresh FAT32 volume mounts + is writable end-to-end.
        try:
            import tempfile
            ds = link.disks(timeout=10)
            blank = next((d for d in ds if d["sectors"] == 128 * MIB // 512), None)
            boot  = next((d for d in ds if d["is_boot"]), None)
            check(blank is not None, "blank disk B enumerated (disks)", "disks=%r" % ds)
            # safety: arming the boot disk is refused
            if boot:
                try:
                    link.arm(boot["idx"]); check(False, "arm <bootdisk> refused")
                except Exception:  # noqa: BLE001
                    check(True, "arm <bootdisk> refused")
            else:
                check(False, "boot disk flagged is_boot", "disks=%r" % ds)
            if blank:
                D = blank["idx"]
                nfat_before = len([1 for l in link.command("vols") if l.split()[1] == "1"])
                # safety: unarmed prepdisk is refused
                try:
                    link.prepdisk(D, "TESTVOL"); check(False, "unarmed prepdisk refused")
                except Exception:  # noqa: BLE001
                    check(True, "unarmed prepdisk refused")
                # arm + prepdisk (partition + format)
                link.arm(D)
                r = link.prepdisk(D, "TESTVOL")
                check(bool(r) and r[0] == "prepared", "prepdisk (GPT + ESP + FAT32)", "%r" % r)
                # a new writable native-FAT (kind 1) volume must have appeared
                nfat_after = len([1 for l in link.command("vols") if l.split()[1] == "1"])
                check(nfat_after > nfat_before, "fresh FAT32 volume mounted",
                      "native-FAT vols %d -> %d" % (nfat_before, nfat_after))
                # write a file to the new volume + read it back byte-exact
                newvol = None
                for l in link.command("vols"):
                    p = l.split(None, 3)
                    if p[1] == "1" and p[2] == "1" and int(p[0]) > 0:
                        newvol = int(p[0])
                if newvol is not None:
                    payload = bytes((i * 5 + 1) & 0xFF for i in range(4000))
                    exp2 = sum((i + 1) * c for i, c in enumerate(payload)) & 0xFFFFFFFF
                    tf = tempfile.NamedTemporaryFile(delete=False); tf.write(payload); tf.close()
                    try:
                        v = link.push_file(newvol, "HELLO.BIN", tf.name)
                    finally:
                        os.unlink(tf.name)
                    check(v, "push a file onto the fresh volume", "vol %d" % newvol)
                    out = link.eval('import uno; d=uno.read(%d,"HELLO.BIN"); '
                                    'print(len(d), sum((i+1)*c for i,c in enumerate(d))&0xffffffff)'
                                    % newvol, timeout=25)
                    g = out[0].split() if out else []
                    check(len(g) == 2 and int(g[0]) == 4000 and int(g[1]) == exp2,
                          "read-back from the fresh volume matches", "got=%r" % out)

                    # 8b) install primitives: mkdir a tree, push into it, boot entry
                    link.mkdir(newvol, "EFI")
                    link.mkdir(newvol, "EFI\\BOOT")
                    tf2 = tempfile.NamedTemporaryFile(delete=False)
                    tf2.write(b"MZ" + bytes(2048)); tf2.close()
                    try:
                        sub = link.push_file(newvol, "EFI\\BOOT\\BOOTX64.EFI", tf2.name)
                    finally:
                        os.unlink(tf2.name)
                    check(sub, "mkdir + push into a created subdir")
                    try:
                        r = link.makeboot(D)
                        check(bool(r) and r[0] == "boot-entry added",
                              "makeboot: UEFI boot entry authored for the fresh ESP", "%r" % r)
                    except Exception as e:  # noqa: BLE001
                        check(False, "makeboot: UEFI boot entry authored", str(e))
                else:
                    check(False, "found the fresh writable volume in vols")
        except Exception as e:  # noqa: BLE001
            check(False, "raw-disk authoring (partition/format)", str(e))

        # 9) devices: the read-only introspection verb. unodevices is not on
        #    master yet, so the weak devmgr_list_str stub answers today - this
        #    asserts the verb is WIRED (dispatch reaches it and it fails with the
        #    pending message, never "unknown-verb"), and upgrades itself to the
        #    real assertion the moment the strong symbol links in.
        try:
            rows = link.devices(timeout=10)
            check(len(rows) > 0, "devices: listing returned", "%d device(s)" % len(rows))
            check(all(":" in r["loc"] for r in rows), "devices: rows parse as bb:dd.f",
                  "%r" % (rows[:3],))
        except RuntimeError as e:  # device replied err - stub, or a real failure
            check("unodevices pending" in str(e),
                  "devices: verb wired (unodevices not built yet)", str(e))
        except Exception as e:  # noqa: BLE001
            check(False, "devices: verb wired", str(e))

    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.5)
        q.kill()
        link.close()

    print("\n" + (">> remote channel OK" if ok else ">> remote channel FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
