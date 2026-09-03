#!/usr/bin/env python3
"""urctail.py -- the Cosmo's platform log, live, on the dev PC (M6).

Dials into the URC listener UnoDOS runs on the Cosmo (:5099), prints every
LOG frame as it arrives -- on connect the box replays the last 32 KB of its
ring (the boot story), then streams each line as it is logged -- and, if
asked, runs a verb first. This is the "debug in a live OS" loop: no reboot
into Linux, no readlog.sh, the msdc:/usb-bulk:/net:/perf:/pc64: lines land
here as they happen.

    python3 cosmo64/urctail.py                 # find the box, tail its log
    python3 cosmo64/urctail.py 192.168.2.254   # a known address
    python3 cosmo64/urctail.py --verb probe    # run a verb, then tail
    python3 cosmo64/urctail.py --grab out.png  # QOI screen grab to a PNG, then tail

Finding the box: it is a DHCP lease and it moves, so with no address given
every host on 192.168.2.0/24 is tried on :5099 -- except .100, which is
devbuntu's x86 URC bridge and answers HELLO with no verbs. Do NOT probe
the box's port with a bare connect() from elsewhere while this runs: it
serves one connection at a time and reclaims the slot only after a silent-
link timeout.
"""
import os, socket, struct, sys, threading, time, zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "pc64", "tools"))
from unoauto_remote import UnoAutoLink

SKIP = {"192.168.2.100"}
PORT = 5099


def sweep():
    found = []
    def t(i):
        ip = "192.168.2.%d" % i
        if ip in SKIP:
            return
        try:
            found.append((ip, socket.create_connection((ip, PORT), timeout=1.5)))
        except OSError:
            pass
    ths = [threading.Thread(target=t, args=(i,)) for i in range(2, 255)]
    for x in ths: x.start()
    for x in ths: x.join()
    for _, s in found[1:]:
        s.close()
    return found[0] if found else (None, None)


def write_png(path, w, h, rgba):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    rows = []
    for y in range(h):
        row = bytearray(b"\0")
        for x in range(w):
            o = (y * w + x) * 4
            row += rgba[o:o + 3]
        rows.append(bytes(row))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main(argv):
    verb, grab, ip = None, None, None
    a = argv[1:]
    while a:
        x = a.pop(0)
        if x == "--verb":
            verb = a.pop(0).split()
        elif x == "--grab":
            grab = a.pop(0)
        else:
            ip = x
    if ip:
        sock = socket.create_connection((ip, PORT), timeout=5)
    else:
        print("sweeping 192.168.2.0/24 for a URC listener on :%d ..." % PORT, flush=True)
        ip, sock = sweep()
        if not sock:
            sys.exit("no listener -- is UnoDOS up with the hub, and leased?")
    print("connected to %s:%d" % (ip, PORT), flush=True)
    link = UnoAutoLink()
    link.on_log(lambda ch, text: print("[%s] %s" % (ch, text), flush=True))
    link.attach_stream(sock)
    if not link.wait_hello(20):
        sys.exit("no HELLO within 20 s")
    if verb:
        try:
            for line in link.command(*verb, timeout=30.0):
                print("  > " + line)
        except Exception as e:
            print("  verb failed: %s" % e)
    if grab:
        w, h, rgba = link.screen_grab(scale=2, timeout=60.0)
        write_png(grab, w, h, rgba)
        print("wrote %s (%dx%d)" % (grab, w, h), flush=True)
    print("--- tailing (Ctrl-C to stop) ---", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    link.close()


if __name__ == "__main__":
    main(sys.argv)
