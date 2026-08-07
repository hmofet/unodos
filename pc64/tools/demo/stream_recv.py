#!/usr/bin/env python3
"""stream_recv - the host end of unostream (pc64/UNOSTREAM.md).

Listens on a TCP port, accepts ONE connection from the guest (`stream start
<host-ip> <port> [fps] [scale]` over URC; from a QEMU SLIRP guest the host is
10.0.2.2), parses the 16-byte hello + binary QOI keyframe/delta frames,
maintains the RGBA canvas, and pipes every frame raw into ffmpeg:

    ffmpeg -f rawvideo -pixel_format rgba -video_size WxH -framerate FPS -i -
           -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p out.mp4

Alongside the mp4 it writes:
    out.timing.jsonl   one JSON line per received frame
                       {"i","t","bytes","type","seg"}  (type 0=key 1=delta)
    out.png            the final canvas (cursor included - the guest
                       composites it before encoding)
    out.stats.json     frames/keyframes/deltas/bytes/decode_errors/segments/
                       geometry + first/last arrival times

A hello arriving MID-stream (first byte 0x55, never a frame type) is a stream
reset - the desktop resolution changed. The current mp4 segment is finalized
and a new file suffixed -2 (-3, ...) is started on the new geometry.

stdlib only; the QOI decoder is reused from tools/unoauto_remote.py.
"""
import argparse, json, os, socket, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # pc64/tools
from unoauto_remote import qoi_decode              # noqa: E402

TILE = 32


