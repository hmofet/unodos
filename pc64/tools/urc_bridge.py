#!/usr/bin/env python3
"""urc_bridge - headless file-driven driver for the UnoDOS remote channel.

Drives MANY boxes from ONE process and ONE port. Each box that dials in gets its
own directory under the root:

    <root>/<box>/session.log   every LOG/MSG/RSP frame for that box, timestamped
    <root>/<box>/cmd.txt       append one command per line; the bridge sends it
    <root>/bridge.log          accepts, disconnects, stale-link drops
    <root>/links.txt           who is connected right now
    <root>/boxes.conf          optional "<ip> <name>" lines to name boxes
    <root>/cmd.txt             "@<box> <cmd>", "@all <cmd>", or a bare command
                               when exactly one box is connected

Command forms (in any cmd.txt):
    "/msg <text>"                    free-form message
    "push <vol> <path> <localfile>"  A/B OS update (chunked)
    anything else                    a URC command (iwl/reboot/vols/...)

Usage: python3 urc_bridge.py [ports] [root]    (defaults: 5099, ~/urc)
       ports is one port or several: "5099" or "5099,5098"

Several ports because boxes carry their dial-out address in their own DEBUG.CFG
(`remote=<ip>:<port>`), so consolidating bridges must not require reaching every
box first to repoint it. Listen where they already dial; they all land in the
same place regardless of which port they used.

Why this is no longer one process per box
-----------------------------------------
It used to be, and why that had to change is worth keeping. UnoAutoLink's accept
loop calls its reader INLINE, so it never returns to accept() while a socket is
alive, and it holds exactly one socket. That gave two failure modes, both of
which cost real time on the ZimaBlade:

  * ONE BOX PER PROCESS. Watching two machines meant two bridges on two ports
    with two directories, and no way to drive both from one place.

  * A DEAD LINK WEDGED THE BRIDGE. A box that hard-resets (the `reboot` verb, a
    watchdog, a power cut) sends no FIN, so its socket lingers ESTABLISHED on
    our side. The reader stayed blocked in recv() on that corpse and never
    looped back to accept, so the box's re-dial was never taken and the log
    just went quiet.

The previous fix watched for a SECOND ESTABLISHED connection on the port and
shut the stale socket down. That only works if the box picks a different source
port. UnoDOS's TCP stack picks from a very small pool (:40001 and :40002 in
practice), so when it reuses the port the corpse holds, the kernel cannot
complete the handshake at all: no second connection appears, the detector never
fires, and the bridge stays silent until someone restarts it. That happened
three times in one evening.

So the accept loop now runs on its own thread and never blocks, every link gets
its own reader thread, and a stale link is removed two independent ways:

  1. A fresh connection from a peer that already has a link replaces it at once.
     Covers a box re-dialling from a different source port.
  2. A link silent for IDLE_PROBE_S is probed and dropped if it does not answer
     within PROBE_TIMEOUT_S. Closing our end frees the 4-tuple, which is the
     only thing that lets a box reusing its source port reconnect. Covers the
     case (1) cannot see.

TCP keepalive is still no help: UnoDOS's stack does not answer keepalive probes,
so any keepalive aggressive enough to spot a corpse also tears down a healthy
but quiet link. The probe here is an ordinary URC command, which the box does
answer, and it is only sent when the link is idle with nothing in flight.
"""
import os, sys, time, socket, threading, datetime, queue

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unoauto_remote import UnoAutoLink

IDLE_PROBE_S = 60.0        # silence after which we ask a link whether it lives
PROBE_TIMEOUT_S = 10.0     # how long that probe waits before we call it dead
LONG_VERBS = ("test", "py", "install", "prepdisk", "mkfs")


def _ts():
    return datetime.datetime.now().strftime("%H:%M:%S")


def _drop(link):
    """Close a link's socket for good. Freeing the 4-tuple is the point: a box
    that reuses its source port cannot reconnect until we let go of it."""
    link._stop = True
    sock = link._sock
    for call in ("shutdown", "close"):
        try:
            if sock is None:
                break
            if call == "shutdown":
                sock.shutdown(socket.SHUT_RDWR)
            else:
                sock.close()
        except OSError:
            pass
    link._sock = None
    link._connected.clear()


class _LiveLink(UnoAutoLink):
    """UnoAutoLink with the socket supplied from outside, plus a receive stamp.

    The base class owns its listener and serves one connection at a time. Here
    the bridge owns the listener so it can serve many boxes and hands each
    accepted socket to its own link. last_rx is what the idle probe reads."""

    def __init__(self):
        super().__init__()
        self.last_rx = time.time()

    def _dispatch(self, line):
        self.last_rx = time.time()
        super()._dispatch(line)

    def adopt(self, conn):
        """Drive this link over an already-accepted socket.

        attach_stream() is the library's own name for this and is what the
        serial path uses, so prefer it; the fallback covers an older deployed
        unoauto_remote.py that predates it."""
        attach = getattr(self, "attach_stream", None)
        if attach is not None:
            attach(conn)
            return
        self._sock = conn
        self._connected.set()
        threading.Thread(target=self._reader, args=(conn,), daemon=True).start()
        self.send("HELLO", "host 1")


