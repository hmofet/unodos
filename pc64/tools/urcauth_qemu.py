#!/usr/bin/env python3
"""End-to-end gate for the URC PRIVILEGE GATE (unoauto_gate.c), in QEMU.

unoautomate and the URC channel ship in production, where the channel is
disarmed until a console user arms it and every connection must present a
token (see pc64/unoauto_gate.h).  That security path is the one part of the
subsystem a normal run never exercises: production images have no DEBUG.CFG,
and arming needs a human at a screen reading a random token.

The `urc-auth=<token>` DEBUG.CFG key exists for exactly this.  It runs the
PRODUCTION rules in a debug image with a token we chose, and arms
OBSERVE|DRIVE but deliberately NOT SYSTEM - so one boot can prove all three
outcomes:

  1. a verb before `auth`                 -> err auth-required
  2. a wrong token                        -> err, and does not authenticate
  3. the right token                      -> ok, and names the granted powers
  4. an OBSERVE verb (probe)              -> ok
  5. a DRIVE verb (launch)                -> ok
  6. a SYSTEM verb (writesec / py)        -> err "denied (needs automate.system)"
  7. `caps`                               -> reports observe/drive on, system off
  8. an unknown verb                      -> err unknown-verb (fail-closed table)

Needs UNO_DEBUG=1 ./build.sh + WSL (qemu/OVMF/mtools).  Exit 0 iff all pass.
"""
import os, sys, time, socket, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq
import listen_qemu as lq
from unoauto_remote import UnoAutoLink

DISK  = "/tmp/urcauth_disk.img"
FAT   = "/tmp/urcauth_fat.img"
LPORT = 5099
HFWD  = 5698
TOKEN = "147025"                    # 6 digits, as UNOAUTO_TOKEN_CHARS wants
SECTOR, MIB = 512, 1 << 20


