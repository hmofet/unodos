#!/usr/bin/env python3
"""urc_bridge - headless file-driven driver for the UnoDOS remote channel.

    ~/urc/session.log   every LOG/MSG/RSP frame, timestamped
    ~/urc/cmd.txt       append one command per line; the bridge sends it
                          "/msg <text>"                    free-form message
                          "push <vol> <path> <localfile>"  A/B OS update (chunked)
                          anything else                    a URC command (iwl/reboot/vols/...)
Usage: python3 urc_bridge.py [port]

Robustness note - recovering a link the box drops without a FIN
---------------------------------------------------------------
The ZimaBlade re-flashes / hard-resets itself over the `reboot` verb, which
tears the box down WITHOUT sending a TCP FIN. Its old ESTABLISHED socket then
lingers on our side, and UnoAutoLink's single-threaded reader stays blocked in
recv() on that corpse - so it never loops back to accept() the box's re-dial,
and session.log goes silent until a manual restart.

TCP keepalive cannot fix this: the box runs UnoDOS's own minimal TCP stack,
which does not answer keepalive probes, so any keepalive aggressive enough to
notice the corpse also tears down a healthy-but-quiet link. Instead we use the
box's own re-dial as the liveness signal. This bridge is the only listener and
the box is the only client, so exactly one ESTABLISHED connection to our port
is normal; a SECOND ESTABLISHED connection means the box has re-dialed behind a
stale socket. When we see that, we shutdown() the stale socket, which makes the
blocked recv() return so the accept loop takes the fresh dial-in on its own -
no peer cooperation, no manual restart, recovery within a poll (~1s).
"""
import os, sys, time, datetime, socket
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unoauto_remote import UnoAutoLink

HOME = os.path.expanduser("~/urc")
LOG = os.path.join(HOME, "session.log")
CMD = os.path.join(HOME, "cmd.txt")


def log(line):
    ts = datetime.datetime.now().strftime("%H:%M:%S")
    with open(LOG, "a") as f:
        f.write("[%s] %s\n" % (ts, line))


def established_to_port(port):
    """Count ESTABLISHED IPv4 TCP connections whose LOCAL port == `port`, from
    /proc/net/tcp (state 01 = ESTABLISHED). Returns -1 if the file can't be read.
    The listening socket (state 0A) and closing sockets (FIN-WAIT/TIME-WAIT) are
    not counted, so a clean reconnect never trips the re-dial detector - only a
    genuine stale-plus-fresh overlap does."""
    hexport = "%04X" % port
    n = 0
    try:
        with open("/proc/net/tcp") as f:
            next(f)  # skip header
            for line in f:
                p = line.split()
                if len(p) < 4:
                    continue
                if p[3] == "01" and p[1].endswith(":" + hexport):
                    n += 1
    except OSError:
        return -1
    return n


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
    preempted = None          # the stale socket object we last shut down
    cooldown = 0              # loops to wait after a preempt for TCP states to settle
    pos = os.path.getsize(CMD)
    while True:
        # Re-dial preemption: if the box has dialed in again while our reader is
        # still holding a (now stale) socket, a second ESTABLISHED conn appears
        # on our port. Shut the stale socket down so the reader unblocks and the
        # accept loop takes the fresh dial-in. UnoAutoLink keeps the live socket
        # in ._sock (None while disconnected); identity tracking + a short
        # cooldown keep us from ever hitting a freshly-accepted healthy socket.
        if cooldown > 0:
            cooldown -= 1
        else:
            sk = getattr(link, "_sock", None)
            if sk is not None and sk is not preempted and established_to_port(port) >= 2:
                log("re-dial detected on :%d - preempting stale link socket" % port)
                try:
                    sk.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                preempted = sk
                cooldown = 8          # ~2.4s: let the stale socket leave ESTABLISHED
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