class Box:
    """One machine: its link, its files, its command stream."""

    def __init__(self, name, root):
        self.name = name
        self.dir = os.path.join(root, name)
        os.makedirs(self.dir, exist_ok=True)
        self.log_path = os.path.join(self.dir, "session.log")
        self.cmd_path = os.path.join(self.dir, "cmd.txt")
        open(self.cmd_path, "a").close()
        self.link = None
        self.busy = 0                      # commands in flight, for the probe
        self.lock = threading.Lock()
        self.pos = os.path.getsize(self.cmd_path)
        # One worker per box, fed by a queue. Per-box commands stay STRICTLY
        # ordered (a box answers one at a time, and interleaved request and
        # response lines in a log you have to read later are worse than
        # useless), while different boxes still run concurrently and neither
        # can stall the accept loop or the liveness probes.
        self.q = queue.Queue()
        threading.Thread(target=self._worker, daemon=True).start()

    def _worker(self):
        while True:
            self.run(self.q.get())

    def log(self, line):
        with open(self.log_path, "a") as f:
            f.write("[%s] %s\n" % (_ts(), line))

    # ---- link lifecycle ---------------------------------------------------
    def attach(self, conn, peer):
        """Take a freshly accepted socket. Any previous link goes first: a box
        only dials again because the old one is gone, whatever our side still
        believes about it."""
        with self.lock:
            if self.link is not None:
                self.log("replacing stale link (re-dial from %s:%d)" % peer)
                _drop(self.link)
            link = _LiveLink()
            link.on_log(lambda ch, t, s=self: s.log("LOG %-7s %s" % (ch, t)))
            link.on_message(lambda m, s=self: s.log("MSG %s" % m))
            link.adopt(conn)
            self.link = link
        self.log("link CONNECTED from %s:%d" % peer)

    def drop(self, why):
        with self.lock:
            if self.link is None:
                return
            _drop(self.link)
            self.link = None
        self.log("link dropped: %s" % why)

    def connected(self):
        link = self.link
        return link is not None and link._connected.is_set()

    # ---- liveness ---------------------------------------------------------
    def probe_if_idle(self):
        """A link that has said nothing for a while may be a corpse, so ask it.
        Only when idle AND nothing is in flight, so a box legitimately busy for
        90 seconds inside a `py` or `test` is never mistaken for dead."""
        link = self.link
        if link is None or self.busy:
            return
        if time.time() - link.last_rx < IDLE_PROBE_S:
            return
        try:
            link.command("uptime", timeout=PROBE_TIMEOUT_S)
            link.last_rx = time.time()
        except Exception:                                        # noqa: BLE001
            self.drop("silent %.0fs, no answer to liveness probe" % IDLE_PROBE_S)

    # ---- command execution ------------------------------------------------
    def run(self, cmd):
        self.log(">> %s" % cmd)
        link = self.link
        if link is None:
            self.log("   ! not connected")
            return
        self.busy += 1
        try:
            if cmd.startswith("/msg "):
                link.message(cmd[5:])
                return
            if cmd.startswith("push "):
                parts = cmd.split(None, 3)
                if len(parts) != 4:
                    self.log("   ! push needs: push <vol> <path> <localfile>")
                    return
                _, vol, path, local = parts
                ok = link.push_file(int(vol), path, local, timeout=90.0,
                                    progress=lambda o, t: self.log("   push %d/%d" % (o, t)))
                self.log("   push %s" % ("VERIFIED" if ok else "FAILED"))
                return
            verb, _, rest = cmd.partition(" ")
            args = rest.split(" ") if rest else []
            to = 90.0 if verb in LONG_VERBS else 15.0
            for line in link.command(verb, *args, timeout=to):
                self.log("   %s" % line)
            self.log("   ok")
        except Exception as e:                                   # noqa: BLE001
            self.log("   ! %s" % e)
        finally:
            self.busy -= 1

    def spawn(self, cmd):
        """Queue for this box's worker. A 90-second command on one box must not
        stop another box, the accept loop, or the liveness probes; but two
        commands to the SAME box must not overlap either."""
        self.q.put(cmd)

    def pump(self):
        """Execute whatever was appended to this box's cmd.txt."""
        try:
            sz = os.path.getsize(self.cmd_path)
            if sz < self.pos:                      # truncated: resync
                self.pos = sz
                return
            if sz == self.pos:
                return
            with open(self.cmd_path) as f:
                f.seek(self.pos)
                new = f.read()
            self.pos = sz
        except OSError as e:
            self.log("cmd read error: %s" % e)
            return
        for raw in new.splitlines():
            cmd = raw.strip()
            if cmd:
                self.spawn(cmd)


