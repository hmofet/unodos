#!/usr/bin/env python3
"""unoauto_remote - the dev-PC end of the UnoDOS pc64 remote channel.

pc64 dials OUT to this listener (its TCP stack is client-only), so run this on
the machine you develop from and put its LAN address in the pc64 stick's
STRESS.CFG:  `remote=<this-ip>:<port>`.  Then boot pc64 (a debug build): its
logs stream in here and you can drive it back - probe, launch apps, inject
input, run Python on-device - in either direction.

Protocol (URC): newline-delimited text frames, symmetric both ways.
    HELLO <name> <api>          handshake
    LOG   <chan> <text>         a log line from pc64
    MSG   <text>                free-form message, either direction
    CMD   <id> <verb> <args>    a command request, either direction
    RSP   <id> <ok|err|end> ..  response lines, terminated by `end`
    BYE                         graceful close

Two ways to use it:
  - as a library:   link = UnoAutoLink(port=5099); link.listen()
                     link.wait_connected(); print(link.probe())
  - as a CLI:        python unoauto_remote.py --listen 0.0.0.0:5099
                     (prints incoming logs; type command lines to send)

QEMU: from a SLIRP guest the host is 10.0.2.2, so set remote=10.0.2.2:<port>.
Plaintext, LAN-only by intent - do not expose the port to untrusted networks.
"""
import socket, threading, itertools, sys


