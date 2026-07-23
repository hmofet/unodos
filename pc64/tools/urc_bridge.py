#!/usr/bin/env python3
"""urc_bridge - headless file-driven driver for the UnoDOS remote channel.

    ~/urc/session.log   every LOG/MSG/RSP frame, timestamped
    ~/urc/cmd.txt       append one command per line; the bridge sends it
                          "/msg <text>"                    free-form message
                          "push <vol> <path> <localfile>"  A/B OS update (chunked)
                          anything else                    a URC command (iwl/reboot/vols/...)
Usage: python3 urc_bridge.py [port]
"""
import os, sys, time, datetime
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unoauto_remote import UnoAutoLink

HOME = os.path.expanduser("~/urc")
LOG = os.path.join(HOME, "session.log")
CMD = os.path.join(HOME, "cmd.txt")


def log(line):
    ts = datetime.datetime.now().strftime("%H:%M:%S")
    with open(LOG, "a") as f:
        f.write("[%s] %s\n" % (ts, line))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5099
    os.makedirs(HOME, exist_ok=True)
    open(CMD, "a").close()
    link = UnoAutoLink("0.0.0.0", port)
    link.on_log(lambda ch, t: log("LOG %-7s %s" % (ch, t)))
    link.on_message(lambda m: log("MSG %s" % m))
    link.listen()
    log("bridge up, listening on :%d" % port)
    was = False
    pos = os.path.getsize(CMD)
    while True:
        now = link._connected.is_set()
        if now != was:
            log("link %s" % ("CONNECTED" if now else "disconnected"))
            was = now
        try:
            sz = os.path.getsize(CMD)
            if sz > pos:
                with open(CMD) as f:
                    f.seek(pos); new = f.read()
                pos = sz
                for raw in new.splitlines():
                    cmd = raw.strip()
                    if not cmd:
                        continue
                    log(">> %s" % cmd)
                    if cmd.startswith("/msg "):
                        link.message(cmd[5:]); continue
                    if cmd.startswith("push "):
                        parts = cmd.split(None, 3)
                        if len(parts) != 4:
                            log("   ! push needs: push <vol> <path> <localfile>"); continue
                        _, vol, path, local = parts
                        try:
                            ok = link.push_file(int(vol), path, local, timeout=90.0,
                                                progress=lambda o, t: log("   push %d/%d" % (o, t)))
                            log("   push %s" % ("VERIFIED" if ok else "FAILED"))
                        except Exception as e:  # noqa: BLE001
                            log("   ! push error: %s" % e)
                        continue
                    verb, _, rest = cmd.partition(" ")
                    args = rest.split(" ") if rest else []
                    try:
                        to = 90.0 if verb in ("test", "py") else 15.0
                        for l in link.command(verb, *args, timeout=to):
                            log("   %s" % l)
                        log("   ok")
                    except Exception as e:  # noqa: BLE001
                        log("   ! %s" % e)
            elif sz < pos:
                pos = sz
        except OSError as e:
            log("bridge error: %s" % e)
        time.sleep(0.3)


if __name__ == "__main__":
    main()
