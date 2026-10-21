#!/usr/bin/env python3
"""urc_bridge - headless file-driven driver for the UnoDOS remote channel.

    ~/urc/session.log   every LOG/MSG/RSP frame, timestamped
    ~/urc/cmd.txt       append one command per line; the bridge sends it
                          "/msg <text>"                    free-form message
                          "push <vol> <path> <localfile>"  A/B OS update (chunked)
                          anything else                    a URC command (iwl/reboot/vols/...)
Usage: python3 urc_bridge.py [port]
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


def enable_keepalive(sock):
    """Arm aggressive TCP keepalive on an accepted URC socket.

    The ZimaBlade re-flashes/hard-resets itself over the `reboot` verb, which
    tears the box down WITHOUT sending a FIN. The old ESTABLISHED socket then
    lingers forever on our side, and UnoAutoLink's single-threaded reader stays
    blocked in recv() on that corpse - so it never loops back to accept() the
    box's re-dial, and session.log goes silent until a manual restart.

    Keepalive is the fix: the kernel probes the dead peer, and when the probes
    go unanswered it errors the socket, so the blocked recv() raises, the reader
    returns, and the accept loop picks up the queued re-dial on its own. Timing
    (idle 5s, probe every 2s, 3 misses) declares a vanished peer dead in ~11s.
    A healthy-but-quiet link is never disturbed - its peer's kernel ACKs the
    probes without any application traffic, which also resets the idle timer.
    """
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        if hasattr(socket, "TCP_KEEPIDLE"):
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 5)
        if hasattr(socket, "TCP_KEEPINTVL"):
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 2)
        if hasattr(socket, "TCP_KEEPCNT"):
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
        return True
    except OSError as e:
        log("keepalive setup failed: %s" % e)
        return False


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
    ka_armed = None                       # the socket object we last armed
    pos = os.path.getsize(CMD)
    while True:
        # Arm keepalive on each freshly-accepted link socket. UnoAutoLink stores
        # the live accepted socket in ._sock (None while disconnected); a new
        # dial-in is a new object, so identity tracking re-arms every reconnect.
        sk = getattr(link, "_sock", None)
        if sk is not None and sk is not ka_armed:
            if enable_keepalive(sk):
                log("keepalive armed on link socket")
            ka_armed = sk
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
