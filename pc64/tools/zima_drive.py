#!/usr/bin/env python3
"""Zima r8169 bring-up driver over the unoautomate URC link.

Listens for the Zimaboard to dial in (STRESS.CFG remote=192.168.2.100:5099 over
the ASIX USB-ethernet), then runs a read-only discovery sweep of the onboard
Realtek r8169 via the `eth` verb. Everything (LOG stream + command replies) is
echoed to stdout and appended to /tmp/zima-drive.log.

  python3 tools/zima_drive.py [--port 5099] [--wait 1200]

The sweep does NOT write any NIC registers (no wreg/wphy) - it maps + brings the
NIC up (eth rerun) and reads state, so it is safe to run unattended. Interactive
poking (wreg/wphy, retry loops) comes after, once we see what this sweep shows.
"""
import sys, os, time, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink

LOG = "/tmp/zima-drive.log"

def stamp():
    return time.strftime("%H:%M:%S")

def emit(line):
    s = "%s %s" % (stamp(), line)
    print(s, flush=True)
    try:
        with open(LOG, "a") as f:
            f.write(s + "\n")
    except OSError:
        pass

def run_cmd(link, verb, *args):
    label = " ".join([verb] + [str(a) for a in args])
    try:
        lines = link.command(verb, *args, timeout=8.0)
        for l in lines:
            emit("  [%s] %s" % (label, l))
        if not lines:
            emit("  [%s] (ok, no output)" % label)
    except Exception as e:
        emit("  [%s] ERROR: %s" % (label, e))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5099)
    ap.add_argument("--wait", type=float, default=1200.0,
                    help="seconds to wait for the board to dial in")
    args = ap.parse_args()

    emit("=== zima_drive: listening on 0.0.0.0:%d (waiting up to %.0fs) ===" % (args.port, args.wait))
    link = UnoAutoLink(port=args.port)
    link.on_log(lambda chan, text: emit("  LOG[%s] %s" % (chan, text)))
    link.on_message(lambda text: emit("  MSG %s" % text))
    link.listen()

    if not link.wait_connected(args.wait):
        emit("!! no dial-in within %.0fs - is the board booted and on the LAN?" % args.wait)
        return 1
    emit(">> board connected; waiting for HELLO...")
    link.wait_hello(30.0)
    emit(">> HELLO received - board is up and draining. Settling 3s for boot LOGs...")
    time.sleep(3.0)

    emit("=== SUBSYSTEM PROBE ===")
    try:
        for r in link.probe():
            emit("  probe kind=%d state=%d v1=%d v2=%d name=%s"
                 % (r["kind"], r["state"], r["v1"], r["v2"], r["name"]))
    except Exception as e:
        emit("  probe ERROR: %s" % e)

    emit("=== r8169 DISCOVERY SWEEP (read-only) ===")
    run_cmd(link, "eth", "status")          # present/up/dev/mmio before bring-up
    run_cmd(link, "eth", "rerun")           # force probe + BAR map + hw_start()
    run_cmd(link, "eth", "status")          # after bring-up
    run_cmd(link, "eth", "link")            # PHYstatus decode + negotiated speed
    run_cmd(link, "eth", "mac")             # MAC from MAC0/MAC4
    # key MMIO registers (r32 reads; harmless)
    for off, what in [("37","ChipCmd"), ("3c","IntrMask"), ("3e","IntrStatus"),
                      ("40","TxConfig"), ("44","RxConfig"), ("6c","PHYstatus"),
                      ("f0","MISC")]:
        run_cmd(link, "eth", "reg", off)    # (%s)" % what -- label in log via verb
    # PHY registers over PHYAR: 0=BMCR 1=BMSR 2/3=PHY ID 4=ANAR 5=ANLPAR
    for reg in ["0","1","2","3","4","5"]:
        run_cmd(link, "eth", "phy", reg)

    emit("=== sweep complete. Link left UP; board stays on desktop (noshutdown). ===")
    emit("=== leaving connection open 20s to catch trailing LOGs, then exit (frees port). ===")
    time.sleep(20.0)
    link.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
