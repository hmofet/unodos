#!/usr/bin/env python3
"""End-to-end gate for the netsock multi-connection layer, in QEMU.

Proves the capability the base stack lacked - MANY connections at once, and
TCP *accept* (being a server), not just one client dial-out:

  1. pc64 opens TWO simultaneous outbound TCP connections (to host listeners
     reached via SLIRP's 10.0.2.2) - both reach TCP_ESTABLISHED.
  2. pc64 LISTENs on a port and ACCEPTs an inbound connection the host dials in
     through QEMU hostfwd - net_accept() hands back a connected child socket.
  3. net_sock_count() reflects link + 2 outbound + listener + child all live.

Driven by the debug-only `nst <p1> <p2>` URC verb (unoauto_remote.c). Reuses
remote_qemu's disk builder + the remote link. Needs a debug build first:
    UNO_DEBUG=1 ./build.sh
Run under WSL (qemu-system-x86_64 + OVMF + mtools). Exit 0 iff all checks pass.
"""
import os, sys, socket, subprocess, threading, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq
from unoauto_remote import UnoAutoLink

P1, P2 = 5501, 5502          # host listeners the guest dials OUT to (via 10.0.2.2)
HFWD   = 5591                # host port -> guest :9099 (guest ACCEPTs this)
LINK   = rq.PORT             # the remote channel (guest dials in here)


def hold_listener(port, bag):
    """Accept one connection and hold it open (so the guest stays ESTABLISHED)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port)); srv.listen(1)
    bag.append(srv)
    def acc():
        try:
            c, _ = srv.accept(); bag.append(c)
        except OSError:
            pass
    threading.Thread(target=acc, daemon=True).start()
    return srv


def boot_qemu_hostfwd():
    sh = rq.sh
    sh(["cp", rq.OVMF_VARS, rq.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + rq.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + rq.VARS,
        "-drive", "format=raw,file=" + rq.DISK,
        # SLIRP: guest->10.0.2.2:P reaches host 127.0.0.1:P; hostfwd lets the
        # host reach the guest's :9099 listener at 127.0.0.1:HFWD.
        "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:9099" % HFWD,
        "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def main():
    if not os.path.exists(os.path.join(rq.ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)"); return 1

    bag = []                             # keep host-side sockets alive to test end
    hold_listener(P1, bag); hold_listener(P2, bag)

    link = UnoAutoLink("127.0.0.1", LINK)
    link.listen()
    rq.build_disk()
    q = boot_qemu_hostfwd()
    ok = True
    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and cond
    try:
        if not link.wait_connected(90):
            print("FAIL: pc64 did not dial in"); return 1
        check(True, "pc64 dialed in")

        # Dial into the guest's listener shortly after `nst` starts its poll loop,
        # so net_accept() has an inbound SYN to hand back.
        def dial_in():
            time.sleep(0.4)
            try:
                s = socket.create_connection(("127.0.0.1", HFWD), timeout=5); bag.append(s)
            except OSError as e:
                print("  (hostfwd dial-in failed: %s)" % e)
        threading.Thread(target=dial_in, daemon=True).start()

        lines = link.command("nst", P1, P2, timeout=20)
        kv = {}
        for l in lines:
            k, _, v = l.partition("=")
            kv[k.strip()] = v.strip()
        print("  device reported:", lines)

        check(kv.get("connA") == "2", "outbound conn A established", "state=%s" % kv.get("connA"))
        check(kv.get("connB") == "2", "outbound conn B established", "state=%s" % kv.get("connB"))
        acc = kv.get("accepted", "-1")
        child = acc.split()[0] if acc else "-1"
        check(child.isdigit() and int(child) >= 0, "listen/accept returned a child", acc)
        check("10.0.2.2" in acc, "accepted peer is the host (10.0.2.2)", acc)
        try:
            cnt = int(kv.get("count", "0"))
        except ValueError:
            cnt = 0
        check(cnt >= 4, "socket count reflects all live sockets", "count=%d (link+A+B+listener[+child])" % cnt)

        print("\n>> " + ("netsock multi-connection OK" if ok else "FAILURES ABOVE"))
        return 0 if ok else 1
    finally:
        for s in bag:
            try: s.close()
            except OSError: pass
        link.close()
        try: q.terminate()
        except Exception: pass


if __name__ == "__main__":
    sys.exit(main())
