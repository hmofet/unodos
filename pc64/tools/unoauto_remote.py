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
from contextlib import contextmanager


def qoi_decode(data):
    """Decode a QOI byte string to raw RGBA (4 bytes/pixel), matching the encoder
    in pc64/unoauto_screen.c. Used by `UnoAutoLink.screen_grab`. Pure-Python."""
    import struct
    if data[:4] != b"qoif":
        raise ValueError("not a QOI stream")
    w, h, _ch, _cs = struct.unpack(">IIBB", data[4:14])
    out = bytearray(w * h * 4)
    idx = [(0, 0, 0, 0)] * 64
    r, g, b, a = 0, 0, 0, 255
    p, end, o = 14, len(data) - 8, 0
    total = w * h * 4
    while o < total:
        if p < end:
            op = data[p]; p += 1
            if op == 0xFE:                       # RGB
                r, g, b = data[p], data[p + 1], data[p + 2]; p += 3
            elif op == 0xFF:                     # RGBA
                r, g, b, a = data[p], data[p + 1], data[p + 2], data[p + 3]; p += 4
            elif (op & 0xC0) == 0x00:            # INDEX
                r, g, b, a = idx[op & 0x3F]
            elif (op & 0xC0) == 0x40:            # DIFF
                r = (r + ((op >> 4) & 3) - 2) & 0xFF
                g = (g + ((op >> 2) & 3) - 2) & 0xFF
                b = (b + (op & 3) - 2) & 0xFF
            elif (op & 0xC0) == 0x80:            # LUMA
                b2 = data[p]; p += 1
                vg = (op & 0x3F) - 32
                r = (r + vg - 8 + ((b2 >> 4) & 0x0F)) & 0xFF
                g = (g + vg) & 0xFF
                b = (b + vg - 8 + (b2 & 0x0F)) & 0xFF
            else:                                # RUN (length has a -1 bias)
                for _ in range((op & 0x3F) + 1):
                    out[o:o + 4] = bytes((r, g, b, a)); o += 4
                idx[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = (r, g, b, a)
                continue
            idx[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = (r, g, b, a)
        out[o:o + 4] = bytes((r, g, b, a)); o += 4
    return bytes(out)


class _SerialStream:
    """Adapt a pyserial Serial to the tiny socket-shaped interface the reader and
    writer use (recv()/sendall()/close()), so the exact same URC line protocol
    runs over a UART.  recv() blocks until at least one byte (like a socket), and
    returns b"" only once the port is closed - so the reader loop's `if not data`
    EOF test behaves the same as it does for TCP."""
    def __init__(self, ser):
        self._ser = ser
        self._closed = False

    def recv(self, n=4096):
        while not self._closed:
            b = self._ser.read(1)                 # blocks up to the read-timeout
            if b:
                extra = getattr(self._ser, "in_waiting", 0)
                if extra:
                    b += self._ser.read(extra)    # drain the rest of the burst
                return b
        return b""

    def sendall(self, data):
        self._ser.write(data)
        self._ser.flush()

    def close(self):
        self._closed = True
        try:
            self._ser.close()
        except Exception:  # noqa: BLE001
            pass


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
        self._peer_hello = threading.Event()   # set when the guest's HELLO arrives
        self._guard_token = None
        self._stop = False
        self._disc = None                       # discovery responder socket
        self._disc_cb = None                    # cb(text) note on each offer

    # ---- lifecycle --------------------------------------------------------
    def listen(self, discover=True):
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind((self.host, self.port))
        self._srv.listen(1)
        threading.Thread(target=self._accept_loop, daemon=True).start()
        if discover:
            self._start_discovery(self.port)
        return self

    def on_discovery(self, cb):   self._disc_cb = cb      # cb(text) per offer

    # ---- zero-config discovery responder (netdisc, UDP :5400) -------------
    # A device booted with `discover` in DEBUG.CFG broadcasts a UNODISC PROBE;
    # we answer with an OFFER carrying this listener's ip:port and the device
    # dials in with no address configured (see pc64/netdisc.h). Best-effort: if
    # :5400 is taken, we skip it and a static remote= address still works.
    def _start_discovery(self, urc_port):
        try:
            d = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            d.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            d.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            d.bind(("0.0.0.0", 5400))
        except OSError:
            return
        d.settimeout(0.3)
        self._disc = d
        threading.Thread(target=self._disc_loop, args=(urc_port,), daemon=True).start()

    def _disc_loop(self, urc_port):
        name = (socket.gethostname() or "host").split()[0]
        while not self._stop and self._disc:
            try:
                data, src = self._disc.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                t = data.decode("ascii", "replace").split()
            except Exception:  # noqa: BLE001
                continue
            # UNODISC 1 PROBE <role> <name> <api> -> reply with our OFFER
            if len(t) >= 3 and t[0] == "UNODISC" and t[2] == "PROBE":
                ip = self._local_ip_toward(src[0])
                offer = "UNODISC 1 OFFER host %s 1 %s %d" % (name, ip, urc_port)
                try:
                    self._disc.sendto(offer.encode(), src)
                except OSError:
                    pass
                if self._disc_cb:
                    who = t[4] if len(t) >= 5 else (t[3] if len(t) >= 4 else "a device")
                    self._disc_cb("disc: offered %s:%d to %s (%s)" % (ip, urc_port, who, src[0]))

    @staticmethod
    def _local_ip_toward(dst):
        """The local IP the device can reach us on: the egress interface toward
        the prober (connect sends nothing, just resolves the route)."""
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((dst, 9))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"
        finally:
            s.close()

    def attach_stream(self, stream):
        """Drive the link over an already-open byte stream instead of listening
        for a TCP dial-in.  `stream` needs recv()/sendall()/close() (a connected
        socket, or a _SerialStream).  This is the NIC-independent path: the pc64
        stick sets `remote-serial` in STRESS.CFG and speaks URC over its UART, so
        a box whose only NIC is the one being debugged can still be driven."""
        with self._lock:
            self._sock = stream
        self._connected.set()
        self.send("HELLO", "host 1")
        threading.Thread(target=self._reader, args=(stream,), daemon=True).start()
        return self

    def attach_serial(self, device, baud=115200):
        """Open a real serial port (pyserial) and drive the link over it."""
        import serial  # pyserial; only needed for the serial transport
        ser = serial.Serial(device, baud, timeout=0.2)
        return self.attach_stream(_SerialStream(ser))

    def close(self):
        self._stop = True
        for s in (self._sock, self._srv, self._disc):
            try:
                if s: s.close()
            except OSError:
                pass

    def wait_connected(self, timeout=30.0):
        return self._connected.wait(timeout)

    def wait_hello(self, timeout=30.0):
        """Block until the guest's HELLO arrives - it means pc64 is booted and
        draining its receive path, so commands won't be lost.  Essential on the
        serial transport, which has no connection handshake: a command sent
        before the guest is reading vanishes into an unread FIFO."""
        return self._peer_hello.wait(timeout)

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

    # ---- host-attested guard (dead-man's switch for risky verbs) -----------
    def guard(self, timeout_s, action="reboot", **k):
        """Arm the dead-man's switch before a risky verb. If the box cannot
        service an inbound URC command within timeout_s (the signature of a
        wedge), the debug watchdog hard-resets it and it re-dials home. Any
        later command refreshes the deadline; call safe() once the op returns.
        Returns the session token (int)."""
        r = self.command("guard", int(timeout_s), action, **k)
        self._guard_token = None
        for w in (r[0].split() if r else []):
            if w.startswith("token="):
                try: self._guard_token = int(w.split("=", 1)[1])
                except ValueError: pass
        return self._guard_token

    def pet(self, **k):
        """Explicit keep-alive for a legitimately long op (any command also
        refreshes implicitly). Returns ['petted'] or ['not-armed']."""
        return self.command("pet", **k)

    def safe(self, **k):
        """Disarm the guard (the op returned; stand down). Presents the token
        from guard() so a stale disarm can't clear a fresh guard."""
        if self._guard_token is not None:
            return self.command("safe", self._guard_token, **k)
        return self.command("safe", **k)

    @contextmanager
    def guarded(self, timeout_s, action="reboot"):
        """`with link.guarded(15): link.command("iwl","rerun")` - arm, run, and
        stand down on clean return. If the body wedges the box, control never
        comes back here and neither does any pet, so the box resets on its own -
        which is the point. (safe() may itself time out against a wedged box;
        that's swallowed.)"""
        self.guard(timeout_s, action)
        try:
            yield self
        finally:
            try: self.safe(timeout=2)
            except Exception: pass

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

    def devices(self, **k):
        """Read-only PCI device listing (needs unodevices; raises if not built).

        One dict per device: loc/vendor/device/cls plus `name` (the class name)
        and `driver` (None until unodevices phase 2 reports binding state, then
        the bound driver's name or None for UNCLAIMED). `raw` keeps the device's
        own line, since the format belongs to unodevices, not to URC.

        Splitting the trailing column assumes the class name is a single token
        (`display`, `ethernet` — what uno_devmgr.h specifies); a class name with
        a space in it would be mis-split, so trust `raw` over `name`/`driver` if
        that format ever changes."""
        out = []
        for l in self.command("devices", **k):
            p = l.split(None, 3)
            if len(p) < 3:
                continue
            ven, _, dev = p[1].partition(":")
            rest = p[3] if len(p) > 3 else ""
            # phase 2 appends a driver column; phase 1 stops at the class name.
            name, driver = rest, None
            if " " in rest:
                name, _, last = rest.rpartition(" ")
                driver = None if last == "UNCLAIMED" else last
            out.append({"loc": p[0], "vendor": ven, "device": dev, "cls": p[2],
                        "name": name, "driver": driver, "raw": l})
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

    # ---- raw-disk authoring (partition/format disk B) ---------------------
    def disks(self, **k):
        """List raw disks: dicts of idx/name/sectors/writable/is_boot."""
        out = []
        for l in self.command("disks", **k):
            p = l.split()
            if len(p) >= 5:
                out.append({"idx": int(p[0]), "name": p[1], "sectors": int(p[2]),
                            "writable": p[3] == "1", "is_boot": p[4] == "1"})
        return out

    def arm(self, disk, **k):     return self.command("arm", disk, **k)
    def disarm(self, **k):        return self.command("disarm", **k)
    def gptinit(self, disk, **k): return self.command("gptinit", disk, **k)

    def mkpart(self, disk, first, last, type="esp", name="UNO-ESP", **k):
        return self.command("mkpart", disk, format(first, "x"), format(last, "x"), type, name, **k)

    def mkfs(self, disk, first, sectors, label="UNODOS", **k):
        return self.command("mkfs", disk, format(first, "x"), format(sectors, "x"), label,
                            timeout=k.pop("timeout", 180.0), **k)

    def prepdisk(self, disk, label="UNODOS", **k):
        """Author a fresh GPT + ESP and format it FAT32 (one destructive op)."""
        return self.command("prepdisk", disk, label, timeout=k.pop("timeout", 180.0), **k)

    def mkdir(self, vol, path, **k):
        """Create one directory on a mounted volume (its parent must exist).
        Idempotent: returns ['created'] or ['exists']."""
        return self.command("mkdir", vol, path, **k)

    def makeboot(self, disk, desc="UnoDOS", path=r"\EFI\BOOT\BOOTX64.EFI", **k):
        """Author a UEFI boot entry for the ESP on <disk> (after prepdisk + files)."""
        return self.command("makeboot", disk, desc, path, **k)

    def install_dir(self, disk, esp_dir, label="UNODOS", progress=None):
        """Full install to a raw disk: arm + prepdisk (partition/format), create
        the directory tree + push every file under esp_dir onto the new volume,
        then author a boot entry. Returns True. DESTRUCTIVE - erases the disk."""
        import os
        self.arm(disk)
        r = self.prepdisk(disk, label)
        if not (r and r[0] == "prepared"):
            raise RuntimeError("prepdisk failed: %r" % r)
        vol = None                                   # the fresh writable native-FAT vol
        for l in self.command("vols"):
            p = l.split(None, 3)
            if p[1] == "1" and p[2] == "1" and int(p[0]) > 0:
                vol = int(p[0])
        if vol is None:
            raise RuntimeError("the fresh volume did not mount")
        # collect files + the set of directories they need, shallowest first
        files, dirs = [], set()
        # Derive directory names from the NATIVE relpath, before converting to
        # the device's backslash form. Taking os.path.dirname() of an already
        # backslashed path returns "" on any POSIX host (posixpath splits on "/"
        # only), so `dirs` came out empty, no mkdir was ever issued, and every
        # push to a nested path failed. Silent on Windows, broken on the Linux
        # boxes that actually drive these installs.
        for root, _, fs in os.walk(esp_dir):
            for fn in fs:
                lp = os.path.join(root, fn)
                rel = os.path.relpath(lp, esp_dir)
                files.append((lp, rel.replace(os.sep, "\\")))
                d = os.path.dirname(rel)
                while d:
                    dirs.add(d.replace(os.sep, "\\")); d = os.path.dirname(d)
        for d in sorted(dirs, key=lambda s: s.count("\\")):
            self.mkdir(vol, d)
        for i, (lp, rel) in enumerate(files):
            if progress:
                progress(i, len(files), rel)
            self.push_file(vol, rel, lp)
        self.makeboot(disk)
        return True

    def install(self, disk, make_default=False, **k):
        """Clone the running OS onto <disk> over URC in one armed op: prepdisk +
        native tree clone of the boot ESP. Removable-path bootable
        (\\EFI\\BOOT\\BOOTX64.EFI); writes NO NVRAM Boot#### entry (runtime
        SetVariable is refused post-detach), so `make_default` is inert here.
        Arm the disk first (arm echoes its size and refuses the boot disk)."""
        a = [disk] + (["default"] if make_default else [])
        return self.command("install", *a, timeout=k.pop("timeout", 300.0), **k)

    def readsec(self, disk, lba, n=1, **k):
        import base64
        lines = self.command("readsec", disk, format(lba, "x"), n, **k)
        return base64.b64decode("".join(lines))

    def writesec(self, disk, lba, data, **k):
        import base64
        return self.command("writesec", disk, format(lba, "x"),
                            base64.b64encode(data).decode(), **k)

    # ---- remote desktop: screen grab (the OUT half; key/pointer are the IN) --
    def screen_info(self, **k):
        """`screen info` -> (width, height) of the device desktop."""
        r = self.command("screen", "info", **k)
        p = r[0].split() if r else []
        return (int(p[0]), int(p[1])) if len(p) >= 2 else (0, 0)

    SCREEN_READ_LEN = 2880               # matches SCREEN_READ_MAX on the device

    def _screen_pull(self, n, to=15.0):
        """Pull `n` staged bytes with bounded `screen read <off> <len>` slices
        (a whole frame is far too big for one URC response)."""
        import base64
        buf = bytearray()
        while len(buf) < n:
            rd = self.command("screen", "read", format(len(buf), "x"),
                              self.SCREEN_READ_LEN, timeout=to)
            part = base64.b64decode("".join(rd))
            if not part:
                raise RuntimeError("screen read returned nothing at off %d" % len(buf))
            buf += part
        return bytes(buf[:n])

    def screen_grab(self, scale=1, **k):
        """`screen grab [scale]` stages a full frame on the device and returns its
        `frame <w> <h> qoi <n>` header; the QOI payload is then pulled in bounded
        `screen read <off> <len>` slices. Returns (width, height, rgba_bytes),
        decoded to raw RGBA (4 bytes/pixel). Pure-Python, no deps."""
        to = k.pop("timeout", 15.0)
        r = self.command("screen", "grab", int(scale), timeout=to, **k)
        if not r:
            raise RuntimeError("empty screen reply")
        hdr = r[0].split()                       # frame W H qoi N
        if len(hdr) < 5 or hdr[0] != "frame":
            raise RuntimeError("bad frame header: %r" % r[0])
        w, h, n = int(hdr[1]), int(hdr[2]), int(hdr[4])
        return w, h, qoi_decode(self._screen_pull(n, to))

    def screen_grab_delta(self, scale=1, **k):
        """`screen grab delta [scale]`: the device diffs against the previous grab
        and returns either a full `frame` keyframe or a `delta` of the changed
        tiles. Returns a dict describing the update:
            {'keyframe':True,  'w','h', 'qoi':<full-frame QOI bytes>}
            {'keyframe':False, 'w','h','cols','tw','th','nch','idx':[...],
             'qoi':<changed-tile strip QOI bytes, b'' if nch==0>}
        Feed it to `screen_stream` to reconstruct the current full frame."""
        to = k.pop("timeout", 15.0)
        r = self.command("screen", "grab", "delta", int(scale), timeout=to, **k)
        if not r:
            raise RuntimeError("empty screen reply")
        hdr = r[0].split()
        if hdr[0] == "frame":                    # frame W H qoi N
            w, h, n = int(hdr[1]), int(hdr[2]), int(hdr[4])
            return {"keyframe": True, "w": w, "h": h, "qoi": self._screen_pull(n, to)}
        if hdr[0] == "delta":                    # delta ew eh cols tw th nch strip total
            ew, eh, cols, tw, th, nch, strip, total = (int(x) for x in hdr[1:9])
            u = {"keyframe": False, "w": ew, "h": eh, "cols": cols, "tw": tw,
                 "th": th, "nch": nch, "idx": [], "qoi": b""}
            if nch > 0 and total > 0:
                blob = self._screen_pull(total, to)
                u["qoi"] = blob[:strip]
                man = blob[strip:]
                u["idx"] = [man[i * 2] | (man[i * 2 + 1] << 8) for i in range(nch)]
            return u
        raise RuntimeError("unknown screen reply: %r" % r[0])

    def screen_stream(self, state, scale=1, **k):
        """Apply one `screen grab delta` update to `state` (a dict; pass {} to
        start) and return (w, h, rgba) of the reconstructed full frame. Mirrors
        the C# client's canvas compositor: a keyframe replaces the canvas, a delta
        blits only its changed tiles."""
        u = self.screen_grab_delta(scale, **k)
        if u["keyframe"]:
            state["w"], state["h"] = u["w"], u["h"]
            state["rgba"] = bytearray(qoi_decode(u["qoi"]))
            return u["w"], u["h"], bytes(state["rgba"])
        if not state.get("rgba") or state.get("w") != u["w"] or state.get("h") != u["h"]:
            raise RuntimeError("delta with no matching canvas (seed with a full grab first)")
        if u["nch"] > 0:
            strip = qoi_decode(u["qoi"])          # tw x (nch*th) RGBA
            tw, th, cols = u["tw"], u["th"], u["cols"]
            w, h, rgba = state["w"], state["h"], state["rgba"]
            for i, t in enumerate(u["idx"]):
                col, row = t % cols, t // cols
                dx, dy = col * tw, row * th
                vw, vh = min(tw, w - dx), min(th, h - dy)
                for yy in range(max(0, vh)):
                    so = ((i * th + yy) * tw) * 4
                    do = ((dy + yy) * w + dx) * 4
                    rgba[do:do + vw * 4] = strip[so:so + vw * 4]
        return state["w"], state["h"], bytes(state["rgba"])

    # ---- server-side session capture (the device records on its own tick) --
    @staticmethod
    def _rec_stat(lines):
        """Parse a `frames N bytes B ...` record-status line into a dict."""
        d = {}
        if lines:
            t = lines[0].split()
            for i in range(0, len(t) - 1, 2):
                try:
                    d[t[i]] = int(t[i + 1])
                except ValueError:
                    pass
        return d

    def screen_record_start(self, scale=1, fps=10, **k):
        """`screen record start [scale] [fps]` -> status dict."""
        return self._rec_stat(self.command("screen", "record", "start",
                                            int(scale), int(fps), **k))

    def screen_record_stop(self, **k):
        """`screen record stop` -> final status dict (ring retained for reading)."""
        return self._rec_stat(self.command("screen", "record", "stop", **k))

    def screen_record_status(self, **k):
        """`screen record status` -> live status dict."""
        return self._rec_stat(self.command("screen", "record", "status", **k))

    def screen_record_read_all(self, nbytes, **k):
        """Pull the whole recorded ring (`nbytes` from the stop/status stat)."""
        import base64
        to = k.pop("timeout", 20.0)
        buf = bytearray()
        while len(buf) < nbytes:
            rd = self.command("screen", "record", "read", format(len(buf), "x"),
                              self.SCREEN_READ_LEN, timeout=to)
            part = base64.b64decode("".join(rd))
            if not part:
                break
            buf += part
        return bytes(buf[:nbytes])

    def screen_record_frames(self, stat, **k):
        """Pull the ring described by `stat` and reconstruct every recorded frame
        to raw RGBA. Returns a list of (w, h, rgba). Mirrors the client
        compositor: a keyframe replaces the canvas, a delta blits its tiles."""
        data = self.screen_record_read_all(stat.get("bytes", 0), **k)
        ew, eh, cols = stat["ew"], stat["eh"], stat["cols"]
        tw, th = stat["tw"], stat["th"]
        frames, canvas, p = [], None, 0
        while p + 12 <= len(data):
            typ = data[p]
            nch = data[p + 2] | (data[p + 3] << 8)
            strip = data[p + 4] | (data[p + 5] << 8) | (data[p + 6] << 16) | (data[p + 7] << 24)
            payload = data[p + 8] | (data[p + 9] << 8) | (data[p + 10] << 16) | (data[p + 11] << 24)
            p += 12
            pl = data[p:p + payload]
            p += payload
            if typ == 0:                                     # keyframe
                canvas = bytearray(qoi_decode(pl))
            else:                                            # delta
                if canvas is None:
                    raise RuntimeError("delta before keyframe in recording")
                if nch > 0 and strip > 0:
                    st = qoi_decode(pl[:strip])
                    man = pl[strip:]
                    idx = [man[i * 2] | (man[i * 2 + 1] << 8) for i in range(nch)]
                    for i, t in enumerate(idx):
                        col, row = t % cols, t // cols
                        dx, dy = col * tw, row * th
                        vw, vh = min(tw, ew - dx), min(th, eh - dy)
                        for yy in range(max(0, vh)):
                            so = ((i * th + yy) * tw) * 4
                            do = ((dy + yy) * ew + dx) * 4
                            canvas[do:do + vw * 4] = st[so:so + vw * 4]
            frames.append((ew, eh, bytes(canvas)))
        return frames

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
        elif typ == "HELLO":
            self._peer_hello.set()    # guest is up and reading; safe to drive it
        # BYE: nothing required

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
    ap.add_argument("--serial", metavar="DEVICE[:BAUD]",
                    help="drive over a serial port (e.g. COM3 or /dev/ttyUSB0, optional :baud, "
                         "default 115200) instead of TCP; the pc64 stick uses `remote-serial` "
                         "in STRESS.CFG. NIC-independent - for a box whose only NIC is broken.")
    ap.add_argument("--push", nargs=3, metavar=("VOL", "PATH", "LOCALFILE"),
                    help="wait for pc64 to dial in, push LOCALFILE to <vol>:<path>, then exit")
    ap.add_argument("--chunk", type=int, default=2700, help="push chunk size (raw bytes; fits the 4 KB device line buffer)")
    ap.add_argument("--bootnext", type=int, metavar="N",
                    help="after --push, set BootNext=N (boot Boot#### N next reset)")
    ap.add_argument("--reboot", action="store_true", help="after --push, reboot the target")
    ap.add_argument("--prepdisk", nargs=2, metavar=("DISK", "LABEL"),
                    help="wait for pc64 to dial in, then partition+format raw DISK as a fresh "
                         "FAT32 ESP (DESTRUCTIVE - use `disks` to find the index), then exit")
    ap.add_argument("--install", nargs=2, metavar=("DISK", "ESP_DIR"),
                    help="full install to raw DISK: partition + format + copy the ESP_DIR tree "
                         "(e.g. build/esp) + author a boot entry (DESTRUCTIVE), then exit")
    a = ap.parse_args(argv)
    host, _, port = a.listen.rpartition(":")
    link = UnoAutoLink(host or "0.0.0.0", int(port))

    if a.install:                                # full install-to-internal-disk flow
        disk, esp_dir = int(a.install[0]), a.install[1]
        link.listen()
        print("waiting for pc64 to dial in on %s ..." % a.listen)
        if not link.wait_connected(180):
            print("FAIL: no connection"); link.close(); return 1
        try:
            ds = link.disks()
            tgt = next((d for d in ds if d["idx"] == disk), None)
            if not tgt or tgt["is_boot"]:
                print("FAIL: disk %d missing or is the boot disk. Disks: %r" % (disk, ds))
                link.close(); return 1
            print("DESTRUCTIVE: erasing %s (%.1f MB) and installing UnoDOS from %s"
                  % (tgt["name"], tgt["sectors"] * 512 / 1e6, esp_dir))
            def prog(i, n, rel):
                sys.stdout.write("\r  [%d/%d] %s          " % (i + 1, n, rel)); sys.stdout.flush()
            link.install_dir(disk, esp_dir, progress=prog)
            print("\ninstalled + boot entry added. Reboot to boot the disk (or use reboot).")
        except Exception as e:  # noqa: BLE001
            print("\nFAIL: " + str(e)); link.close(); return 1
        link.close()
        return 0

    if a.prepdisk:                               # one-shot partition+format flow
        disk, label = int(a.prepdisk[0]), a.prepdisk[1]
        link.listen()
        print("waiting for pc64 to dial in on %s ..." % a.listen)
        if not link.wait_connected(180):
            print("FAIL: no connection"); link.close(); return 1
        try:
            ds = link.disks()
            tgt = next((d for d in ds if d["idx"] == disk), None)
            if not tgt:
                print("FAIL: no disk %d. Disks: %r" % (disk, ds)); link.close(); return 1
            if tgt["is_boot"]:
                print("FAIL: disk %d is the boot disk - refused." % disk); link.close(); return 1
            print("DESTRUCTIVE: about to erase %s (%d sectors, %.1f MB) and format it FAT32."
                  % (tgt["name"], tgt["sectors"], tgt["sectors"] * 512 / 1e6))
            link.arm(disk)
            r = link.prepdisk(disk, label)
            print("prepdisk: %s" % (r[0] if r else "ok"))
        except Exception as e:  # noqa: BLE001
            print("FAIL: " + str(e)); link.close(); return 1
        link.close()
        return 0

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
    if a.serial:                                 # NIC-independent serial transport
        dev, baud = a.serial, 115200
        if ":" in a.serial:
            h, _, t = a.serial.rpartition(":")
            if t.isdigit():
                dev, baud = h, int(t)
        try:
            link.attach_serial(dev, baud)
        except ImportError:
            print("FAIL: pyserial not installed (pip install pyserial)"); return 1
        except Exception as e:  # noqa: BLE001
            print("FAIL: cannot open serial %s: %s" % (dev, e)); return 1
        print("unoauto_remote on serial %s @ %d baud. Set pc64 STRESS.CFG: remote-serial" % (dev, baud))
    else:
        link.on_discovery(lambda t: print(t))
        link.listen()
        print("unoauto_remote listening on %s. Set pc64 DEBUG.CFG:" % a.listen)
        print("    remote=<this-machine-ip>:%s   (QEMU SLIRP guest: 10.0.2.2:%s)" % (port, port))
        print("    or just `discover` - this tool answers UNODISC probes on UDP :5400.")
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
