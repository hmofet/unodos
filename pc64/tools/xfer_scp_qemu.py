#!/usr/bin/env python3
"""End-to-end gate for unoxfer's SCP backend, in QEMU, against a real sshd.

WHY THIS IS SEPARATE FROM xfer_qemu.py.  That gate covers the HTTP family and
proves the engine; this one covers the backend the engine will actually be
pointed at most of the time, and it needs something the other does not: a real
SSH server, a real key exchange, and a real `scp` on the far end.  Keeping them
apart means the common gate stays runnable on a box with no sshd.

IT DOES NOT TOUCH YOUR SSH SETUP.  It starts a THROWAWAY sshd under a scratch
directory, on a high port, with its own host key and its own authorized_keys,
and kills it at the end.  Nothing is added to ~/.ssh, and nothing outside the
scratch directory is written - a test that edits the developer's own
authorized_keys to prove a client works has traded a real risk for a
convenience.

The key is generated ON THE DEVICE (`ssh keygen`), its public half is read back
over URC and installed in the throwaway server's authorized_keys.  That is not
incidental: it exercises the one credential path a headless box actually has,
and it is why unoxfer has no key store of its own.

    UNO_DEBUG=1 ./build.sh
    python3 tools/xfer_scp_qemu.py

Exit 0 iff every check passes.
"""
import os, sys, time, shutil, subprocess, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink
import remote_qemu

URC_PORT = 5396
GUEST_HOST = "10.0.2.2"
KEYNAME = "xfergate"


def free_port(lo=2290, hi=2320):
    """CHOOSE the port, do not hardcode it.

    A fixed port collides with a previous run whose sshd outlived it - which is
    exactly what happened the first time this gate ran, and which presents as
    "the throwaway sshd would not start" rather than as "something is already
    listening there".
    """
    import socket as _s
    for p in range(lo, hi):
        t = _s.socket(_s.AF_INET, _s.SOCK_STREAM)
        try:
            t.bind(("127.0.0.1", p))
            return p
        except OSError:
            continue
        finally:
            t.close()
    return 0


SSH_PORT = 0                      # filled in by main()

TREE = {
    "readme.txt":     b"UnoTransfer over SCP\n",
    "odd.bin":        bytes((i * 29 + 7) & 0xFF for i in range(3333)),
    "sub/nested.bin": bytes((i * 11 + 3) & 0xFF for i in range(900)),
}


def build_tree(root):
    for rel, data in TREE.items():
        p = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)