def build_disk():
    """build/esp with a DEBUG.CFG of `listen=<LPORT>` + `urc-auth=<TOKEN>`."""
    disk_sectors = 96 * 2048
    with open(DISK, "wb") as f:
        f.truncate(disk_sectors * SECTOR)
    rq.sh(["sgdisk", "--zap-all", DISK], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rq.sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", DISK],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
    with open(FAT, "wb") as f:
        f.truncate(part_sectors * SECTOR)
    rq.sh(["mformat", "-i", FAT, "-F", "-T", str(part_sectors), "::"],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for root, dirs, files in os.walk(rq.ESP):
        rel = os.path.relpath(root, rq.ESP)
        if rel != ".":
            rq.sh(["mmd", "-i", FAT, "::/" + rel.replace(os.sep, "/")],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for fn in files:
            if rel == "." and fn.lower() in ("debug.cfg", "stress.cfg"):
                continue                          # replaced with our own below
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            rq.sh(["mcopy", "-i", FAT, "-o", src, dst],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cfg = "/tmp/urcauth_debug.cfg"
    with open(cfg, "w", newline="\r\n") as f:
        f.write("listen=%d\n" % LPORT)
        f.write("urc-auth=%s\n" % TOKEN)
        f.write("nostress\n")                     # the fuzz driver is not the subject
    rq.sh(["mcopy", "-i", FAT, "-o", cfg, "::/DEBUG.CFG"],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(FAT, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b:
                break
            df.write(b)


def boot_qemu():
    rq.sh(["cp", rq.OVMF_VARS, rq.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + rq.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + rq.VARS,
        "-drive", "format=raw,file=" + DISK,
        "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:%d" % (HFWD, LPORT),
        "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def dial(deadline):
    """Connect to the guest's listener and wait for its HELLO.  Same retry shape
    as listen_qemu.connect_drive: the box may still be booting."""
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", HFWD), timeout=3)
        except OSError:
            time.sleep(1.0); continue
        link = UnoAutoLink()
        link.attach_stream(s)
        if link.wait_hello(6):
            return link, s
        for c in (link, s):
            try: c.close()
            except Exception: pass
        time.sleep(0.5)
    return None, None


def denied(link, verb, *args, timeout=10):
    """Run a verb expecting refusal.  Returns the refusal text, or None if it was
    NOT refused - which is the interesting failure here, so the caller checks for
    a string rather than for an exception.

    A dropped link counts as refused: the lockout check below deliberately gets
    the channel torn down under it, and "the box stopped talking to me" is a
    refusal by any useful definition."""
    try:
        link.command(verb, *args, timeout=timeout)
        return None
    except RuntimeError as e:
        return str(e)
    except (TimeoutError, OSError) as e:
        return "link gone: %s" % e


def main():
    if not os.path.exists(os.path.join(rq.ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1

    build_disk()
    q = boot_qemu()
    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and bool(cond)

    try:
        link, s = dial(time.time() + 120)
        check(link is not None, "dialed into the gated listener and got its HELLO (:%d)" % LPORT)
        if link is None:
            return 1

        # 1. nothing works before auth.
        e = denied(link, "probe")
        check(e is not None and "auth-required" in e,
              "probe BEFORE auth is refused", repr(e))
        e = denied(link, "uptime")
        check(e is not None and "auth-required" in e,
              "uptime BEFORE auth is refused", repr(e))

        # 2. a wrong token does not authenticate.  (Two bad tries - the third
        #    would trip the three-strikes disarm, which is its own check below.)
        e = denied(link, "auth", "ffffffffffffffff")
        check(e is not None, "wrong token rejected", repr(e))
        e = denied(link, "probe")
        check(e is not None and "auth-required" in e,
              "still unauthenticated after a bad token", repr(e))

        # 3. the right token.
        r = link.command("auth", TOKEN, timeout=10)
        check(any("authenticated" in x for x in r), "correct token authenticates", str(r))
        check(any("observe" in x and "drive" in x for x in r),
              "auth names the granted powers", str(r))

        # 4/5. granted verbs work.
        pr = link.probe(timeout=15)
        check(len(pr) > 0, "OBSERVE verb (probe) allowed after auth", "%d rows" % len(pr))
        r = link.command("launch", 0, timeout=15)
        check(r is not None, "DRIVE verb (launch) allowed after auth", str(r))

        # 6. the ungranted power is refused, and says which one is missing.
        e = denied(link, "py", "print(6*7)")
        check(e is not None and "automate.system" in e,
              "SYSTEM verb (py) refused - names the missing power", repr(e))
        e = denied(link, "writesec", "0", "0")
        check(e is not None and "automate.system" in e,
              "SYSTEM verb (writesec) refused", repr(e))
        e = denied(link, "poweroff")
        check(e is not None and "automate.system" in e,
              "SYSTEM verb (poweroff) refused - the box stays up", repr(e))

        # 7. caps reports the truth.
        r = link.command("caps", timeout=10)
        joined = " ".join(r)
        check("observe 1" in joined and "drive 1" in joined and "system 0" in joined,
              "caps reports observe+drive granted, system withheld", str(r))

        # 8. the verb table is fail-closed: no row = no.
        e = denied(link, "definitely-not-a-verb")
        check(e is not None and "unknown-verb" in e,
              "an unknown verb is refused by the gate", repr(e))

        # 9. three bad tokens stand the channel down entirely.  Done last: it
        #    kills the link, and proves a plaintext LAN protocol is not a
        #    brute-force oracle.
        try: link.close()
        except Exception: pass
        try: s.close()
        except Exception: pass
        time.sleep(1.0)
        link2, s2 = dial(time.time() + 30)
        if link2 is None:
            check(False, "re-dialed for the lockout check")
        else:
            check(True, "re-dial requires auth again (link reset on drop)")
            e = denied(link2, "probe")
            check(e is not None and "auth-required" in e,
                  "the new link is NOT still authenticated", repr(e))
            bad = [denied(link2, "auth", "aaaaaaaaaaaaaaaa") for _ in range(3)]
            check(all(b is not None for b in bad),
                  "three bad tokens all refused", repr(bad))
            # The third refusal is ANSWERED before the disarm lands (it is
            # deferred a frame precisely so it can be) - a mistyped code gets
            # told, not dropped.
            check(bad[2] is not None and "link gone" not in bad[2],
                  "the third refusal is answered, not a dropped connection", repr(bad[2]))
            time.sleep(1.0)
            e = denied(link2, "auth", TOKEN, timeout=10)
            check(e is not None,
                  "then the channel is disarmed - the RIGHT token no longer works",
                  repr(e))
            try: link2.close()
            except Exception: pass
            try: s2.close()
            except Exception: pass
    finally:
        try: q.terminate(); q.wait(timeout=10)
        except Exception:
            try: q.kill()
            except Exception: pass

    print(("\n>> URC privilege gate OK" if ok else "\n>> URC privilege gate FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