class UnoAutoLink:
    def __init__(self, host="0.0.0.0", port=5099):
        self.host, self.port = host, port
        self._srv = None
        self._sock = None
        self._lock = threading.Lock()
        self._ids = itertools.count(1)
        self._pending = {}            # id -> {"lines":[], "ev":Event, "err":bool}
        self._log_cb = None
        self._msg_cb = None
        self._cmd_handlers = {}
        self._connected = threading.Event()
        self._stop = False

    # ---- lifecycle --------------------------------------------------------
    def listen(self):
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind((self.host, self.port))
        self._srv.listen(1)
        threading.Thread(target=self._accept_loop, daemon=True).start()
        return self

    def close(self):
        self._stop = True
        for s in (self._sock, self._srv):
            try:
                if s: s.close()
            except OSError:
                pass

    def wait_connected(self, timeout=30.0):
        return self._connected.wait(timeout)

    # ---- callbacks --------------------------------------------------------
    def on_log(self, cb):     self._log_cb = cb            # cb(chan, text)
    def on_message(self, cb): self._msg_cb = cb            # cb(text)
    def on_command(self, verb, cb):                        # cb(args)->str|list
        self._cmd_handlers[verb] = cb

    # ---- sending ----------------------------------------------------------
    def _raw(self, line):
        with self._lock:
            s = self._sock
        if s:
            try:
                s.sendall((line + "\n").encode("utf-8"))
            except OSError:
                pass

    def send(self, typ, text=""):
        self._raw(typ + " " + text if text else typ)

    def message(self, text):
        self.send("MSG", text)

    def command(self, verb, *args, timeout=5.0):
        """Send `CMD <verb> args`, block for the response, return its ok lines.
        Raises TimeoutError or RuntimeError(err text)."""
        rid = str(next(self._ids))
        rec = {"lines": [], "ev": threading.Event(), "err": False}
        with self._lock:
            self._pending[rid] = rec
        payload = " ".join([verb] + [str(a) for a in args])
        self._raw("CMD %s %s" % (rid, payload))
        ok = rec["ev"].wait(timeout)
        with self._lock:
            self._pending.pop(rid, None)
        if not ok:
            raise TimeoutError("no response to %r" % verb)
        if rec["err"]:
            raise RuntimeError("\n".join(rec["lines"]) or "error")
        return rec["lines"]

    # convenience wrappers over the command language
    def probe(self, **k):
        """Return the PROBE snapshot as dicts: kind/state/v1/v2/name.
        kind 0=module 1=window 2=subsystem (see unoauto.h)."""
        rows = []
        for l in self.command("probe", **k):
            p = l.split(None, 4)
            if len(p) < 5:
                continue
            rows.append({"kind": int(p[0]), "state": int(p[1]),
                         "v1": int(p[2]), "v2": int(p[3]), "name": p[4]})
        return rows
    def logline(self, text, **k):      return self.command("log", text, **k)
    def key(self, scan, uni, ctrl=0, **k): return self.command("key", scan, uni, ctrl, **k)
    def pointer(self, x, y, btn=0, **k):   return self.command("pointer", x, y, btn, **k)
    def apps(self, **k):               return int(self.command("apps", **k)[0])
    def launch(self, n, **k):          return self.command("launch", n, **k)
    def close_top(self, **k):          return self.command("close", **k)
    def uptime(self, **k):             return int(self.command("uptime", **k)[0])
    def poweroff(self, **k):           return self.command("poweroff", **k)
    def test(self, suite="", **k):     return self.command("test", suite, timeout=k.pop("timeout", 60.0), **k)
    def eval(self, src, **k):          return self.command("py", src, timeout=k.pop("timeout", 20.0), **k)
    def reboot(self, **k):             return self.command("reboot", **k)
    def bootnext(self, n, **k):        return self.command("bootnext", n, **k)

    def vols(self, **k):
        """List volumes: dicts of vol/kind/writable/name (kind 0=RAM 1=FAT 2=SFS)."""
        out = []
        for l in self.command("vols", **k):
            p = l.split(None, 3)
            if len(p) < 4:
                continue
            out.append({"vol": int(p[0]), "kind": int(p[1]),
                        "writable": p[2] == "1", "name": p[3]})
        return out

    def push_file(self, vol, path, local_path, chunk=2700, timeout=10.0, progress=None):
        """A/B OS update: stream local_path to <vol>:<path> in `put` chunks, then
        finalize+verify. Returns True iff the device reports `verified`. Raises on
        any per-chunk error (the target is only written at finalize, so a failed
        push never corrupts it)."""
        import base64
        with open(local_path, "rb") as f:
            data = f.read()
        total = len(data)
        off = 0
        while off < total:
            piece = data[off:off + chunk]
            b64 = base64.b64encode(piece).decode("ascii")
            r = self.command("put", vol, path, format(off, "x"), b64, timeout=timeout)
            if not r or int(r[0]) != len(piece):
                raise RuntimeError("put failed at offset 0x%x: %r" % (off, r))
            off += len(piece)
            if progress:
                progress(off, total)
        # Finalize does one on-device uno_fs_write of the whole staged buffer; a
        # multi-MB write over firmware BlockIO can take a while, so allow well
        # past the per-chunk timeout.
        r = self.command("put", vol, path, "done", format(total, "x"),
                         timeout=max(timeout, 300.0))
        return bool(r) and r[0].startswith("verified")

    # ---- receiving --------------------------------------------------------
    def _accept_loop(self):
        while not self._stop:
            try:
                c, _peer = self._srv.accept()
            except OSError:
                break
            with self._lock:
                self._sock = c
            self._connected.set()
            self.send("HELLO", "host 1")
            self._reader(c)

    def _reader(self, c):
        buf = b""
        while not self._stop:
            try:
                data = c.recv(4096)
            except OSError:
                break
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._dispatch(line.decode("utf-8", "replace").rstrip("\r"))
        with self._lock:
            if self._sock is c:
                self._sock = None
                self._connected.clear()

    def _dispatch(self, line):
        if not line:
            return
        typ, _, rest = line.partition(" ")
        if typ == "LOG":
            chan, _, text = rest.partition(" ")
            if self._log_cb: self._log_cb(chan, text)
        elif typ == "MSG":
            if self._msg_cb: self._msg_cb(rest)
        elif typ == "RSP":
            rid, _, tail = rest.partition(" ")
            status, _, text = tail.partition(" ")
            self._on_rsp(rid, status, text)
        elif typ == "CMD":
            rid, _, tail = rest.partition(" ")
            verb, _, args = tail.partition(" ")
            self._on_cmd(rid, verb, args)
        # HELLO / BYE: nothing required

    def _on_rsp(self, rid, status, text):
        with self._lock:
            rec = self._pending.get(rid)
        if not rec:
            return
        if status == "end":
            rec["ev"].set()
        elif status == "err":
            rec["err"] = True
            if text: rec["lines"].append(text)
        else:
            rec["lines"].append(text)

    def _on_cmd(self, rid, verb, args):
        """pc64 -> host commands (bidirectional).  Dispatch to a handler
        registered with on_command(); reply with RSP frames."""
        h = self._cmd_handlers.get(verb)
        if not h:
            self._raw("RSP %s err unknown-verb" % rid)
            self._raw("RSP %s end" % rid)
            return
        try:
            out = h(args)
            if out is None:
                out = []
            elif not isinstance(out, (list, tuple)):
                out = [str(out)]
            for ln in out:
                self._raw("RSP %s ok %s" % (rid, ln))
            self._raw("RSP %s end" % rid)
        except Exception as e:  # noqa: BLE001 - report any handler failure
            self._raw("RSP %s err %s" % (rid, str(e).replace("\n", " ")))
            self._raw("RSP %s end" % rid)


