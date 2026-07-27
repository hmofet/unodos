#!/usr/bin/env python3
"""Unit gate for the host-side URC discovery responder (no device needed).

netdisc_qemu.py proves the DEVICE half of zero-config discovery against a
throwaway L2 responder. This proves the HOST half that ships in the real
clients: with a UnoAutoLink listening, a UNODISC PROBE (what a `discover` device
broadcasts) must draw an OFFER carrying this listener's ip:port, so the device
can dial in with no configured address. The C# UnoRemote client mirrors the same
wire logic (Urc.cs StartDiscovery), so this covers both.

Exit 0 iff the responder answers a PROBE with a well-formed OFFER for our port.
"""
import os, sys, socket, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink

URC_PORT = 5099
DISC_PORT = 5400


def main():
    link = UnoAutoLink(port=URC_PORT)
    try:
        link.listen()                       # starts the TCP listener + discovery responder
    except OSError as e:
        print("FAIL: could not listen on %d: %s" % (URC_PORT, e)); return 1
    if link._disc is None:
        print("FAIL: discovery responder did not bind UDP :%d (port in use?)" % DISC_PORT)
        link.close(); return 1
    time.sleep(0.3)

    ok = True
    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and bool(cond)

    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.settimeout(3.0)
    try:
        probe.sendto(b"UNODISC 1 PROBE pc64 testdev 1", ("127.0.0.1", DISC_PORT))
        try:
            data, _src = probe.recvfrom(512)
        except socket.timeout:
            print("FAIL: no OFFER within 3s"); link.close(); probe.close(); return 1
        t = data.decode("ascii", "replace").split()
        check(len(t) >= 8, "OFFER is well-formed", repr(data))
        if len(t) >= 8:
            check(t[0] == "UNODISC" and t[1] == "1", "UNODISC v1 header", " ".join(t[:2]))
            check(t[2] == "OFFER", "type is OFFER", t[2])
            check(t[3] == "host", "role is host", t[3])
            check(t[4] != "" and " " not in t[4], "name is a single token", t[4])
            check(t[6].count(".") == 3, "carries an IPv4 address", t[6])
            check(t[7] == str(URC_PORT), "advertises our URC port", "%s vs %d" % (t[7], URC_PORT))
    finally:
        probe.close()
        link.close()

    print("\n>> " + ("discovery responder OK" if ok else "FAILURES ABOVE"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