def write_png(path, w, h, rgba):
    """Minimal RGBA8 PNG writer (stdlib zlib; no deps)."""
    import zlib

    def chunk(tag, data):
        c = tag + data
        return (struct.pack(">I", len(data)) + c +
                struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(h):
        raw.append(0)                              # filter: none
        raw += rgba[y * w * 4:(y + 1) * w * 4]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


class StreamReceiver:
    """Accept one unostream connection and record it. Counters are public so a
    gate can assert on them: frames, keyframes, deltas, bytes_rx,
    decode_errors, segments, t_first, t_last, plus the live canvas/w/h."""

    def __init__(self, port, out="out.mp4", host="0.0.0.0", ffmpeg="ffmpeg",
                 verbose=False):
        self.port, self.host, self.out, self.ffmpeg = port, host, out, ffmpeg
        self.verbose = verbose
        self.frames = self.keyframes = self.deltas = 0
        self.bytes_rx = 0
        self.decode_errors = 0
        self.segments = 0
        self.t_first = self.t_last = None
        self.w = self.h = self.fps = self.scale = 0
        self.canvas = None
        self.connected = False
        self.error = None
        self._srv = None
        self._ff = None
        self._timing = None

    # ---- lifecycle ---------------------------------------------------------
    def listen(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.host, self.port))
        s.listen(1)
        self._srv = s
        return self

    def serve_once(self, accept_timeout=180.0):
        """Block until one connection is served to EOF (or error)."""
        if self._srv is None:
            self.listen()
        self._srv.settimeout(accept_timeout)
        try:
            conn, peer = self._srv.accept()
        except socket.timeout:
            self.error = "no connection within %.0fs" % accept_timeout
            return self._finish()
        self.connected = True
        if self.verbose:
            print("stream_recv: %s:%d connected" % peer)
        base, _ = os.path.splitext(self.out)
        self._timing = open(base + ".timing.jsonl", "w")
        try:
            self._run(conn)
        except RuntimeError as e:                  # protocol desync / ffmpeg loss
            self.error = str(e)
        finally:
            conn.close()
            self._finish()
        return self.error is None

    # ---- wire --------------------------------------------------------------
    @staticmethod
    def _recv_exact(conn, n):
        buf = b""
        while len(buf) < n:
            part = conn.recv(n - len(buf))
            if not part:
                return None                        # EOF
            buf += part
        return buf

    def _seg_path(self):
        base, ext = os.path.splitext(self.out)
        return self.out if self.segments == 1 else "%s-%d%s" % (base, self.segments, ext)

    def _start_segment(self):
        self.segments += 1
        path = self._seg_path()
        cmd = [self.ffmpeg, "-y", "-loglevel", "error",
               "-f", "rawvideo", "-pixel_format", "rgba",
               "-video_size", "%dx%d" % (self.w, self.h),
               "-framerate", str(self.fps or 30), "-i", "-",
               "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
               "-pix_fmt", "yuv420p", path]
        try:
            self._ff = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        except OSError as e:
            raise RuntimeError("cannot spawn ffmpeg (%s): %s" % (self.ffmpeg, e))
        if self.verbose:
            print("stream_recv: segment %d -> %s (%dx%d @ %d fps)"
                  % (self.segments, path, self.w, self.h, self.fps))

    def _close_segment(self):
        if self._ff:
            try:
                self._ff.stdin.close()
            except OSError:
                pass
            self._ff.wait()
            self._ff = None

    def _hello(self, blob):
        if blob[:4] != b"UNSM":
            raise RuntimeError("bad hello magic %r" % blob[:4])
        ver, _pad, fps, scale = blob[4], blob[5], blob[6], blob[7]
        w, h = struct.unpack("<HH", blob[8:12])
        if ver != 1:
            raise RuntimeError("unknown protocol version %d" % ver)
        self._close_segment()                      # mid-stream hello = reset
        self.w, self.h, self.fps, self.scale = w, h, fps, scale
        self.canvas = bytearray(w * h * 4)
        self._start_segment()

    def _apply_delta(self, payload):
        if not payload:
            return                                 # valid "nothing changed"
        sw, sh = struct.unpack(">II", payload[4:12])
        if sw != TILE or sh % TILE:
            raise RuntimeError("bad delta strip geometry %dx%d" % (sw, sh))
        nch = sh // TILE
        strip_len = len(payload) - 2 * nch
        if strip_len < 14 + 8:
            raise RuntimeError("delta payload too small for its manifest")
        strip = qoi_decode(payload[:strip_len])
        man = payload[strip_len:]
        idx = [man[i * 2] | (man[i * 2 + 1] << 8) for i in range(nch)]
        cols = (self.w + TILE - 1) // TILE
        for i, t in enumerate(idx):
            col, row = t % cols, t // cols
            dx, dy = col * TILE, row * TILE
            vw, vh = min(TILE, self.w - dx), min(TILE, self.h - dy)
            for yy in range(max(0, vh)):
                so = ((i * TILE + yy) * TILE) * 4
                do = ((dy + yy) * self.w + dx) * 4
                self.canvas[do:do + vw * 4] = strip[so:so + vw * 4]

    def _run(self, conn):
        first = True
        while True:
            b0 = self._recv_exact(conn, 1)
            if b0 is None:
                return                             # clean EOF
            if b0[0] == 0x55:                      # 'U': a (re)hello
                rest = self._recv_exact(conn, 15)
                if rest is None:
                    return
                self.bytes_rx += 16
                self._hello(b0 + rest)
                first = False
                continue
            if first:
                raise RuntimeError("stream did not start with a hello (got 0x%02x)" % b0[0])
            if b0[0] not in (0, 1):
                raise RuntimeError("bad frame type 0x%02x (desync)" % b0[0])
            hdr = self._recv_exact(conn, 7)
            if hdr is None:
                return
            (plen,) = struct.unpack("<I", hdr[3:7])
            payload = self._recv_exact(conn, plen) if plen else b""
            if payload is None:
                return
            now = time.time()
            self.bytes_rx += 8 + plen
            if self.t_first is None:
                self.t_first = now
            self.t_last = now
            try:
                if b0[0] == 0:
                    rgba = qoi_decode(payload)
                    if len(rgba) != self.w * self.h * 4:
                        raise RuntimeError("keyframe size mismatch")
                    self.canvas = bytearray(rgba)
                    self.keyframes += 1
                else:
                    self._apply_delta(payload)
                    self.deltas += 1
            except Exception as e:                 # noqa: BLE001 - count, keep going
                self.decode_errors += 1
                if self.verbose:
                    print("stream_recv: decode error on frame %d: %s" % (self.frames, e))
                continue
            self.frames += 1
            if self._ff:
                try:
                    self._ff.stdin.write(self.canvas)
                except (OSError, BrokenPipeError) as e:
                    raise RuntimeError("ffmpeg pipe broke: %s" % e)
            self._timing.write(json.dumps({"i": self.frames - 1, "t": now,
                                           "bytes": 8 + plen, "type": b0[0],
                                           "seg": self.segments}) + "\n")

    # ---- teardown ----------------------------------------------------------
    def _finish(self):
        self._close_segment()
        if self._timing:
            self._timing.close()
            self._timing = None
        base, _ = os.path.splitext(self.out)
        if self.canvas is not None:
            write_png(base + ".png", self.w, self.h, self.canvas)
        stats = {"frames": self.frames, "keyframes": self.keyframes,
                 "deltas": self.deltas, "bytes": self.bytes_rx,
                 "decode_errors": self.decode_errors, "segments": self.segments,
                 "w": self.w, "h": self.h, "fps": self.fps, "scale": self.scale,
                 "t_first": self.t_first, "t_last": self.t_last,
                 "error": self.error}
        with open(base + ".stats.json", "w") as f:
            json.dump(stats, f, indent=2)
        if self._srv:
            self._srv.close()
            self._srv = None
        return self.error is None

    def px(self, x, y):
        """Canvas pixel (r,g,b,a) - for cursor-visibility assertions."""
        o = (y * self.w + x) * 4
        return tuple(self.canvas[o:o + 4])


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=5398)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--out", default="out.mp4")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--accept-timeout", type=float, default=180.0)
    a = ap.parse_args(argv)
    rx = StreamReceiver(a.port, out=a.out, host=a.host, ffmpeg=a.ffmpeg,
                        verbose=True)
    rx.listen()
    print("stream_recv: listening on %s:%d  (guest: stream start <this-ip> %d "
          "[fps] [scale]; QEMU SLIRP guest uses 10.0.2.2)"
          % (a.host, a.port, a.port))
    try:
        ok = rx.serve_once(a.accept_timeout)
    except RuntimeError as e:
        print("stream_recv: FAIL " + str(e))
        return 1
    print("stream_recv: %d frames (%d key, %d delta), %d bytes, "
          "%d decode error(s), %d segment(s)"
          % (rx.frames, rx.keyframes, rx.deltas, rx.bytes_rx,
             rx.decode_errors, rx.segments))
    return 0 if ok and rx.decode_errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