# ---- CLI -------------------------------------------------------------------
def _cli(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="0.0.0.0:5099",
                    help="bind address host:port (default 0.0.0.0:5099)")
    ap.add_argument("--push", nargs=3, metavar=("VOL", "PATH", "LOCALFILE"),
                    help="wait for pc64 to dial in, push LOCALFILE to <vol>:<path>, then exit")
    ap.add_argument("--chunk", type=int, default=2700, help="push chunk size (raw bytes; fits the 4 KB device line buffer)")
    ap.add_argument("--bootnext", type=int, metavar="N",
                    help="after --push, set BootNext=N (boot Boot#### N next reset)")
    ap.add_argument("--reboot", action="store_true", help="after --push, reboot the target")
    a = ap.parse_args(argv)
    host, _, port = a.listen.rpartition(":")
    link = UnoAutoLink(host or "0.0.0.0", int(port))

    if a.push:                                   # one-shot A/B OS-update flow
        vol, path, localfile = a.push
        link.listen()
        print("waiting for pc64 to dial in on %s ..." % a.listen)
        if not link.wait_connected(180):
            print("FAIL: no connection"); link.close(); return 1
        import os
        size = os.path.getsize(localfile)
        print("pushing %s (%d bytes) -> vol %s:%s" % (localfile, size, vol, path))
        last = [0]
        def prog(done, tot):
            pct = done * 100 // tot
            if pct != last[0]:
                last[0] = pct
                sys.stdout.write("\r  %d%% (%d/%d)" % (pct, done, tot)); sys.stdout.flush()
        try:
            ok = link.push_file(int(vol), path, localfile, chunk=a.chunk, progress=prog)
        except Exception as e:  # noqa: BLE001
            print("\nFAIL: " + str(e)); link.close(); return 1
        print("\n%s" % ("verified" if ok else "FAIL: not verified"))
        if ok and a.bootnext is not None:
            try:
                link.bootnext(a.bootnext); print("BootNext=%d set" % a.bootnext)
            except Exception as e:  # noqa: BLE001
                print("bootnext failed: " + str(e))
        if ok and a.reboot:
            print("rebooting target"); link.reboot()
        link.close()
        return 0 if ok else 1

    link.on_log(lambda ch, t: print("[%-7s] %s" % (ch, t)))
    link.on_message(lambda m: print("<msg> " + m))
    link.listen()
    print("unoauto_remote listening on %s. Set pc64 STRESS.CFG:" % a.listen)
    print("    remote=<this-machine-ip>:%s   (QEMU SLIRP guest: 10.0.2.2:%s)" % (port, port))
    print("Type a command line to send (probe / vols / launch 0 / py print(6*7) /")
    print("  uptime / reboot / bootnext <n>). Prefix /msg for a free-form message.")
    print("For an A/B OS push use: --push <vol> <path> <localfile> [--reboot]. Ctrl-D quits.\n")
    try:
        for raw in sys.stdin:
            cmd = raw.strip()
            if not cmd:
                continue
            if cmd.startswith("/msg "):
                link.message(cmd[5:]); continue
            verb, _, rest = cmd.partition(" ")
            args = rest.split(" ") if rest else []
            try:
                for l in link.command(verb, *args):
                    print("  " + l)
                print("  ok")
            except Exception as e:  # noqa: BLE001
                print("  ! " + str(e))
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