def start_sshd(base):
    """A throwaway sshd, entirely inside `base`."""
    hostkey = os.path.join(base, "host_ed25519")
    authk = os.path.join(base, "authorized_keys")
    cfg = os.path.join(base, "sshd_config")
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", hostkey],
                   check=True)
    open(authk, "w").close()
    os.chmod(authk, 0o600)
    with open(cfg, "w") as f:
        f.write(
            "Port %d\n"
            "ListenAddress 127.0.0.1\n"
            "HostKey %s\n"
            "AuthorizedKeysFile %s\n"
            "PasswordAuthentication no\n"
            "KbdInteractiveAuthentication no\n"
            "UsePAM no\n"
            "PidFile %s/sshd.pid\n"
            "StrictModes no\n"
            "PubkeyAcceptedAlgorithms +ssh-ed25519\n"
            % (SSH_PORT, hostkey, authk, base))
    p = subprocess.Popen(["/usr/sbin/sshd", "-D", "-e", "-f", cfg],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(0.7)
    if p.poll() is not None:
        print("FAIL: the throwaway sshd would not start:\n" +
              p.stderr.read().decode(errors="replace"))
        return None, authk
    return p, authk


def main():
    esp = os.path.join(HERE, "..", "build", "esp")
    if not os.path.isdir(esp) or not os.path.exists(os.path.join(esp, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1
    if not shutil.which("ssh-keygen") or not os.path.exists("/usr/sbin/sshd"):
        print("SKIP: no sshd/ssh-keygen on this host")
        return 0

    global SSH_PORT
    SSH_PORT = free_port()
    if not SSH_PORT:
        print("SKIP: no free port in 2290-2320")
        return 0

    base = tempfile.mkdtemp(prefix="xfer-sshd-")
    served = os.path.join(base, "served")
    build_tree(served)
    sshd, authk = start_sshd(base)
    if not sshd:
        shutil.rmtree(base, ignore_errors=True)
        return 1
    print("     throwaway sshd on 127.0.0.1:%d, scratch in %s" % (SSH_PORT, base))

    remote_qemu.PORT = URC_PORT
    link = UnoAutoLink("127.0.0.1", URC_PORT)
    link.listen()
    remote_qemu.build_disk()
    q = remote_qemu.boot_qemu()

    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and bool(cond)

    def xfer(*a, timeout=60):
        return link.command("xfer", *a, timeout=timeout)

    try:
        if not link.wait_connected(120):
            print("FAIL: the guest never dialled in - is this the DEBUG build?")
            return 1
        print("PASS guest dialled in")

        # --- a key, generated ON THE DEVICE ---------------------------------
        try:
            link.command("ssh", "keyrm", KEYNAME, timeout=30)
        except RuntimeError:
            pass                                  # it was not there; fine
        out = link.command("ssh", "keygen", KEYNAME, timeout=60)
        check(bool(out), "the device generated an ed25519 key", " ".join(out)[:70])

        pub = link.command("ssh", "keypub", KEYNAME, timeout=30)
        line = next((l for l in pub if l.startswith("ssh-ed25519")), "")
        check(line.startswith("ssh-ed25519"), "read the public half back over URC",
              line[:60])
        if not line:
            return 1
        with open(authk, "w") as f:
            f.write(line + "\n")
        os.chmod(authk, 0o600)

        user = os.environ.get("USER") or "arin"
        site = "xgate"
        r = xfer("site", site, "scp", GUEST_HOST, str(SSH_PORT), user, KEYNAME, served)
        check(any("saved" in l for l in r), "saved an SCP site", " ".join(r)[:80])

        # --- listing over SSH, which SCP does by parsing `ls` ---------------
        ls = xfer("ls", site, served)
        names = " ".join(ls)
        check("readme.txt" in names and "odd.bin" in names and "sub" in names,
              "xfer ls over SCP (ls -l parsed)", names[:100])
        check(any(l.startswith("d ") and l.endswith("sub") for l in ls),
              "the directory row is typed as a directory", names[:100])

        # --- and the transfer ----------------------------------------------
        r = xfer("pull", site, served, "1", "\\SCPT", "-r")
        rid = None
        for l in r:
            if l.startswith("id="):
                rid = int(l.split("=")[1].split()[0])
        check(rid is not None, "recursive SCP pull started", " ".join(r)[:80])

        last = ""
        t0 = time.time()
        while time.time() - t0 < 240:
            st = xfer("status", str(rid))
            last = st[0] if st else ""
            if "state=done" in last or "state=failed" in last:
                break
            time.sleep(1.0)
        check("state=done" in last, "recursive SCP job reached done", last[:120])

        def guest_sum(path):
            out = link.eval(
                'import uno; d=uno.read(1,"%s"); '
                'print(len(d) if d is not None else -1, '
                '(sum((i+1)*b for i,b in enumerate(d))&0xffffffff) if d else 0)'
                % path.replace("\\", "\\\\"), timeout=60)
            parts = out[0].split() if out else []
            if not parts:
                return (-1, 0)
            return (int(parts[0]), int(parts[1]) if len(parts) > 1 else 0)

        for rel, data in TREE.items():
            dos = "\\SCPT\\" + "\\".join(
                (p.split(".")[0].upper()[:8] + "." + p.split(".")[-1].upper()[:3])
                if "." in p else p.upper()[:8]
                for p in rel.split("/"))
            got = guest_sum(dos)
            want = (len(data), sum((i + 1) * b for i, b in enumerate(data)) & 0xFFFFFFFF)
            check(got == want, "SCP: " + rel, "%s -> %r want %r" % (dos, got, want))

        # --- push the other way, and read it back on the HOST ---------------
        r = xfer("push", site, "1", "\\SCPT\\README.TXT", served + "/back.txt")
        pid = None
        for l in r:
            if l.startswith("id="):
                pid = int(l.split("=")[1].split()[0])
        if pid is not None:
            t0 = time.time()
            while time.time() - t0 < 120:
                st = xfer("status", str(pid))
                last = st[0] if st else ""
                if "state=done" in last or "state=failed" in last:
                    break
                time.sleep(1.0)
            check("state=done" in last, "SCP push reached done", last[:110])
            hostside = os.path.join(served, "back.txt")
            got = open(hostside, "rb").read() if os.path.exists(hostside) else b""
            check(got == TREE["readme.txt"],
                  "the pushed file arrived on the HOST byte-identical",
                  "%d bytes" % len(got))

        # --- a MISMATCHED host key must stop, not shrug --------------------
        # Restart the server with a DIFFERENT host key: the box has already
        # recorded the first one, so this is the trust-on-first-use promise
        # being tested rather than described.
        sshd.kill(); sshd.wait()
        os.remove(os.path.join(base, "host_ed25519"))
        os.remove(os.path.join(base, "host_ed25519.pub"))
        sshd2, _ = start_sshd(base)
        if sshd2:
            with open(authk, "w") as f:
                f.write(line + "\n")
            os.chmod(authk, 0o600)
            try:
                msg = " ".join(xfer("ls", site, served))
            except RuntimeError as e:
                msg = str(e)
            check("MISMATCH" in msg.upper(),
                  "a changed host key is REFUSED, loudly", msg[:100])
            sshd2.kill()
    finally:
        try:
            link.close()
        except Exception:
            pass
        q.kill()
        try:
            sshd.kill()
        except Exception:
            pass
        shutil.rmtree(base, ignore_errors=True)

    print("\n%s" % ("PASSED" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
