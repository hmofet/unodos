#!/usr/bin/env python3
"""UnoDOS/Genesis tape (UT01) <-> WAV tooling.

The console writes blocks through the PSG (Model 1 headphone jack) and
reads them through a comparator on control port 2 pin 1 (see
genesis/tape.i and docs/GENESIS-STORAGE.md). This tool is the PC side
of the loop: encode a file into a playable WAV, decode a recorded WAV
back into a file, and self-test the round trip.

Format (KCS at 1200 baud): '0' = one cycle of 1200 Hz, '1' = two cycles
of 2400 Hz; byte = start(0) + 8 LSB-first + 2 stop(1); block = leader
(2400 Hz) + "UT01" + name[12] + len.w(BE) + data + sum.w(BE).

Usage:
  python mktape.py encode <file> <out.wav> [--name NAME]
  python mktape.py decode <in.wav> <out-file>
  python mktape.py selftest
"""
import sys, wave, struct, os

RATE = 44100
F0, F1 = 1200, 2400
AMP = 110          # 8-bit unsigned samples around 128

class Encoder:
    """Edge-accurate AFSK: every bit cell is whole cycles (1x1200 or
    2x2400), emitted as toggling half-periods on a running time
    accumulator - no phase glitches at bit boundaries."""
    def __init__(self):
        self.samples = bytearray()
        self.t = 0.0
        self.cur = 0
        self.level = True

    def halves(self, n, freq):
        for _ in range(n):
            self.t += 1.0 / (2 * freq)
            end = int(self.t * RATE)
            v = 128 + AMP if self.level else 128 - AMP
            self.samples += bytes([v]) * (end - self.cur)
            self.cur = end
            self.level = not self.level

    def bit(self, b):
        if b:
            self.halves(4, F1)   # two cycles of 2400 Hz
        else:
            self.halves(2, F0)   # one cycle of 1200 Hz

    def byte(self, v):
        self.bit(0)
        for i in range(8):
            self.bit((v >> i) & 1)
        self.bit(1)
        self.bit(1)

def encode(data, name):
    name = name.encode("ascii")[:12].ljust(12, b"\0")
    blk = b"UT01" + name + struct.pack(">H", len(data)) + data
    blk += struct.pack(">H", sum(data) & 0xFFFF)
    e = Encoder()
    e.halves(7200, F1)                     # ~1.5s leader at 2400 Hz
    for byte in blk:
        e.byte(byte)
    e.halves(2400, F1)                     # tail leader
    return bytes(e.samples)

def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(RATE)
        w.writeframes(samples)

def read_wav(path):
    with wave.open(path, "rb") as w:
        raw = w.readframes(w.getnframes())
        width, rate, ch = w.getsampwidth(), w.getframerate(), w.getnchannels()
    if width == 2:
        raw = bytes(((struct.unpack_from("<h", raw, i * 2)[0] >> 8) + 128) & 0xFF
                    for i in range(len(raw) // 2))
    if ch == 2:
        raw = raw[::2]
    return raw, rate

def half_periods(samples, rate):
    """Zero-crossing half-period lengths, scaled to console poll counts
    (the console's SHORT/LONG thresholds are reused verbatim)."""
    mid = 128
    last = samples[0] >= mid
    start = 0
    out = []
    for i, s in enumerate(samples):
        lvl = s >= mid
        if lvl != last:
            out.append((i - start) / rate)
            start = i
            last = lvl
    return out

# the console's classifier, in seconds (TAPE_THRESH/BREAK at ~5.7us/poll)
TH_SHORT = 55 * 5.7e-6
TH_BREAK = 220 * 5.7e-6

class Decoder:
    """Mirror of tape_feed_half + tape_rx_byte (genesis/tape.i)."""
    def __init__(self):
        self.state = 0; self.cnt = 0; self.bits = 0; self.byte = 0
        self.need = 0; self.bstate = 0; self.length = 0; self.sum = 0
        self.name = bytearray(); self.data = bytearray(); self.done = 0
        self.sumhi = 0

    def feed(self, sec):
        sym = 0 if sec < TH_SHORT else (1 if sec < TH_BREAK else 2)
        if sym == 2:
            self.state = 0; self.cnt = 0
            return
        st = self.state
        if st == 0:
            if sym == 0:
                self.cnt += 1
                if self.cnt >= 64:
                    self.state = 1
            else:
                self.cnt = 0
        elif st == 1:
            if sym == 1:
                self.state = 2
        elif st == 2:
            if sym == 1:
                self.state, self.bits, self.byte = 3, 0, 0
            else:
                self.state = 1
        elif st == 3:
            if sym == 1:
                self.state = 4
            else:
                self.state, self.need = 5, 3
        elif st == 4:
            self._record(0) if sym == 1 else self._resync()
        elif st == 5:
            if sym == 0:
                self.need -= 1
                if self.need == 0:
                    self._record(1)
            else:
                self._resync()

    def _resync(self):
        self.state = 1

    def _record(self, bit):
        self.byte = (self.byte >> 1) | (0x80 if bit else 0)
        self.bits += 1
        if self.bits == 8:
            self._rx(self.byte)
            self.state = 1
        else:
            self.state = 3

    def _rx(self, b):
        p = self.bstate
        if p < 4:
            if b != b"UT01"[p]:
                self.bstate = 0; self.sum = 0; return
        elif p < 16:
            self.name.append(b)
        elif p == 16:
            self.length = b << 8
        elif p == 17:
            self.length |= b
            if self.length > 2047:
                self.bstate = 0; return
        else:
            i = p - 18
            if i < self.length:
                self.data.append(b); self.sum = (self.sum + b) & 0xFFFF
            elif i == self.length:
                self.sumhi = b << 8; self.bstate += 1; return
            else:
                self.done = 1 if (self.sumhi | b) == self.sum else 2
                return
        self.bstate += 1

def decode(samples, rate):
    d = Decoder()
    for hp in half_periods(samples, rate):
        d.feed(hp)
        if d.done:
            break
    return d

def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "encode":
        data = open(sys.argv[2], "rb").read()[:2047]
        name = os.path.basename(sys.argv[2]).upper()
        for a, v in zip(sys.argv, sys.argv[1:]):
            if a == "--name":
                name = v
        write_wav(sys.argv[3], encode(data, name))
        print(f"wrote {sys.argv[3]}: {len(data)} bytes as {name[:12]}")
    elif len(sys.argv) >= 2 and sys.argv[1] == "decode":
        samples, rate = read_wav(sys.argv[2])
        d = decode(samples, rate)
        if d.done != 1:
            sys.exit(f"decode failed (state {d.done}, got {len(d.data)})")
        open(sys.argv[3], "wb").write(bytes(d.data))
        name = d.name.split(b"\0")[0].decode("ascii", "replace")
        print(f"decoded {name}: {len(d.data)} bytes -> {sys.argv[3]}")
    elif len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        payload = b"Round trip through the UnoDOS tape format.\r" * 8
        d = decode(encode(payload, "SELFTEST.TXT"), RATE)
        assert d.done == 1, f"decode state {d.done}"
        assert bytes(d.data) == payload, "payload mismatch"
        print(f"selftest OK: {len(payload)} bytes round-tripped")
    else:
        print(__doc__)

if __name__ == "__main__":
    main()