class Bridge:
    def __init__(self, ports, root):
        self.ports = ports
        self.root = os.path.expanduser(root)
        os.makedirs(self.root, exist_ok=True)
        self.log_path = os.path.join(self.root, "bridge.log")
        self.cmd_path = os.path.join(self.root, "cmd.txt")
        open(self.cmd_path, "a").close()
        self.cmd_pos = os.path.getsize(self.cmd_path)
        self.boxes = {}                    # name -> Box
        self.names = self._load_names()
        self.lock = threading.Lock()

    def _load_names(self):
        """Optional boxes.conf: '<ip> <name>' per line, '#' starts a comment."""
        names = {}
        try:
            with open(os.path.join(self.root, "boxes.conf")) as f:
                for line in f:
                    line = line.split("#", 1)[0].split()
                    if len(line) >= 2:
                        names[line[0]] = line[1]
        except OSError:
            pass
        return names

    def log(self, line):
        with open(self.log_path, "a") as f:
            f.write("[%s] %s\n" % (_ts(), line))

    def box(self, name):
        with self.lock:
            b = self.boxes.get(name)
            if b is None:
                b = Box(name, self.root)
                self.boxes[name] = b
            return b

    def write_links(self):
        try:
            with open(os.path.join(self.root, "links.txt"), "w") as f:
                for name in sorted(self.boxes):
                    f.write("%-16s %s\n" % (name, "connected"
                                            if self.boxes[name].connected()
                                            else "disconnected"))
        except OSError:
            pass

    # ---- accepting --------------------------------------------------------
    def serve(self):
        opened = []
        for port in self.ports:
            try:
                srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                srv.bind(("0.0.0.0", port))
                srv.listen(8)    # was 1: a queued dial-in must not be refused
            except OSError as e:
                self.log("cannot listen on :%d: %s" % (port, e))
                continue
            opened.append(port)
            threading.Thread(target=self._accept_loop, args=(srv, port),
                             daemon=True).start()
        if not opened:
            self.log("no ports could be opened, giving up")
            raise SystemExit(1)
        self.log("bridge up on %s, root %s"
                 % (",".join(":%d" % p for p in opened), self.root))

    def _accept_loop(self, srv, port):
        """Does nothing but accept. Everything a connection needs afterwards
        happens on its own thread, so no box can wedge the listener."""
        while True:
            try:
                conn, peer = srv.accept()
            except OSError as e:
                self.log("accept failed: %s" % e)
                time.sleep(1.0)
                continue
            name = self.names.get(peer[0], peer[0].replace(".", "-"))
            self.log("accepted %s:%d on :%d as '%s'"
                     % (peer[0], peer[1], port, name))
            try:
                self.box(name).attach(conn, peer)
            except Exception as e:                               # noqa: BLE001
                self.log("attach failed for %s: %s" % (name, e))
            self.write_links()

    # ---- the root command file --------------------------------------------
    def pump_root(self):
        """'@<box> <cmd>' targets one box, '@all <cmd>' every connected one. A
        bare command goes to the only connected box when there is exactly one,
        so the common single-box case stays as convenient as it was."""
        try:
            sz = os.path.getsize(self.cmd_path)
            if sz < self.cmd_pos:
                self.cmd_pos = sz
                return
            if sz == self.cmd_pos:
                return
            with open(self.cmd_path) as f:
                f.seek(self.cmd_pos)
                new = f.read()
            self.cmd_pos = sz
        except OSError:
            return
        for raw in new.splitlines():
            line = raw.strip()
            if not line:
                continue
            if line.startswith("@"):
                target, _, cmd = line[1:].partition(" ")
                cmd = cmd.strip()
                if not cmd:
                    continue
                if target == "all":
                    for b in list(self.boxes.values()):
                        if b.connected():
                            b.spawn(cmd)
                elif target in self.boxes:
                    self.boxes[target].spawn(cmd)
                else:
                    self.log("no such box '%s' for: %s" % (target, cmd))
                continue
            live = [b for b in self.boxes.values() if b.connected()]
            if len(live) == 1:
                live[0].spawn(line)
            else:
                self.log("ambiguous (%d boxes connected), use @<box>: %s"
                         % (len(live), line))

    def run(self):
        self.serve()
        state = {}
        while True:
            self.pump_root()
            for b in list(self.boxes.values()):
                b.pump()
                b.probe_if_idle()
                now = b.connected()
                if state.get(b.name) != now:
                    state[b.name] = now
                    b.log("link %s" % ("CONNECTED" if now else "disconnected"))
                    self.log("%s %s" % (b.name, "connected" if now else "disconnected"))
                    self.write_links()
            time.sleep(0.3)


def main():
    spec = sys.argv[1] if len(sys.argv) > 1 else "5099"
    ports = [int(p) for p in spec.split(",") if p.strip()]
    root = sys.argv[2] if len(sys.argv) > 2 else "~/urc"
    Bridge(ports, root).run()


if __name__ == "__main__":
    main()
