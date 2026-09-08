#!/usr/bin/env python3
"""cosmo64/bootlog.py -- catch a whole boot story over URC, before it scrolls.

    python3 cosmo64/bootlog.py                 # wait for the NEXT boot, save it
    python3 cosmo64/bootlog.py --now           # dial whatever is up right now
    python3 cosmo64/bootlog.py -o boot.txt     # where to save (default bootlog.txt)

WHY. The URC connect replay streams the last ~98 KB of the platform log, and
the shell logs a USB bulk statistics line about once a second, so a boot
story is pushed out of that window within a few minutes of the desktop
coming up. On 2026-09-08 the first boot of the M13 image saw no SD card, and
by the time `vols` had said so, the `sd:` lines that would have said why were
gone; the eMMC preamble (readlog.sh, from Trixie) is the only other copy.
This tool removes the race: it waits for the CURRENT listener to disappear
(the reboot), then sweeps the LAN for :5099 every couple of seconds and dials
the moment the shell answers -- which is seconds after the lease, well inside
the window -- records the replay plus a stretch of live log, asks `vols`, and
saves the lot. It prints the storage story and the audio story on the way out.

The address moves (a DHCP lease: .65, .254, .121 and .56 have all been the
phone), so it sweeps rather than remembers. 192.168.2.100 is skipped, as in
urctail.py: that is devbuntu's own :5099, not the Cosmo.
"""
import os, re, socket, sys, threading, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pc64", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unoauto_remote import UnoAutoLink

PORT = 5099
SKIP = {"192.168.2.100"}
STORY = re.compile(r"sdmmc|msdc|blk|storage|card|vol |mount|fat|pmic|rail|afe:|snd|"
                   r"entering uno_main|modload|VERB", re.I)


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


def main(argv):
    now, out, live = False, "bootlog.txt", 45.0
    a = argv[1:]
    while a:
        x = a.pop(0)
        if x == "--now": now = True
        elif x == "-o": out = a.pop(0)
        elif x == "--live": live = float(a.pop(0))
        else: sys.exit("usage: bootlog.py [--now] [-o file] [--live seconds]")
    if not now:
        ip, s0 = sweep()
        if s0:
            s0.close()
            print("UnoDOS is up at %s -- waiting for it to go down (reboot it now)" % ip, flush=True)
            while True:
                try:
                    s0 = socket.create_connection((ip, PORT), timeout=1.5); s0.close(); time.sleep(2)
                except OSError:
                    break
            print("down; waiting for the next boot", flush=True)
    t0 = time.time()
    ip = sock = None
    while time.time() - t0 < 1200:
        ip, sock = sweep()
        if sock:
            break
        time.sleep(2)
    if not sock:
        sys.exit("no URC listener within 20 minutes")
    sock.settimeout(None)         # LOAD-BEARING: see qharness.py's note on the reader thread
    print("connected to %s after %.0f s" % (ip, time.time() - t0), flush=True)
    lines = []
    link = UnoAutoLink()
    link.on_log(lambda ch, t: lines.append("[%s] %s" % (ch, t)))
    link.attach_stream(sock)
    print("hello:", link.wait_hello(30), flush=True)
    time.sleep(live)
    try:
        for row in link.command("vols", timeout=15.0):
            lines.append("[VERB] vols: " + row)
    except Exception as e:
        lines.append("[VERB] vols ERR %s" % e)
    link.close()
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("%d lines saved to %s" % (len(lines), out), flush=True)
    for l in lines:
        if STORY.search(l) and "usb-bulk" not in l and "modload: arena" not in l:
            print(l)


if __name__ == "__main__":
    main(sys.argv)
